#pragma once

#include "config.hpp"
#include "encoder.hpp"
#include "stats.hpp"

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/// Manages the full GStreamer pipeline:
///   rtspsrc → decode → nvv4l2h264enc → GstRTSPServer
///
/// Features:
///   - Hardware-accelerated decode & encode (Jetson NVENC)
///   - Strict CBR bandwidth control for 5G reliability
///   - Watchdog: auto-restart if no frames for N seconds
///   - Auto-reconnect with exponential backoff on RTSP failure
///   - RTSP server output for go2rtc consumption

class Pipeline {
public:
    Pipeline(const AppConfig& config, Stats& stats);
    ~Pipeline();

    // Non-copyable
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /// Build and start the pipeline + RTSP server.
    bool start();

    /// Stop the pipeline and RTSP server.
    void stop();

    /// Check if pipeline is running.
    bool is_running() const { return running_.load(); }

    /// Run the watchdog check. Returns true if pipeline is healthy.
    bool watchdog_check();

    /// Restart the pipeline (used by watchdog or error recovery).
    bool restart();

    /// Update bitrate at runtime.
    void set_bitrate(uint32_t target_kbps, uint32_t max_kbps);

private:
    AppConfig config_;
    Stats& stats_;
    Encoder encoder_;

    GstRTSPServer* rtsp_server_ = nullptr;
    GstRTSPMountPoints* mounts_ = nullptr;
    GstRTSPMediaFactory* factory_ = nullptr;
    guint server_source_id_ = 0;

    std::atomic<bool> running_{false};
    std::mutex pipeline_mutex_;

    int reconnect_delay_s_ = 3;  // Current reconnect delay (exponential backoff)

    /// Build the GStreamer launch string for the RTSP factory.
    std::string build_launch_string() const;

    /// Callback: called when media is constructed by the RTSP factory.
    static void on_media_configure(GstRTSPMediaFactory* factory,
                                    GstRTSPMedia* media,
                                    gpointer user_data);

    /// Callback: handle pad-added for identity element (frame counting).
    static GstPadProbeReturn on_buffer_probe(GstPad* pad,
                                              GstPadProbeInfo* info,
                                              gpointer user_data);

    /// Start the GstRTSPServer.
    bool start_rtsp_server();

    /// Stop the GstRTSPServer.
    void stop_rtsp_server();
};
