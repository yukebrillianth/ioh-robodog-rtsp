#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

/// Real-time statistics tracking for the encoder pipeline.
/// Thread-safe — counters can be updated from GStreamer callback threads.

class Stats {
public:
    Stats() = default;

    /// Call when pipeline starts/restarts to reset frame counters.
    void reset();

    /// Call on each encoded frame.
    void on_frame_encoded();

    /// Increment reconnect counter.
    void on_reconnect();

    /// Increment pipeline restart counter.
    void on_pipeline_restart();

    /// Print current stats to stdout.
    void print() const;

    /// Get uptime string.
    std::string get_uptime_string() const;

    // Accessors
    uint64_t frame_count() const { return frame_count_.load(); }
    uint32_t reconnect_count() const { return reconnect_count_.load(); }
    uint32_t restart_count() const { return restart_count_.load(); }

    /// Get time since last frame was received (for watchdog).
    double seconds_since_last_frame() const;

private:
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint32_t> reconnect_count_{0};
    std::atomic<uint32_t> restart_count_{0};

    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint start_time_ = Clock::now();
    mutable std::atomic<int64_t> last_frame_time_ns_{0};

    // For FPS calculation
    mutable std::atomic<uint64_t> last_fps_frame_count_{0};
    mutable std::atomic<int64_t> last_fps_time_ns_{0};
};
