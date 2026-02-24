#pragma once

#include <string>
#include <cstdint>

/// Configuration structures for the RTSP re-encoder

struct RtspConfig {
    std::string url = "rtsp://192.168.1.120:554/test";
    std::string transport = "tcp";
    int latency_ms = 200;
    int reconnect_delay_s = 3;
    int max_reconnect_attempts = 0;  // 0 = unlimited
};

struct EncoderConfig {
    int width = 1280;
    int height = 720;
    int framerate = 30;
    uint32_t max_bitrate_kbps = 2000;
    uint32_t target_bitrate_kbps = 1800;
    int idr_interval = 30;
    std::string preset = "UltraLowLatency";
    std::string profile = "high";
    std::string control_rate = "cbr";
};

struct OutputConfig {
    int port = 8554;
    std::string path = "/stream";
};

struct StatsConfig {
    bool enabled = true;
    int interval_s = 5;
};

struct ResilienceConfig {
    int watchdog_timeout_s = 10;
    int max_pipeline_restarts = 0;  // 0 = unlimited
};

struct AppConfig {
    RtspConfig rtsp;
    EncoderConfig encoder;
    OutputConfig output;
    StatsConfig stats;
    ResilienceConfig resilience;
};

/// Load configuration from YAML file.
/// Falls back to defaults for any missing fields.
AppConfig load_config(const std::string& path);

/// Validate configuration, throw on invalid values.
void validate_config(const AppConfig& cfg);

/// Print configuration summary to stdout.
void print_config(const AppConfig& cfg);
