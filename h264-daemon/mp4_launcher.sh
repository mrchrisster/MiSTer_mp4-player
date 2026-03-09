#!/bin/bash
# mp4_launcher.sh - Launch mp4_play from OSD triggers
# Polls FPGA status register and launches mp4_play when OSD button pressed
#
# Install: Copy to /media/fat/linux/
# Run: Add to /media/fat/linux/user-startup.sh:
#      /media/fat/linux/mp4_launcher.sh &

# Configuration (edit these as needed)
AXI_STATUS="0xFF200000"           # FPGA status register
DEFAULT_VIDEO="/media/fat/videos/demo.mp4"
VIDEO_DIR="/media/fat/videos"
MP4_PLAY="/media/fat/linux/mp4_play"
THREADS=2                          # Decoder threads (1-2)

# CPU frequency control
CPUFREQ="/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
STOCK_FREQ=800000                  # 800 MHz (stock DE10-nano)
OVERCLOCK_FREQ=1000000             # 1000 MHz (safe with active cooling)

# WARNING: Requires heatsink + fan for 1000 MHz operation
# To disable overclock: Set OVERCLOCK_FREQ=800000

echo "[mp4_launcher] Starting MP4 launcher daemon..."

# Safety check: Only run when Groovy core is loaded
# Reading AXI registers from other cores can cause system hangs!
check_core() {
    if [ -f /tmp/CORENAME ]; then
        local corename=$(cat /tmp/CORENAME 2>/dev/null)
        if [ "$corename" != "Groovy" ]; then
            echo "[mp4_launcher] WARNING: Wrong core loaded ($corename) - exiting to prevent system hang"
            echo "[mp4_launcher] This launcher only works with Groovy core!"
            return 1
        fi
    else
        echo "[mp4_launcher] WARNING: /tmp/CORENAME not found - cannot verify core"
        echo "[mp4_launcher] Proceeding anyway (risky!)"
    fi
    return 0
}

# Initial core check
if ! check_core; then
    echo "[mp4_launcher] Waiting for Groovy core..."
fi

echo "[mp4_launcher] Groovy core detected - safe to proceed"
echo "[mp4_launcher] Watching for OSD triggers..."
echo "[mp4_launcher]   - FC2: Select video from OSD file browser (Load Video)"
echo "[mp4_launcher]   - T0: Play default video ($DEFAULT_VIDEO)"
echo "[mp4_launcher]   - T1: Stop playback"
echo "[mp4_launcher]   - T2: Play random video from $VIDEO_DIR"

# Set CPU frequency
set_cpu_freq() {
    local freq=$1
    if [ -w "$CPUFREQ" ]; then
        echo $freq > "$CPUFREQ"
        echo "[mp4_launcher] CPU frequency set to $freq kHz ($((freq/1000)) MHz)"
    else
        echo "[mp4_launcher] WARNING: Cannot write to $CPUFREQ (need root or cpufreq driver)"
    fi
}

# Stop video playback and restore stock frequency
stop_video() {
    echo "[mp4_launcher] Stopping playback..."
    killall -9 mp4_play 2>/dev/null
    sleep 0.1
    set_cpu_freq $STOCK_FREQ
}

# Launch mp4_play with overclock
launch_video() {
    local video_path="$1"

    # Stop any existing playback first
    stop_video

    # Overclock CPU for better H.264 decode performance
    echo "[mp4_launcher] Overclocking CPU for video playback..."
    set_cpu_freq $OVERCLOCK_FREQ
    sleep 0.1

    # Launch new instance
    echo "[mp4_launcher] Launching: $video_path"
    "$MP4_PLAY" "$video_path" -t $THREADS > /tmp/mp4_play.log 2>&1 &
}

# Get random video from directory
get_random_video() {
    find "$VIDEO_DIR" -type f \( -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.mov' -o -iname '*.m4v' \) 2>/dev/null | shuf -n 1
}

# Get selected video file from MiSTer's open file descriptors
get_selected_file() {
    local main_pid=$(pidof MiSTer)
    if [ -z "$main_pid" ]; then
        echo ""
        return
    fi

    # Find video file in Main_MiSTer's open file descriptors
    ls -l /proc/$main_pid/fd/ 2>/dev/null | grep "/media/fat/" | grep -E "\.(mp4|mkv|mov|m4v)$" | awk '{print $NF}' | head -1
}

# Cleanup on exit
cleanup() {
    echo "[mp4_launcher] Shutting down..."
    stop_video
    echo "[mp4_launcher] Exited cleanly."
    exit 0
}

trap cleanup SIGINT SIGTERM

# Main polling loop
prev_status=0
poll_count=0

while true; do
    # Check if core changed (to prevent system hangs from reading bad AXI)
    if ! check_core; then
        echo "[mp4_launcher] Non-Groovy core detected. Sleeping..."
        stop_video
        
        # Sleep until Groovy comes back
        while ! check_core; do
            sleep 2
        done
        echo "[mp4_launcher] Groovy core restored. Resuming polling."
        prev_status=0  # Reset status tracking
    fi

    # Read status register (returns hex like "0x00000003")
    status=$(devmem $AXI_STATUS 2>/dev/null)

    # Convert hex to decimal
    status=$((status))

    # Detect rising edges on trigger bits
    rising=$(( status & ~prev_status ))

    # T0: Play default video
    if (( rising & 0x1 )); then
        launch_video "$DEFAULT_VIDEO"
    fi

    # T1: Stop playback
    if (( rising & 0x2 )); then
        stop_video
    fi

    # T2: Play random video
    if (( rising & 0x4 )); then
        random_video=$(get_random_video)
        if [ -n "$random_video" ]; then
            launch_video "$random_video"
        else
            echo "[mp4_launcher] No videos found in $VIDEO_DIR, using default"
            launch_video "$DEFAULT_VIDEO"
        fi
    fi

    # Bit 5: OSD file selection (user selected video via FC2)
    if (( rising & 0x20 )); then
        selected_file=$(get_selected_file)
        if [ -n "$selected_file" ]; then
            echo "[mp4_launcher] File selected from OSD: $selected_file"
            launch_video "$selected_file"
        else
            echo "[mp4_launcher] File selection detected but couldn't find file, using default"
            launch_video "$DEFAULT_VIDEO"
        fi
    fi

    prev_status=$status
    sleep 0.1  # Poll every 100ms (0.1% CPU usage)
done
