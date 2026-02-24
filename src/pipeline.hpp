#pragma once

#include "config.hpp"
#include "encoder.hpp"
#include "stats.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <atomic>
#include <mutex>
#include <string>

/// Manages two GStreamer pipelines bridged via appsink→appsrc:
///
///   Pipeline 1 (Encoder - always running):
///     rtspsrc → rtph264depay → h264parse → nvv4l2decoder → nvvidconv
///     → nvv4l2h264enc (CBR) → h264parse → appsink
///
///   Pipeline 2 (RTSP Server - on demand):
///     GstRTSPServer factory: appsrc → h264parse → rtph264pay
///
/// This split is necessary because rtspsrc uses dynamic pads that
/// don't work reliably inside a GstRTSPServer factory launch string.

class Pipeline {
public:
    Pipeline(const AppConfig& config, Stats& stats);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /// Build and start both pipelines.
    bool start();

    /// Stop everything.
    void stop();

    /// Check if pipeline is running.
    bool is_running() const { return running_.load(); }

    /// Run watchdog check. Returns true if healthy.
    bool watchdog_check();

    /// Restart the encoder pipeline (keeps RTSP server running).
    bool restart_encoder();

    /// Update bitrate at runtime.
    void set_bitrate(uint32_t target_kbps, uint32_t max_kbps);

    /// Pull a sample from the encoder (used by RTSP server feeder).
    GstSample* pull_latest_sample();

    /// Get the cached caps string for appsrc.
    std::string get_caps_string() const;

    /// Check if we have valid caps from the encoder.
    bool has_caps() const { return has_caps_.load(); }

private:
    AppConfig config_;
    Stats& stats_;
    Encoder encoder_;

    // ---- Encoder Pipeline ----
    GstElement* enc_pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstBus* enc_bus_ = nullptr;

    // ---- RTSP Server ----
    GstRTSPServer* rtsp_server_ = nullptr;
    guint server_source_id_ = 0;

    // ---- State ----
    std::atomic<bool> running_{false};
    std::atomic<bool> has_caps_{false};
    std::mutex mutex_;
    std::string caps_string_;

    int reconnect_delay_s_ = 3;

    /// Build the encoder pipeline with individual elements.
    bool build_encoder_pipeline();

    /// Start the RTSP server with a custom factory.
    bool start_rtsp_server();

    /// Stop encoder pipeline.
    void stop_encoder();

    /// Stop RTSP server.
    void stop_rtsp_server();

    // ---- GStreamer Callbacks ----
    /// Called when rtspsrc creates a new dynamic pad — links to rtph264depay.
    static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer depay);

    /// Handle bus messages (error, EOS, state changes).
    static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer data);
};

// ============================================================================
// Custom RTSP Media Factory
// ============================================================================

#define TYPE_ENCODER_FACTORY (encoder_factory_get_type())
G_DECLARE_FINAL_TYPE(EncoderFactory, encoder_factory, ENCODER, FACTORY, GstRTSPMediaFactory)

struct _EncoderFactory {
    GstRTSPMediaFactory parent;
    Pipeline* pipeline;
};

GType encoder_factory_get_type(void);
GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline);
