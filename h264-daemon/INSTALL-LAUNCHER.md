# MP4 Launcher Daemon - Installation Guide

The `mp4_launcher` daemon monitors OSD trigger buttons and automatically launches `mp4_play` when you press buttons in the Groovy core menu.

## Features

- ✅ Launch videos from OSD buttons (no SSH needed!)
- ✅ Play default video, random video, or stop playback
- ✅ **Auto-overclock:** CPU 800MHz → 1000MHz during playback for smoother video
- ✅ Auto-restore stock frequency when video stops
- ✅ Minimal CPU usage (0.1% - just polls a register every 100ms)
- ✅ Auto-starts on MiSTer boot
- ✅ **Safety:** Auto-detects core changes and exits to prevent system hangs

## ⚠️ Overclock Warning

The launcher **automatically overclocks the CPU to 1000 MHz** during video playback for better H.264 decode performance. This helps reduce frame drops on complex videos.

**Requirements:**
- ✅ **Active cooling (heatsink + fan) REQUIRED**
- ✅ Stable power supply (5V 2A minimum)

**To disable overclocking:** Edit `mp4_launcher.sh` on MiSTer and change:
```bash
OVERCLOCK_FREQ=800000    # Disable overclock (use stock 800 MHz)
```

The daemon automatically restores stock 800 MHz frequency when video stops or daemon exits.

## Build

The launcher is now a bash script (no compilation needed!). You only need to build `mp4_play`:

```bash
cd h264-daemon
make clean
make
```

## Installation

**1. Copy binaries and scripts to MiSTer:**
```bash
scp mp4_play mp4_launcher.sh root@your-mister:/media/fat/linux/
ssh root@your-mister "chmod +x /media/fat/linux/mp4_launcher.sh"
```

**2. Create video directory on MiSTer:**
```bash
ssh root@your-mister
mkdir -p /media/fat/videos
```

**3. Copy a test video (optional):**
```bash
scp demo.mp4 root@your-mister:/media/fat/videos/
```

**4. Set up auto-start:**

On MiSTer, edit `/media/fat/linux/user-startup.sh`:
```bash
nano /media/fat/linux/user-startup.sh
```

Add this line:
```bash
# MP4 Launcher Daemon
/media/fat/linux/mp4_launcher.sh > /tmp/mp4_launcher.log 2>&1 &
```

**5. Reboot MiSTer or start manually:**
```bash
# Start manually:
/media/fat/linux/mp4_launcher.sh &

# Or reboot:
reboot
```

## Usage

**In Groovy core OSD (after FPGA recompile with trigger buttons):**

1. Press F12 to open OSD
2. Press "Play Video" → plays `/media/fat/videos/demo.mp4`
3. Press "Stop Video" → stops playback
4. Press "Play Random" → picks random .mp4/.mkv/.mov from `/media/fat/videos/`

## Customization

Edit `mp4_launcher.sh` directly on MiSTer (no recompilation needed!):
```bash
nano /media/fat/linux/mp4_launcher.sh
```

Change these variables at the top:
- **Default video path:** `DEFAULT_VIDEO="/media/fat/videos/demo.mp4"`
- **Video directory:** `VIDEO_DIR="/media/fat/videos"`
- **Thread count:** `THREADS=2`
- **Disable overclock:** `OVERCLOCK_FREQ=800000`

Save and restart the daemon:
```bash
killall mp4_launcher.sh
/media/fat/linux/mp4_launcher.sh &
```

## Core Safety

**IMPORTANT:** The launcher only works with the **Groovy core**. Reading FPGA registers from other cores can cause system hangs!

The launcher includes automatic safety checks:
- **On startup:** Verifies Groovy core is loaded (exits if not)
- **During operation:** Checks every 10 seconds and exits if core changes
- **Safe behavior:** Stops playback and restores stock CPU frequency on exit

If you see this warning, it means you loaded a different core:
```
[mp4_launcher] WARNING: Wrong core loaded (NES) - exiting to prevent system hang
```

**To use the launcher again:** Load the Groovy core, then restart the daemon.

## OSD Trigger Buttons (need to add to Groovy.sv)

Add these to CONF_STR in Groovy.sv:
```systemverilog
"T0,Play Video;",
"T1,Stop Video;",
"T2,Play Random;",
```

Status bits:
- `status[0]` = T0 trigger (Play default)
- `status[1]` = T1 trigger (Stop)
- `status[2]` = T2 trigger (Random)

## Troubleshooting

**Check if daemon is running:**
```bash
ps aux | grep mp4_launcher
```

**View daemon log:**
```bash
cat /tmp/mp4_launcher.log
```

**Test manually:**
```bash
# Kill daemon
killall mp4_launcher.sh

# Run in foreground to see debug output
/media/fat/linux/mp4_launcher.sh
```

**Check video playback log:**
```bash
tail -f /tmp/mp4_play.log
```

## Video Organization

Organize your videos however you like in `/media/fat/videos/`:

```
/media/fat/videos/
├── demo.mp4              ← Default video
├── movies/
│   ├── film1.mkv
│   └── film2.mp4
└── shows/
    ├── episode1.mp4
    └── episode2.mkv
```

The "Play Random" button searches all subdirectories.
