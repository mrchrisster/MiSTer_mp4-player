# ARM DMA Architecture — Developer Reference

## Overview

The ARM daemon (`h264-daemon/main.cpp`) decodes MP4 video, scales the raw YUV420P planes,
and memcpy's them to uncached DDR3. The FPGA reads those planes via the `fpga2sdram` Avalon
port, converts them to BGR565 in a hardware pipeline, and writes the result to a double
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
  RGB A @ 0x30000000 ◄────── burst write BGR565
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

## Pixel Format — BGR565 Little-Endian

Each pixel is 16 bits, stored **little-endian** in DDR3 (low byte at lower address).

**CRITICAL — the ASCAL uses BGR565, not standard RGB565.**

Empirically confirmed via test_rgb_direct (ARM writes pixels directly to framebuffer):
- Write 0xF800 → displays as BLUE (confirms B in bits [15:11])
- Write 0x07E0 → displays as GREEN (confirms G in bits [10:5])
- Write 0x001F → displays as RED (confirms R in bits [4:0])
- Write 0xFFFF → displays as WHITE ✓

16-bit word layout:

```
Bit:  15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
      B4 B3 B2 B1 B0 | G5 G4 G3 G2 G1 G0 | R4 R3 R2 R1 R0
```

| Bits | Channel | Width |
|------|---------|-------|
| [15:11] | Blue  | 5 bits |
| [10:5]  | Green | 6 bits |
| [4:0]   | Red   | 5 bits |

**Byte order in DDR3 (little-endian):**
Both ARM and FPGA DMA must store pixels with the low byte at the lower address.
ARM `uint16_t` writes are natively little-endian. The FPGA DMA's `rgb_buf` stores
`[even] = pixel[7:0]` (low byte), `[odd] = pixel[15:8]` (high byte), and `pack_beat()`
maps `rgb_buf[base+0]` → `writedata[7:0]` → lowest DDR3 byte address. ASCAL reads
`pixel = {readdata[15:8], readdata[7:0]}` which reconstructs the correct 16-bit value.

**C packing for ARM (little-endian CPU):**
```c
uint16_t pixel = (b & 0xF8) << 8 | (g & 0xFC) << 3 | (r >> 3);
// ARM stores little-endian natively; ASCAL reads via Avalon which matches.
```

**Verilog packing for FPGA (yuv_to_rgb.sv output):**
```verilog
rgb565 <= {B8[7:3], G8[7:2], R8[7:3]};  // BGR565: B[15:11] G[10:5] R[4:0]
```

**Verilog storage in rgb_buf (yuv_fb_dma.v):**
```verilog
rgb_buf[{px, 1'b0}] <= pipe_rgb[ 7:0];   // low byte at even index
rgb_buf[{px, 1'b1}] <= pipe_rgb[15:8];   // high byte at odd index
```

Do **not** use `libswscale` (`AV_PIX_FMT_RGB565BE`) — it produces RGB565, not BGR565.
In Phase 1.5 the FPGA handles YUV→BGR565 conversion; the ARM only writes raw YUV420P planes.

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

## Decode Architecture

`play_video()` runs two concurrent threads on the dual Cortex-A9:

```
Core 1 — decoder thread          Core 0 — display thread
─────────────────────────         ──────────────────────────────────────
av_read_frame / decode            wait for frame in FrameQueue
av_frame_move_ref → queue         scale YUV → DDR3 → FPGA DMA → poll done
signal not_empty                  sleep to target_us
                                  wait VBlank (50 ms timeout)
                                  page flip
```

Wall time per frame ≈ `max(decode, scale+DMA+VBL)` instead of the sum.
With `threads=1` (default): decoder owns Core 1; display owns Core 0.

### `write_yuv_and_dma()` — key implementation points

**Scale LUTs** (rebuilt once per unique source resolution):
```cpp
uint16_t x_map[FB_W], y_map[FB_H];    // luma
uint16_t ux_map[FB_W/2], uy_map[FB_H/2]; // chroma
// x_map[dx] = dx * src_w / FB_W  — eliminates per-pixel divide
```

**Identity fast-path** (src == 640×480): each Y row is `memcpy`'d directly to the
DDR3 YUV region without a temporary buffer, saving one 307 KB copy per frame.

**DMB SY before trigger** — ensures all ARM AXI write-buffer stores to the DDR3
YUV region are fully committed before the FPGA DMA trigger write crosses the H2F
LW AXI path. Without this, the FPGA can start reading while ARM writes are still
in-flight, causing a read-after-write hazard and 2–3× DMA slowdowns:
```cpp
__asm__ volatile ("dmb sy" ::: "memory");
axi[AXI_RGB_BASE_IDX] = rgb_back_phys;
axi[AXI_CTRL_IDX] = (axi[AXI_CTRL_IDX] & 1u) | AXI_DMA_TRIG_BIT;
```

**DMA timeout** — 200 ms spin-poll guard; sets `g_stop=1` and returns if
`dma_done` bit never asserts. This is the runtime guard against a mismatched or
missing FPGA bitstream:
```cpp
while (!(axi[AXI_STATUS_IDX] & AXI_DMA_DONE_BIT)) {
    if (now_us() - dma_t0 > 200000LL) { g_stop = 1; return; }
}
```

**Decoder fast flags:**
```cpp
dec->skip_loop_filter = AVDISCARD_ALL;       // skip H.264 deblock: -15–25% decode time
dec->flags2          |= AV_CODEC_FLAG2_FAST; // non-spec fast paths: -5–10%
```

**Timing:** uses `gettimeofday` + `usleep` (GLIBC 2.0) instead of
`clock_gettime` / `nanosleep` (GLIBC 2.17) to avoid dynamic linker issues on
MiSTer's older libc.

### Display loop — key implementation points

**Drop threshold:** one full frame period (≈33.3 ms at 30 fps). Frames arriving
0–33 ms late are displayed; the brief timing slip is imperceptible. Frames more
than one period late are dropped. Using 0.5× caused unnecessary drops of frames
that were only 18–27 ms late.

**Clock reset after drop:** after each drop, `start_wall_us` and `start_pts_s`
are re-anchored to `now_us()` and the dropped frame's PTS. This prevents cascade
drops where one slow frame skews the clock and every subsequent frame appears late.

**VBlank timeout:** 50 ms — if no VBlank arrives (FB_EN not active or ASCAL not
running), playback continues without tearing protection rather than hanging.

**Page flip:**
```cpp
axi[AXI_CTRL_IDX] = (uint32_t)back;   // writes buf_sel; dma_trigger=0
{ int tmp = front; front = back; back = tmp; }
```

### Startup AXI access

`main()` only **writes** to AXI registers (init buf_sel, YUV/RGB base addresses).
The Cyclone V H2F LW AXI bridge uses posted writes — they complete immediately
from the CPU's perspective even if no FPGA slave responds. **No reads are issued
in `main()`**, so launching the app with a non-Groovy core loaded does not hang
or crash the system. The DMA timeout in `write_yuv_and_dma()` is the runtime
safety net if playback is attempted against a wrong bitstream.

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
- `-lpthread` — required: decode-ahead uses a `pthread` decoder thread
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
2. Fill Buffer A with solid blue (`0xF800` in BGR565 = `[0xF8, 0x00]` big-endian):

```bash
python3 -c "
import mmap, os
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 640*480*2, mmap.MAP_SHARED,
              mmap.PROT_READ | mmap.PROT_WRITE, offset=0x30000000)
m.seek(0); m.write(b'\xF8\x00' * 640 * 480)
m.close(); os.close(fd)"
```

Expected: solid **blue** HDMI output (0xF800 = BGR565 with B=31, G=0, R=0).

For solid **red**, use `b'\x00\x1F'` (0x001F = BGR565 with B=0, G=0, R=31).

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

**Frame drop threshold:** one full frame period late (≈33.3 ms at 30 fps).
Frames 0–33 ms late are displayed; only genuine spikes beyond one period are dropped.
After each drop the master clock is re-anchored to prevent cascade drops.

**VSync integration:**
- ARM sleeps to 1 ms before `target_us`, then spins on VBlank bit (`axi[0] & 4`).
- 50 ms VBlank timeout — playback continues without vsync if FB_EN is not active.
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
