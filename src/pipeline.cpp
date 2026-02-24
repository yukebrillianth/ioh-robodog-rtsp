#include "pipeline.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

// All output to stderr (stdout reserved for H.264 data in exec mode)
#define LOG(msg) std::cerr << msg << std::endl

// ============================================================================
//  Custom RTSP Media Factory (only used in RTSP mode)
// ============================================================================

G_DEFINE_TYPE(EncoderFactory, encoder_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl* url);

static void encoder_factory_class_init(EncoderFactoryClass* klass) {
    GstRTSPMediaFactoryClass* factory_class = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
    factory_class->create_element = encoder_factory_create_element;
}

static void encoder_factory_init(EncoderFactory*) {}

GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline) {
    EncoderFactory* factory = (EncoderFactory*)g_object_new(TYPE_ENCODER_FACTORY, NULL);
    factory->pipeline = pipeline;
    gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(factory), TRUE);
    return GST_RTSP_MEDIA_FACTORY(factory);
}

static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl*) {
    EncoderFactory* self = ENCODER_FACTORY(factory);
    Pipeline* pipeline = self->pipeline;

    LOG("[RTSP-SERVER] Client connected, creating serve pipeline...");

    GstElement* bin = gst_bin_new("serve-bin");
    GstElement* appsrc = gst_element_factory_make("appsrc", "appsrc0");
    GstElement* h264parse = gst_element_factory_make("h264parse", "parse0");
    GstElement* pay = gst_element_factory_make("rtph264pay", "pay0");

    if (!appsrc || !h264parse || !pay) {
        LOG("[RTSP-SERVER] Failed to create serve pipeline elements");
        gst_object_unref(bin);
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(appsrc),
        "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", TRUE,
        "block", FALSE, "max-bytes", (guint64)(2 * 1024 * 1024), "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(G_OBJECT(h264parse), "config-interval", -1, NULL);
    g_object_set(G_OBJECT(pay), "config-interval", -1, "pt", 96, NULL);

    gst_bin_add_many(GST_BIN(bin), appsrc, h264parse, pay, NULL);
    if (!gst_element_link_many(appsrc, h264parse, pay, NULL)) {
        LOG("[RTSP-SERVER] Failed to link serve pipeline");
        gst_object_unref(bin);
        return nullptr;
    }

    // Feeder thread
    gst_object_ref(appsrc);
    std::thread([pipeline, appsrc]() {
        LOG("[RTSP-SERVER] Feeder thread started");
        while (pipeline->is_running()) {
            GstSample* sample = pipeline->pull_latest_sample();
            if (sample) {
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                if (buffer) {
                    GstBuffer* copy = gst_buffer_copy(buffer);
                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), copy);
                    if (ret != GST_FLOW_OK) { gst_sample_unref(sample); break; }
                }
                gst_sample_unref(sample);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        LOG("[RTSP-SERVER] Feeder thread stopped");
        gst_object_unref(appsrc);
    }).detach();

    LOG("[RTSP-SERVER] Serve pipeline created");
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

// Buffer probe for frame counting
static GstPadProbeReturn frame_count_probe(GstPad*, GstPadProbeInfo*, gpointer user_data) {
    static_cast<Stats*>(user_data)->on_frame_encoded();
    return GST_PAD_PROBE_OK;
}

// ============================================================================
//  Shared pipeline builder — creates everything from rtspsrc to encoder output
//  then attaches the provided sink_element at the end
// ============================================================================

bool Pipeline::build_pipeline_common(GstElement*& pipeline, GstElement* sink_element) {
    std::lock_guard<std::mutex> lock(mutex_);

    pipeline = gst_pipeline_new("encoder-pipeline");
    if (!pipeline) { LOG("[ENCODER] Failed to create pipeline"); return false; }

    GstElement* rtspsrc   = gst_element_factory_make("rtspsrc",       "src");
    GstElement* depay     = gst_element_factory_make("rtph264depay",  "depay");
    GstElement* parse_in  = gst_element_factory_make("h264parse",     "parse_in");
    GstElement* decoder   = gst_element_factory_make("nvv4l2decoder", "decoder");
    GstElement* vidconv   = gst_element_factory_make("nvvidconv",     "vidconv");
    GstElement* encoder   = gst_element_factory_make("nvv4l2h264enc", "encoder");
    GstElement* parse_out = gst_element_factory_make("h264parse",     "parse_out");

    if (!rtspsrc || !depay || !parse_in || !decoder || !vidconv ||
        !encoder || !parse_out || !sink_element) {
        LOG("[ENCODER] Failed to create elements. Check GStreamer plugins.");
        if (!rtspsrc)   LOG("  - rtspsrc missing");
        if (!decoder)   LOG("  - nvv4l2decoder missing (nvidia-l4t-gstreamer)");
        if (!vidconv)   LOG("  - nvvidconv missing");
        if (!encoder)   LOG("  - nvv4l2h264enc missing");
        gst_object_unref(pipeline); pipeline = nullptr;
        return false;
    }

    // Configure rtspsrc
    g_object_set(G_OBJECT(rtspsrc),
        "location",          config_.rtsp.url.c_str(),
        "protocols",         (config_.rtsp.transport == "tcp") ? 4 : 1,
        "latency",           (guint)config_.rtsp.latency_ms,
        "tcp-timeout",       (guint64)5000000,
        "retry",             (guint)5,
        "do-retransmission", FALSE,
        "drop-on-latency",   TRUE,
        "ntp-sync",          FALSE,
        NULL);

    // Configure decoder
    g_object_set(G_OBJECT(decoder), "enable-max-performance", TRUE, NULL);

    // h264parse: inline SPS/PPS for decoder
    g_object_set(G_OBJECT(parse_in), "config-interval", -1, NULL);

    // Configure NVENC encoder
    encoder_.configure(encoder,
                       config_.encoder.target_bitrate_kbps,
                       config_.encoder.max_bitrate_kbps,
                       config_.encoder.idr_interval,
                       config_.encoder.preset,
                       config_.encoder.profile,
                       config_.encoder.control_rate);

    // Output h264parse: inject SPS/PPS with every IDR for fast stream start
    g_object_set(G_OBJECT(parse_out), "config-interval", -1, NULL);

    // Add all to pipeline
    gst_bin_add_many(GST_BIN(pipeline),
        rtspsrc, depay, parse_in, decoder, vidconv, encoder, parse_out, sink_element,
        NULL);

    // Link: depay → parse_in
    if (!gst_element_link(depay, parse_in)) {
        LOG("[ENCODER] Link failed: depay → parse_in");
        gst_object_unref(pipeline); pipeline = nullptr; return false;
    }

    // parse_in → decoder (auto-negotiate)
    if (!gst_element_link(parse_in, decoder)) {
        LOG("[ENCODER] Link failed: parse_in → decoder");
        gst_object_unref(pipeline); pipeline = nullptr; return false;
    }

    // decoder → vidconv
    if (!gst_element_link(decoder, vidconv)) {
        LOG("[ENCODER] Link failed: decoder → vidconv");
        gst_object_unref(pipeline); pipeline = nullptr; return false;
    }

    // vidconv → encoder (with NVMM caps, NO framerate — decoder outputs 0/1)
    {
        std::ostringstream ss;
        ss << "video/x-raw(memory:NVMM),format=NV12"
           << ",width=" << config_.encoder.width
           << ",height=" << config_.encoder.height;
        GstCaps* caps = gst_caps_from_string(ss.str().c_str());
        if (!gst_element_link_filtered(vidconv, encoder, caps)) {
            LOG("[ENCODER] Link failed: vidconv → encoder with caps: " << ss.str());
            gst_caps_unref(caps);
            gst_object_unref(pipeline); pipeline = nullptr; return false;
        }
        gst_caps_unref(caps);
    }

    // encoder → parse_out (byte-stream)
    GstCaps* enc_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream");
    if (!gst_element_link_filtered(encoder, parse_out, enc_caps)) {
        LOG("[ENCODER] Link failed: encoder → parse_out");
        gst_caps_unref(enc_caps);
        gst_object_unref(pipeline); pipeline = nullptr; return false;
    }
    gst_caps_unref(enc_caps);

    // parse_out → sink_element
    if (!gst_element_link(parse_out, sink_element)) {
        LOG("[ENCODER] Link failed: parse_out → sink");
        gst_object_unref(pipeline); pipeline = nullptr; return false;
    }

    // Dynamic pad linking for rtspsrc
    g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(Pipeline::on_pad_added), depay);

    // Frame count probe
    GstPad* sink_pad = gst_element_get_static_pad(sink_element, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          frame_count_probe, &stats_, NULL);
        gst_object_unref(sink_pad);
    }

    // Bus watch
    enc_bus_ = gst_element_get_bus(pipeline);
    gst_bus_add_watch(enc_bus_, Pipeline::on_bus_message, this);

    LOG("[ENCODER] Pipeline built OK");
    return true;
}

// ============================================================================
//  STDOUT MODE — output H.264 byte-stream directly to fd=1 (stdout)
//  Zero buffering, go2rtc reads directly via exec:
// ============================================================================

bool Pipeline::build_stdout_pipeline() {
    GstElement* fdsink = gst_element_factory_make("fdsink", "stdout_sink");
    if (!fdsink) {
        LOG("[ENCODER] Failed to create fdsink");
        return false;
    }

    // fd=1 = stdout, sync=false = don't throttle to clock (real-time output)
    g_object_set(G_OBJECT(fdsink),
        "fd", 1,
        "sync", FALSE,
        NULL);

    return build_pipeline_common(enc_pipeline_, fdsink);
}

bool Pipeline::start_stdout_mode() {
    if (running_.load()) { LOG("[PIPELINE] Already running"); return false; }

    stdout_mode_ = true;

    if (!build_stdout_pipeline()) {
        LOG("[PIPELINE] Failed to build stdout pipeline");
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG("[PIPELINE] Failed to set PLAYING");
        stop_encoder();
        return false;
    }

    running_.store(true);
    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;

    LOG("============================================");
    LOG("  Pipeline RUNNING (stdout mode)");
    LOG("  Source: " << config_.rtsp.url);
    LOG("  Output: stdout (H.264 byte-stream)");
    LOG("============================================");

    return true;
}

bool Pipeline::restart_stdout() {
    int max_restarts = config_.resilience.max_pipeline_restarts;
    uint32_t current = stats_.restart_count();
    if (max_restarts > 0 && static_cast<int>(current) >= max_restarts) {
        LOG("[PIPELINE] Max restarts reached"); return false;
    }

    stats_.on_pipeline_restart();
    LOG("[PIPELINE] Restarting (attempt #" << stats_.restart_count()
        << "), wait " << reconnect_delay_s_ << "s...");

    std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_s_));
    reconnect_delay_s_ = std::min(reconnect_delay_s_ * 2, 30);

    stop_encoder();

    if (!build_stdout_pipeline()) { LOG("[PIPELINE] Rebuild failed"); return false; }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG("[PIPELINE] Restart PLAYING failed");
        stop_encoder(); return false;
    }

    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
    LOG("[PIPELINE] Restarted OK");
    return true;
}

// ============================================================================
//  RTSP SERVER MODE
// ============================================================================

bool Pipeline::build_encoder_pipeline() {
    GstElement* sink = gst_element_factory_make("appsink", "enc_sink");
    if (!sink) { LOG("[ENCODER] Failed to create appsink"); return false; }

    GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(sink),
        "emit-signals", FALSE, "sync", FALSE,
        "max-buffers", (guint)3, "drop", TRUE, "caps", caps, NULL);
    gst_caps_unref(caps);
    appsink_ = sink;

    return build_pipeline_common(enc_pipeline_, sink);
}

bool Pipeline::start() {
    if (running_.load()) { LOG("[PIPELINE] Already running"); return false; }

    stdout_mode_ = false;

    if (!build_encoder_pipeline()) {
        LOG("[PIPELINE] Failed to build encoder pipeline"); return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG("[PIPELINE] Failed to start"); stop_encoder(); return false;
    }

    if (!start_rtsp_server()) {
        LOG("[PIPELINE] Failed to start RTSP server"); stop_encoder(); return false;
    }

    running_.store(true);
    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;

    LOG("============================================");
    LOG("  Pipeline RUNNING (RTSP server mode)");
    LOG("  Source:  " << config_.rtsp.url);
    LOG("  Output:  rtsp://localhost:" << config_.output.port << config_.output.path);
    LOG("============================================");

    return true;
}

bool Pipeline::restart_encoder() {
    int max_restarts = config_.resilience.max_pipeline_restarts;
    uint32_t current = stats_.restart_count();
    if (max_restarts > 0 && static_cast<int>(current) >= max_restarts) {
        LOG("[PIPELINE] Max restarts reached"); return false;
    }

    stats_.on_pipeline_restart();
    LOG("[PIPELINE] Restarting encoder (attempt #" << stats_.restart_count() << ")...");

    std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_s_));
    reconnect_delay_s_ = std::min(reconnect_delay_s_ * 2, 30);

    stop_encoder();

    if (!build_encoder_pipeline()) { LOG("[PIPELINE] Rebuild failed"); return false; }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG("[PIPELINE] Restart PLAYING failed"); stop_encoder(); return false;
    }

    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
    LOG("[PIPELINE] Encoder restarted OK");
    return true;
}

bool Pipeline::start_rtsp_server() {
    rtsp_server_ = gst_rtsp_server_new();
    if (!rtsp_server_) { LOG("[RTSP-SERVER] Failed to create"); return false; }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", config_.output.port);
    gst_rtsp_server_set_service(rtsp_server_, port_str);

    GstRTSPMediaFactory* factory = encoder_factory_new(this);
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(rtsp_server_);
    gst_rtsp_mount_points_add_factory(mounts, config_.output.path.c_str(), factory);
    g_object_unref(mounts);

    server_source_id_ = gst_rtsp_server_attach(rtsp_server_, NULL);
    if (server_source_id_ == 0) {
        LOG("[RTSP-SERVER] Failed to attach");
        g_object_unref(rtsp_server_); rtsp_server_ = nullptr;
        return false;
    }

    LOG("[RTSP-SERVER] rtsp://localhost:" << config_.output.port << config_.output.path);
    return true;
}

// ============================================================================
//  Common
// ============================================================================

void Pipeline::stop() {
    if (!running_.load()) return;
    LOG("[PIPELINE] Stopping...");
    running_.store(false);
    stop_encoder();
    stop_rtsp_server();
    LOG("[PIPELINE] Stopped");
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
    if (server_source_id_ > 0) { g_source_remove(server_source_id_); server_source_id_ = 0; }
    if (rtsp_server_) { g_object_unref(rtsp_server_); rtsp_server_ = nullptr; }
}

bool Pipeline::watchdog_check() {
    if (!running_.load()) return false;
    double since = stats_.seconds_since_last_frame();
    if (since > static_cast<double>(config_.resilience.watchdog_timeout_s) && stats_.frame_count() > 0) {
        LOG("[WATCHDOG] No frames for " << since << "s");
        return false;
    }
    return true;
}

void Pipeline::set_bitrate(uint32_t target_kbps, uint32_t max_kbps) {
    config_.encoder.target_bitrate_kbps = target_kbps;
    config_.encoder.max_bitrate_kbps = max_kbps;
    encoder_.set_bitrate(target_kbps, max_kbps);
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
//  Callbacks
// ============================================================================

void Pipeline::on_pad_added(GstElement*, GstPad* new_pad, gpointer user_data) {
    GstElement* depay = static_cast<GstElement*>(user_data);

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, NULL);

    if (caps) {
        GstStructure* s = gst_caps_get_structure(caps, 0);
        const gchar* media_type = gst_structure_get_name(s);

        if (g_str_has_prefix(media_type, "application/x-rtp")) {
            const gchar* encoding = gst_structure_get_string(s, "encoding-name");
            if (encoding && g_strcmp0(encoding, "H264") == 0) {
                GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
                if (sink_pad && !gst_pad_is_linked(sink_pad)) {
                    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
                    if (ret == GST_PAD_LINK_OK) {
                        LOG("[ENCODER] Linked rtspsrc → depay (H.264 found)");
                    } else {
                        LOG("[ENCODER] Link failed: rtspsrc → depay: " << ret);
                    }
                }
                if (sink_pad) gst_object_unref(sink_pad);
            }
        }
        gst_caps_unref(caps);
    }
}

gboolean Pipeline::on_bus_message(GstBus*, GstMessage* msg, gpointer data) {
    Pipeline* self = static_cast<Pipeline*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            LOG("[ENCODER] ERROR: " << (err ? err->message : "unknown"));
            if (debug) { LOG("[ENCODER] Debug: " << debug); g_free(debug); }
            if (err) g_error_free(err);
            self->stats_.on_reconnect();
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr; gchar* debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            LOG("[ENCODER] WARNING: " << (err ? err->message : "unknown"));
            if (debug) g_free(debug);
            if (err) g_error_free(err);
            break;
        }
        case GST_MESSAGE_EOS:
            LOG("[ENCODER] EOS — will restart");
            self->stats_.on_reconnect();
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->enc_pipeline_)) {
                GstState old_s, new_s, pending;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                LOG("[ENCODER] State: " << gst_element_state_get_name(old_s)
                    << " → " << gst_element_state_get_name(new_s));
            }
            break;
        default: break;
    }
    return TRUE;
}
