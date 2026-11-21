#include "bridge/execution_dispatcher.hpp"
#include "bridge/http_transport_adapter.hpp"
#include "bridge/operation_cache_interface.hpp"
#include "bridge/operation_registry.hpp"
#include "bridge/v8_sdk_host_bridge.hpp"

#include <libplatform/libplatform.h>
#include <v8.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace lamp::bridge;

class InMemoryTestCache final : public IOperationCache {
public:
    OperationCacheLookup Lookup(
        const OperationDescriptor& descriptor,
        const RequestEnvelope& request
    ) override {
        const std::string key = descriptor.id + ":" +
            std::string(request.payload.begin(), request.payload.end());
        if (const auto it = entries_.find(key); it != entries_.end()) {
            return {CacheResult::HitLocal, it->second, false};
        }
        return {CacheResult::Miss, std::nullopt, false};
    }

    void Store(
        const OperationDescriptor& descriptor,
        const RequestEnvelope& request,
        const ResultEnvelope& result
    ) override {
        const std::string key = descriptor.id + ":" +
            std::string(request.payload.begin(), request.payload.end());
        entries_[key] = result;
    }

    void InvalidateOperation(const OperationId&) override {
        entries_.clear();
    }

private:
    std::unordered_map<std::string, ResultEnvelope> entries_;
};

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

struct V8PlatformGuard final {
    std::unique_ptr<v8::Platform> platform;

    V8PlatformGuard() {
        platform = v8::platform::NewDefaultPlatform();
        v8::V8::InitializePlatform(platform.get());
        if (!v8::V8::Initialize()) {
            throw std::runtime_error("Failed to initialize V8");
        }
    }

    ~V8PlatformGuard() {
        v8::V8::Dispose();
        v8::V8::DisposePlatform();
    }
};

} // namespace

int main() {
    V8PlatformGuard v8_guard;

    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator =
        v8::ArrayBuffer::Allocator::NewDefaultAllocator();

    v8::Isolate* isolate = v8::Isolate::New(create_params);
    {
        v8::Isolate::Scope isolate_scope(isolate);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = v8::Context::New(isolate);
        v8::Context::Scope context_scope(context);

        auto registry = std::make_shared<OperationRegistry>();
        auto host_bridge = std::make_shared<V8SdkHostBridge>(
            isolate,
            context,
            registry
        );
        EXPECT(host_bridge->InstallGlobalBridge());

        // Define SDK-compatible createLampApp binding inside V8 and invoke it
        // using the canonical INTERFACE.md shape (`users.get`, `cache: { ttl: 60 }`).
        const char* js_program = R"JS(
            function createLampApp(config = {}) {
                const ops = new Map();
                const app = {
                    register(id, def) {
                        const meta = {
                            id,
                            execution: {
                                mode: config.execution ?? "adaptive",
                                concurrency: "concurrent",
                                timeoutMs: def.execution?.timeoutMs ?? 30000
                            },
                            cache: {
                                enabled: Boolean(config.cache?.enabled && (def.cache?.ttl ?? 0) > 0),
                                ttlSeconds: def.cache?.ttl ?? 0
                            }
                        };
                        ops.set(id, def);
                        globalThis.__lampNativeBridge.registerOperation(
                            id,
                            meta,
                            (rawJsonInput, metadataJson) => {
                                const parsed = rawJsonInput ? JSON.parse(rawJsonInput) : {};
                                const metaObj = metadataJson ? JSON.parse(metadataJson) : {};
                                if (def.input && !def.input.validate(parsed)) {
                                    const err = new Error("Invalid input schema");
                                    err.code = "INVALID_ARGUMENT";
                                    err.identifier = "INPUT_VALIDATION_FAILED";
                                    throw err;
                                }
                                return def.handler({
                                    input: parsed,
                                    ctx: { metadata: metaObj }
                                });
                            }
                        );
                        return app;
                    },
                    start() {
                        globalThis.__lampNativeBridge.markStarted(JSON.stringify(config));
                    },
                    stop() {
                        globalThis.__lampNativeBridge.markStopped();
                    }
                };
                return app;
            }

            let invocations = 0;
            const UserInputSchema = {
                validate(v) {
                    return typeof v === "object" && v !== null && typeof v.id === "string" && v.id.length > 0;
                }
            };

            const app = createLampApp({
                transport: "auto",
                serialization: "protobuf",
                execution: "adaptive",
                cache: { enabled: true }
            });

            app.register("users.get", {
                input: UserInputSchema,
                cache: { ttl: 60 },
                handler: async ({ input, ctx }) => {
                    invocations += 1;
                    await Promise.resolve();
                    return {
                        id: input.id,
                        seq: invocations,
                        subject: ctx.metadata["lamp.auth.subject"] ?? "anon"
                    };
                }
            });

            app.start();
            globalThis.__lampTestStop = () => app.stop();
        )JS";

        v8::TryCatch try_catch(isolate);
        v8::Local<v8::String> src = v8::String::NewFromUtf8(
            isolate,
            js_program
        ).ToLocalChecked();
        v8::Local<v8::Script> script;
        EXPECT(v8::Script::Compile(context, src).ToLocal(&script));
        v8::Local<v8::Value> eval_out;
        if (!script->Run(context).ToLocal(&eval_out)) {
            if (try_catch.HasCaught()) {
                v8::String::Utf8Value err(isolate, try_catch.Exception());
                std::cerr << "V8 JS exception: " << (*err ? *err : "<unknown>") << "\n";
            }
        }
        EXPECT(!eval_out.IsEmpty());
        EXPECT(!try_catch.HasCaught());

        EXPECT(host_bridge->IsStarted());
        EXPECT(host_bridge->RegisteredHandlerCount() == 1);
        EXPECT(registry->Contains("users.get"));

        auto cache = std::make_shared<InMemoryTestCache>();
        ExecutionDispatcher dispatcher(registry, host_bridge, {}, cache);

        // 1. Valid request executes async Promise JS handler & caches result
        {
            RequestEnvelope req;
            req.request_id = 1;
            req.operation_id = "users.get";
            req.transport = TransportKind::Http;
            req.format = SerializationFormat::Json;
            req.AddMetadata("lamp.auth.subject", "alice");
            const std::string body = R"({"id":"u-42"})";
            req.payload.assign(body.begin(), body.end());

            const auto res = dispatcher.Dispatch(req);
            EXPECT(res.IsSuccess());
            EXPECT(res.backend == ExecutionBackend::JavaScript);
            const std::string out(res.payload.begin(), res.payload.end());
            EXPECT(out.find("\"id\":\"u-42\"") != std::string::npos);
            EXPECT(out.find("\"seq\":1") != std::string::npos);
            EXPECT(out.find("\"subject\":\"alice\"") != std::string::npos);

            const auto http_resp = HttpTransportAdapter::BuildResponse(res, {});
            EXPECT(http_resp.statusCode() == 200);
        }

        // 2. Repeated identical request hits LocalOperationCache (`seq` stays 1)
        {
            RequestEnvelope req;
            req.request_id = 2;
            req.operation_id = "users.get";
            req.transport = TransportKind::Http;
            req.format = SerializationFormat::Json;
            const std::string body = R"({"id":"u-42"})";
            req.payload.assign(body.begin(), body.end());

            const auto res = dispatcher.Dispatch(req);
            EXPECT(res.IsSuccess());
            EXPECT(res.backend == ExecutionBackend::Cache);
            const std::string out(res.payload.begin(), res.payload.end());
            EXPECT(out.find("\"seq\":1") != std::string::npos);
        }

        // 3. Invalid schema input throws structured INVALID_ARGUMENT -> HTTP 400
        {
            RequestEnvelope req;
            req.request_id = 3;
            req.operation_id = "users.get";
            req.transport = TransportKind::Http;
            req.format = SerializationFormat::Json;
            const std::string body = R"({"id":""})";
            req.payload.assign(body.begin(), body.end());

            const auto res = dispatcher.Dispatch(req);
            EXPECT(!res.IsSuccess());
            EXPECT(res.error.code == OperationErrorCode::InvalidArgument);
            const auto http_resp = HttpTransportAdapter::BuildResponse(res, {});
            EXPECT(http_resp.statusCode() == 400);
        }

        // 4. Stop SDK lifecycle & verify IsStarted() transitions to false
        {
            v8::Local<v8::String> stop_src =
                v8::String::NewFromUtf8Literal(isolate, "globalThis.__lampTestStop();");
            v8::Local<v8::Script> stop_script;
            EXPECT(v8::Script::Compile(context, stop_src).ToLocal(&stop_script));
            EXPECT(!stop_script->Run(context).IsEmpty());
            EXPECT(!host_bridge->IsStarted());
        }
    }

    isolate->Dispose();
    delete create_params.array_buffer_allocator;

    if (g_failed != 0) {
        std::cout << "Lamp V8 SDK host bridge: FAILED (" << g_failed << " assertions)\n";
        return 1;
    }
    std::cout << "Lamp V8 SDK host bridge: passed\n";
    return 0;
}
