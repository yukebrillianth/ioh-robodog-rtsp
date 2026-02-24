#!/bin/bash
# =============================================================================
# Setup & Build Script for Jetson Orin NX
# Installs dependencies, builds the RTSP encoder, and installs go2rtc
# =============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "========================================="
echo "  RTSP Encoder Setup — Jetson Orin NX"
echo "========================================="
echo "  Project: ${PROJECT_DIR}"
echo ""

# ---- 1. Install system dependencies ----
echo "[1/5] Installing system dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools \
    libgstrtspserver-1.0-dev \
    libyaml-cpp-dev

# ---- 2. Verify NVIDIA GStreamer plugins ----
echo ""
echo "[2/5] Verifying NVIDIA GStreamer plugins..."
if gst-inspect-1.0 nvv4l2h264enc > /dev/null 2>&1; then
    echo "  ✓ nvv4l2h264enc found"
else
    echo "  ✗ nvv4l2h264enc NOT found — installing nvidia-l4t-gstreamer..."
    sudo apt-get install -y nvidia-l4t-gstreamer || {
        echo "  WARNING: Could not install nvidia-l4t-gstreamer."
        echo "  Make sure JetPack is properly installed."
    }
fi

if gst-inspect-1.0 nvv4l2decoder > /dev/null 2>&1; then
    echo "  ✓ nvv4l2decoder found"
else
    echo "  ✗ nvv4l2decoder NOT found"
fi

if gst-inspect-1.0 nvvidconv > /dev/null 2>&1; then
    echo "  ✓ nvvidconv found"
else
    echo "  ✗ nvvidconv NOT found"
fi

# ---- 3. Build the RTSP encoder ----
echo ""
echo "[3/5] Building RTSP encoder..."
cd "${PROJECT_DIR}"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "  ✓ Build complete: ${PROJECT_DIR}/build/rtsp_encoder"

# ---- 4. Install go2rtc ----
echo ""
echo "[4/5] Installing go2rtc..."
GO2RTC_VERSION="1.9.4"
GO2RTC_ARCH="arm64"  # Jetson Orin NX is ARM64

GO2RTC_BIN="/usr/local/bin/go2rtc"
if [ -f "${GO2RTC_BIN}" ]; then
    echo "  go2rtc already installed at ${GO2RTC_BIN}"
else
    echo "  Downloading go2rtc v${GO2RTC_VERSION} for ${GO2RTC_ARCH}..."
    wget -q "https://github.com/AlexxIT/go2rtc/releases/download/v${GO2RTC_VERSION}/go2rtc_linux_${GO2RTC_ARCH}" \
         -O /tmp/go2rtc
    sudo chmod +x /tmp/go2rtc
    sudo mv /tmp/go2rtc "${GO2RTC_BIN}"
    echo "  ✓ go2rtc installed at ${GO2RTC_BIN}"
fi

# ---- 5. Create systemd services ----
echo ""
echo "[5/5] Creating systemd services..."

# Encoder service
sudo tee /etc/systemd/system/rtsp-encoder.service > /dev/null << EOF
[Unit]
Description=RTSP Re-Encoder (NVENC)
After=network.target

[Service]
Type=simple
ExecStart=${PROJECT_DIR}/build/rtsp_encoder --config ${PROJECT_DIR}/config.yaml
WorkingDirectory=${PROJECT_DIR}
Restart=always
RestartSec=5
User=$(whoami)

# Resource limits
LimitNOFILE=65535
Nice=-10

[Install]
WantedBy=multi-user.target
EOF

# go2rtc service
sudo tee /etc/systemd/system/go2rtc.service > /dev/null << EOF
[Unit]
Description=go2rtc WebRTC Server
After=rtsp-encoder.service
Wants=rtsp-encoder.service

[Service]
Type=simple
ExecStart=${GO2RTC_BIN} -config ${PROJECT_DIR}/go2rtc.yaml
WorkingDirectory=${PROJECT_DIR}
Restart=always
RestartSec=5
User=$(whoami)

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
echo "  ✓ Services created: rtsp-encoder.service, go2rtc.service"

# ---- Done ----
echo ""
echo "========================================="
echo "  Setup Complete!"
echo "========================================="
echo ""
echo "  To start manually:"
echo "    ./build/rtsp_encoder --config config.yaml"
echo "    go2rtc -config go2rtc.yaml"
echo ""
echo "  To start as services:"
echo "    sudo systemctl start rtsp-encoder"
echo "    sudo systemctl start go2rtc"
echo ""
echo "  To enable on boot:"
echo "    sudo systemctl enable rtsp-encoder go2rtc"
echo ""
echo "  WebRTC viewer:"
echo "    http://<jetson-ip>:1984"
echo ""
echo "  Monitor encoder:"
echo "    journalctl -f -u rtsp-encoder"
echo ""
