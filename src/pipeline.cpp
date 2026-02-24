#include "pipeline.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

// ============================================================================
// Pipeline Construction
// ============================================================================

Pipeline::Pipeline(const AppConfig& config, Stats& stats)
    : config_(config), stats_(stats) {
    reconnect_delay_s_ = config_.rtsp.reconnect_delay_s;
}

Pipeline::~Pipeline() {
    stop();
}

std::string Pipeline::build_launch_string() const {
    std::ostringstream ss;

    // ---- RTSP Source ----
    // TCP transport for reliability over 5G
    // Low latency settings, short timeout for fast failure detection
    ss << "( rtspsrc location=" << config_.rtsp.url
       << " protocols=" << config_.rtsp.transport
       << " latency=" << config_.rtsp.latency_ms
       << " tcp-timeout=5000000"        // 5 sec TCP timeout (microseconds)
       << " retry=5"                     // Retry count on connection failure
       << " do-retransmission=false"     // Don't request retransmission (adds latency)
       << " drop-on-latency=true"        // Drop late packets instead of buffering
       << " ntp-sync=false"
       << " buffer-mode=auto"
       << " name=src";

    // ---- Depay & Parse ----
    ss << " src. ! rtph264depay ! h264parse";

    // ---- Hardware Decoder (Jetson NVENC) ----
    // nvv4l2decoder decodes to NVMM memory for zero-copy to encoder
    ss << " ! nvv4l2decoder"
       << " enable-max-performance=true"
       << " drop-frame-interval=0"
       << " disable-dpb=true";          // Disable decoded picture buffer for lower latency

    // ---- Video Conversion (if resolution change needed) ----
    // nvvidconv handles scaling in GPU memory (zero-copy)
    ss << " ! nvvidconv";

    // ---- Resolution / Framerate Filter ----
    ss << " ! video/x-raw(memory:NVMM)"
       << ",width=" << config_.encoder.width
       << ",height=" << config_.encoder.height
       << ",framerate=" << config_.encoder.framerate << "/1";

    // ---- Hardware Encoder (Jetson NVENC) ----
    // nvv4l2h264enc properties are set via the Encoder class in on_media_configure
    ss << " ! nvv4l2h264enc"
       << " name=encoder"
       << " bitrate=" << config_.encoder.target_bitrate_kbps * 1000
       << " peak-bitrate=" << config_.encoder.max_bitrate_kbps * 1000
       << " control-rate=1"             // CBR
       << " preset-level=2"             // UltraFast
       << " profile=4"                  // High
       << " idrinterval=" << config_.encoder.idr_interval
       << " insert-sps-pps=true"
       << " maxperf-enable=true"
       << " vbv-size=" << (config_.encoder.target_bitrate_kbps * 1000 / 30);

    // ---- Identity element for frame counting ----
    ss << " ! identity name=frame_counter signal-handoffs=false";

    // ---- Output Parse & RTP Payloader ----
    ss << " ! h264parse config-interval=-1";  // Send SPS/PPS with every IDR
    ss << " ! rtph264pay name=pay0 pt=96"
       << " config-interval=-1"          // Send SPS/PPS inline
       << " aggregate-mode=zero-latency" // Don't wait to aggregate NALUs
       << " )";

    return ss.str();
}

// ============================================================================
// RTSP Server Management
// ============================================================================

bool Pipeline::start_rtsp_server() {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);

    // Create RTSP server
    rtsp_server_ = gst_rtsp_server_new();
    if (!rtsp_server_) {
        std::cerr << "[PIPELINE] Failed to create RTSP server" << std::endl;
        return false;
    }

    // Set port
    gchar port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", config_.output.port);
    gst_rtsp_server_set_service(rtsp_server_, port_str);

    // Create factory for the stream
    factory_ = gst_rtsp_media_factory_new();
    if (!factory_) {
        std::cerr << "[PIPELINE] Failed to create RTSP media factory" << std::endl;
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
        return false;
    }

    // Set the launch pipeline
    std::string launch_str = build_launch_string();
    std::cout << "[PIPELINE] Launch string: " << launch_str << std::endl;
    gst_rtsp_media_factory_set_launch(factory_, launch_str.c_str());

    // Share the pipeline between all clients (important: single source, multiple viewers)
    gst_rtsp_media_factory_set_shared(factory_, TRUE);

    // Set low latency
    gst_rtsp_media_factory_set_latency(factory_, config_.rtsp.latency_ms);

    // Connect media-configure signal for encoder setup & frame counting
    g_signal_connect(factory_, "media-configure",
                     G_CALLBACK(Pipeline::on_media_configure), this);

    // Mount the factory
    mounts_ = gst_rtsp_server_get_mount_points(rtsp_server_);
    gst_rtsp_mount_points_add_factory(mounts_, config_.output.path.c_str(), factory_);
    g_object_unref(mounts_);
    mounts_ = nullptr;

    // Attach server to the default main context
    server_source_id_ = gst_rtsp_server_attach(rtsp_server_, NULL);
    if (server_source_id_ == 0) {
        std::cerr << "[PIPELINE] Failed to attach RTSP server" << std::endl;
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
        return false;
    }

    std::cout << "[PIPELINE] RTSP server started on port " << config_.output.port
              << ", path: " << config_.output.path << std::endl;
    std::cout << "[PIPELINE] Stream available at: rtsp://localhost:" 
              << config_.output.port << config_.output.path << std::endl;

    return true;
}

void Pipeline::stop_rtsp_server() {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);

    if (server_source_id_ > 0) {
        g_source_remove(server_source_id_);
        server_source_id_ = 0;
    }

    if (rtsp_server_) {
        g_object_unref(rtsp_server_);
        rtsp_server_ = nullptr;
    }

    factory_ = nullptr;  // Owned by mount points, already freed
    mounts_ = nullptr;
}

// ============================================================================
// Callbacks
// ============================================================================

void Pipeline::on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                   GstRTSPMedia* media,
                                   gpointer user_data) {
    Pipeline* self = static_cast<Pipeline*>(user_data);

    std::cout << "[PIPELINE] Media configured, setting up encoder and probes..." << std::endl;

    GstElement* element = gst_rtsp_media_get_element(media);
    if (!element) {
        std::cerr << "[PIPELINE] ERROR: Could not get media element" << std::endl;
        return;
    }

    // Find and configure the encoder element
    GstElement* enc = gst_bin_get_by_name(GST_BIN(element), "encoder");
    if (enc) {
        self->encoder_.configure(enc,
                                  self->config_.encoder.target_bitrate_kbps,
                                  self->config_.encoder.max_bitrate_kbps,
                                  self->config_.encoder.idr_interval,
                                  self->config_.encoder.preset,
                                  self->config_.encoder.profile,
                                  self->config_.encoder.control_rate);
        gst_object_unref(enc);
    } else {
        std::cerr << "[PIPELINE] WARNING: Could not find encoder element" << std::endl;
    }

    // Attach buffer probe to identity element for frame counting
    GstElement* counter = gst_bin_get_by_name(GST_BIN(element), "frame_counter");
    if (counter) {
        GstPad* src_pad = gst_element_get_static_pad(counter, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                              Pipeline::on_buffer_probe, user_data, NULL);
            gst_object_unref(src_pad);
        }
        gst_object_unref(counter);
    }

    gst_object_unref(element);

    // Reset stats for this new media session
    self->stats_.reset();
    self->reconnect_delay_s_ = self->config_.rtsp.reconnect_delay_s;  // Reset backoff
    std::cout << "[PIPELINE] Pipeline is PLAYING" << std::endl;
}

GstPadProbeReturn Pipeline::on_buffer_probe(GstPad* /*pad*/,
                                             GstPadProbeInfo* /*info*/,
                                             gpointer user_data) {
    Pipeline* self = static_cast<Pipeline*>(user_data);
    self->stats_.on_frame_encoded();
    return GST_PAD_PROBE_OK;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool Pipeline::start() {
    if (running_.load()) {
        std::cerr << "[PIPELINE] Already running" << std::endl;
        return false;
    }

    if (!start_rtsp_server()) {
        return false;
    }

    running_.store(true);
    stats_.reset();

    std::cout << "[PIPELINE] =============================" << std::endl;
    std::cout << "[PIPELINE]  Encoder pipeline STARTED" << std::endl;
    std::cout << "[PIPELINE]  Waiting for client connection..." << std::endl;
    std::cout << "[PIPELINE] =============================" << std::endl;

    return true;
}

void Pipeline::stop() {
    if (!running_.load()) return;

    std::cout << "[PIPELINE] Stopping..." << std::endl;
    running_.store(false);
    stop_rtsp_server();
    std::cout << "[PIPELINE] Stopped" << std::endl;
}

bool Pipeline::watchdog_check() {
    if (!running_.load()) return false;

    double since_last = stats_.seconds_since_last_frame();
    uint64_t frames = stats_.frame_count();

    // Only trigger watchdog if we've been running long enough and had frames before
    // or if we've been waiting too long for the first frame
    if (since_last > static_cast<double>(config_.resilience.watchdog_timeout_s)) {
        if (frames > 0) {
            std::cerr << "[WATCHDOG] No frames for " << since_last
                      << "s (threshold: " << config_.resilience.watchdog_timeout_s
                      << "s) — triggering restart" << std::endl;
            return false;  // Unhealthy
        }
    }

    return true;  // Healthy
}

bool Pipeline::restart() {
    int max_restarts = config_.resilience.max_pipeline_restarts;
    uint32_t current_restarts = stats_.restart_count();

    if (max_restarts > 0 && static_cast<int>(current_restarts) >= max_restarts) {
        std::cerr << "[PIPELINE] Max restarts (" << max_restarts << ") reached. Giving up."
                  << std::endl;
        return false;
    }

    stats_.on_pipeline_restart();
    std::cout << "[PIPELINE] Restarting pipeline (attempt #" 
              << stats_.restart_count() << ")..." << std::endl;

    // Exponential backoff
    std::cout << "[PIPELINE] Waiting " << reconnect_delay_s_ << "s before restart..."
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_s_));

    // Increase backoff (max 30s)
    reconnect_delay_s_ = std::min(reconnect_delay_s_ * 2, 30);

    stop();
    return start();
}

void Pipeline::set_bitrate(uint32_t target_kbps, uint32_t max_kbps) {
    config_.encoder.target_bitrate_kbps = target_kbps;
    config_.encoder.max_bitrate_kbps = max_kbps;
    encoder_.set_bitrate(target_kbps, max_kbps);
}
