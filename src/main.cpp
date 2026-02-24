// =============================================================================
// RTSP Re-Encoder for WebRTC
// Jetson Orin NX — Indosat 5G AI-RAN Demo
//
// Ingests RTSP from robot dog camera, re-encodes with NVENC at lower bitrate,
// serves as local RTSP for go2rtc to consume and serve as WebRTC.
//
// Usage: ./rtsp_encoder [--config config.yaml]
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

static std::atomic<bool> g_running{true};
static GMainLoop* g_main_loop = nullptr;

static void signal_handler(int) {
    const char msg[] = "\n[MAIN] Shutting down...\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    g_running.store(false);
    if (g_main_loop && g_main_loop_is_running(g_main_loop)) {
        g_main_loop_quit(g_main_loop);
    }
}

static std::string parse_config_path(int argc, char* argv[]) {
    std::string path = "config.yaml";
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            path = argv[i + 1]; i++;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [-c config.yaml]" << std::endl;
            std::cout << "  Re-encodes RTSP at lower bitrate, serves local RTSP for go2rtc" << std::endl;
            exit(0);
        }
    }
    return path;
}

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "  RTSP Re-Encoder for WebRTC" << std::endl;
    std::cout << "  Jetson Orin NX | 5G AI-RAN Demo" << std::endl;
    std::cout << "========================================" << std::endl;

    std::string config_path = parse_config_path(argc, argv);

    AppConfig config;
    try {
        config = load_config(config_path);
        validate_config(config);
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] Config error: " << e.what() << std::endl;
        return 1;
    }
    print_config(config);

    gst_init(&argc, &argv);
    std::cout << "[MAIN] GStreamer: " << gst_version_string() << std::endl;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    g_main_loop = g_main_loop_new(NULL, FALSE);

    Stats stats;
    Pipeline pipeline(config, stats);

    if (!pipeline.start()) {
        std::cerr << "[MAIN] Failed to start" << std::endl;
        g_main_loop_unref(g_main_loop);
        return 1;
    }

    // Monitor thread: stats + watchdog
    std::thread monitor([&]() {
        auto last_stats = std::chrono::steady_clock::now();
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running.load()) break;

            if (config.stats.enabled) {
                auto now = std::chrono::steady_clock::now();
                auto dt = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count();
                if (dt >= config.stats.interval_s) {
                    stats.print();
                    last_stats = now;
                }
            }

            if (!pipeline.watchdog_check()) {
                std::cerr << "[WATCHDOG] Restarting encoder..." << std::endl;
                if (g_running.load() && !pipeline.restart_encoder()) {
                    std::cerr << "[MAIN] Restart failed, exiting" << std::endl;
                    g_running.store(false);
                    g_main_loop_quit(g_main_loop);
                }
            }
        }
    });

    std::cout << "[MAIN] Running (Ctrl+C to stop)..." << std::endl;
    g_main_loop_run(g_main_loop);

    g_running.store(false);
    if (monitor.joinable()) monitor.join();
    pipeline.stop();

    std::cout << std::endl << "[STATS] Final: ";
    stats.print();

    g_main_loop_unref(g_main_loop);
    std::cout << "[MAIN] Done." << std::endl;

    // Force exit — gst_deinit() blocks on NvMMLite
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        _exit(0);
    }).detach();

    return 0;
}
