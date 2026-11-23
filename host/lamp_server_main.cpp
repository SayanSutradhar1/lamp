// ============================================================
// Lamp Framework - Standalone Native Runtime Server (`lamp_server`)
//
// Usage:
//   lamp_server --entry <app.js> [--host 127.0.0.1] [--port 3000]
//               [--min-threads 2] [--max-threads 16]
//               [--max-cache-ttl-ms 300000] [--max-cache-entries 4096]
// ============================================================

#include "host/lamp_app_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

void OnSignal(int) noexcept {
    g_stop_requested.store(true, std::memory_order_release);
}

void PrintUsage() {
    std::cout
        << "Usage: lamp_server --entry <app.js> [--host 127.0.0.1] [--port 3000]\n"
        << "                   [--min-threads 2] [--max-threads 16]\n"
        << "                   [--max-cache-ttl-ms 300000] [--max-cache-entries 4096]\n";
}

bool ReadFileText(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string entry_path;
    lamp::host::LampAppServerConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--entry" && i + 1 < argc) {
            entry_path = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            try {
                const int p = std::stoi(argv[++i]);
                if (p < 0 || p > 65535) {
                    std::cerr << "Invalid --port value\n";
                    return 2;
                }
                config.port = static_cast<std::uint16_t>(p);
            } catch (...) {
                std::cerr << "Invalid --port value\n";
                return 2;
            }
        } else if (arg == "--min-threads" && i + 1 < argc) {
            config.worker_pool.min_threads = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-threads" && i + 1 < argc) {
            config.worker_pool.max_threads = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-cache-ttl-ms" && i + 1 < argc) {
            config.wire_cache.max_ttl_limit =
                std::chrono::milliseconds(std::stoll(argv[++i]));
        } else if (arg == "--max-cache-entries" && i + 1 < argc) {
            config.wire_cache.max_entries = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (entry_path.empty()) {
        std::cerr << "Missing required --entry <app.js>\n";
        PrintUsage();
        return 2;
    }

    if (!ReadFileText(entry_path, config.entry_source)) {
        std::cerr << "Failed to open entry script: " << entry_path << "\n";
        return 1;
    }
    config.entry_name = entry_path;

    std::signal(SIGINT, &OnSignal);
    std::signal(SIGTERM, &OnSignal);

    try {
        lamp::host::LampAppServer server(std::move(config));
        server.Start();

        std::cout << "Lamp binary runtime listening on lmp://"
                  << server.Host() << ":" << server.Port()
                  << " (ws://" << server.Host() << ":" << server.Port()
                  << " for browser LMP1/LCB1)\n"
                  << std::flush;

        while (!g_stop_requested.load(std::memory_order_acquire) &&
               server.IsRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        server.Shutdown();
        std::cout << "Lamp runtime stopped cleanly\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Fatal startup error: " << ex.what() << "\n";
        return 1;
    }
}
