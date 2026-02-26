# ARM DMA Architecture — Developer Reference

## Overview

The ARM daemon (`h264-daemon/main.cpp`) decodes MP4 video, scales the raw YUV420P planes,
and memcpy's them to uncached DDR3. The FPGA reads those planes via the `fpga2sdram` Avalon
port, converts them to RBG565 in a hardware pipeline, and writes the result to a double
framebuffer. The ASCAL scaler reads from the active framebuffer buffer and outputs to HDMI.

```
ARM (Cortex-A9):
  FFmpeg decode → YUV420P frame
        │
        │ nearest-neighbour scale to 640×480
        │ memcpy 460 KB to DDR3 @ 0x3012C000 (O_SYNC, no cache)
        │ write rgb_base register (back buffer address)
        │ write dma_trigger bit
        │ poll dma_done bit (sticky latch)
        │                            │
        ▼                            │
  DDR3:                    FPGA:     │
  Y @ 0x3012C000 ──────► yuv_fb_dma (Avalon MM master)
  U @ 0x30177000           │ burst read Y/U/V via fpga2sdram RAM1
  V @ 0x30189C00           │ nearest-neighbour chroma upsampling
                            │ yuv_to_rgb (4-stage BT.601 pipeline)
  RGB A @ 0x30000000 ◄────── burst write RBG565
  RGB B @ 0x30096000        │ assert done pulse → dma_done_latch
        │                   │
        │  ASCAL reads FB_BASE (buf_sel selects A or B)
        ▼
      HDMI output
```

---

## Architectural Context (DE10-Nano)

The memory and register access methods described here are specific implementations of the standard DE10-Nano HPS-FPGA communication architecture.

*   The AXI register block at `0xFF200000` is accessed via the **Lightweight HPS-to-FPGA Bridge**. This bridge must be enabled in the Linux Device Tree to make it visible to the OS.
*   The YUV frame data, residing in DDR3 memory starting at `0x3012C000`, is read by a custom-built **Avalon Host (Master)** on the FPGA (`yuv_fb_dma`). This master component uses the dedicated `FPGA-to-SDRAM` interface for high-speed access, implementing the Avalon-MM Burst protocol for efficient data transfer.

For a more general overview of this architecture, please refer to the `memory_addressation_summary.md` document.

---

## DDR3 Memory Map

| Region | Physical address | Size | Notes |
|---|---|---|---|
| RGB Buffer A | `0x30000000` | 614,400 bytes | 640×480×2, front or back |
| RGB Buffer B | `0x30096000` | 614,400 bytes | 640×480×2, front or back |
| YUV Y plane  | `0x3012C000` | 307,200 bytes | 640×480 luma |
| YUV U plane  | `0x30177000` | 76,800 bytes  | 320×240 Cb chroma |
| YUV V plane  | `0x30189C00` | 76,800 bytes  | 320×240 Cr chroma |

**Why 0x30000000?**
The DE10-nano has 1 GB DDR3 (0x00000000–0x3FFFFFFF). Linux typically occupies < 512 MB.
MiSTer's ASCAL buffer lives at 0x20000000–0x20800000, rotation buffers at
0x24000000–0x26000000. The 768 MB mark (0x30000000) is safely above all of these.
The YUV region ends at `0x30189C00 + 76800 = 0x3019C000` — well within the 1 GB limit.

---

## Pixel Format — RBG565 Big-Endian

Each pixel is 16 bits, written **big-endian** (high byte at lower DDR3 address).

**CRITICAL — the ASCAL uses RBG565, not standard RGB565. Blue and Green are swapped.**

Hardware-confirmed (Feb 2026) via exhaustive palette test:

```
Bit:  15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
       R4 R3 R2 R1 R0   B5 B4 B3 B2 B1 B0   G4 G3 G2 G1 G0
```

| Bits | Channel | Width |
|------|---------|-------|
| [15:11] | Red   | 5 bits |
| [10:5]  | Blue  | 6 bits |  ← swapped vs standard RGB565 |
| [4:0]   | Green | 5 bits |  ← swapped vs standard RGB565 |

Do **not** use `libswscale` (`AV_PIX_FMT_RGB565BE`) — it produces incorrect colors for this
format on Cortex-A9. In Phase 1.5 the FPGA handles conversion; the ARM only needs to
write raw YUV420P planes.

**Byte order verification:**
```
Write [0xF8, 0x00] → ASCAL reads 0xF800 → R=31, B=0, G=0 → RED ✓
Write [0x00, 0xF8] → ASCAL reads 0x00F8 → R=0, B=7, G=24  → wrong ✗
```

---

## AXI LW Bridge Registers

The Cyclone V H2F Lightweight AXI bridge is exposed at physical address `0xFF200000`.
The `mp4_ctrl_regs` Verilog module implements six 32-bit registers:

| ARM address | Word index | Access | Description |
|---|---|---|---|
| `0xFF200000` | `axi[0]` | Read | **Status** — bit 2 = `fb_vbl` (VBlank, CDC'd to clk_sys); bit 3 = `dma_done` (sticky latch, clears on this read) |
| `0xFF200008` | `axi[2]` | R/W | **Control** — bit 0 = `buf_sel` (0=Buffer A, 1=Buffer B); bit 1 = `dma_trigger` (write 1 to start DMA, auto-clears) |
| `0xFF200010` | `axi[4]` | R/W | **YUV Y base** — DDR3 byte address of Y plane (default `0x3012C000`) |
| `0xFF200014` | `axi[5]` | R/W | **YUV U base** — DDR3 byte address of U plane (default `0x30177000`) |
| `0xFF200018` | `axi[6]` | R/W | **YUV V base** — DDR3 byte address of V plane (default `0x30189C00`) |
| `0xFF20001C` | `axi[7]` | R/W | **RGB base** — DDR3 byte address of RGB output (updated per-frame to back buffer) |

**Note:** Array index N = byte offset N×4. `axi[2]` = byte offset `0x8`, `axi[4]` = `0x10`, etc.

**`dma_done` sticky latch:** The FPGA's `done` pulse is ~10 ns at 100 MHz. The ARM's AXI
polling loop runs at ~100 ns granularity. Without latching the ARM would never catch the pulse.
The `dma_done_latch` register is set by the FPGA pulse and cleared automatically by the ARM's
read of `axi[0]` — no explicit clear write needed.

---

## mmap Setup

```cpp
#define FB_PHYS   0x30000000UL
#define FB_W      640
#define FB_H      480
#define FB_SIZE   (FB_W * FB_H * 2)                          // 614400 bytes per buffer
#define FB_TOTAL  (FB_SIZE * 2)                               // 1228800 bytes total
#define YUV_Y_PHYS   (FB_PHYS + FB_TOTAL)                    // 0x3012C000
#define YUV_Y_SIZE   (FB_W * FB_H)                           // 307200
#define YUV_U_SIZE   ((FB_W/2) * (FB_H/2))                   // 76800
#define YUV_V_SIZE   ((FB_W/2) * (FB_H/2))                   // 76800
#define YUV_TOTAL    (YUV_Y_SIZE + YUV_U_SIZE + YUV_V_SIZE)  // 460800
#define AXI_PHYS  0xFF200000UL
#define AXI_SIZE  4096

int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);

// Both RGB framebuffers — contiguous
void* fb_all = mmap(NULL, FB_TOTAL, PROT_READ | PROT_WRITE,
                    MAP_SHARED, mem_fd, FB_PHYS);
uint8_t* fb_buf[2] = {
    (uint8_t*)fb_all,            // Buffer A: 0x30000000
    (uint8_t*)fb_all + FB_SIZE   // Buffer B: 0x30096000
};

// YUV planes — written by ARM, read by FPGA DMA
void* yuv_map = mmap(NULL, YUV_TOTAL, PROT_READ | PROT_WRITE,
                     MAP_SHARED, mem_fd, YUV_Y_PHYS);
uint8_t* yuv_y = (uint8_t*)yuv_map;
uint8_t* yuv_u = yuv_y + YUV_Y_SIZE;
uint8_t* yuv_v = yuv_u + YUV_U_SIZE;

// AXI control registers
void* axi_map = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE,
                     MAP_SHARED, mem_fd, AXI_PHYS);
volatile uint32_t* axi = (volatile uint32_t*)axi_map;

// One-time register initialization:
axi[2] = 0;                                          // buf_sel = Buffer A
axi[4] = (uint32_t)YUV_Y_PHYS;                      // Y base
axi[5] = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE;         // U base
axi[6] = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE + YUV_U_SIZE; // V base
axi[7] = (uint32_t)FB_PHYS;                          // initial RGB base = Buffer A
```

---

## FFmpeg Decode Loop (Phase 1.5)

```cpp
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// Scale YUV420P planes, write to DDR3, trigger FPGA DMA, poll done
static void write_yuv_and_dma(const AVFrame* f,
                               uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                               uint32_t rgb_back_phys,
                               volatile uint32_t* axi)
{
    // 1. Nearest-neighbour scale Y → 640×480
    static uint8_t tmp_y[640 * 480];
    for (int dy = 0; dy < 480; dy++) {
        int sy = dy * f->height / 480;
        for (int dx = 0; dx < 640; dx++)
            tmp_y[dy*640 + dx] = f->data[0][sy*f->linesize[0] + dx*f->width/640];
    }
    memcpy(yuv_y, tmp_y, 640*480);

    // 2. Nearest-neighbour scale U/V → 320×240
    static uint8_t tmp_u[320*240], tmp_v[320*240];
    for (int dy = 0; dy < 240; dy++) {
        int sy = dy * (f->height/2) / 240;
        for (int dx = 0; dx < 320; dx++) {
            int sx = dx * (f->width/2) / 320;
            tmp_u[dy*320 + dx] = f->data[1][sy*f->linesize[1] + sx];
            tmp_v[dy*320 + dx] = f->data[2][sy*f->linesize[2] + sx];
        }
    }
    memcpy(yuv_u, tmp_u, 320*240);
    memcpy(yuv_v, tmp_v, 320*240);

    // 3. Tell FPGA where to write, trigger DMA
    axi[7] = rgb_back_phys;                          // RGB output base
    axi[2] = (axi[2] & 1u) | (1u << 1);             // dma_trigger=1, preserve buf_sel

    // 4. Poll dma_done (sticky latch — cleared by this read automatically)
    while (!(axi[0] & (1u << 3))) { /* spin */ }
}

static void play_video(const char* path,
                       volatile uint32_t* axi,
                       uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                       bool benchmark, int threads, double seek_s)
{
    int front = 0, back = 1;

    AVFormatContext* fmt = NULL;
    avformat_open_input(&fmt, path, NULL, NULL);
    avformat_find_stream_info(fmt, NULL);

    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            { vstream = i; break; }

    AVCodecParameters* par  = fmt->streams[vstream]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext*    dec   = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    if (threads > 0) dec->thread_count = threads;
    avcodec_open2(dec, codec, NULL);

    if (seek_s > 0.0)
        av_seek_frame(fmt, -1, (int64_t)(seek_s * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

    AVPacket*  pkt   = av_packet_alloc();
    AVFrame*   frame = av_frame_alloc();
    AVRational tb    = fmt->streams[vstream]->time_base;

    int64_t start_wall_us = 0;
    double  start_pts_s   = 0.0;
    bool    clk_init      = false;
    double  prev_pts_s    = -1.0;
    double  frame_period_s = 1.0 / 30.0;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(dec, pkt);

        while (avcodec_receive_frame(dec, frame) == 0) {
            if (frame->pts == AV_NOPTS_VALUE) continue;
            const double frame_pts_s = frame->pts * av_q2d(tb);

            // A. Init master clock
            if (!clk_init) {
                start_wall_us = now_us();
                start_pts_s   = frame_pts_s;
                clk_init      = true;
            }

            // B. Update frame period from PTS delta
            if (prev_pts_s >= 0.0) {
                const double dp = frame_pts_s - prev_pts_s;
                if (dp > 0.001 && dp < 0.2)
                    frame_period_s = dp;
            }
            prev_pts_s = frame_pts_s;

            // C. Compute target display time
            const int64_t target_us = start_wall_us
                                    + (int64_t)((frame_pts_s - start_pts_s) * 1e6);

            // D. Drop if more than half a frame period late
            if (!benchmark) {
                const int64_t drop_thresh_us = (int64_t)(frame_period_s * 0.5e6);
                if (now_us() > target_us + drop_thresh_us) continue;
            }

            // E. Scale YUV, memcpy to DDR3, trigger FPGA DMA, poll done
            {
                const uint32_t rgb_back_phys = 0x30000000UL + (uint32_t)back * (640*480*2);
                write_yuv_and_dma(frame, yuv_y, yuv_u, yuv_v, rgb_back_phys, axi);
            }

            // F. Sleep until 1 ms before target
            if (!benchmark) {
                const long to_sleep = (long)(target_us - now_us()) - 1000;
                if (to_sleep > 500) {
                    struct timespec ts = { to_sleep/1000000, (to_sleep%1000000)*1000 };
                    nanosleep(&ts, NULL);
                }

                // G. Wait for VBlank
                while (!(axi[0] & (1u << 2))) { /* spin */ }
            }

            // H. Page flip
            axi[2] = (uint32_t)back;         // sets buf_sel, dma_trigger=0

            // I. Swap front/back
            { int tmp = front; front = back; back = tmp; }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}
```

---

## Build (Cross-Compile from x86 Linux)

**Tested environment:** Ubuntu 20.04 WSL, `arm-linux-gnueabihf-g++` toolchain,
FFmpeg static libraries at `/opt/ffmpeg-arm`.

```bash
arm-linux-gnueabihf-g++ -O2 -o mp4_play main.cpp \
    -I/opt/ffmpeg-arm/include \
    -L/opt/ffmpeg-arm/lib \
    -lavformat -lavcodec -lavutil -lswresample \
    -lz -lm -lpthread -static
```

**Key flags:**
- `-lz` — required: libavformat's matroska and MOV demuxers link against zlib
- `-lswresample` — required by libavcodec even when audio is not decoded
- `-static` — self-contained ARM32 ELF; no library dependencies on MiSTer Linux

Result: ~11 MB static binary. Deploy to `/media/fat/mp4_play`.

**Native build on DE10-nano:**
```bash
g++ -O2 -o mp4_play main.cpp \
    -lavformat -lavcodec -lavutil -lswresample -lz -lm -lpthread
```

---

## Hardware Smoke Tests

### Test 1 — Framebuffer pixel path

1. Enable MP4 mode in the MiSTer OSD (**Video Mode → MP4**).
2. Fill Buffer A with solid red (`0xF800` big-endian = `[0xF8, 0x00]`):

```bash
python3 -c "
import mmap, os
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 640*480*2, mmap.MAP_SHARED,
              mmap.PROT_READ | mmap.PROT_WRITE, offset=0x30000000)
m.seek(0); m.write(b'\xF8\x00' * 640 * 480)
m.close(); os.close(fd)"
```

Expected: solid red HDMI output.

### Test 2 — FPGA DMA (devmem)

```bash
# Confirm AXI register defaults (should already be correct after bitstream load):
devmem 0xFF200010  # Y base → should read 0x3012C000
devmem 0xFF200014  # U base → should read 0x30177000
devmem 0xFF200018  # V base → should read 0x30189C00

# Trigger DMA (YUV planes must already be written; defaults are garbage → black or noise):
devmem 0xFF20001C 32 0x30000000   # RGB output = Buffer A
devmem 0xFF200008 32 0x2           # dma_trigger = 1

# Poll dma_done (bit 3):
python3 -c "
import mmap, os, struct, time
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 4096, mmap.MAP_SHARED,
              mmap.PROT_READ | mmap.PROT_WRITE, offset=0xFF200000)
for _ in range(20):
    status = struct.unpack('<I', m[0:4])[0]
    print(f'status=0x{status:08X}  dma_done={(status>>3)&1}  vbl={(status>>2)&1}')
    time.sleep(0.05)
m.close(); os.close(fd)"
```

Expected: `dma_done` bit goes to 1 once, then returns to 0 (sticky latch cleared by each read).

### Test 3 — VBlank pulse

Run the same Python poll loop above; the `vbl` bit should toggle roughly every 16 ms (60 Hz).

### Test 4 — Full playback

```bash
mp4_play /media/fat/videos/test.mp4
```

Watch stderr — `YUV+DMA avg` should be 4–8 ms/frame. If > 15 ms, investigate DDR3 contention
or Avalon backpressure.

---

## Frame Pacing & VSync

**Master-clock absolute-target approach** (no cumulative drift):
- `start_wall_us` and `start_pts_s` captured on first decoded frame.
- Each frame: `target_us = start_wall_us + (frame_pts - start_pts) * 1e6`.
- Errors from `nanosleep` inaccuracy do not compound.

**Frame drop threshold:** half a frame period late (typically ~16 ms / 2 = 8 ms).

**VSync integration:**
- ARM sleeps to 1 ms before `target_us`, then spins on VBlank bit (`axi[0] & 4`).
- `fb_vbl` from ASCAL is synchronized to `clk_sys` via a 2-FF CDC register in `sys_top.v`.
- Page flip (`axi[2] = back`) occurs within one AXI transaction of VBlank assertion — effectively tearing-free.

---

## Known Limitations

| Limitation | Impact | Future fix |
|---|---|---|
| Hardcoded 640×480 | All video scaled to 640×480 regardless of source resolution | Phase 4: variable resolution |
| Nearest-neighbour ARM scale | Blocky on non-640×480 sources | Future: bilinear scale |
| Manual OSD toggle | User must enable "MP4 mode" before daemon | Phase 3: auto-trigger |
| No audio | Audio stream ignored | Phase 5: AUDIO_L/R via Groovy sound module |
| Groovy emu DDRAM stalled | Groovy's native video decode disabled while daemon runs | Phase 4: arbiter |
| No hardware VPU | ARM software decode limits to ~480p @ 30fps | Hardware limitation |
| Single YUV buffer | ARM may overwrite planes while FPGA DMA is in flight if poll is skipped | Current: poll prevents this; no double-buffering of YUV |
