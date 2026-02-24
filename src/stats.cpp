#include "stats.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

void Stats::reset() {
    frame_count_.store(0);
    start_time_ = Clock::now();
    last_frame_time_ns_.store(0);
    last_fps_frame_count_.store(0);
    last_fps_time_ns_.store(0);
}

void Stats::on_frame_encoded() {
    frame_count_.fetch_add(1);
    auto now = Clock::now().time_since_epoch().count();
    last_frame_time_ns_.store(now);
}

void Stats::on_reconnect() {
    reconnect_count_.fetch_add(1);
}

void Stats::on_pipeline_restart() {
    restart_count_.fetch_add(1);
}

double Stats::seconds_since_last_frame() const {
    int64_t last = last_frame_time_ns_.load();
    if (last == 0) {
        // No frame received yet — return time since start
        auto now = Clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_).count();
        return elapsed;
    }
    auto now_ns = Clock::now().time_since_epoch().count();
    return static_cast<double>(now_ns - last) / 1e9;
}

std::string Stats::get_uptime_string() const {
    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

    int hours = static_cast<int>(elapsed / 3600);
    int minutes = static_cast<int>((elapsed % 3600) / 60);
    int seconds = static_cast<int>(elapsed % 60);

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":"
        << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

void Stats::print() const {
    uint64_t current_frames = frame_count_.load();
    auto now_ns = Clock::now().time_since_epoch().count();

    // Calculate FPS over the stats interval
    uint64_t prev_frames = last_fps_frame_count_.load();
    int64_t prev_time = last_fps_time_ns_.load();

    double fps = 0.0;
    if (prev_time > 0) {
        double dt = static_cast<double>(now_ns - prev_time) / 1e9;
        if (dt > 0.0) {
            fps = static_cast<double>(current_frames - prev_frames) / dt;
        }
    }

    // Update for next interval
    last_fps_frame_count_.store(current_frames);
    last_fps_time_ns_.store(now_ns);

    double since_last = seconds_since_last_frame();

    std::cout << "[STATS] uptime=" << get_uptime_string()
              << " | frames=" << current_frames
              << " | fps=" << std::fixed << std::setprecision(1) << fps
              << " | last_frame=" << std::fixed << std::setprecision(1) << since_last << "s ago"
              << " | reconnects=" << reconnect_count_.load()
              << " | restarts=" << restart_count_.load()
              << std::endl;
}
