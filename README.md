# RTSP Re-Encoder for WebRTC (Jetson Orin NX)

Low-latency RTSP re-encoder using NVIDIA NVENC hardware acceleration, designed for the **Indosat 5G AI-RAN Demo** — streaming robot dog video from Kayoon Surabaya to Barcelona via WebRTC.

## Architecture

```
Robot Dog Camera (RTSP, high bitrate)
  → C++ Encoder (Jetson NVENC, CBR 2 Mbps)
  → Local RTSP Server (:8554/stream)
  → go2rtc (WebRTC + STUN/TURN)
  → Browser in Barcelona
```

## Why Re-Encode?

Direct RTSP over 5G is unreliable — the robot dog outputs uncontrolled bitrate that exceeds 5G capacity, causing choppy video. This encoder:

- **Clamps bitrate** to a strict 2 Mbps CBR (no spikes)
- **Uses VBV buffer** to ensure frame-level compliance
- **Hardware accelerated** via Jetson NVENC (~5ms encode latency)
- **Auto-reconnects** with exponential backoff on RTSP failure
- **Watchdog** restarts pipeline if frames stop flowing

## Quick Start (Jetson Orin NX)

```bash
# 1. Clone and setup
git clone <repo-url>
cd webrtc-server
chmod +x scripts/setup_jetson.sh
sudo ./scripts/setup_jetson.sh

# 2. Edit config (optional)
nano config.yaml    # Adjust RTSP URL, bitrate, etc.
nano go2rtc.yaml    # Add TURN server for Barcelona

# 3. Run
./build/rtsp_encoder --config config.yaml &
go2rtc -config go2rtc.yaml &

# 4. Open browser
# http://<jetson-ip>:1984
```

## Configuration

### `config.yaml` — Encoder Settings

| Parameter                       | Default                         | Description                 |
| ------------------------------- | ------------------------------- | --------------------------- |
| `rtsp.url`                      | `rtsp://192.168.1.120:554/test` | Robot dog camera URL        |
| `rtsp.transport`                | `tcp`                           | TCP for reliability over 5G |
| `encoder.max_bitrate_kbps`      | `2000`                          | Max bitrate (2 Mbps)        |
| `encoder.target_bitrate_kbps`   | `1800`                          | Target bitrate              |
| `encoder.width` × `height`      | `1280×720`                      | Output resolution           |
| `encoder.framerate`             | `30`                            | Target FPS                  |
| `encoder.preset`                | `UltraLowLatency`               | Encoder preset              |
| `encoder.control_rate`          | `cbr`                           | CBR for 5G reliability      |
| `resilience.watchdog_timeout_s` | `10`                            | Auto-restart threshold      |

### `go2rtc.yaml` — WebRTC Settings

Add a TURN server for Surabaya → Barcelona NAT traversal:

```yaml
webrtc:
  ice_servers:
    - urls: [stun:stun.l.google.com:19302]
    - urls: [turn:your-turn-server:3478]
      username: "user"
      credential: "pass"
```

## Systemd Services

```bash
# Start
sudo systemctl start rtsp-encoder
sudo systemctl start go2rtc

# Enable on boot
sudo systemctl enable rtsp-encoder go2rtc

# Monitor
journalctl -f -u rtsp-encoder
journalctl -f -u go2rtc
```

## Monitoring

The encoder prints periodic stats:

```
[STATS] uptime=00:15:32 | frames=27960 | fps=30.0 | last_frame=0.0s ago | reconnects=0 | restarts=0
```

## Troubleshooting

| Symptom                      | Fix                                                                |
| ---------------------------- | ------------------------------------------------------------------ |
| Choppy over 5G               | Lower `max_bitrate_kbps` (try 1500 or 1000)                        |
| High latency                 | Use `UltraLowLatency` preset, reduce `latency_ms`                  |
| Can't connect from Barcelona | Add TURN server in `go2rtc.yaml`                                   |
| No stream in go2rtc          | Check encoder is running: `curl http://localhost:1984/api/streams` |
| `nvv4l2h264enc` not found    | Run: `sudo apt install nvidia-l4t-gstreamer`                       |

## Build Requirements

- **Platform**: NVIDIA Jetson Orin NX (JetPack 5.x / 6.x)
- **Dependencies**: GStreamer 1.0 + NVIDIA plugins, yaml-cpp, cmake
- **go2rtc**: Auto-installed by setup script
