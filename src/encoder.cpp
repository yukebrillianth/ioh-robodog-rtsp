#include "encoder.hpp"
#include <iostream>

// nvv4l2h264enc preset values (Jetson specific)
// These correspond to the GStreamer plugin's preset property:
//   1 = DisablePreset
//   2 = UltraFastPreset (UltraLowLatency)
//   3 = FastPreset (LowLatency)
//   4 = MediumPreset (HP - high performance)
//   5 = SlowPreset (HQ - high quality)
enum NvV4L2Preset {
    PRESET_DISABLE          = 1,
    PRESET_ULTRA_FAST       = 2,  // UltraLowLatency
    PRESET_FAST             = 3,  // LowLatency
    PRESET_MEDIUM           = 4,  // HP
    PRESET_SLOW             = 5,  // HQ
};

// nvv4l2h264enc profile values
enum NvV4L2Profile {
    PROFILE_BASELINE = 0,
    PROFILE_MAIN     = 2,
    PROFILE_HIGH     = 4,
};

// nvv4l2h264enc control-rate values
enum NvV4L2ControlRate {
    RATE_DISABLE = 0,
    RATE_CBR     = 1,
    RATE_VBR     = 2,
};

int Encoder::preset_to_enum(const std::string& preset) {
    if (preset == "UltraLowLatency" || preset == "ultrafast") return PRESET_ULTRA_FAST;
    if (preset == "LowLatency" || preset == "fast")           return PRESET_FAST;
    if (preset == "HP" || preset == "medium")                  return PRESET_MEDIUM;
    if (preset == "HQ" || preset == "slow")                    return PRESET_SLOW;
    std::cerr << "[ENCODER] Unknown preset '" << preset 
              << "', defaulting to UltraFast" << std::endl;
    return PRESET_ULTRA_FAST;
}

int Encoder::profile_to_enum(const std::string& profile) {
    if (profile == "baseline") return PROFILE_BASELINE;
    if (profile == "main")     return PROFILE_MAIN;
    if (profile == "high")     return PROFILE_HIGH;
    std::cerr << "[ENCODER] Unknown profile '" << profile 
              << "', defaulting to High" << std::endl;
    return PROFILE_HIGH;
}

int Encoder::control_rate_to_enum(const std::string& rate) {
    if (rate == "cbr") return RATE_CBR;
    if (rate == "vbr") return RATE_VBR;
    std::cerr << "[ENCODER] Unknown control rate '" << rate 
              << "', defaulting to CBR" << std::endl;
    return RATE_CBR;
}

void Encoder::configure(GstElement* encoder_element,
                        uint32_t target_bitrate_kbps,
                        uint32_t max_bitrate_kbps,
                        int idr_interval,
                        const std::string& preset,
                        const std::string& profile,
                        const std::string& control_rate) {
    encoder_ = encoder_element;
    target_bitrate_kbps_ = target_bitrate_kbps;
    max_bitrate_kbps_ = max_bitrate_kbps;

    if (!encoder_) {
        std::cerr << "[ENCODER] Error: encoder element is null!" << std::endl;
        return;
    }

    // Bitrate properties are in bits/sec for nvv4l2h264enc
    g_object_set(G_OBJECT(encoder_),
        "bitrate",        target_bitrate_kbps * 1000u,
        "peak-bitrate",   max_bitrate_kbps * 1000u,
        "control-rate",   control_rate_to_enum(control_rate),
        "preset-level",   preset_to_enum(preset),
        "profile",        profile_to_enum(profile),
        "idrinterval",    idr_interval,
        "insert-sps-pps", TRUE,
        "maxperf-enable", TRUE,    // Maximize encoder clock for lowest latency
        NULL);

    // Enable VBV (Video Buffering Verifier) for strict bitrate adherence
    // Use a small VBV buffer to prevent bitrate spikes — critical for 5G
    // vbv-size is in bits, set to ~1 frame worth at target bitrate
    uint32_t vbv_size = (target_bitrate_kbps * 1000u) / 30;  // ~1 frame
    g_object_set(G_OBJECT(encoder_),
        "vbv-size",      vbv_size,
        NULL);

    std::cout << "[ENCODER] Configured: " << target_bitrate_kbps << " kbps target, "
              << max_bitrate_kbps << " kbps max, "
              << control_rate << " mode, " << preset << " preset, "
              << profile << " profile, IDR every " << idr_interval << " frames"
              << std::endl;
}

void Encoder::set_bitrate(uint32_t target_kbps, uint32_t max_kbps) {
    if (!encoder_) {
        std::cerr << "[ENCODER] Cannot set bitrate: encoder not initialized" << std::endl;
        return;
    }

    target_bitrate_kbps_ = target_kbps;
    max_bitrate_kbps_ = max_kbps;

    // Can be changed at runtime without pipeline restart
    g_object_set(G_OBJECT(encoder_),
        "bitrate",      target_kbps * 1000u,
        "peak-bitrate", max_kbps * 1000u,
        NULL);

    std::cout << "[ENCODER] Bitrate updated: " << target_kbps << " / " 
              << max_kbps << " kbps" << std::endl;
}
