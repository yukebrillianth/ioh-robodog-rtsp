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

/// GStreamer pipeline for RTSP re-encoding with NVIDIA NVENC.
///
/// Two output modes:
///
///   STDOUT MODE (default, lowest latency):
///     rtspsrc → depay → parse → nvv4l2decoder → nvvidconv
///     → nvv4l2h264enc → h264parse → fdsink(stdout)
///     Used with go2rtc exec: for zero intermediate buffering.
///
///   RTSP SERVER MODE (--rtsp flag):
///     Same encoder pipeline → appsink → feeder → appsrc
///     → GstRTSPServer → clients

class Pipeline {
public:
    Pipeline(const AppConfig& config, Stats& stats);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // ---- Stdout mode (lowest latency, for go2rtc exec:) ----
    bool start_stdout_mode();
    bool restart_stdout();

    // ---- RTSP server mode ----
    bool start();
    bool restart_encoder();

    void stop();
    bool is_running() const { return running_.load(); }
    bool watchdog_check();
    void set_bitrate(uint32_t target_kbps, uint32_t max_kbps);

    // For RTSP server feeder
    GstSample* pull_latest_sample();
    std::string get_caps_string() const;
    bool has_caps() const { return has_caps_.load(); }

private:
    AppConfig config_;
    Stats& stats_;
    Encoder encoder_;

    GstElement* enc_pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;  // Only used in RTSP mode
    GstBus* enc_bus_ = nullptr;

    // RTSP server (only used in RTSP mode)
    GstRTSPServer* rtsp_server_ = nullptr;
    guint server_source_id_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> has_caps_{false};
    bool stdout_mode_ = false;
    std::mutex mutex_;
    std::string caps_string_;
    int reconnect_delay_s_ = 3;

    // Build pipeline for stdout output (fdsink)
    bool build_stdout_pipeline();

    // Build pipeline for RTSP server output (appsink)
    bool build_encoder_pipeline();

    // Shared element creation
    bool build_pipeline_common(GstElement*& pipeline, GstElement* sink_element);

    bool start_rtsp_server();
    void stop_encoder();
    void stop_rtsp_server();

    static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer depay);
    static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer data);
};

// Custom RTSP Media Factory (only used in RTSP mode)
#define TYPE_ENCODER_FACTORY (encoder_factory_get_type())
G_DECLARE_FINAL_TYPE(EncoderFactory, encoder_factory, ENCODER, FACTORY, GstRTSPMediaFactory)

struct _EncoderFactory {
    GstRTSPMediaFactory parent;
    Pipeline* pipeline;
};

GType encoder_factory_get_type(void);
GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline);
