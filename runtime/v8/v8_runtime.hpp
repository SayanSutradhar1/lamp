#pragma once

#include "v8_platform.hpp"
#include "v8_isolate.hpp"
#include "v8_context.hpp"
#include "v8_script_engine.hpp"
#include "v8_native_binding.hpp"
#include "v8_module_loader.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lamp::v8
{

    // ============================================================
    // Runtime Errors
    // ============================================================

    class V8RuntimeError final : public std::runtime_error
    {
    public:
        explicit V8RuntimeError(std::string message)
            : std::runtime_error(std::move(message)) {}
    };

    // ============================================================
    // Runtime State
    // ============================================================

    enum class V8RuntimeState
    {
        Created,
        Initializing,
        Running,
        Stopping,
        Stopped,
        Failed
    };

    // ============================================================
    // Runtime Configuration
    // ============================================================

    struct V8RuntimeConfig final
    {
        V8PlatformConfig platform{};
        V8IsolateConfig isolate{};
        V8ContextConfig context{};
        V8NativeBindingConfig bindings{};
        V8ModuleLoaderConfig modules{};
    };

    // ============================================================
    // V8 Runtime
    // ============================================================
    //
    // Responsibilities:
    //
    // - Coordinate V8 runtime initialization.
    // - Own one isolate and JavaScript context.
    // - Own the script engine and module loader.
    // - Own the native binding registry.
    // - Expose a unified JavaScript execution API.
    // - Enforce deterministic component destruction order.
    //
    // Platform ownership:
    //
    // V8Platform is process-wide.
    //
    // This class initializes the platform when necessary,
    // but does not shut it down automatically.
    //
    // The application must shut down V8Platform only after
    // every V8Runtime instance has been destroyed.
    //
    // Threading:
    //
    // A runtime is intended to belong to one Lamp worker.
    //
    // Operations are serialized through operationMutex_.
    //
    // Shutdown must not be invoked reentrantly from a
    // JavaScript callback executing on this runtime.
    //
    // ============================================================

    class V8Runtime final
    {
    public:
        // ========================================================
        // Construction
        // ========================================================

        explicit V8Runtime(
            V8RuntimeConfig config = {})
            : config_(std::move(config)) {}

        ~V8Runtime() noexcept
        {
            shutdown();
        }

        V8Runtime(const V8Runtime &) = delete;
        V8Runtime &operator=(const V8Runtime &) = delete;

        V8Runtime(V8Runtime &&) = delete;
        V8Runtime &operator=(V8Runtime &&) = delete;

        // ========================================================
        // Initialization
        // ========================================================

        void initialize()
        {
            std::lock_guard<std::mutex> operationLock(
                operationMutex_);

            {
                std::lock_guard<std::mutex> stateLock(
                    stateMutex_);

                if (state_ == V8RuntimeState::Running)
                {
                    return;
                }

                if (state_ != V8RuntimeState::Created)
                {
                    throw V8RuntimeError(
                        "V8 runtime cannot be initialized "
                        "from its current state");
                }

                state_ = V8RuntimeState::Initializing;
            }

            try
            {
                // ------------------------------------------------
                // Initialize Process-Wide Platform
                // ------------------------------------------------

                V8Platform::instance().initialize(
                    config_.platform);

                // ------------------------------------------------
                // Create Isolate
                // ------------------------------------------------

                auto isolate = std::make_unique<V8Isolate>(
                    config_.isolate);

                // ------------------------------------------------
                // Create Context
                // ------------------------------------------------

                auto context = std::make_unique<V8Context>(
                    *isolate,
                    config_.context);

                // ------------------------------------------------
                // Create Script Engine
                // ------------------------------------------------

                auto scriptEngine =
                    std::make_unique<V8ScriptEngine>(
                        *context);

                // ------------------------------------------------
                // Create Native Bindings
                // ------------------------------------------------

                auto bindings =
                    std::make_unique<V8NativeBinding>(
                        *context,
                        config_.bindings);

                // ------------------------------------------------
                // Create Module Loader
                // ------------------------------------------------

                auto moduleLoader =
                    std::make_unique<V8ModuleLoader>(
                        *context,
                        config_.modules);

                // ------------------------------------------------
                // Publish Initialized Components
                // ------------------------------------------------

                isolate_ = std::move(isolate);
                context_ = std::move(context);
                scriptEngine_ = std::move(scriptEngine);
                bindings_ = std::move(bindings);
                moduleLoader_ = std::move(moduleLoader);

                setState(V8RuntimeState::Running);
            }
            catch (...)
            {
                // Local component owners unwind in reverse
                // construction order. Published components,
                // if any, are also cleaned up.

                destroyComponents();

                setState(V8RuntimeState::Failed);
                throw;
            }
        }

        // ========================================================
        // Execute Classic JavaScript
        // ========================================================

        [[nodiscard]]
        V8ScriptResult execute(
            std::string_view source,
            const V8ScriptConfig &config = {})
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            return scriptEngine_->execute(
                source,
                config);
        }

        // ========================================================
        // Execute JavaScript Without Result
        // ========================================================

        void executeVoid(
            std::string_view source,
            const V8ScriptConfig &config = {})
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            scriptEngine_->executeVoid(
                source,
                config);
        }

        // ========================================================
        // Execute ECMAScript Entry Module
        // ========================================================

        void executeModule(
            const std::filesystem::path &entryPath)
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            moduleLoader_->executeEntry(entryPath);
        }

        // ========================================================
        // Register Native Function
        // ========================================================

        void registerNativeFunction(
            std::string name,
            V8NativeHandler handler)
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            bindings_->registerFunction(
                std::move(name),
                std::move(handler));
        }

        // ========================================================
        // Install Registered Native Functions
        // ========================================================

        void installNativeFunctions()
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            bindings_->installAll();
        }

        // ========================================================
        // Access Context Through Scoped Callback
        // ========================================================
        //
        // Allows advanced integrations to interact directly
        // with V8 while preserving the runtime operation lock.
        //
        // Do not retain local V8 handles after the callback.
        //
        // Do not call other V8Runtime methods from the callback,
        // because operationMutex_ is non-recursive.
        //
        // ========================================================

        template <typename Callback>
        decltype(auto) withContext(Callback &&callback)
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            requireRunning();

            return context_->withScope(
                std::forward<Callback>(callback));
        }

        // ========================================================
        // Runtime State
        // ========================================================

        [[nodiscard]]
        V8RuntimeState state() const noexcept
        {
            std::lock_guard<std::mutex> lock(
                stateMutex_);

            return state_;
        }

        [[nodiscard]]
        bool isRunning() const noexcept
        {
            return state() == V8RuntimeState::Running;
        }

        [[nodiscard]]
        const V8RuntimeConfig &config() const noexcept
        {
            return config_;
        }

        // ========================================================
        // Shutdown
        // ========================================================
        //
        // Destruction order:
        //
        // 1. Module loader
        // 2. Native bindings
        // 3. Script engine
        // 4. JavaScript context
        // 5. V8 isolate
        //
        // The process-wide V8Platform remains initialized.
        //
        // IMPORTANT:
        //
        // JavaScript must not retain callable references
        // to native binding payloads when those payloads
        // are destroyed.
        //
        // All execution must be quiescent before shutdown.
        //
        // ========================================================

        void shutdown() noexcept
        {
            std::lock_guard<std::mutex> lock(
                operationMutex_);

            const auto current = state();

            if (current == V8RuntimeState::Stopped ||
                current == V8RuntimeState::Stopping)
            {
                return;
            }

            setState(V8RuntimeState::Stopping);

            destroyComponents();

            setState(V8RuntimeState::Stopped);
        }

    private:
        // ========================================================
        // Validate Runtime State
        // ========================================================

        void requireRunning() const
        {
            if (state() != V8RuntimeState::Running ||
                !isolate_ ||
                !context_ ||
                !scriptEngine_ ||
                !bindings_ ||
                !moduleLoader_)
            {

                throw V8RuntimeError(
                    "V8 runtime is not initialized or running");
            }
        }

        // ========================================================
        // State Transition
        // ========================================================

        void setState(
            V8RuntimeState newState) noexcept
        {
            std::lock_guard<std::mutex> lock(
                stateMutex_);

            state_ = newState;
        }

        // ========================================================
        // Component Destruction
        // ========================================================

        void destroyComponents() noexcept
        {
            // Persistent module handles must be released
            // while the context and isolate are still alive.

            moduleLoader_.reset();

            // Binding callbacks must be quiescent.
            bindings_.reset();

            scriptEngine_.reset();

            context_.reset();

            isolate_.reset();
        }

        // ========================================================
        // Internal State
        // ========================================================

        V8RuntimeConfig config_{};

        mutable std::mutex stateMutex_;
        std::mutex operationMutex_;

        V8RuntimeState state_ = V8RuntimeState::Created;

        std::unique_ptr<V8Isolate> isolate_;
        std::unique_ptr<V8Context> context_;

        std::unique_ptr<V8ScriptEngine> scriptEngine_;
        std::unique_ptr<V8NativeBinding> bindings_;
        std::unique_ptr<V8ModuleLoader> moduleLoader_;
    };

} // namespace lamp::v8