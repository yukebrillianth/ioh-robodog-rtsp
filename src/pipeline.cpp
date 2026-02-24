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
    g_object_set(G_OBJECT(appsrc),
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        "block", FALSE,
        "max-bytes", (guint64)(1024 * 1024),  // 1MB buffer
        NULL);

    // Set caps if available
    if (pipeline->has_caps()) {
        std::string caps_str = pipeline->get_caps_string();
        if (!caps_str.empty()) {
            GstCaps* caps = gst_caps_from_string(caps_str.c_str());
            if (caps) {
                g_object_set(G_OBJECT(appsrc), "caps", caps, NULL);
                gst_caps_unref(caps);
                std::cout << "[RTSP-SERVER] Set appsrc caps: " << caps_str << std::endl;
            }
        }
    } else {
        // Fallback caps
        GstCaps* caps = gst_caps_from_string("video/x-h264,stream-format=byte-stream,alignment=au");
        g_object_set(G_OBJECT(appsrc), "caps", caps, NULL);
        gst_caps_unref(caps);
    }

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
    // We ref the appsrc and capture the pipeline pointer
    gst_object_ref(appsrc);

    std::thread feeder([pipeline, appsrc]() {
        std::cout << "[RTSP-SERVER] Feeder thread started" << std::endl;
        while (pipeline->is_running()) {
            GstSample* sample = pipeline->pull_latest_sample();
            if (sample) {
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                if (buffer) {
                    // Make a copy of the buffer for appsrc
                    GstBuffer* buf_copy = gst_buffer_copy(buffer);
                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf_copy);
                    if (ret != GST_FLOW_OK) {
                        gst_sample_unref(sample);
                        break;  // Client disconnected or error
                    }
                }
                gst_sample_unref(sample);
            } else {
                // No sample yet, wait briefly
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

bool Pipeline::build_encoder_pipeline() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build pipeline string for the encoder
    // rtspsrc → depay → parse → nvv4l2decoder → nvvidconv → nvv4l2h264enc → parse → appsink
    std::ostringstream ss;

    ss << "rtspsrc location=" << config_.rtsp.url
       << " protocols=" << config_.rtsp.transport
       << " latency=" << config_.rtsp.latency_ms
       << " tcp-timeout=5000000"
       << " retry=5"
       << " do-retransmission=false"
       << " drop-on-latency=true"
       << " ntp-sync=false"
       << " name=src"

       << " src. ! rtph264depay ! h264parse"

       // Hardware decoder (Jetson)
       << " ! nvv4l2decoder"
       << " enable-max-performance=true"
       << " disable-dpb=true"

       // Scale/convert in GPU memory
       << " ! nvvidconv"

       // Resolution + framerate filter
       << " ! video/x-raw(memory:NVMM)"
       << ",width=" << config_.encoder.width
       << ",height=" << config_.encoder.height
       << ",framerate=" << config_.encoder.framerate << "/1"

       // Hardware encoder (Jetson NVENC)
       << " ! nvv4l2h264enc"
       << " name=encoder"
       << " bitrate=" << config_.encoder.target_bitrate_kbps * 1000
       << " peak-bitrate=" << config_.encoder.max_bitrate_kbps * 1000
       << " control-rate=1"
       << " preset-level=2"
       << " profile=4"
       << " idrinterval=" << config_.encoder.idr_interval
       << " insert-sps-pps=true"
       << " maxperf-enable=true"
       << " vbv-size=" << (config_.encoder.target_bitrate_kbps * 1000 / 30)

       // Parse output
       << " ! h264parse config-interval=-1"

       // Output to appsink
       << " ! appsink name=enc_sink emit-signals=true sync=false"
       << " max-buffers=2 drop=true";

    std::string pipeline_str = ss.str();
    std::cout << "[ENCODER] Pipeline: " << pipeline_str << std::endl;

    GError* error = nullptr;
    enc_pipeline_ = gst_parse_launch(pipeline_str.c_str(), &error);
    if (error) {
        std::cerr << "[ENCODER] Pipeline parse error: " << error->message << std::endl;
        g_error_free(error);
        return false;
    }
    if (!enc_pipeline_) {
        std::cerr << "[ENCODER] Failed to create pipeline" << std::endl;
        return false;
    }

    // Get the appsink element
    appsink_ = gst_bin_get_by_name(GST_BIN(enc_pipeline_), "enc_sink");
    if (!appsink_) {
        std::cerr << "[ENCODER] Failed to get appsink" << std::endl;
        gst_object_unref(enc_pipeline_);
        enc_pipeline_ = nullptr;
        return false;
    }

    // Connect new-sample signal
    g_signal_connect(appsink_, "new-sample", G_CALLBACK(Pipeline::on_new_sample), this);

    // Setup bus watch for error handling
    enc_bus_ = gst_element_get_bus(enc_pipeline_);
    gst_bus_add_watch(enc_bus_, Pipeline::on_bus_message, this);

    // Handle rtspsrc dynamic pads
    GstElement* rtspsrc = gst_bin_get_by_name(GST_BIN(enc_pipeline_), "src");
    if (rtspsrc) {
        // rtspsrc already connected via gst_parse_launch, but we need
        // to handle dynamic pads. Actually gst_parse_launch handles this
        // through the "!" syntax. If it doesn't work, we'll use pad-added.
        gst_object_unref(rtspsrc);
    }

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

        if (appsink_) {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }

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

GstFlowReturn Pipeline::on_new_sample(GstAppSink* sink, gpointer data) {
    Pipeline* self = static_cast<Pipeline*>(data);

    // Just count the frame for stats — the sample stays in appsink's queue
    // and is pulled by the RTSP server's feeder thread
    self->stats_.on_frame_encoded();

    // Capture caps on first frame
    if (!self->has_caps_.load()) {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (sample) {
            GstCaps* caps = gst_sample_get_caps(sample);
            if (caps) {
                gchar* caps_str = gst_caps_to_string(caps);
                if (caps_str) {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->caps_string_ = caps_str;
                    self->has_caps_.store(true);
                    std::cout << "[ENCODER] Captured caps: " << caps_str << std::endl;
                    g_free(caps_str);
                }
            }

            // Push the sample back? No — we consumed it. The feeder will get subsequent ones.
            // This first sample is lost, but it's just one frame.
            gst_sample_unref(sample);
        }
    }

    return GST_FLOW_OK;
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

            // Schedule restart (don't restart in callback — deadlock risk)
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

void Pipeline::on_pad_added(GstElement* /*src*/, GstPad* new_pad, gpointer data) {
    Pipeline* self = static_cast<Pipeline*>(data);
    (void)self;

    gchar* pad_name = gst_pad_get_name(new_pad);
    std::cout << "[ENCODER] New pad added: " << pad_name << std::endl;
    g_free(pad_name);
}
