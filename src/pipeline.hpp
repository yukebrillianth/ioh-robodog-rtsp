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

/// RTSP Re-encoder pipeline for Jetson Orin NX.
///
/// Encoder pipeline (always running):
///   rtspsrc → rtph264depay → h264parse → nvv4l2decoder → nvvidconv
///   → nvv4l2h264enc (CBR) → h264parse → appsink
///
/// RTSP Server (on-demand per client):
///   Custom factory: appsrc → h264parse → rtph264pay (name=pay0)
///   Feeder thread bridges appsink → appsrc

class Pipeline {
public:
    Pipeline(const AppConfig& config, Stats& stats);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    bool watchdog_check();
    bool restart_encoder();
    void set_bitrate(uint32_t target_kbps, uint32_t max_kbps);

    // Used by RTSP server feeder thread
    GstSample* pull_latest_sample();
    std::string get_caps_string() const;
    bool has_caps() const { return has_caps_.load(); }

private:
    AppConfig config_;
    Stats& stats_;
    Encoder encoder_;

    GstElement* enc_pipeline_ = nullptr;
    GstElement* appsink_ = nullptr;
    GstBus* enc_bus_ = nullptr;

    GstRTSPServer* rtsp_server_ = nullptr;
    guint server_source_id_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> has_caps_{false};
    std::mutex mutex_;
    std::string caps_string_;
    int reconnect_delay_s_ = 3;

    bool build_encoder_pipeline();
    bool start_rtsp_server();
    void stop_encoder();
    void stop_rtsp_server();

    static void on_pad_added(GstElement* src, GstPad* new_pad, gpointer depay);
    static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer data);
};

// Custom RTSP Media Factory
#define TYPE_ENCODER_FACTORY (encoder_factory_get_type())
G_DECLARE_FINAL_TYPE(EncoderFactory, encoder_factory, ENCODER, FACTORY, GstRTSPMediaFactory)

struct _EncoderFactory {
    GstRTSPMediaFactory parent;
    Pipeline* pipeline;
};

GType encoder_factory_get_type(void);
GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline);
