#!/bin/bash
# =============================================================================
# Start RTSP Re-Encoder + go2rtc
# Run this on the Jetson Orin NX
# =============================================================================

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

CONFIG="${PROJECT_DIR}/config.yaml"
ENCODER="${PROJECT_DIR}/build/rtsp_encoder"
GO2RTC_CONFIG="${PROJECT_DIR}/go2rtc.yaml"

echo "=== Starting RTSP Re-Encoder ==="

# Check encoder binary
if [ ! -f "$ENCODER" ]; then
    echo "ERROR: Encoder not found. Build first:"
    echo "  cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

# Start encoder in background
echo "[1/2] Starting encoder..."
$ENCODER -c "$CONFIG" &
ENCODER_PID=$!
echo "  PID: $ENCODER_PID"

# Wait for RTSP server to be ready
sleep 3

# Start go2rtc
echo "[2/2] Starting go2rtc..."
if command -v go2rtc &>/dev/null; then
    go2rtc -c "$GO2RTC_CONFIG" &
    GO2RTC_PID=$!
    echo "  PID: $GO2RTC_PID"
elif [ -f "/usr/local/bin/go2rtc" ]; then
    /usr/local/bin/go2rtc -c "$GO2RTC_CONFIG" &
    GO2RTC_PID=$!
    echo "  PID: $GO2RTC_PID"
else
    echo "WARNING: go2rtc not found. Install it or start manually."
    GO2RTC_PID=""
fi

echo ""
echo "=== RUNNING ==="
echo "  WebRTC:  http://$(hostname -I | awk '{print $1}'):1984"
echo "  RTSP:    rtsp://localhost:9554/stream"
echo ""
echo "Press Ctrl+C to stop"

# Trap Ctrl+C to cleanup
cleanup() {
    echo ""
    echo "Stopping..."
    [ -n "$GO2RTC_PID" ] && kill $GO2RTC_PID 2>/dev/null
    kill $ENCODER_PID 2>/dev/null
    wait
    echo "Stopped."
}
trap cleanup INT TERM

# Wait for either to exit
wait
