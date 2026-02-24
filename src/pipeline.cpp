#include "pipeline.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

// ============================================================================
//  Custom RTSP Media Factory
// ============================================================================

G_DEFINE_TYPE(EncoderFactory, encoder_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl* url);

static void encoder_factory_class_init(EncoderFactoryClass* klass) {
    GstRTSPMediaFactoryClass* fc = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
    fc->create_element = encoder_factory_create_element;
}

static void encoder_factory_init(EncoderFactory*) {}

GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline) {
    EncoderFactory* f = (EncoderFactory*)g_object_new(TYPE_ENCODER_FACTORY, NULL);
    f->pipeline = pipeline;
    gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(f), TRUE);
    return GST_RTSP_MEDIA_FACTORY(f);
}

/// Called when go2rtc/client connects: appsrc → h264parse → rtph264pay(pay0)
static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl*) {
    EncoderFactory* self = ENCODER_FACTORY(factory);
    Pipeline* pipeline = self->pipeline;

    std::cout << "[SERVER] Client connected" << std::endl;

    GstElement* bin      = gst_bin_new("serve-bin");
    GstElement* appsrc   = gst_element_factory_make("appsrc",      "appsrc0");
    GstElement* parse    = gst_element_factory_make("h264parse",   "parse0");
    GstElement* pay      = gst_element_factory_make("rtph264pay",  "pay0");

    if (!appsrc || !parse || !pay) {
        std::cerr << "[SERVER] Failed to create elements" << std::endl;
        gst_object_unref(bin);
        return nullptr;
    }

    // Configure appsrc with H.264 caps
    GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(appsrc),
        "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE,
        "block", FALSE, "max-bytes", (guint64)(2 * 1024 * 1024),
        "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(G_OBJECT(parse), "config-interval", -1, NULL);
    g_object_set(G_OBJECT(pay),   "config-interval", -1, "pt", 96, NULL);

    gst_bin_add_many(GST_BIN(bin), appsrc, parse, pay, NULL);
    if (!gst_element_link_many(appsrc, parse, pay, NULL)) {
        std::cerr << "[SERVER] Link failed" << std::endl;
        gst_object_unref(bin);
        return nullptr;
    }

    // GstRTSPServer finds "pay0" automatically — no manual ghost pad

    // Feeder thread: pull H.264 from encoder appsink → push to this appsrc
    gst_object_ref(appsrc);
    std::thread([pipeline, appsrc]() {
        std::cout << "[SERVER] Feeder started" << std::endl;
        while (pipeline->is_running()) {
            GstSample* sample = pipeline->pull_latest_sample();
            if (sample) {
                GstBuffer* buf = gst_sample_get_buffer(sample);
                if (buf) {
                    GstBuffer* copy = gst_buffer_copy(buf);
                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), copy);
                    if (ret != GST_FLOW_OK) { gst_sample_unref(sample); break; }
                }
                gst_sample_unref(sample);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        std::cout << "[SERVER] Feeder stopped" << std::endl;
        gst_object_unref(appsrc);
    }).detach();

    return bin;
}

// ============================================================================
//  Pipeline
// ============================================================================

Pipeline::Pipeline(const AppConfig& config, Stats& stats)
    : config_(config), stats_(stats) {
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
}

Pipeline::~Pipeline() { stop(); }

// Frame count probe
static GstPadProbeReturn frame_probe(GstPad*, GstPadProbeInfo*, gpointer data) {
    static_cast<Stats*>(data)->on_frame_encoded();
    return GST_PAD_PROBE_OK;
}

bool Pipeline::build_encoder_pipeline() {
    std::lock_guard<std::mutex> lock(mutex_);

    enc_pipeline_ = gst_pipeline_new("encoder");
    if (!enc_pipeline_) return false;

    // Create all elements
    GstElement* src      = gst_element_factory_make("rtspsrc",       "src");
    GstElement* depay    = gst_element_factory_make("rtph264depay",  "depay");
    GstElement* parse_in = gst_element_factory_make("h264parse",     "parse_in");
    GstElement* decoder  = gst_element_factory_make("nvv4l2decoder", "decoder");
    GstElement* conv     = gst_element_factory_make("nvvidconv",     "conv");
    GstElement* enc      = gst_element_factory_make("nvv4l2h264enc", "enc");
    GstElement* parse_out= gst_element_factory_make("h264parse",     "parse_out");
    GstElement* sink     = gst_element_factory_make("appsink",       "enc_sink");

    if (!src || !depay || !parse_in || !decoder || !conv || !enc || !parse_out || !sink) {
        std::cerr << "[ENC] Missing GStreamer plugins!" << std::endl;
        if (!src)     std::cerr << "  - rtspsrc" << std::endl;
        if (!decoder) std::cerr << "  - nvv4l2decoder" << std::endl;
        if (!conv)    std::cerr << "  - nvvidconv" << std::endl;
        if (!enc)     std::cerr << "  - nvv4l2h264enc" << std::endl;
        gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
        return false;
    }

    // Configure rtspsrc
    g_object_set(G_OBJECT(src),
        "location",        config_.rtsp.url.c_str(),
        "protocols",       (config_.rtsp.transport == "tcp") ? 4 : 1,
        "latency",         (guint)config_.rtsp.latency_ms,
        "tcp-timeout",     (guint64)5000000,
        "retry",           (guint)5,
        "do-retransmission", FALSE,
        "drop-on-latency", TRUE,
        "ntp-sync",        FALSE,
        NULL);

    // Decoder
    g_object_set(G_OBJECT(decoder), "enable-max-performance", TRUE, NULL);

    // Input parse: inline SPS/PPS
    g_object_set(G_OBJECT(parse_in), "config-interval", -1, NULL);

    // Encoder (NVENC)
    encoder_.configure(enc,
                       config_.encoder.target_bitrate_kbps,
                       config_.encoder.max_bitrate_kbps,
                       config_.encoder.idr_interval,
                       config_.encoder.preset,
                       config_.encoder.profile,
                       config_.encoder.control_rate);

    // Output parse: inject SPS/PPS with every IDR
    g_object_set(G_OBJECT(parse_out), "config-interval", -1, NULL);

    // Appsink
    GstCaps* sink_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(sink),
        "emit-signals", FALSE, "sync", FALSE,
        "max-buffers", (guint)3, "drop", TRUE,
        "caps", sink_caps, NULL);
    gst_caps_unref(sink_caps);
    appsink_ = sink;

    // Add all to pipeline
    gst_bin_add_many(GST_BIN(enc_pipeline_),
        src, depay, parse_in, decoder, conv, enc, parse_out, sink, NULL);

    // Link static elements
    if (!gst_element_link(depay, parse_in) ||
        !gst_element_link(parse_in, decoder) ||
        !gst_element_link(decoder, conv)) {
        std::cerr << "[ENC] Link failed (depay→decoder→conv)" << std::endl;
        gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
        return false;
    }

    // conv → enc (NVMM caps, NO framerate — decoder outputs 0/1)
    {
        std::ostringstream ss;
        ss << "video/x-raw(memory:NVMM),format=NV12"
           << ",width=" << config_.encoder.width
           << ",height=" << config_.encoder.height;
        GstCaps* caps = gst_caps_from_string(ss.str().c_str());
        if (!gst_element_link_filtered(conv, enc, caps)) {
            std::cerr << "[ENC] Link failed (conv→enc): " << ss.str() << std::endl;
            gst_caps_unref(caps);
            gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
            return false;
        }
        gst_caps_unref(caps);
    }

    // enc → parse_out (byte-stream)
    GstCaps* enc_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream");
    if (!gst_element_link_filtered(enc, parse_out, enc_caps)) {
        std::cerr << "[ENC] Link failed (enc→parse_out)" << std::endl;
        gst_caps_unref(enc_caps);
        gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
        return false;
    }
    gst_caps_unref(enc_caps);

    // parse_out → sink
    if (!gst_element_link(parse_out, sink)) {
        std::cerr << "[ENC] Link failed (parse_out→sink)" << std::endl;
        gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
        return false;
    }

    // Dynamic pad for rtspsrc → depay
    g_signal_connect(src, "pad-added", G_CALLBACK(Pipeline::on_pad_added), depay);

    // Frame counter probe
    GstPad* pad = gst_element_get_static_pad(sink, "sink");
    if (pad) {
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, frame_probe, &stats_, NULL);
        gst_object_unref(pad);
    }

    // Bus watch
    enc_bus_ = gst_element_get_bus(enc_pipeline_);
    gst_bus_add_watch(enc_bus_, Pipeline::on_bus_message, this);

    std::cout << "[ENC] Pipeline built OK" << std::endl;
    return true;
}

// ============================================================================

bool Pipeline::start() {
    if (running_.load()) return false;

    if (!build_encoder_pipeline()) {
        std::cerr << "[PIPE] Build failed" << std::endl;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[PIPE] PLAYING failed" << std::endl;
        stop_encoder();
        return false;
    }

    if (!start_rtsp_server()) {
        std::cerr << "[PIPE] RTSP server failed" << std::endl;
        stop_encoder();
        return false;
    }

    running_.store(true);
    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;

    std::cout << "============================================" << std::endl;
    std::cout << "  RUNNING" << std::endl;
    std::cout << "  Input:   " << config_.rtsp.url << std::endl;
    std::cout << "  Output:  rtsp://localhost:" << config_.output.port
              << config_.output.path << std::endl;
    std::cout << "============================================" << std::endl;

    return true;
}

void Pipeline::stop() {
    if (!running_.load()) return;
    std::cout << "[PIPE] Stopping..." << std::endl;
    running_.store(false);
    stop_encoder();
    stop_rtsp_server();
    std::cout << "[PIPE] Stopped" << std::endl;
}

bool Pipeline::restart_encoder() {
    int max = config_.resilience.max_pipeline_restarts;
    uint32_t cur = stats_.restart_count();
    if (max > 0 && (int)cur >= max) {
        std::cerr << "[PIPE] Max restarts reached" << std::endl;
        return false;
    }

    stats_.on_pipeline_restart();
    std::cout << "[PIPE] Restart #" << stats_.restart_count()
              << ", wait " << reconnect_delay_s_ << "s..." << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_s_));
    reconnect_delay_s_ = std::min(reconnect_delay_s_ * 2, 30);

    stop_encoder();
    if (!build_encoder_pipeline()) return false;

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) { stop_encoder(); return false; }

    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
    std::cout << "[PIPE] Restarted OK" << std::endl;
    return true;
}

bool Pipeline::watchdog_check() {
    if (!running_.load()) return false;
    double since = stats_.seconds_since_last_frame();
    if (since > (double)config_.resilience.watchdog_timeout_s && stats_.frame_count() > 0) {
        std::cerr << "[WATCH] No frames for " << since << "s" << std::endl;
        return false;
    }
    return true;
}

void Pipeline::set_bitrate(uint32_t t, uint32_t m) {
    config_.encoder.target_bitrate_kbps = t;
    config_.encoder.max_bitrate_kbps = m;
    encoder_.set_bitrate(t, m);
}

GstSample* Pipeline::pull_latest_sample() {
    if (!appsink_ || !running_.load()) return nullptr;
    return gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), GST_MSECOND * 100);
}

std::string Pipeline::get_caps_string() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    return caps_string_;
}

// ============================================================================
//  RTSP Server
// ============================================================================

bool Pipeline::start_rtsp_server() {
    rtsp_server_ = gst_rtsp_server_new();
    if (!rtsp_server_) return false;

    char port[16];
    snprintf(port, sizeof(port), "%d", config_.output.port);
    gst_rtsp_server_set_service(rtsp_server_, port);

    GstRTSPMediaFactory* factory = encoder_factory_new(this);
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(rtsp_server_);
    gst_rtsp_mount_points_add_factory(mounts, config_.output.path.c_str(), factory);
    g_object_unref(mounts);

    server_source_id_ = gst_rtsp_server_attach(rtsp_server_, NULL);
    if (server_source_id_ == 0) {
        g_object_unref(rtsp_server_); rtsp_server_ = nullptr;
        return false;
    }

    std::cout << "[SERVER] rtsp://localhost:" << config_.output.port
              << config_.output.path << std::endl;
    return true;
}

void Pipeline::stop_encoder() {
    if (enc_pipeline_) {
        gst_element_set_state(enc_pipeline_, GST_STATE_NULL);
        if (enc_bus_) { gst_bus_remove_watch(enc_bus_); gst_object_unref(enc_bus_); enc_bus_ = nullptr; }
        appsink_ = nullptr;
        gst_object_unref(enc_pipeline_); enc_pipeline_ = nullptr;
    }
}

void Pipeline::stop_rtsp_server() {
    if (server_source_id_) { g_source_remove(server_source_id_); server_source_id_ = 0; }
    if (rtsp_server_) { g_object_unref(rtsp_server_); rtsp_server_ = nullptr; }
}

// ============================================================================
//  Callbacks
// ============================================================================

void Pipeline::on_pad_added(GstElement*, GstPad* pad, gpointer data) {
    GstElement* depay = static_cast<GstElement*>(data);
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, NULL);
    if (!caps) return;

    GstStructure* s = gst_caps_get_structure(caps, 0);
    const gchar* name = gst_structure_get_name(s);

    if (g_str_has_prefix(name, "application/x-rtp")) {
        const gchar* enc = gst_structure_get_string(s, "encoding-name");
        if (enc && g_strcmp0(enc, "H264") == 0) {
            GstPad* sink = gst_element_get_static_pad(depay, "sink");
            if (sink && !gst_pad_is_linked(sink)) {
                if (gst_pad_link(pad, sink) == GST_PAD_LINK_OK) {
                    std::cout << "[ENC] Linked rtspsrc → depay" << std::endl;
                }
            }
            if (sink) gst_object_unref(sink);
        }
    }
    gst_caps_unref(caps);
}

gboolean Pipeline::on_bus_message(GstBus*, GstMessage* msg, gpointer data) {
    Pipeline* self = static_cast<Pipeline*>(data);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::cerr << "[ENC] ERROR: " << (err ? err->message : "?") << std::endl;
            if (dbg) { std::cerr << "[ENC] " << dbg << std::endl; g_free(dbg); }
            if (err) g_error_free(err);
            self->stats_.on_reconnect();
            break;
        }
        case GST_MESSAGE_EOS:
            std::cout << "[ENC] EOS" << std::endl;
            self->stats_.on_reconnect();
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->enc_pipeline_)) {
                GstState o, n, p;
                gst_message_parse_state_changed(msg, &o, &n, &p);
                std::cout << "[ENC] " << gst_element_state_get_name(o) << " → "
                          << gst_element_state_get_name(n) << std::endl;
            }
            break;
        default: break;
    }
    return TRUE;
}
