#include "pipeline.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

// ============================================================================
//  Custom RTSP Media Factory (GObject boilerplate)
// ============================================================================

G_DEFINE_TYPE(EncoderFactory, encoder_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl* url);

static void encoder_factory_class_init(EncoderFactoryClass* klass) {
    GstRTSPMediaFactoryClass* factory_class = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
    factory_class->create_element = encoder_factory_create_element;
}

static void encoder_factory_init(EncoderFactory* /*self*/) {
    // Nothing to init
}

GstRTSPMediaFactory* encoder_factory_new(Pipeline* pipeline) {
    EncoderFactory* factory = (EncoderFactory*)g_object_new(TYPE_ENCODER_FACTORY, NULL);
    factory->pipeline = pipeline;

    // Shared: all clients see the same stream
    gst_rtsp_media_factory_set_shared(GST_RTSP_MEDIA_FACTORY(factory), TRUE);

    return GST_RTSP_MEDIA_FACTORY(factory);
}

/// Called by GstRTSPServer when a client connects: creates the serving pipeline.
/// Pipeline: appsrc → h264parse → rtph264pay
static GstElement* encoder_factory_create_element(GstRTSPMediaFactory* factory,
                                                    const GstRTSPUrl* /*url*/) {
    EncoderFactory* self = ENCODER_FACTORY(factory);
    Pipeline* pipeline = self->pipeline;

    std::cout << "[RTSP-SERVER] Client connected, creating serve pipeline..." << std::endl;

    // Create the serving bin
    GstElement* bin = gst_bin_new("serve-bin");

    // appsrc: receives H.264 encoded buffers from encoder pipeline
    GstElement* appsrc = gst_element_factory_make("appsrc", "appsrc0");
    GstElement* h264parse = gst_element_factory_make("h264parse", "parse0");
    GstElement* pay = gst_element_factory_make("rtph264pay", "pay0");

    if (!appsrc || !h264parse || !pay) {
        std::cerr << "[RTSP-SERVER] Failed to create serve pipeline elements" << std::endl;
        gst_object_unref(bin);
        return nullptr;
    }

    // Configure appsrc
    // Set caps for H.264 byte-stream
    GstCaps* caps = gst_caps_from_string(
        "video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(appsrc),
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        "block", FALSE,
        "max-bytes", (guint64)(2 * 1024 * 1024),  // 2MB buffer
        "caps", caps,
        NULL);
    gst_caps_unref(caps);

    // Configure h264parse to output config with every IDR
    g_object_set(G_OBJECT(h264parse), "config-interval", -1, NULL);

    // Configure rtph264pay for low latency
    g_object_set(G_OBJECT(pay), "config-interval", -1, "pt", 96, NULL);

    // Add to bin and link
    gst_bin_add_many(GST_BIN(bin), appsrc, h264parse, pay, NULL);
    if (!gst_element_link_many(appsrc, h264parse, pay, NULL)) {
        std::cerr << "[RTSP-SERVER] Failed to link serve pipeline" << std::endl;
        gst_object_unref(bin);
        return nullptr;
    }

    // Ghost pad: expose rtph264pay's src pad as the bin's pad
    GstPad* src_pad = gst_element_get_static_pad(pay, "src");
    GstPad* ghost_pad = gst_ghost_pad_new("src", src_pad);
    gst_pad_set_active(ghost_pad, TRUE);
    gst_element_add_pad(bin, ghost_pad);
    gst_object_unref(src_pad);

    // Start a feeder thread: pull from encoder appsink, push to this appsrc
    gst_object_ref(appsrc);

    std::thread feeder([pipeline, appsrc]() {
        std::cout << "[RTSP-SERVER] Feeder thread started" << std::endl;
        while (pipeline->is_running()) {
            GstSample* sample = pipeline->pull_latest_sample();
            if (sample) {
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                if (buffer) {
                    GstBuffer* buf_copy = gst_buffer_copy(buffer);
                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf_copy);
                    if (ret != GST_FLOW_OK) {
                        gst_sample_unref(sample);
                        break;
                    }
                }
                gst_sample_unref(sample);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        std::cout << "[RTSP-SERVER] Feeder thread stopped" << std::endl;
        gst_object_unref(appsrc);
    });
    feeder.detach();

    std::cout << "[RTSP-SERVER] Serve pipeline created successfully" << std::endl;
    return bin;
}

// ============================================================================
//  Pipeline Implementation
// ============================================================================

Pipeline::Pipeline(const AppConfig& config, Stats& stats)
    : config_(config), stats_(stats) {
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
}

Pipeline::~Pipeline() {
    stop();
}

// ============================================================================
//  Encoder Pipeline
// ============================================================================

/// Buffer probe callback for frame counting on appsink's sink pad.
/// Runs in the streaming thread — must be fast.
static GstPadProbeReturn frame_count_probe(GstPad* /*pad*/,
                                            GstPadProbeInfo* /*info*/,
                                            gpointer user_data) {
    Stats* stats = static_cast<Stats*>(user_data);
    stats->on_frame_encoded();
    return GST_PAD_PROBE_OK;
}

bool Pipeline::build_encoder_pipeline() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build pipeline elements individually for proper dynamic pad handling
    // rtspsrc → rtph264depay → h264parse → (caps) → nvv4l2decoder → nvvidconv → (caps) → nvv4l2h264enc → h264parse → appsink

    enc_pipeline_ = gst_pipeline_new("encoder-pipeline");
    if (!enc_pipeline_) {
        std::cerr << "[ENCODER] Failed to create pipeline" << std::endl;
        return false;
    }

    // Create elements
    GstElement* rtspsrc     = gst_element_factory_make("rtspsrc",       "src");
    GstElement* depay       = gst_element_factory_make("rtph264depay",  "depay");
    GstElement* parse_in    = gst_element_factory_make("h264parse",     "parse_in");
    GstElement* decoder     = gst_element_factory_make("nvv4l2decoder", "decoder");
    GstElement* vidconv     = gst_element_factory_make("nvvidconv",     "vidconv");
    GstElement* encoder     = gst_element_factory_make("nvv4l2h264enc", "encoder");
    GstElement* parse_out   = gst_element_factory_make("h264parse",     "parse_out");
    GstElement* sink        = gst_element_factory_make("appsink",       "enc_sink");

    // Verify all elements created
    if (!rtspsrc || !depay || !parse_in || !decoder || !vidconv ||
        !encoder || !parse_out || !sink) {
        std::cerr << "[ENCODER] Failed to create one or more elements. Check GStreamer plugins:" << std::endl;
        if (!rtspsrc)   std::cerr << "  - rtspsrc missing (gst-plugins-good)" << std::endl;
        if (!depay)     std::cerr << "  - rtph264depay missing (gst-plugins-good)" << std::endl;
        if (!decoder)   std::cerr << "  - nvv4l2decoder missing (nvidia-l4t-gstreamer)" << std::endl;
        if (!vidconv)   std::cerr << "  - nvvidconv missing (nvidia-l4t-gstreamer)" << std::endl;
        if (!encoder)   std::cerr << "  - nvv4l2h264enc missing (nvidia-l4t-gstreamer)" << std::endl;
        if (!sink)      std::cerr << "  - appsink missing (gst-plugins-base)" << std::endl;
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }

    // ---- Configure rtspsrc ----
    g_object_set(G_OBJECT(rtspsrc),
        "location",           config_.rtsp.url.c_str(),
        "protocols",          (config_.rtsp.transport == "tcp") ? 4 : 1,  // GST_RTSP_LOWER_TRANS_TCP=4, UDP=1
        "latency",            (guint)config_.rtsp.latency_ms,
        "tcp-timeout",        (guint64)5000000,   // 5 sec
        "retry",              (guint)5,
        "do-retransmission",  FALSE,
        "drop-on-latency",    TRUE,
        "ntp-sync",           FALSE,
        NULL);

    // ---- Configure decoder ----
    g_object_set(G_OBJECT(decoder),
        "enable-max-performance", TRUE,
        "disable-dpb",           TRUE,
        NULL);

    // ---- Configure encoder ----
    encoder_.configure(encoder,
                       config_.encoder.target_bitrate_kbps,
                       config_.encoder.max_bitrate_kbps,
                       config_.encoder.idr_interval,
                       config_.encoder.preset,
                       config_.encoder.profile,
                       config_.encoder.control_rate);

    // ---- Configure output parse ----
    g_object_set(G_OBJECT(parse_out), "config-interval", -1, NULL);

    // ---- Configure appsink ----
    // No signal needed — feeder thread uses try_pull_sample
    // Frame counting done via pad probe (no conflict)
    GstCaps* sink_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    g_object_set(G_OBJECT(sink),
        "emit-signals", FALSE,
        "sync",         FALSE,
        "max-buffers",  (guint)3,
        "drop",         TRUE,
        "caps",         sink_caps,
        NULL);
    gst_caps_unref(sink_caps);
    appsink_ = sink;

    // ---- Add all elements to pipeline ----
    gst_bin_add_many(GST_BIN(enc_pipeline_),
        rtspsrc, depay, parse_in, decoder, vidconv, encoder, parse_out, sink,
        NULL);

    // ---- Link static elements: depay → parse_in → decoder → vidconv → encoder → parse_out → sink ----
    // depay → parse_in
    if (!gst_element_link(depay, parse_in)) {
        std::cerr << "[ENCODER] Failed to link depay → parse_in" << std::endl;
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }

    // parse_in → decoder (with explicit byte-stream caps)
    GstCaps* h264_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
    if (!gst_element_link_filtered(parse_in, decoder, h264_caps)) {
        std::cerr << "[ENCODER] Failed to link parse_in → decoder with caps" << std::endl;
        gst_caps_unref(h264_caps);
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }
    gst_caps_unref(h264_caps);

    // decoder → vidconv
    if (!gst_element_link(decoder, vidconv)) {
        std::cerr << "[ENCODER] Failed to link decoder → vidconv" << std::endl;
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }

    // vidconv → encoder (with resolution/framerate caps on NVMM memory)
    {
        std::ostringstream caps_ss;
        caps_ss << "video/x-raw(memory:NVMM)"
                << ",format=NV12"
                << ",width=" << config_.encoder.width
                << ",height=" << config_.encoder.height
                << ",framerate=" << config_.encoder.framerate << "/1";
        GstCaps* raw_caps = gst_caps_from_string(caps_ss.str().c_str());
        if (!gst_element_link_filtered(vidconv, encoder, raw_caps)) {
            std::cerr << "[ENCODER] Failed to link vidconv → encoder with caps: "
                      << caps_ss.str() << std::endl;
            gst_caps_unref(raw_caps);
            gst_object_unref(enc_pipeline_);
            enc_pipeline_ = nullptr;
            return false;
        }
        gst_caps_unref(raw_caps);
    }

    // encoder → parse_out → sink
    // Force byte-stream output from encoder
    GstCaps* enc_out_caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream");
    if (!gst_element_link_filtered(encoder, parse_out, enc_out_caps)) {
        std::cerr << "[ENCODER] Failed to link encoder → parse_out" << std::endl;
        gst_caps_unref(enc_out_caps);
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }
    gst_caps_unref(enc_out_caps);

    if (!gst_element_link(parse_out, sink)) {
        std::cerr << "[ENCODER] Failed to link parse_out → sink" << std::endl;
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }

    // ---- Handle rtspsrc dynamic pads ----
    // rtspsrc creates pads dynamically when RTSP DESCRIBE/SETUP completes
    // We connect to "pad-added" to link rtspsrc → depay when the video pad appears
    g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(Pipeline::on_pad_added), depay);

    // ---- Frame count probe on appsink's sink pad ----
    GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
    if (sink_pad) {
        gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          frame_count_probe, &stats_, NULL);
        gst_object_unref(sink_pad);
    }

    // ---- Bus watch ----
    enc_bus_ = gst_element_get_bus(enc_pipeline_);
    gst_bus_add_watch(enc_bus_, Pipeline::on_bus_message, this);

    std::cout << "[ENCODER] Pipeline built successfully" << std::endl;
    return true;
}

void Pipeline::stop_encoder() {
    if (enc_pipeline_) {
        gst_element_set_state(enc_pipeline_, GST_STATE_NULL);

        if (enc_bus_) {
            gst_bus_remove_watch(enc_bus_);
            gst_object_unref(enc_bus_);
            enc_bus_ = nullptr;
        }

        // Don't unref appsink separately — it's owned by the pipeline
        appsink_ = nullptr;

        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
    }
}

// ============================================================================
//  RTSP Server
// ============================================================================

bool Pipeline::start_rtsp_server() {
    rtsp_server_ = gst_rtsp_server_new();
    if (!rtsp_server_) {
        std::cerr << "[RTSP-SERVER] Failed to create server" << std::endl;
        return false;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", config_.output.port);
    gst_rtsp_server_set_service(rtsp_server_, port_str);

    // Create our custom factory
    GstRTSPMediaFactory* factory = encoder_factory_new(this);
    if (!factory) {
        std::cerr << "[RTSP-SERVER] Failed to create factory" << std::endl;
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
        return false;
    }

    // Mount it
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(rtsp_server_);
    gst_rtsp_mount_points_add_factory(mounts, config_.output.path.c_str(), factory);
    g_object_unref(mounts);

    // Attach to main context
    server_source_id_ = gst_rtsp_server_attach(rtsp_server_, NULL);
    if (server_source_id_ == 0) {
        std::cerr << "[RTSP-SERVER] Failed to attach server" << std::endl;
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
        return false;
    }

    std::cout << "[RTSP-SERVER] Listening on port " << config_.output.port
              << ", path: " << config_.output.path << std::endl;
    std::cout << "[RTSP-SERVER] URL: rtsp://localhost:" << config_.output.port
              << config_.output.path << std::endl;

    return true;
}

void Pipeline::stop_rtsp_server() {
    if (server_source_id_ > 0) {
        g_source_remove(server_source_id_);
        server_source_id_ = 0;
    }
    if (rtsp_server_) {
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
    }
}

// ============================================================================
//  Public Interface
// ============================================================================

bool Pipeline::start() {
    if (running_.load()) {
        std::cerr << "[PIPELINE] Already running" << std::endl;
        return false;
    }

    // 1. Build and start the encoder pipeline
    if (!build_encoder_pipeline()) {
        std::cerr << "[PIPELINE] Failed to build encoder pipeline" << std::endl;
        return false;
    }

    // Set encoder pipeline to PLAYING
    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[PIPELINE] Failed to start encoder pipeline" << std::endl;
        stop_encoder();
        return false;
    }

    std::cout << "[ENCODER] Pipeline state → PLAYING" << std::endl;

    // 2. Start RTSP server
    if (!start_rtsp_server()) {
        std::cerr << "[PIPELINE] Failed to start RTSP server" << std::endl;
        stop_encoder();
        return false;
    }

    running_.store(true);
    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;

    std::cout << "============================================" << std::endl;
    std::cout << "  Pipeline RUNNING" << std::endl;
    std::cout << "  RTSP source:  " << config_.rtsp.url << std::endl;
    std::cout << "  RTSP output:  rtsp://localhost:" << config_.output.port
              << config_.output.path << std::endl;
    std::cout << "  go2rtc UI:    http://localhost:1984" << std::endl;
    std::cout << "============================================" << std::endl;

    return true;
}

void Pipeline::stop() {
    if (!running_.load()) return;

    std::cout << "[PIPELINE] Stopping..." << std::endl;
    running_.store(false);
    stop_encoder();
    stop_rtsp_server();
    std::cout << "[PIPELINE] Stopped" << std::endl;
}

bool Pipeline::watchdog_check() {
    if (!running_.load()) return false;

    double since_last = stats_.seconds_since_last_frame();
    uint64_t frames = stats_.frame_count();

    if (since_last > static_cast<double>(config_.resilience.watchdog_timeout_s) && frames > 0) {
        std::cerr << "[WATCHDOG] No frames for " << since_last
                  << "s — triggering encoder restart" << std::endl;
        return false;
    }

    return true;
}

bool Pipeline::restart_encoder() {
    int max_restarts = config_.resilience.max_pipeline_restarts;
    uint32_t current = stats_.restart_count();

    if (max_restarts > 0 && static_cast<int>(current) >= max_restarts) {
        std::cerr << "[PIPELINE] Max restarts reached (" << max_restarts << ")" << std::endl;
        return false;
    }

    stats_.on_pipeline_restart();
    std::cout << "[PIPELINE] Restarting encoder (attempt #" << stats_.restart_count()
              << "), waiting " << reconnect_delay_s_ << "s..." << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_s_));
    reconnect_delay_s_ = std::min(reconnect_delay_s_ * 2, 30);

    // Only restart the encoder, keep RTSP server running
    stop_encoder();

    if (!build_encoder_pipeline()) {
        std::cerr << "[PIPELINE] Failed to rebuild encoder" << std::endl;
        return false;
    }

    GstStateChangeReturn ret = gst_element_set_state(enc_pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[PIPELINE] Failed to restart encoder to PLAYING" << std::endl;
        stop_encoder();
        return false;
    }

    stats_.reset();
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
    std::cout << "[PIPELINE] Encoder restarted successfully" << std::endl;
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
//  GStreamer Callbacks
// ============================================================================

void Pipeline::on_pad_added(GstElement* /*src*/, GstPad* new_pad, gpointer user_data) {
    GstElement* depay = static_cast<GstElement*>(user_data);

    // Only link video pads (ignore audio or other)
    GstCaps* pad_caps = gst_pad_get_current_caps(new_pad);
    if (!pad_caps) {
        pad_caps = gst_pad_query_caps(new_pad, NULL);
    }

    if (pad_caps) {
        GstStructure* s = gst_caps_get_structure(pad_caps, 0);
        const gchar* media_type = gst_structure_get_name(s);

        if (g_str_has_prefix(media_type, "application/x-rtp")) {
            // Check if this is a video RTP stream
            const gchar* encoding = gst_structure_get_string(s, "encoding-name");
            if (encoding && (g_strcmp0(encoding, "H264") == 0)) {
                GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
                if (sink_pad && !gst_pad_is_linked(sink_pad)) {
                    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
                    if (ret == GST_PAD_LINK_OK) {
                        std::cout << "[ENCODER] Linked rtspsrc → rtph264depay (H.264 stream found)"
                                  << std::endl;
                    } else {
                        std::cerr << "[ENCODER] Failed to link rtspsrc → depay: "
                                  << ret << std::endl;
                    }
                }
                if (sink_pad) gst_object_unref(sink_pad);
            }
        }

        gst_caps_unref(pad_caps);
    }
}

gboolean Pipeline::on_bus_message(GstBus* /*bus*/, GstMessage* msg, gpointer data) {
    Pipeline* self = static_cast<Pipeline*>(data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);

            std::cerr << "[ENCODER] ERROR: " << (err ? err->message : "unknown") << std::endl;
            if (debug) {
                std::cerr << "[ENCODER] Debug: " << debug << std::endl;
                g_free(debug);
            }
            if (err) g_error_free(err);

            self->stats_.on_reconnect();
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            std::cerr << "[ENCODER] WARNING: " << (err ? err->message : "unknown") << std::endl;
            if (debug) g_free(debug);
            if (err) g_error_free(err);
            break;
        }

        case GST_MESSAGE_EOS:
            std::cout << "[ENCODER] End of stream — will restart" << std::endl;
            self->stats_.on_reconnect();
            break;

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->enc_pipeline_)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                std::cout << "[ENCODER] State: "
                          << gst_element_state_get_name(old_state) << " → "
                          << gst_element_state_get_name(new_state) << std::endl;
            }
            break;
        }

        default:
            break;
    }

    return TRUE;
}
