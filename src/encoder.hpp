#pragma once

#include <gst/gst.h>
#include <string>
#include <cstdint>

/// Manages the NVENC hardware encoder element configuration.
/// Provides runtime bitrate adjustment without pipeline restart.

class Encoder {
public:
    Encoder() = default;
    ~Encoder() = default;

    /// Configure the nvv4l2h264enc element with the given parameters.
    /// Must be called before the pipeline transitions to PLAYING.
    void configure(GstElement* encoder_element,
                   uint32_t target_bitrate_kbps,
                   uint32_t max_bitrate_kbps,
                   int idr_interval,
                   const std::string& preset,
                   const std::string& profile,
                   const std::string& control_rate);

    /// Change bitrate at runtime (no pipeline restart needed).
    void set_bitrate(uint32_t target_kbps, uint32_t max_kbps);

    /// Get current configured bitrate.
    uint32_t get_target_bitrate_kbps() const { return target_bitrate_kbps_; }
    uint32_t get_max_bitrate_kbps() const { return max_bitrate_kbps_; }

private:
    GstElement* encoder_ = nullptr;
    uint32_t target_bitrate_kbps_ = 0;
    uint32_t max_bitrate_kbps_ = 0;

    /// Map preset string to nvv4l2h264enc preset enum value.
    int preset_to_enum(const std::string& preset);
    /// Map profile string to enum value.
    int profile_to_enum(const std::string& profile);
    /// Map control rate string to enum value.
    int control_rate_to_enum(const std::string& rate);
};
