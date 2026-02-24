// =============================================================================
// RTSP Re-Encoder for WebRTC via go2rtc
// Jetson Orin NX — Indosat 5G AI-RAN Demo
//
// Ingests RTSP from robot dog camera, re-encodes with NVIDIA NVENC (CBR),
// and serves as local RTSP for go2rtc to consume and serve as WebRTC.
//
// Usage: ./rtsp_encoder [--config path/to/config.yaml]
// =============================================================================

#include "config.hpp"
#include "pipeline.hpp"
#include "stats.hpp"

#include <gst/gst.h>
#include <iostream>
#include <csignal>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <unistd.h>

// ============================================================================
// Globals for signal handling
// ============================================================================

static std::atomic<bool> g_running{true};
static GMainLoop* g_main_loop = nullptr;

static void signal_handler(int signum) {
    const char* sig_name = (signum == SIGINT) ? "SIGINT" : 
                           (signum == SIGTERM) ? "SIGTERM" : "UNKNOWN";
    // Using write() for async-signal-safety
    const char msg[] = "\n[MAIN] Signal received, shutting down gracefully...\n";
    (void)!write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)sig_name;

    g_running.store(false);
    if (g_main_loop && g_main_loop_is_running(g_main_loop)) {
        g_main_loop_quit(g_main_loop);
    }
}

// ============================================================================
// CLI Argument Parsing
// ============================================================================

static std::string parse_config_path(int argc, char* argv[]) {
    std::string config_path = "config.yaml";

    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--config") == 0 || 
             std::strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            config_path = argv[i + 1];
            i++;
        } else if (std::strcmp(argv[i], "--help") == 0 || 
                   std::strcmp(argv[i], "-h") == 0) {
            std::cout << "RTSP Re-Encoder for Jetson Orin NX" << std::endl;
            std::cout << std::endl;
            std::cout << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cout << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c, --config <path>  Config file path (default: config.yaml)" << std::endl;
            std::cout << "  -h, --help           Show this help" << std::endl;
            std::cout << std::endl;
            std::cout << "Architecture:" << std::endl;
            std::cout << "  Robot Dog (RTSP) → NVENC Re-encode (CBR) → Local RTSP → go2rtc → WebRTC" << std::endl;
            exit(0);
        }
    }

    return config_path;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    // ---- Banner ----
    std::cout << "========================================" << std::endl;
    std::cout << "  RTSP Re-Encoder for WebRTC" << std::endl;
    std::cout << "  Jetson Orin NX | 5G AI-RAN Demo" << std::endl;
    std::cout << "  Indosat — Surabaya → Barcelona" << std::endl;
    std::cout << "========================================" << std::endl;

    // ---- Parse CLI args ----
    std::string config_path = parse_config_path(argc, argv);

    // ---- Load & validate config ----
    AppConfig config;
    try {
        config = load_config(config_path);
        validate_config(config);
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] Configuration error: " << e.what() << std::endl;
        return 1;
    }
    print_config(config);

    // ---- Initialize GStreamer ----
    gst_init(&argc, &argv);
    std::cout << "[MAIN] GStreamer initialized: " << gst_version_string() << std::endl;

    // ---- Setup signal handlers ----
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ---- Create main loop ----
    g_main_loop = g_main_loop_new(NULL, FALSE);

    // ---- Create pipeline ----
    Stats stats;
    Pipeline pipeline(config, stats);

    if (!pipeline.start()) {
        std::cerr << "[MAIN] Failed to start pipeline" << std::endl;
        g_main_loop_unref(g_main_loop);
        gst_deinit();
        return 1;
    }

    // ---- Stats & Watchdog Timer ----
    // Run stats printing and watchdog in a separate thread so the
    // GMainLoop can run uninterrupted for RTSP serving
    std::thread monitor_thread([&]() {
        auto last_stats_time = std::chrono::steady_clock::now();

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!g_running.load()) break;

            // ---- Stats printing ----
            if (config.stats.enabled) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_stats_time).count();

                if (elapsed >= config.stats.interval_s) {
                    stats.print();
                    last_stats_time = now;
                }
            }

            // ---- Watchdog ----
            // Only restart the encoder pipeline, keep RTSP server running
            // so go2rtc/clients don't need to reconnect
            if (!pipeline.watchdog_check()) {
                std::cerr << "[MAIN] Watchdog triggered — restarting encoder" << std::endl;

                if (g_running.load()) {
                    if (!pipeline.restart_encoder()) {
                        std::cerr << "[MAIN] Failed to restart encoder" << std::endl;
                        g_running.store(false);
                        g_main_loop_quit(g_main_loop);
                    } else {
                        std::cout << "[MAIN] Encoder restarted successfully" << std::endl;
                    }
                }
            }
        }
    });

    // ---- Run GMainLoop (blocks until quit) ----
    std::cout << "[MAIN] Running main loop (Ctrl+C to stop)..." << std::endl;
    g_main_loop_run(g_main_loop);

    // ---- Cleanup ----
    std::cout << "[MAIN] Shutting down..." << std::endl;
    g_running.store(false);

    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }

    pipeline.stop();

    // Print final stats
    std::cout << std::endl;
    std::cout << "[MAIN] === Final Statistics ===" << std::endl;
    stats.print();

    g_main_loop_unref(g_main_loop);
    g_main_loop = nullptr;

    // NOTE: Skip gst_deinit() — it blocks indefinitely on Jetson
    // due to NvMMLite cleanup. Force exit after brief delay.
    std::cout << "[MAIN] Clean shutdown complete." << std::endl;

    // Force exit in case any detached threads are still blocking
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cerr << "[MAIN] Force exit (cleanup timeout)" << std::endl;
        _exit(0);
    }).detach();

    return 0;
}
