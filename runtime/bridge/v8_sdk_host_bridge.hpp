#pragma once

// ============================================================
// Lamp Framework
// Phase 11 - V8 <-> TypeScript SDK Host Bridge
//
// File: runtime/bridge/v8_sdk_host_bridge.hpp
//
// Purpose:
//   Expose `globalThis.__lampNativeBridge` inside an embedded
//   V8 context so SDK `createLampApp` / `app.register(...)`
//   calls register real operations into OperationRegistry and
//   execute sync or microtask-settled Promise handlers via
//   ExecutionDispatcher.
//
// Threading:
//   Construct, install, execute, and destroy on the isolate's
//   owning V8 thread. Must be destroyed before isolate disposal.
// ============================================================

#include "execution_dispatcher.hpp"
#include "operation_descriptor.hpp"
#include "operation_registry.hpp"
#include "request_envelope.hpp"
#include "result_envelope.hpp"

#include <v8.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::bridge {

class V8SdkHostBridge final : public IJavaScriptOperationExecutor {
public:
    V8SdkHostBridge(
        v8::Isolate* isolate,
        v8::Local<v8::Context> context,
        std::shared_ptr<OperationRegistry> registry
    )
        : isolate_(isolate),
          registry_(std::move(registry)),
          owner_thread_(std::this_thread::get_id()) {
        if (isolate_ != nullptr && !context.IsEmpty()) {
            context_.Reset(isolate_, context);
        }
    }

    ~V8SdkHostBridge() override {
        handlers_.clear();
        context_.Reset();
    }

    V8SdkHostBridge(const V8SdkHostBridge&) = delete;
    V8SdkHostBridge& operator=(const V8SdkHostBridge&) = delete;

    // Installs `globalThis.__lampNativeBridge` in the entered V8 context.
    [[nodiscard]]
    bool InstallGlobalBridge() {
        if (!OnOwnerThread() || isolate_ == nullptr || context_.IsEmpty()) {
            return false;
        }

        v8::HandleScope handle_scope(isolate_);
        auto context = context_.Get(isolate_);
        if (context.IsEmpty()) {
            return false;
        }
        v8::Context::Scope context_scope(context);

        auto bridge_obj = v8::Object::New(isolate_);
        auto self_ext = v8::External::New(
            isolate_,
            this,
            v8::kExternalPointerTypeTagDefault
        );

        auto set_fn = [&](const char* name, v8::FunctionCallback cb) -> bool {
            v8::Local<v8::Function> fn;
            if (!v8::Function::New(context, cb, self_ext).ToLocal(&fn)) {
                return false;
            }
            return bridge_obj
                ->Set(
                    context,
                    v8::String::NewFromUtf8(isolate_, name).ToLocalChecked(),
                    fn
                )
                .FromMaybe(false);
        };

        if (!set_fn("registerOperation", &V8SdkHostBridge::OnRegisterOperation) ||
            !set_fn("markStarted", &V8SdkHostBridge::OnMarkStarted) ||
            !set_fn("markStopped", &V8SdkHostBridge::OnMarkStopped)) {
            return false;
        }

        return context->Global()
            ->Set(
                context,
                v8::String::NewFromUtf8Literal(isolate_, "__lampNativeBridge"),
                bridge_obj
            )
            .FromMaybe(false);
    }

    [[nodiscard]]
    bool IsStarted() const noexcept {
        return started_.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::size_t RegisteredHandlerCount() const noexcept {
        return handlers_.size();
    }

    // Executes an operation invoked via ExecutionDispatcher::Dispatch.
    [[nodiscard]]
    ResultEnvelope Execute(
        JavaScriptHandlerReference reference,
        const RequestEnvelope& request
    ) override {
        if (!OnOwnerThread()) {
            return MakeError(
                request,
                OperationErrorCode::Unavailable,
                "V8_WRONG_THREAD",
                "JavaScript execution requested from non-owner thread"
            );
        }

        if (isolate_ == nullptr || context_.IsEmpty()) {
            return MakeError(
                request,
                OperationErrorCode::Unavailable,
                "V8_UNAVAILABLE",
                "JavaScript runtime is unavailable"
            );
        }

        const auto it = handlers_.find(reference.handler_id);
        if (it == handlers_.end()) {
            return MakeError(
                request,
                OperationErrorCode::NotFound,
                "JS_HANDLER_NOT_FOUND",
                "JavaScript SDK handler was not found"
            );
        }

        if (request.IsCancelled()) {
            return ResultEnvelope::Cancelled(request);
        }
        if (request.IsExpired()) {
            return ResultEnvelope::DeadlineExceeded(request);
        }

        v8::HandleScope handle_scope(isolate_);
        auto context = context_.Get(isolate_);
        v8::Context::Scope context_scope(context);
        v8::TryCatch try_catch(isolate_);

        auto fn = it->second.Get(isolate_);
        if (fn.IsEmpty()) {
            return MakeError(
                request,
                OperationErrorCode::Unavailable,
                "V8_HANDLER_EMPTY",
                "JavaScript SDK handler handle is empty"
            );
        }

        const std::string raw_json(
            reinterpret_cast<const char*>(request.payload.data()),
            request.payload.size()
        );
        const std::string metadata_json = SerializeMetadataJson(request.metadata);

        v8::Local<v8::Value> args[2] = {
            v8::String::NewFromUtf8(
                isolate_,
                raw_json.data(),
                v8::NewStringType::kNormal,
                static_cast<int>(raw_json.size())
            ).ToLocalChecked(),
            v8::String::NewFromUtf8(
                isolate_,
                metadata_json.data(),
                v8::NewStringType::kNormal,
                static_cast<int>(metadata_json.size())
            ).ToLocalChecked()
        };

        v8::Local<v8::Value> result;
        if (!fn->Call(context, v8::Undefined(isolate_), 2, args).ToLocal(&result)) {
            return MapThrownValue(request, try_catch.Exception(), context);
        }

        // Support async SDK handlers resolving via V8 microtasks.
        if (result->IsPromise()) {
            auto promise = result.As<v8::Promise>();
            constexpr int kMaxMicrotaskCheckpoints = 64;
            for (int i = 0;
                 i < kMaxMicrotaskCheckpoints &&
                 promise->State() == v8::Promise::kPending;
                 ++i) {
                isolate_->PerformMicrotaskCheckpoint();
            }

            if (promise->State() == v8::Promise::kPending) {
                return MakeError(
                    request,
                    OperationErrorCode::NotImplemented,
                    "ASYNC_PENDING_UNSETTLED",
                    "Promise did not settle within synchronous V8 microtask turn"
                );
            }
            if (promise->State() == v8::Promise::kRejected) {
                return MapThrownValue(request, promise->Result(), context);
            }
            result = promise->Result();
        }

        if (result->IsUndefined()) {
            return ResultEnvelope::Success(
                request,
                {},
                SerializationFormat::Json,
                ExecutionBackend::JavaScript
            );
        }

        v8::Local<v8::String> json_out;
        if (!v8::JSON::Stringify(context, result).ToLocal(&json_out)) {
            return MakeError(
                request,
                OperationErrorCode::SerializationFailed,
                "JSON_ENCODE_FAILED",
                "Failed to serialize JS SDK output to JSON"
            );
        }

        v8::String::Utf8Value utf8(isolate_, json_out);
        if (*utf8 == nullptr) {
            return MakeError(
                request,
                OperationErrorCode::SerializationFailed,
                "JSON_UTF8_FAILED",
                "Failed to encode JS SDK output as UTF-8"
            );
        }

        const auto* ptr = reinterpret_cast<const std::uint8_t*>(*utf8);
        ResultEnvelope::Payload payload(ptr, ptr + utf8.length());
        return ResultEnvelope::Success(
            request,
            std::move(payload),
            SerializationFormat::Json,
            ExecutionBackend::JavaScript
        );
    }

private:
    static void OnRegisterOperation(const v8::FunctionCallbackInfo<v8::Value>& info) {
        auto* self = UnwrapSelf(info);
        auto* isolate = info.GetIsolate();
        if (self == nullptr || !self->registry_ || info.Length() < 3 ||
            !info[0]->IsString() || !info[1]->IsObject() || !info[2]->IsFunction()) {
            isolate->ThrowError("registerOperation expects (operationId, descriptor, handler)");
            return;
        }

        auto context = isolate->GetCurrentContext();
        v8::String::Utf8Value op_id_utf8(isolate, info[0]);
        if (*op_id_utf8 == nullptr) {
            isolate->ThrowError("Invalid operationId");
            return;
        }

        OperationDescriptor desc;
        desc.id = std::string(*op_id_utf8, op_id_utf8.length());
        desc.execution.mode = ExecutionMode::JavaScript;
        desc.input_format = SerializationFormat::Json;
        desc.output_format = SerializationFormat::Json;

        auto meta_obj = info[1].As<v8::Object>();
        ParseDescriptorOverrides(isolate, context, meta_obj, desc);

        const std::uint64_t handler_id = self->next_handler_id_++;
        JavaScriptHandlerReference ref{handler_id};

        OperationBackends backends;
        backends.javascript = ref;

        const auto reg_res = self->registry_->Register(std::move(desc), backends);
        if (!reg_res.Ok()) {
            const std::string msg = "Failed to register operation: " + reg_res.message;
            isolate->ThrowError(
                v8::String::NewFromUtf8(isolate, msg.c_str()).ToLocalChecked()
            );
            return;
        }

        v8::Global<v8::Function> persistent;
        persistent.Reset(isolate, info[2].As<v8::Function>());
        self->handlers_.emplace(handler_id, std::move(persistent));

        info.GetReturnValue().Set(v8::Number::New(isolate, static_cast<double>(handler_id)));
    }

    static void OnMarkStarted(const v8::FunctionCallbackInfo<v8::Value>& info) {
        if (auto* self = UnwrapSelf(info)) {
            self->started_.store(true, std::memory_order_release);
        }
    }

    static void OnMarkStopped(const v8::FunctionCallbackInfo<v8::Value>& info) {
        if (auto* self = UnwrapSelf(info)) {
            self->started_.store(false, std::memory_order_release);
        }
    }

    static V8SdkHostBridge* UnwrapSelf(
        const v8::FunctionCallbackInfo<v8::Value>& info
    ) noexcept {
        if (info.Data().IsEmpty() || !info.Data()->IsValue()) {
            return nullptr;
        }
        auto val = info.Data().As<v8::Value>();
        if (!val->IsExternal()) {
            return nullptr;
        }
        return static_cast<V8SdkHostBridge*>(
            val.As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault)
        );
    }

    static void ParseDescriptorOverrides(
        v8::Isolate* isolate,
        v8::Local<v8::Context> context,
        v8::Local<v8::Object> meta_obj,
        OperationDescriptor& desc
    ) {
        auto get_prop = [&](v8::Local<v8::Object> obj, const char* key)
            -> v8::Local<v8::Value> {
            v8::Local<v8::Value> v;
            if (obj->Get(
                    context,
                    v8::String::NewFromUtf8(isolate, key).ToLocalChecked()
                ).ToLocal(&v)) {
                return v;
            }
            return {};
        };

        if (auto cache_val = get_prop(meta_obj, "cache");
            !cache_val.IsEmpty() && cache_val->IsObject()) {
            auto cache_obj = cache_val.As<v8::Object>();
            auto enabled = get_prop(cache_obj, "enabled");
            auto ttl = get_prop(cache_obj, "ttlSeconds");
            if (!enabled.IsEmpty() && enabled->IsBoolean() && enabled->BooleanValue(isolate) &&
                !ttl.IsEmpty() && ttl->IsNumber()) {
                const auto secs = static_cast<std::int64_t>(
                    ttl->NumberValue(context).FromMaybe(0.0)
                );
                if (secs > 0) {
                    desc.cache.mode = CacheMode::Local;
                    desc.cache.ttl = std::chrono::seconds(secs);
                    // OperationRegistry requires cacheable operations to declare
                    // read_only and idempotent capabilities.
                    desc.capabilities.read_only = true;
                    desc.capabilities.idempotent = true;
                }
            }
        }

        if (auto exec_val = get_prop(meta_obj, "execution");
            !exec_val.IsEmpty() && exec_val->IsObject()) {
            auto exec_obj = exec_val.As<v8::Object>();
            if (auto timeout_val = get_prop(exec_obj, "timeoutMs");
                !timeout_val.IsEmpty() && timeout_val->IsNumber()) {
                const auto ms = static_cast<std::int64_t>(
                    timeout_val->NumberValue(context).FromMaybe(30000.0)
                );
                if (ms > 0) {
                    desc.limits.timeout = std::chrono::milliseconds(ms);
                }
            }
        }
    }

    [[nodiscard]]
    static std::string SerializeMetadataJson(const RequestMetadata& metadata) {
        std::string out = "{";
        for (std::size_t i = 0; i < metadata.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            AppendJsonString(out, metadata[i].key);
            out.push_back(':');
            AppendJsonString(out, metadata[i].value);
        }
        out.push_back('}');
        return out;
    }

    static void AppendJsonString(std::string& out, std::string_view text) {
        out.push_back('"');
        for (unsigned char c : text) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
                out.push_back(static_cast<char>(c));
            } else if (c >= 0x20) {
                out.push_back(static_cast<char>(c));
            }
        }
        out.push_back('"');
    }

    [[nodiscard]]
    ResultEnvelope MapThrownValue(
        const RequestEnvelope& request,
        v8::Local<v8::Value> thrown,
        v8::Local<v8::Context> context
    ) const {
        if (thrown.IsEmpty() || !thrown->IsObject()) {
            return MakeError(
                request,
                OperationErrorCode::ExecutionFailed,
                "JS_EXECUTION_ERROR",
                "JavaScript SDK operation threw an error"
            );
        }

        auto obj = thrown.As<v8::Object>();
        auto get_str = [&](const char* key) -> std::string {
            v8::Local<v8::Value> v;
            if (!obj->Get(
                    context,
                    v8::String::NewFromUtf8(isolate_, key).ToLocalChecked()
                ).ToLocal(&v) ||
                !v->IsString()) {
                return {};
            }
            v8::String::Utf8Value utf8(isolate_, v);
            return *utf8 ? std::string(*utf8, utf8.length()) : std::string{};
        };

        const std::string code_str = get_str("code");
        const std::string identifier = get_str("identifier");
        const std::string message = get_str("message");

        return MakeError(
            request,
            MapErrorCode(code_str),
            identifier.empty() ? std::string("JS_EXECUTION_ERROR") : identifier,
            message.empty() ? std::string("JavaScript operation failed") : message
        );
    }

    [[nodiscard]]
    static OperationErrorCode MapErrorCode(std::string_view code) noexcept {
        if (code == "INVALID_ARGUMENT") return OperationErrorCode::InvalidArgument;
        if (code == "UNAUTHENTICATED") return OperationErrorCode::Unauthenticated;
        if (code == "PERMISSION_DENIED") return OperationErrorCode::PermissionDenied;
        if (code == "NOT_FOUND") return OperationErrorCode::NotFound;
        if (code == "ALREADY_EXISTS") return OperationErrorCode::AlreadyExists;
        if (code == "CONFLICT") return OperationErrorCode::Conflict;
        if (code == "RESOURCE_EXHAUSTED") return OperationErrorCode::ResourceExhausted;
        if (code == "FAILED_PRECONDITION") return OperationErrorCode::FailedPrecondition;
        if (code == "UNAVAILABLE") return OperationErrorCode::Unavailable;
        if (code == "CANCELLED") return OperationErrorCode::Cancelled;
        if (code == "DEADLINE_EXCEEDED") return OperationErrorCode::DeadlineExceeded;
        if (code == "SERIALIZATION_FAILED") return OperationErrorCode::SerializationFailed;
        if (code == "NOT_IMPLEMENTED") return OperationErrorCode::NotImplemented;
        return OperationErrorCode::ExecutionFailed;
    }

    [[nodiscard]]
    static ResultEnvelope MakeError(
        const RequestEnvelope& request,
        OperationErrorCode code,
        std::string identifier,
        std::string message
    ) {
        OperationError err;
        err.code = code;
        err.identifier = std::move(identifier);
        err.message = std::move(message);
        err.retryable =
            (code == OperationErrorCode::Unavailable ||
             code == OperationErrorCode::ResourceExhausted);
        return ResultEnvelope::Failure(
            request,
            std::move(err),
            ExecutionBackend::JavaScript
        );
    }

    [[nodiscard]]
    bool OnOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner_thread_;
    }

    v8::Isolate* isolate_ = nullptr;
    v8::Global<v8::Context> context_;
    std::shared_ptr<OperationRegistry> registry_;
    std::thread::id owner_thread_;
    std::atomic<bool> started_{false};
    std::uint64_t next_handler_id_ = 1;
    std::unordered_map<std::uint64_t, v8::Global<v8::Function>> handlers_;
};

} // namespace lamp::bridge
