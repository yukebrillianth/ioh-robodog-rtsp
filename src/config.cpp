#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <stdexcept>
#include <fstream>

AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    // Check file exists
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "[CONFIG] Warning: Config file '" << path 
                  << "' not found, using defaults." << std::endl;
        return cfg;
    }
    f.close();

    try {
        YAML::Node root = YAML::LoadFile(path);

        // RTSP section
        if (root["rtsp"]) {
            auto n = root["rtsp"];
            if (n["url"])                    cfg.rtsp.url = n["url"].as<std::string>();
            if (n["transport"])              cfg.rtsp.transport = n["transport"].as<std::string>();
            if (n["latency_ms"])             cfg.rtsp.latency_ms = n["latency_ms"].as<int>();
            if (n["reconnect_delay_s"])      cfg.rtsp.reconnect_delay_s = n["reconnect_delay_s"].as<int>();
            if (n["max_reconnect_attempts"]) cfg.rtsp.max_reconnect_attempts = n["max_reconnect_attempts"].as<int>();
        }

        // Encoder section
        if (root["encoder"]) {
            auto n = root["encoder"];
            if (n["width"])              cfg.encoder.width = n["width"].as<int>();
            if (n["height"])             cfg.encoder.height = n["height"].as<int>();
            if (n["framerate"])          cfg.encoder.framerate = n["framerate"].as<int>();
            if (n["max_bitrate_kbps"])   cfg.encoder.max_bitrate_kbps = n["max_bitrate_kbps"].as<uint32_t>();
            if (n["target_bitrate_kbps"]) cfg.encoder.target_bitrate_kbps = n["target_bitrate_kbps"].as<uint32_t>();
            if (n["idr_interval"])       cfg.encoder.idr_interval = n["idr_interval"].as<int>();
            if (n["preset"])             cfg.encoder.preset = n["preset"].as<std::string>();
            if (n["profile"])            cfg.encoder.profile = n["profile"].as<std::string>();
            if (n["control_rate"])       cfg.encoder.control_rate = n["control_rate"].as<std::string>();
        }

        // Output section
        if (root["output"]) {
            auto n = root["output"];
            if (n["port"]) cfg.output.port = n["port"].as<int>();
            if (n["path"]) cfg.output.path = n["path"].as<std::string>();
        }

        // Stats section
        if (root["stats"]) {
            auto n = root["stats"];
            if (n["enabled"])    cfg.stats.enabled = n["enabled"].as<bool>();
            if (n["interval_s"]) cfg.stats.interval_s = n["interval_s"].as<int>();
        }

        // Resilience section
        if (root["resilience"]) {
            auto n = root["resilience"];
            if (n["watchdog_timeout_s"])    cfg.resilience.watchdog_timeout_s = n["watchdog_timeout_s"].as<int>();
            if (n["max_pipeline_restarts"]) cfg.resilience.max_pipeline_restarts = n["max_pipeline_restarts"].as<int>();
        }

    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("[CONFIG] YAML parse error: ") + e.what());
    }

    return cfg;
}

void validate_config(const AppConfig& cfg) {
    if (cfg.rtsp.url.empty()) {
        throw std::runtime_error("[CONFIG] RTSP URL cannot be empty");
    }
    if (cfg.rtsp.transport != "tcp" && cfg.rtsp.transport != "udp") {
        throw std::runtime_error("[CONFIG] RTSP transport must be 'tcp' or 'udp'");
    }
    if (cfg.encoder.width < 0 || cfg.encoder.height < 0) {
        throw std::runtime_error("[CONFIG] Encoder width/height cannot be negative");
    }
    if (cfg.encoder.framerate < 1 || cfg.encoder.framerate > 120) {
        throw std::runtime_error("[CONFIG] Framerate must be between 1 and 120");
    }
    if (cfg.encoder.max_bitrate_kbps < 100 || cfg.encoder.max_bitrate_kbps > 50000) {
        throw std::runtime_error("[CONFIG] Max bitrate must be between 100 and 50000 kbps");
    }
    if (cfg.encoder.target_bitrate_kbps > cfg.encoder.max_bitrate_kbps) {
        throw std::runtime_error("[CONFIG] Target bitrate cannot exceed max bitrate");
    }
    if (cfg.encoder.idr_interval < 1) {
        throw std::runtime_error("[CONFIG] IDR interval must be >= 1");
    }
    if (cfg.output.port < 1 || cfg.output.port > 65535) {
        throw std::runtime_error("[CONFIG] Output port must be 1-65535");
    }
}

void print_config(const AppConfig& cfg) {
    std::cout << "========================================" << std::endl;
    std::cout << "  RTSP Re-Encoder Configuration" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  RTSP Source:  " << cfg.rtsp.url << std::endl;
    std::cout << "  Transport:    " << cfg.rtsp.transport << std::endl;
    std::cout << "  Latency:      " << cfg.rtsp.latency_ms << " ms" << std::endl;
    std::cout << "  Resolution:   " << cfg.encoder.width << "x" << cfg.encoder.height << std::endl;
    std::cout << "  Framerate:    " << cfg.encoder.framerate << " fps" << std::endl;
    std::cout << "  Bitrate:      " << cfg.encoder.target_bitrate_kbps << " / "
              << cfg.encoder.max_bitrate_kbps << " kbps (target/max)" << std::endl;
    std::cout << "  Rate Control: " << cfg.encoder.control_rate << std::endl;
    std::cout << "  Preset:       " << cfg.encoder.preset << std::endl;
    std::cout << "  Profile:      " << cfg.encoder.profile << std::endl;
    std::cout << "  IDR Interval: " << cfg.encoder.idr_interval << " frames" << std::endl;
    std::cout << "  RTSP Output:  rtsp://localhost:" << cfg.output.port 
              << cfg.output.path << std::endl;
    std::cout << "  Watchdog:     " << cfg.resilience.watchdog_timeout_s << "s" << std::endl;
    std::cout << "========================================" << std::endl;
}

void print_config_stderr(const AppConfig& cfg) {
    std::cerr << "  Source:     " << cfg.rtsp.url << std::endl;
    std::cerr << "  Transport:  " << cfg.rtsp.transport << std::endl;
    std::cerr << "  Latency:    " << cfg.rtsp.latency_ms << " ms" << std::endl;
    std::cerr << "  Resolution: " << cfg.encoder.width << "x" << cfg.encoder.height << std::endl;
    std::cerr << "  Bitrate:    " << cfg.encoder.target_bitrate_kbps << " / "
              << cfg.encoder.max_bitrate_kbps << " kbps" << std::endl;
    std::cerr << "  IDR:        " << cfg.encoder.idr_interval << " frames" << std::endl;
    std::cerr << "  Preset:     " << cfg.encoder.preset << std::endl;
    std::cerr << "  Profile:    " << cfg.encoder.profile << std::endl;
}
