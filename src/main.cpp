// =============================================================================
// RTSP Re-Encoder for WebRTC via go2rtc
// Jetson Orin NX — Indosat 5G AI-RAN Demo
//
// Mode 1 (default): Output H.264 to stdout for go2rtc exec: mode (lowest latency)
// Mode 2 (--rtsp):  Output via local RTSP server for go2rtc rtsp: mode
//
// Usage:
//   go2rtc exec mode (recommended):
//     go2rtc.yaml: exec:./rtsp_encoder -c config.yaml#video=h264
//
//   Standalone RTSP server mode:
//     ./rtsp_encoder --rtsp -c config.yaml
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
// Globals
// ============================================================================

static std::atomic<bool> g_running{true};
static GMainLoop* g_main_loop = nullptr;

static void signal_handler(int /*signum*/) {
    g_running.store(false);
    if (g_main_loop && g_main_loop_is_running(g_main_loop)) {
        g_main_loop_quit(g_main_loop);
    }
}

// ============================================================================
// CLI Parsing
// ============================================================================

struct CliArgs {
    std::string config_path = "config.yaml";
    bool rtsp_mode = false;  // false = stdout mode (default, low latency)
};

static CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
            args.config_path = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--rtsp") == 0) {
            args.rtsp_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cerr << "RTSP Re-Encoder for Jetson Orin NX" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Usage: " << argv[0] << " [OPTIONS]" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Modes:" << std::endl;
            std::cerr << "  (default)    Output H.264 to stdout — use with go2rtc exec:" << std::endl;
            std::cerr << "  --rtsp       Output via local RTSP server" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Options:" << std::endl;
            std::cerr << "  -c, --config <path>  Config file (default: config.yaml)" << std::endl;
            std::cerr << "  -h, --help           Show this help" << std::endl;
            exit(0);
        }
    }
    return args;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    CliArgs args = parse_args(argc, argv);

    // Print banner to stderr (stdout is for video data in exec mode)
    std::cerr << "========================================" << std::endl;
    std::cerr << "  RTSP Re-Encoder for WebRTC" << std::endl;
    std::cerr << "  Jetson Orin NX | 5G AI-RAN Demo" << std::endl;
    std::cerr << "  Mode: " << (args.rtsp_mode ? "RTSP Server" : "Stdout (go2rtc exec)") << std::endl;
    std::cerr << "========================================" << std::endl;

    // Load config
    AppConfig config;
    try {
        config = load_config(args.config_path);
        validate_config(config);
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] Configuration error: " << e.what() << std::endl;
        return 1;
    }
    // Print config to stderr
    print_config_stderr(config);

    // Initialize GStreamer
    gst_init(&argc, &argv);
    std::cerr << "[MAIN] GStreamer initialized: " << gst_version_string() << std::endl;

    // Signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Main loop
    g_main_loop = g_main_loop_new(NULL, FALSE);

    // Create pipeline
    Stats stats;
    Pipeline pipeline(config, stats);

    bool ok;
    if (args.rtsp_mode) {
        ok = pipeline.start();
    } else {
        ok = pipeline.start_stdout_mode();
    }

    if (!ok) {
        std::cerr << "[MAIN] Failed to start pipeline" << std::endl;
        g_main_loop_unref(g_main_loop);
        return 1;
    }

    // Stats & watchdog thread (prints to stderr)
    std::thread monitor([&]() {
        auto last_stats = std::chrono::steady_clock::now();
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running.load()) break;

            // Stats
            if (config.stats.enabled) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count();
                if (elapsed >= config.stats.interval_s) {
                    stats.print();  // prints to stderr
                    last_stats = now;
                }
            }

            // Watchdog
            if (!pipeline.watchdog_check()) {
                std::cerr << "[WATCHDOG] Restarting encoder..." << std::endl;
                if (g_running.load()) {
                    if (args.rtsp_mode) {
                        pipeline.restart_encoder();
                    } else {
                        pipeline.restart_stdout();
                    }
                }
            }
        }
    });

    std::cerr << "[MAIN] Running (Ctrl+C to stop)..." << std::endl;
    g_main_loop_run(g_main_loop);

    // Cleanup
    std::cerr << "[MAIN] Shutting down..." << std::endl;
    g_running.store(false);
    if (monitor.joinable()) monitor.join();
    pipeline.stop();

    std::cerr << std::endl << "[MAIN] === Final Statistics ===" << std::endl;
    stats.print();

    g_main_loop_unref(g_main_loop);
    std::cerr << "[MAIN] Done." << std::endl;

    // Force exit after 2s (NvMMLite cleanup blocks)
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        _exit(0);
    }).detach();

    return 0;
}
