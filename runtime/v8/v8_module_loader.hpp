#pragma once

#include "v8_context.hpp"

#include <v8.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::v8 {

// ============================================================
// Module Loader Errors
// ============================================================

enum class V8ModuleErrorType {
    Resolution,
    FileSystem,
    Compilation,
    Instantiation,
    Evaluation,
    InvalidState
};

class V8ModuleError final : public std::runtime_error {
public:
    V8ModuleError(
        V8ModuleErrorType type,
        std::string message,
        std::string modulePath = {}
    )
        : std::runtime_error(std::move(message)),
          type_(type),
          modulePath_(std::move(modulePath)) {}

    [[nodiscard]]
    V8ModuleErrorType type() const noexcept {
        return type_;
    }

    [[nodiscard]]
    const std::string& modulePath() const noexcept {
        return modulePath_;
    }

private:
    V8ModuleErrorType type_;
    std::string modulePath_;
};

// ============================================================
// Module Loader Configuration
// ============================================================

struct V8ModuleLoaderConfig final {

    // Root directory for application modules.
    std::filesystem::path rootDirectory =
        std::filesystem::current_path();

    // Maximum size of one module source file.
    std::size_t maxModuleBytes = 16 * 1024 * 1024;

    // Maximum number of modules in the cache.
    std::size_t maxModules = 4096;

    // Restrict filesystem modules to rootDirectory.
    bool restrictToRoot = true;
};

// ============================================================
// V8 Module Loader
// ============================================================
//
// Responsibilities:
//
// - Resolve relative and absolute module specifiers.
// - Restrict filesystem access to an application root.
// - Read JavaScript source files.
// - Compile ECMAScript modules.
// - Cache compiled modules.
// - Resolve static imports.
// - Instantiate dependency graphs.
// - Evaluate synchronous module graphs.
//
// Limitations:
//
// - No TypeScript transpilation.
// - No Node.js package resolution.
// - No bare package specifiers.
// - No JSON modules.
// - No dynamic import() support.
// - No asynchronous top-level-await completion.
// - No hot module replacement.
//
// Lifetime:
//
// The loader borrows a V8Context.
//
// The context and its isolate must outlive this loader.
//
// All V8 operations must occur while the isolate is
// entered and protected by the appropriate V8 locker.
//
// ============================================================

class V8ModuleLoader final {
public:

    explicit V8ModuleLoader(
        V8Context& context,
        V8ModuleLoaderConfig config = {}
    )
        : context_(&context),
          config_(std::move(config)) {

        if (!context_->isValid()) {
            throw V8ModuleError(
                V8ModuleErrorType::InvalidState,
                "Module loader requires a valid V8 context"
            );
        }

        if (config_.maxModuleBytes == 0 ||
            config_.maxModules == 0) {

            throw V8ModuleError(
                V8ModuleErrorType::InvalidState,
                "Module loader limits must be greater than zero"
            );
        }

        std::error_code error;

        rootDirectory_ = std::filesystem::canonical(
            config_.rootDirectory,
            error
        );

        if (error ||
            !std::filesystem::is_directory(rootDirectory_)) {

            throw V8ModuleError(
                V8ModuleErrorType::FileSystem,
                "Invalid module root directory: " +
                    config_.rootDirectory.string()
            );
        }
    }

    ~V8ModuleLoader() noexcept {
        dispose();
    }

    V8ModuleLoader(const V8ModuleLoader&) = delete;
    V8ModuleLoader& operator=(const V8ModuleLoader&) = delete;

    V8ModuleLoader(V8ModuleLoader&&) = delete;
    V8ModuleLoader& operator=(V8ModuleLoader&&) = delete;

    // ========================================================
    // Load and Evaluate Entry Module
    // ========================================================
    //
    // The entry module and its static dependencies are
    // compiled, instantiated, and evaluated.
    //
    // This implementation accepts only synchronous
    // evaluation completion.
    //
    // The loader must remain alive while modules are
    // being instantiated or evaluated.
    //
    // ========================================================

    void executeEntry(
        const std::filesystem::path& entryPath
    ) {
        ensureValid();

        V8Context::Scope scope(*context_);

        auto* isolate = scope.isolate();
        auto context = scope.context();

        ::v8::TryCatch tryCatch(isolate);

        auto resolvedPath = resolveEntry(entryPath);

        auto module = loadModule(
            isolate,
            context,
            resolvedPath
        );

        // ----------------------------------------------------
        // Instantiate Dependency Graph
        // ----------------------------------------------------

        if (module->GetStatus() ==
            ::v8::Module::Status::kUninstantiated) {

            // Preserve the previous active loader so nested
            // module-loading operations restore it correctly.

            auto* previousLoader = activeLoader_;
            activeLoader_ = this;

            try {
                const auto instantiated =
                    module->InstantiateModule(
                        context,
                        &V8ModuleLoader::resolveCallback
                    );

                activeLoader_ = previousLoader;

                if (instantiated.IsNothing() ||
                    !instantiated.FromJust()) {

                    throw makeV8Error(
                        isolate,
                        tryCatch,
                        V8ModuleErrorType::Instantiation,
                        resolvedPath
                    );
                }

            } catch (...) {
                activeLoader_ = previousLoader;
                throw;
            }
        }

        // ----------------------------------------------------
        // Evaluate Module Graph
        // ----------------------------------------------------

        const auto status = module->GetStatus();

        if (status == ::v8::Module::Status::kErrored) {
            throw V8ModuleError(
                V8ModuleErrorType::Evaluation,
                "Module is in an errored state",
                resolvedPath.string()
            );
        }

        if (status == ::v8::Module::Status::kEvaluated) {
            return;
        }

        if (status != ::v8::Module::Status::kInstantiated) {
            throw V8ModuleError(
                V8ModuleErrorType::InvalidState,
                "Module is not ready for evaluation",
                resolvedPath.string()
            );
        }

        ::v8::Local<::v8::Value> evaluationResult;

        if (!module->Evaluate(context)
                 .ToLocal(&evaluationResult)) {

            throw makeV8Error(
                isolate,
                tryCatch,
                V8ModuleErrorType::Evaluation,
                resolvedPath
            );
        }

        // ECMAScript module evaluation returns a Promise.
        //
        // A synchronous graph should settle immediately.
        // Pending evaluation is not supported by this
        // initial loader.

        if (evaluationResult->IsPromise()) {
            auto promise =
                evaluationResult.As<::v8::Promise>();

            if (promise->State() ==
                ::v8::Promise::PromiseState::kPending) {

                throw V8ModuleError(
                    V8ModuleErrorType::Evaluation,
                    "Asynchronous module evaluation "
                    "is not supported yet",
                    resolvedPath.string()
                );
            }

            if (promise->State() ==
                ::v8::Promise::PromiseState::kRejected) {

                throw V8ModuleError(
                    V8ModuleErrorType::Evaluation,
                    "Module evaluation promise was rejected",
                    resolvedPath.string()
                );
            }
        }

        if (module->GetStatus() ==
            ::v8::Module::Status::kErrored) {

            throw V8ModuleError(
                V8ModuleErrorType::Evaluation,
                "Module evaluation failed",
                resolvedPath.string()
            );
        }
    }

    // ========================================================
    // Cache Inspection
    // ========================================================

    [[nodiscard]]
    std::size_t cachedModuleCount() const noexcept {
        return modules_.size();
    }

    [[nodiscard]]
    const std::filesystem::path&
    rootDirectory() const noexcept {
        return rootDirectory_;
    }

    // ========================================================
    // Disposal
    // ========================================================
    //
    // Must not overlap module execution or callbacks.
    //
    // Context and isolate must remain alive.
    //
    // ========================================================

    void dispose() noexcept {
        if (disposed_) {
            return;
        }

        disposed_ = true;

        if (context_ == nullptr ||
            !context_->isValid()) {

            // Persistent V8 handles cannot safely be reset
            // after their isolate has been destroyed.
            //
            // Enforce the required lifetime ordering.

            if (!modules_.empty()) {
                std::terminate();
            }

            return;
        }

        V8Context::Scope scope(*context_);

        for (auto& [path, record] : modules_) {
            (void)path;
            record.module.Reset();
        }

        modules_.clear();
        modulePaths_.clear();
    }

private:

    // ========================================================
    // Cached Module Record
    // ========================================================

    struct ModuleRecord final {
        ::v8::Global<::v8::Module> module;
    };

    // ========================================================
    // Validate Loader State
    // ========================================================

    void ensureValid() const {
        if (disposed_ ||
            context_ == nullptr ||
            !context_->isValid()) {

            throw V8ModuleError(
                V8ModuleErrorType::InvalidState,
                "Module loader is disposed or its context is invalid"
            );
        }
    }

    // ========================================================
    // Root Containment Check
    // ========================================================

    [[nodiscard]]
    bool isWithinRoot(
        const std::filesystem::path& path
    ) const {
        auto rootIt = rootDirectory_.begin();
        auto pathIt = path.begin();

        for (;
             rootIt != rootDirectory_.end();
             ++rootIt, ++pathIt) {

            if (pathIt == path.end() ||
                *rootIt != *pathIt) {

                return false;
            }
        }

        return true;
    }

    // ========================================================
    // Canonicalize and Validate Path
    // ========================================================

    [[nodiscard]]
    std::filesystem::path validatePath(
        const std::filesystem::path& path
    ) const {
        std::error_code error;

        auto canonicalPath =
            std::filesystem::canonical(path, error);

        if (error) {
            throw V8ModuleError(
                V8ModuleErrorType::Resolution,
                "Cannot resolve module path: " +
                    path.string()
            );
        }

        if (config_.restrictToRoot &&
            !isWithinRoot(canonicalPath)) {

            throw V8ModuleError(
                V8ModuleErrorType::Resolution,
                "Module path escapes application root: " +
                    canonicalPath.string()
            );
        }

        if (!std::filesystem::is_regular_file(
                canonicalPath,
                error
            ) || error) {

            throw V8ModuleError(
                V8ModuleErrorType::FileSystem,
                "Module is not a regular file: " +
                    canonicalPath.string()
            );
        }

        return canonicalPath;
    }

    // ========================================================
    // Resolve Entry Path
    // ========================================================

    [[nodiscard]]
    std::filesystem::path resolveEntry(
        const std::filesystem::path& entryPath
    ) const {
        auto candidate = entryPath.is_absolute()
            ? entryPath
            : rootDirectory_ / entryPath;

        return validatePath(candidate);
    }

    // ========================================================
    // Resolve Imported Module
    // ========================================================
    //
    // Supported:
    //
    //   import "./routes.js";
    //   import "../utils.js";
    //   import "/absolute/path/module.js";
    //
    // Bare specifiers such as "express" are rejected.
    //
    // ========================================================

    [[nodiscard]]
    std::filesystem::path resolveImport(
        std::string_view specifier,
        const std::filesystem::path& referrer
    ) const {
        if (specifier.empty()) {
            throw V8ModuleError(
                V8ModuleErrorType::Resolution,
                "Empty module specifier",
                referrer.string()
            );
        }

        const std::string name(specifier);

        std::filesystem::path candidate;

        if (name.starts_with("./") ||
            name.starts_with("../")) {

            candidate = referrer.parent_path() / name;

        } else if (
            std::filesystem::path(name).is_absolute()
        ) {
            candidate = name;

        } else {
            throw V8ModuleError(
                V8ModuleErrorType::Resolution,
                "Unsupported module specifier: " + name,
                referrer.string()
            );
        }

        return validatePath(candidate);
    }

    // ========================================================
    // Read Module Source
    // ========================================================

    [[nodiscard]]
    std::string readSource(
        const std::filesystem::path& path
    ) const {
        std::error_code error;

        const auto fileSize =
            std::filesystem::file_size(path, error);

        if (error ||
            fileSize > config_.maxModuleBytes ||
            fileSize >
                static_cast<std::uintmax_t>(
                    std::numeric_limits<std::size_t>::max()
                )) {

            throw V8ModuleError(
                V8ModuleErrorType::FileSystem,
                "Module file is unavailable or exceeds size limit",
                path.string()
            );
        }

        std::ifstream file(
            path,
            std::ios::binary
        );

        if (!file) {
            throw V8ModuleError(
                V8ModuleErrorType::FileSystem,
                "Failed to open module",
                path.string()
            );
        }

        std::string source(
            static_cast<std::size_t>(fileSize),
            '\0'
        );

        if (!source.empty() &&
            !file.read(
                source.data(),
                static_cast<std::streamsize>(source.size())
            )) {

            throw V8ModuleError(
                V8ModuleErrorType::FileSystem,
                "Failed to read complete module source",
                path.string()
            );
        }

        return source;
    }

    // ========================================================
    // Create V8 String
    // ========================================================

    [[nodiscard]]
    static ::v8::Local<::v8::String> makeString(
        ::v8::Isolate* isolate,
        std::string_view value
    ) {
        if (value.size() >
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {

            throw V8ModuleError(
                V8ModuleErrorType::Compilation,
                "Module string exceeds V8 input limits"
            );
        }

        ::v8::Local<::v8::String> result;

        if (!::v8::String::NewFromUtf8(
                isolate,
                value.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(value.size())
            ).ToLocal(&result)) {

            throw V8ModuleError(
                V8ModuleErrorType::Compilation,
                "Failed to create V8 module string"
            );
        }

        return result;
    }

    // ========================================================
    // Compile and Cache Module
    // ========================================================

    [[nodiscard]]
    ::v8::Local<::v8::Module> loadModule(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Context> context,
        const std::filesystem::path& path
    ) {
        const auto key = path.string();

        if (auto iterator = modules_.find(key);
            iterator != modules_.end()) {

            return iterator->second.module.Get(isolate);
        }

        if (modules_.size() >= config_.maxModules) {
            throw V8ModuleError(
                V8ModuleErrorType::InvalidState,
                "Maximum cached module count exceeded",
                key
            );
        }

        auto source = readSource(path);

        ::v8::TryCatch tryCatch(isolate);

        auto sourceString = makeString(
            isolate,
            source
        );

        auto resourceName = makeString(
            isolate,
            key
        );

        ::v8::ScriptOrigin origin(
            resourceName,
            0,
            0,
            false,
            -1,
            ::v8::Local<::v8::Value>(),
            false,
            false,
            true
        );

        ::v8::ScriptCompiler::Source compilerSource(
            sourceString,
            origin
        );

        ::v8::Local<::v8::Module> module;

        if (!::v8::ScriptCompiler::CompileModule(
                isolate,
                &compilerSource
            ).ToLocal(&module)) {

            throw makeV8Error(
                isolate,
                tryCatch,
                V8ModuleErrorType::Compilation,
                path
            );
        }

        ModuleRecord record;
        record.module.Reset(isolate, module);

        modules_.emplace(
            key,
            std::move(record)
        );

        // Identity hash is used only as a bucket key.
        // Actual module identity is checked during lookup.

        modulePaths_[module->GetIdentityHash()]
            .push_back(key);

        return module;
    }

    // ========================================================
    // Find Referrer Module Path
    // ========================================================

    [[nodiscard]]
    std::filesystem::path findModulePath(
        ::v8::Isolate* isolate,
        ::v8::Local<::v8::Module> module
    ) const {
        const auto hash = module->GetIdentityHash();

        auto bucket = modulePaths_.find(hash);

        if (bucket == modulePaths_.end()) {
            throw V8ModuleError(
                V8ModuleErrorType::Resolution,
                "Unknown importing module"
            );
        }

        for (const auto& key : bucket->second) {
            auto iterator = modules_.find(key);

            if (iterator == modules_.end()) {
                continue;
            }

            auto cached = iterator->second.module.Get(isolate);

            if (cached == module) {
                return std::filesystem::path(key);
            }
        }

        throw V8ModuleError(
            V8ModuleErrorType::Resolution,
            "Unable to identify importing module"
        );
    }

    // ========================================================
    // Static Import Resolution Callback
    // ========================================================
    //
    // V8 invokes this callback while instantiating
    // a module graph.
    //
    // C++ exceptions must never escape this callback.
    //
    // ========================================================

    static ::v8::MaybeLocal<::v8::Module> resolveCallback(
        ::v8::Local<::v8::Context> context,
        ::v8::Local<::v8::String> specifier,
        ::v8::Local<::v8::FixedArray> importAttributes,
        ::v8::Local<::v8::Module> referrer
    ) {
        (void)importAttributes;

        auto* isolate = ::v8::Isolate::GetCurrent();

        if (isolate == nullptr) {
            return {};
        }

        auto* loader = activeLoader_;

        if (loader == nullptr) {
            throwJavaScriptError(
                isolate,
                "No active Lamp module loader"
            );

            return {};
        }

        try {
            ::v8::String::Utf8Value utf8(
                isolate,
                specifier
            );

            if (*utf8 == nullptr) {
                throw V8ModuleError(
                    V8ModuleErrorType::Resolution,
                    "Failed to decode import specifier"
                );
            }

            std::string name(
                *utf8,
                static_cast<std::size_t>(utf8.length())
            );

            auto referrerPath =
                loader->findModulePath(
                    isolate,
                    referrer
                );

            auto resolved =
                loader->resolveImport(
                    name,
                    referrerPath
                );

            return loader->loadModule(
                isolate,
                context,
                resolved
            );

        } catch (const std::exception& error) {

            throwJavaScriptError(
                isolate,
                error.what()
            );

            return {};

        } catch (...) {

            throwJavaScriptError(
                isolate,
                "Unknown native module resolution error"
            );

            return {};
        }
    }

    // ========================================================
    // JavaScript Error Creation
    // ========================================================

    static void throwJavaScriptError(
        ::v8::Isolate* isolate,
        std::string_view message
    ) noexcept {
        ::v8::Local<::v8::String> text;

        if (!::v8::String::NewFromUtf8(
                isolate,
                message.data(),
                ::v8::NewStringType::kNormal,
                static_cast<int>(
                    std::min(
                        message.size(),
                        static_cast<std::size_t>(
                            std::numeric_limits<int>::max()
                        )
                    )
                )
            ).ToLocal(&text)) {

            isolate->ThrowException(
                ::v8::Undefined(isolate)
            );

            return;
        }

        isolate->ThrowException(
            ::v8::Exception::Error(text)
        );
    }

    // ========================================================
    // Convert V8 Exception
    // ========================================================

    [[nodiscard]]
    static V8ModuleError makeV8Error(
        ::v8::Isolate* isolate,
        ::v8::TryCatch& tryCatch,
        V8ModuleErrorType type,
        const std::filesystem::path& path
    ) {
        std::string message = "Unknown V8 module error";

        if (!tryCatch.Exception().IsEmpty()) {
            ::v8::String::Utf8Value utf8(
                isolate,
                tryCatch.Exception()
            );

            if (*utf8 != nullptr) {
                message.assign(
                    *utf8,
                    static_cast<std::size_t>(utf8.length())
                );
            }
        }

        return V8ModuleError(
            type,
            std::move(message),
            path.string()
        );
    }

    // ========================================================
    // Internal State
    // ========================================================

    V8Context* context_ = nullptr;

    V8ModuleLoaderConfig config_{};

    std::filesystem::path rootDirectory_;

    std::unordered_map<
        std::string,
        ModuleRecord
    > modules_;

    std::unordered_map<
        int,
        std::vector<std::string>
    > modulePaths_;

    bool disposed_ = false;

    // Active loader during module instantiation.
    //
    // This must be set before calling InstantiateModule.
    //
    // A thread-local pointer is used because V8 invokes
    // module resolution callbacks synchronously.

    inline static thread_local V8ModuleLoader*
        activeLoader_ = nullptr;
};

} // namespace lamp::v8