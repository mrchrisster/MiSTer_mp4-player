# MISSION BRIEF: Native OSD MP4 Player (Groovy_MiSTer Hybrid)

## 1. PROJECT OBJECTIVE
Create a custom MiSTer FPGA core based on the `Groovy_MiSTer` architecture that natively plays `.mp4` (H.264) video files selected from the MiSTer OSD. The user must not touch the Linux terminal during playback. Video decoding is handled by a C++ daemon running on the ARM (HPS) that decodes frames and writes raw YUV420P planes directly to DDR3. The FPGA reads those planes, performs YUV→RBG565 conversion in hardware, writes the result to a double framebuffer, and the ASCAL scaler outputs to HDMI.

---

## 2. THE "FPGA YUV DMA" ARCHITECTURE (current)

```
ARM (Cortex-A9):                   FPGA (Cyclone V):
  FFmpeg decode                      yuv_fb_dma (Avalon master)
    │                                  │ reads Y/U/V planes from DDR3
    │ memcpy YUV420P                    │ via fpga2sdram RAM1 (64-bit, 29-bit addr)
    ▼                                  │
  DDR3 YUV region                      │ feeds yuv_to_rgb pipeline
  0x3012C000–0x30212BFF               │ (4-stage BT.601, 1 px/clock)
    ▲                                  │
    │                                  │ writes RBG565 to back RGB buffer
  AXI write:                          ▼
  - set rgb_base (back buf)         DDR3 RGB buffers: A=0x30000000, B=0x30096000
  - write dma_trigger bit           │
  - poll dma_done bit               ASCAL reads FB_BASE (buf_sel selects A or B)
                                    ▼
                                   HDMI output
```

> **Architecture history:**
> 1. **ioctl-FIFO** (abandoned) — MiSTer SPI protocol at ~1–3 MB/s; far below 9 MB/s needed.
> 2. **ARM SW YUV→RGB565** (Phase 2/2.5) — ARM converted pixels in software; fast enough for
>    30fps but consumed ~80% of one Cortex-A9 core just for pixel math.
> 3. **FPGA YUV DMA** (Phase 1.5, current) — ARM memcpy's 460 KB of raw YUV planes; FPGA does
>    all pixel math using Cyclone V DSP multipliers. ARM frees ~80% of its cycle budget.

---

### A. The FPGA Side

#### ASCAL Framebuffer (`MISTER_FB=1`)
* **Mode Toggle:** `O[60],Video Mode,Core,MP4;` — `status[60]` drives `FB_EN`.
* **OSD file browser:** `FC2,MP4,Load Video;` — launches file browser for `.mp4`.
* **Fixed framebuffer parameters:**

  | Signal | Value | Meaning |
  |---|---|---|
  | `FB_BASE` | switches per `buf_sel` | Buffer A or B (see double buffer) |
  | `FB_WIDTH` | `640` | pixels per row |
  | `FB_HEIGHT` | `480` | rows |
  | `FB_FORMAT` | `5'd4` | RBG565 (16-bit) |
  | `FB_STRIDE` | `1280` | bytes per row |

#### Double Framebuffer

| Buffer | Physical address | Size |
|---|---|---|
| Buffer A | `0x30000000` | 614,400 bytes |
| Buffer B | `0x30096000` | 614,400 bytes |

`buf_sel` (AXI control register bit 0) selects which buffer ASCAL displays. The daemon writes into the back buffer while ASCAL reads the front buffer.

#### YUV Planes (written by ARM, read by FPGA DMA)

| Plane | Physical address | Size |
|---|---|---|
| Y (luma) | `0x3012C000` | 307,200 bytes (640×480) |
| U (Cb) | `0x30177000` | 76,800 bytes (320×240) |
| V (Cr) | `0x30189C00` | 76,800 bytes |

#### YUV→RBG565 Pipeline (`rtl/yuv_to_rgb.sv`)

4-stage fully-pipelined BT.601 limited-range conversion:
- **Stage 0:** register raw Y/U/V inputs
- **Stage 1:** subtract BT.601 offsets (`c=Y-16`, `d=U-128`, `e=V-128`)
- **Stage 2:** constant multiplications → Cyclone V DSP blocks (`298*c`, `409*e`, `516*d`, etc.)
- **Stage 3:** accumulate with rounding constant (`+128`), add/subtract partial products
- **Stage 4:** arithmetic shift right 8, clamp to [0,255], pack RBG565

Total latency: **4 clock cycles** from valid input to valid output, 1 pixel/clock throughput.

#### FPGA DMA Engine (`rtl/yuv_fb_dma.v`)

Avalon MM master on Cyclone V's `fpga2sdram` RAM1 port (64-bit data, 29-bit address). Processes one 640×480 frame per trigger:

1. For each of the 480 rows:
   - Even rows: burst-read 40 beats of U (320 bytes), 40 beats of V, 80 beats of Y
   - Odd rows: reuse cached U/V (YUV420 chroma sub-sampling), fetch new Y only
2. Feed Y/U/V to `yuv_to_rgb` pipeline one pixel per clock (chroma upsampled by nearest-neighbour: `u_buf[x/2]`)
3. Collect RBG565 output after 5-clock pipeline latency (4 pipeline stages + 1 registered input stage)
4. Burst-write 160 beats (1280 bytes = one complete row) to the RGB output buffer
5. Assert `done` pulse when all 480 rows are written

Burst lengths (64-bit Avalon bus, all bytes enabled):
- Y read: 80 beats × 8 bytes = 640 bytes/row
- UV read: 40 beats × 8 bytes = 320 bytes/row
- RGB write: 160 beats × 8 bytes = 1280 bytes/row

#### AXI LW Bridge Registers (`rtl/mp4_ctrl_regs.v`)

H2F Lightweight AXI bridge at physical `0xFF200000`:

| Offset | ARM index | Access | Description |
|---|---|---|---|
| `0x000` | `axi[0]` | Read | **Status** — bit 2 = `fb_vbl` (VBlank pulse, CDC'd); bit 3 = `dma_done` (sticky latch, clears on read) |
| `0x008` | `axi[2]` | R/W | **Control** — bit 0 = `buf_sel` (0=A, 1=B); bit 1 = `dma_trigger` (write 1 to start, auto-clears) |
| `0x010` | `axi[4]` | R/W | **YUV Y base** — DDR3 byte address of Y plane |
| `0x014` | `axi[5]` | R/W | **YUV U base** — DDR3 byte address of U plane |
| `0x018` | `axi[6]` | R/W | **YUV V base** — DDR3 byte address of V plane |
| `0x01C` | `axi[7]` | R/W | **RGB base** — DDR3 byte address of RGB output (set to back buffer each frame) |

`dma_done` is a **sticky latch**: set by the one-clock `done` pulse from `yuv_fb_dma` (which at 100 MHz is ~10 ns — far too short for ARM polling to catch); cleared automatically when the ARM reads the status register.

#### Groovy Emu DDRAM — Permanently Stalled

In MP4 mode the ASCAL reads directly from the framebuffer. `Groovy.sv` uses DDRAM via a `ddram` wrapper module for its native video decode path, but that output is unused. Rather than arbitrate the bus, the emu's DDRAM signals are stubbed out in `sys_top.v` (`DDRAM_BUSY=1'b1`, `DDRAM_DOUT=64'd0`, `DDRAM_DOUT_READY=1'b0`). The emu's video decode simply stalls indefinitely with no visual output.

### B. The ARM Background Daemon (`h264-daemon/main.cpp`)

* **Input:** MP4 file path as CLI argument.
* **Decode:** `libavformat` + `libavcodec` — H.264 (and any FFmpeg codec). Output format: `AV_PIX_FMT_YUV420P`.
* **Scale:** Nearest-neighbour software scale of Y/U/V planes to 640×480 / 320×240. No `libswscale` — direct index arithmetic.
* **Write:** `memcpy` scaled planes to uncached DDR3 (`O_SYNC` mmap) at 0x3012C000.
* **Trigger FPGA DMA:**
  - Write `yuv_rgb_base` register (AXI `axi[7]`) to back-buffer physical address.
  - Write `dma_trigger` bit in control register.
  - Spin-poll `dma_done` bit in status register (sticky, so never missed).
* **Frame pacing:** Master-clock PTS sync — `target_us = start_wall_us + (frame_pts - start_pts) * 1e6`. Drops frames more than half a frame period late.
* **VSync page flip:** Spin on `fb_vbl` bit → write `buf_sel` at VBlank edge.
* **Timing report** (stderr): `YUV+DMA avg` shows combined memcpy + FPGA DMA time per frame.

### C. Daemon Usage

```bash
# Enable MP4 mode in OSD: Video Mode → MP4

# Start playback:
/media/fat/mp4_play /media/fat/videos/intro.mp4

# Benchmark mode (no frame drops, no pacing — measures raw decode+DMA speed):
/media/fat/mp4_play /media/fat/videos/intro.mp4 -b

# Seek to 30 seconds in, then play:
/media/fat/mp4_play /media/fat/videos/intro.mp4 -ss 30

# Multi-threaded decode (2 threads):
/media/fat/mp4_play /media/fat/videos/intro.mp4 -t 2
```

---

## 3. TECHNICAL CONSTRAINTS & HARDWARE MAPPING

| Item | Value |
|---|---|
| Base project | `Groovy_MiSTer` |
| FPGA target | Cyclone V SE (DE10-nano) |
| ARM CPU | Cortex-A9 dual-core @ 800 MHz, no VPU |
| DDR3 total | 1 GB (0x00000000–0x3FFFFFFF) |
| Buffer A | `0x30000000` (768 MB) |
| Buffer B | `0x30096000` |
| YUV Y plane | `0x3012C000` |
| YUV U plane | `0x30177000` |
| YUV V plane | `0x30189C00` |
| Single RGB frame | 640 × 480 × 2 = 614,400 bytes |
| YUV total | 307,200 + 76,800 + 76,800 = 460,800 bytes |
| Pixel format | **RBG565 big-endian** — R[15:11] B[10:5] G[4:0], high byte first |
| FPGA macro | `MISTER_FB=1` (Groovy.qsf line 53) |
| `FB_EN` control | OSD `status[60]` |
| AXI bridge | `cyclonev_hps_interface_hps2fpga_light_weight` <br> *(See `memory_addressation_summary.md` for details on HPS-FPGA bridges)* |
| fpga2sdram port | RAM1 — 64-bit data, 29-bit address (8-byte granularity) <br> *(See `memory_addressation_summary.md` for details on FPGA-to-SDRAM access)* |

---

## 4. IMPLEMENTATION PHASES

### Phase 1: Verilog Framebuffer (COMPLETE)
- [x] Enable `MISTER_FB=1` in `Groovy.qsf`
- [x] Add `O[60],Video Mode,Core,MP4;` to `CONF_STR`
- [x] Add `ifdef MISTER_FB` assigns for FB_BASE/WIDTH/HEIGHT/FORMAT/STRIDE/FORCE_BLANK
- [x] Remove h264_ioctl_fifo and all LW AXI bridge wiring
- [x] Tie `ioctl_wait = 1'b0`
- [x] Stub `jtframe_hsize` (pre-existing missing module)

### Phase 2: C++ Daemon — Single Buffer (COMPLETE)
- [x] `mmap /dev/mem` at 0x30000000 — verified red screen smoke test
- [x] FFmpeg file open — AVFormatContext, stream detection, codec open
- [x] Frame decode loop — `av_read_frame` → `avcodec_send_packet` → `avcodec_receive_frame`
- [x] Direct YUV420P → RBG565BE pixel conversion (hand-written, BT.601, no libswscale)
- [x] Frame pacing — master-clock PTS sync with frame drop on late frames

### Phase 2.5: Double Buffering & VSync (COMPLETE)
- [x] `rtl/mp4_ctrl_regs.v` — AXI3 LW H2F slave (VBlank status + buf_sel control)
- [x] `sys/sys_top.v` — `cyclonev_hps_interface_hps2fpga_light_weight` primitive, 2-FF CDC for `hdmi_vbl`, `fb_base_sel` mux
- [x] `files.qip` — `rtl/mp4_ctrl_regs.v` added to build
- [x] Daemon: double buffer, VSync poll, VSync-locked page flip

### Phase 1.5: FPGA YUV→RGB Conversion (COMPLETE — awaiting hardware test)
- [x] `rtl/yuv_to_rgb.sv` — 4-stage fully-pipelined BT.601 converter, Cyclone V DSP inference
- [x] `rtl/yuv_fb_dma.v` — Avalon MM master: read YUV420P from DDR3, convert, write RBG565
- [x] `rtl/mp4_ctrl_regs.v` — extended: dma_trigger, dma_done (sticky latch), 4 YUV/RGB address regs
- [x] `sys/sys_top.v` — yuv_fb_dma instantiated on RAM1 wires; emu DDRAM stalled
- [x] `h264-daemon/main.cpp` — memcpy YUV planes, trigger FPGA DMA, poll dma_done, page flip
- [x] `files.qip` — `rtl/yuv_fb_dma.v` and `rtl/yuv_to_rgb.sv` added to Quartus build
- [x] `sim/dma_smoke_tb.sv` — Icarus Verilog testbench: all 8 beats PASS (pure-red BT.601 math verified)

### Phase 3: OSD Integration (Future)
- Daemon auto-launched when user selects an MP4 from the OSD file browser.
- Requires hooking into the MiSTer main binary's ROM-load notification.

### Phase 4: Variable Resolution (Future)
- ARM reports actual video dimensions; FPGA adjusts FB_WIDTH/HEIGHT via status register.
- yuv_fb_dma parameterized on run-time width/height.

### Phase 5: Audio (Future)
- Route decoded audio stream to AUDIO_L/R via the existing Groovy sound module.

---

## 5. HARDWARE SMOKE TESTS

### 5A — Framebuffer path (no daemon)

1. In the MiSTer OSD, toggle **Video Mode → MP4**.
2. Fill Buffer A with solid red (RBG565BE `0xF800` = bytes `0xF8 0x00`):

```bash
python3 -c "
import mmap, os
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, 640*480*2, mmap.MAP_SHARED,
              mmap.PROT_READ | mmap.PROT_WRITE, offset=0x30000000)
m.seek(0); m.write(b'\xF8\x00' * 640 * 480)
m.close(); os.close(fd)"
```

Expected: solid red 640×480 image on HDMI.

### 5B — FPGA DMA (devmem test)

```bash
# Set YUV addresses (defaults already in FPGA reset state, but explicit):
devmem 0xFF200010 32 0x3012C000   # Y base
devmem 0xFF200014 32 0x30177000   # U base
devmem 0xFF200018 32 0x30189C00   # V base
devmem 0xFF20001C 32 0x30000000   # RGB output → Buffer A

# Trigger DMA:
devmem 0xFF200008 32 0x2           # dma_trigger = 1

# Poll until done (bit 3 of status):
while ! devmem 0xFF200000 | grep -q "0x.*[8-9A-Fa-f]"; do true; done
echo "DMA done"
```

If YUV planes have been pre-filled, Buffer A should now contain the converted RBG565 image.

### 5C — Full daemon

```bash
mp4_play /media/fat/videos/test.mp4
```

Watch stderr for `YUV+DMA avg` timing. Expected ~3–5 ms/frame for FPGA DMA (640×480 pixels). If the ARM decode is bottlenecking, increase thread count: `mp4_play test.mp4 -t 2`.

---

## 6. KEY FILES

| File | Role |
|---|---|
| `Groovy.sv` | emu module — FB_* assigns, CONF_STR, `status[60]` toggle |
| `Groovy.qsf` | Project settings — `MISTER_FB=1` macro (line 53) |
| `sys/sys_top.v` | Top-level — LW AXI bridge, CDC, fb_base_sel mux, yuv_fb_dma instantiation, emu DDRAM stubs |
| `rtl/mp4_ctrl_regs.v` | AXI3 LW H2F slave — VBlank, dma_done latch, dma_trigger, 4 address registers |
| `rtl/yuv_fb_dma.v` | Avalon MM master DMA engine — YUV420P → RBG565 frame conversion |
| `rtl/yuv_to_rgb.sv` | 4-stage BT.601 pipeline — 1 px/clock, 4-cycle latency, DSP inference |
| `rtl/JTFRAME/jtframe_hsize.v` | Passthrough stub (pre-existing gap) |
| `rtl/h264_ioctl_fifo.sv` | Archived — not in build, may repurpose as command channel later |
| `h264-daemon/main.cpp` | ARM daemon — decode, scale, memcpy YUV, trigger DMA, VSync page flip |
| `doc/arm_dma_architecture.md` | Full ARM developer reference (updated for Phase 1.5) |
| `doc/yuv-rgb.md` | Phase 1.5 FPGA implementation reference |
| `files.qip` | Quartus file list |
| `compile.bat` | Quartus build script |
