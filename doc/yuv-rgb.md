# Phase 1.5 — FPGA YUV→RBG565 Conversion: Implementation Reference

> **Status: COMPLETE** (Feb 2026) — awaiting first hardware synthesis and test.

---

## 1. OBJECTIVE

Offload YUV420P → RBG565 color-space conversion from the ARM Cortex-A9 to Cyclone V FPGA
fabric. The ARM writes raw YUV planes to DDR3 and triggers the FPGA DMA engine, which reads
the planes via the `fpga2sdram` port, converts via a 4-stage BT.601 pipeline, and writes
RBG565 pixels to the active back buffer. The ARM is freed from all pixel math.

---

## 2. DATA FLOW

```
ARM writes 460 KB YUV420P planes
  Y @ 0x3012C000  (307200 bytes, 640×480, 1 byte/pixel)
  U @ 0x30177000  (76800 bytes,  320×240, 1 byte/pixel)
  V @ 0x30189C00  (76800 bytes,  320×240, 1 byte/pixel)
         │
         │ memcpy (O_SYNC mmap, no cache)
         ▼
       DDR3
         │
         │ Avalon MM burst read (fpga2sdram RAM1, 64-bit, 29-bit address)
         ▼
  yuv_fb_dma (FSM)
    │  line buffer: y_buf[640], u_buf[320], v_buf[320]
    │  chroma upsampled: u_buf[x>>1], v_buf[x>>1]
    ▼
  yuv_to_rgb (4-stage BT.601 pipeline)
    latency: 5 clocks total (1 registered input + 4 pipeline stages)
    throughput: 1 pixel/clock
         │
         │ collect into rgb_buf[1280] (byte-addressed line buffer)
         ▼
  Avalon MM burst write (same RAM1 port, 64-bit)
    → RGB back buffer (0x30000000 or 0x30096000)
         │
         │ done pulse → dma_done_latch in mp4_ctrl_regs
         ▼
  ARM reads dma_done bit (axi[0] & 8) → page flip
```

---

## 3. PIXEL FORMAT — RBG565 BIG-ENDIAN

**Critical:** The MiSTer ASCAL scaler uses **RBG565**, not standard RGB565. Blue and Green
are swapped relative to the standard definition. Pixels are stored big-endian (high byte at
the lower DDR3 address).

```
Bit:  15 14 13 12 11 | 10  9  8  7  6  5 |  4  3  2  1  0
       R4 R3 R2 R1 R0   B5 B4 B3 B2 B1 B0   G4 G3 G2 G1 G0
```

| Bits | Channel | Width |
|------|---------|-------|
| [15:11] | Red   | 5 bits |
| [10:5]  | Blue  | 6 bits |  ← swapped vs standard RGB565 |
| [4:0]   | Green | 5 bits |  ← swapped vs standard RGB565 |

**Memory byte order (big-endian):**
- High byte (bits [15:8]) at lower DDR3 address.
- Low byte (bits [7:0]) at higher DDR3 address.

**Avalon writedata byte assignment** (64-bit bus, 4 pixels per beat):
```
writedata[7:0]   = pixel[0][15:8]  (pixel 0 high byte → lowest address)
writedata[15:8]  = pixel[0][7:0]   (pixel 0 low byte)
writedata[23:16] = pixel[1][15:8]  (pixel 1 high byte)
writedata[31:24] = pixel[1][7:0]
writedata[39:32] = pixel[2][15:8]
writedata[47:40] = pixel[2][7:0]
writedata[55:48] = pixel[3][15:8]
writedata[63:56] = pixel[3][7:0]   (pixel 3 low byte → highest address)
```

**FPGA pack function (Verilog, from `rtl/yuv_fb_dma.v`):**
```verilog
// Pack 4 consecutive pixels from rgb_buf starting at byte offset b*8.
// rgb_buf stores [b*8+0]=p0_high, [b*8+1]=p0_low, [b*8+2]=p1_high, ...
// Avalon little-endian: writedata[7:0] → DDR3 lowest address.
// For big-endian pixels, [7:0] must be the HIGH byte of pixel 0.
function [63:0] pack_beat;
    input [7:0] b;   // beat index (0–159 for 640-pixel row)
    reg  [10:0] base;
    begin
        base = {b, 3'b000};   // b * 8  (11-bit, max 159×8 = 1272)
        pack_beat = {rgb_buf[base+7], rgb_buf[base+6],
                     rgb_buf[base+5], rgb_buf[base+4],
                     rgb_buf[base+3], rgb_buf[base+2],
                     rgb_buf[base+1], rgb_buf[base+0]};
    end
endfunction
```

**ARM software pack (BT.601 limited-range):**
```cpp
const int c = (int)Y - 16;
const int d = (int)U - 128;
const int e = (int)V - 128;
int r = (298*c         + 409*e + 128) >> 8;
int g = (298*c - 100*d - 208*e + 128) >> 8;
int b = (298*c + 516*d         + 128) >> 8;
r = r < 0 ? 0 : r > 255 ? 255 : r;
g = g < 0 ? 0 : g > 255 ? 255 : g;
b = b < 0 ? 0 : b > 255 ? 255 : b;

// RBG565BE: R[15:11] B[10:5] G[4:0], high byte first
uint16_t px = ((uint16_t)(r & 0xF8) << 8)
            | ((uint16_t)(b & 0xFC) << 3)
            |  (uint16_t)(g >> 3);
*dst++ = (uint8_t)(px >> 8);
*dst++ = (uint8_t)(px & 0xFF);
```

---

## 4. `yuv_to_rgb.sv` — 4-Stage BT.601 Pipeline

**File:** `rtl/yuv_to_rgb.sv`

**BT.601 limited-range coefficients** (Y ∈ [16,235], U/V ∈ [16,240]):
```
c = Y  - 16
d = Cb - 128   (U)
e = Cr - 128   (V)
R = (298*c         + 409*e + 128) >> 8   clamp [0,255]
G = (298*c - 100*d - 208*e + 128) >> 8   clamp [0,255]
B = (298*c + 516*d         + 128) >> 8   clamp [0,255]
```

| Stage | Operation | Registers used |
|---|---|---|
| 0 | Register raw Y/U/V inputs | `s0_Y`, `s0_U`, `s0_V`, `s0_valid` |
| 1 | Subtract BT.601 offsets → signed 9-bit `c`, `d`, `e` | `s1_c`, `s1_d`, `s1_e`, `s1_valid` |
| 2 | Constant multiplications → signed 20-bit products (`298*c`, `409*e`, `100*d`, `208*e`, `516*d`) | `s2_yy`…`s2_p4`, `s2_valid` |
| 3 | Accumulate R/G/B with rounding constant (+128) | `s3_R_acc`, `s3_G_acc`, `s3_B_acc`, `s3_valid` |
| 4 | Shift right 8, clamp [0,255], pack RBG565 | `rgb565`, `data_valid_out` |

**Pipeline latency:** 4 clocks from `data_valid_in` to `data_valid_out`.

**Quartus DSP inference:** The 5 constant multiplications in Stage 2 (`298*c`, `409*e`, `100*d`, `208*e`, `516*d`) each map to one Cyclone V DSP block (9-bit × 10-bit → 19-bit signed product). Zero LUT cost for the multiplications.

**Output format:** `rgb565[15:0]` = `{R8[7:3], B8[7:2], G8[7:3]}` — RBG565, identical byte order to what the ARM would write.

---

## 5. `yuv_fb_dma.v` — Avalon Master DMA Engine

**File:** `rtl/yuv_fb_dma.v`

### Interface

```verilog
module yuv_fb_dma (
    input  wire        clk,
    input  wire        reset,

    // Control
    input  wire        trigger,       // one-clock pulse to start a frame
    output reg         done,          // one-clock pulse when frame complete

    // YUV source and RGB destination (DDR3 byte addresses)
    input  wire [31:0] yuv_y_base,
    input  wire [31:0] yuv_u_base,
    input  wire [31:0] yuv_v_base,
    input  wire [31:0] rgb_base,

    // Avalon MM master (fpga2sdram RAM1 — 64-bit, 29-bit byte-addressed word address)
    // See memory_addressation_summary.md for architectural details on Avalon Masters
    // and the FPGA-to-SDRAM interface.
    output reg  [28:0] avl_address,
    output reg   [7:0] avl_burstcount,
    input  wire        avl_waitrequest,
    input  wire [63:0] avl_readdata,
    input  wire        avl_readdatavalid,
    output reg         avl_read,
    output reg  [63:0] avl_writedata,
    output reg   [7:0] avl_byteenable,
    output reg         avl_write
);
```

### Memory Parameters

```verilog
parameter W         = 640;   // frame width (pixels)
parameter H         = 480;   // frame height (rows)
parameter Y_BEATS   = 80;    // W/8 = 640/8 = 80 read beats per Y row
parameter UV_BEATS  = 40;    // (W/2)/8 = 320/8 = 40 read beats per U or V row
parameter RGB_BEATS = 160;   // (W*2)/8 = 1280/8 = 160 write beats per row
parameter PIPE_LAT  = 5;     // total pipeline latency: 1 (pipe_Y reg) + 4 (yuv_to_rgb)
```

### Internal Line Buffers

```verilog
reg [7:0]  y_buf  [0:639];   // Y luma, 1 byte/pixel
reg [7:0]  u_buf  [0:319];   // U chroma, 1 byte/chroma-pixel
reg [7:0]  v_buf  [0:319];   // V chroma, 1 byte/chroma-pixel
reg [7:0]  rgb_buf[0:1279];  // RGB output, 2 bytes/pixel (big-endian)
```

### FSM State Machine

```
S_IDLE ──(trigger)──► S_FETCH_U
                           │ (assert avl_read for U burst)
                           ▼
                       S_RECV_U
                           │ (receive 40 beats → u_buf)
                           ▼
                       S_FETCH_V
                           │
                           ▼
                       S_RECV_V
                           │
                           ▼
                       S_FETCH_Y
                           │ (assert avl_read for Y burst)
                           ▼
                       S_RECV_Y
                           │ (receive 80 beats → y_buf)
                           ▼
                       S_PROCESS
                           │ (feed pipeline 640 px; collect 640 px into rgb_buf)
                           ▼
                       S_WRITE ──────────────────────────────────────────┐
                           │ (burst write 160 beats from rgb_buf)        │
                           ▼                                             │
                       S_NEXT_ROW                                        │
                           │ (row < 479) ──► S_FETCH_U (even) or         │
                           │                S_FETCH_Y (odd, reuse UV)    │
                           │ (row == 479) ──► S_DONE_ST                  │
                           ▼                                             │
                       S_DONE_ST ──(assert done pulse)──► S_IDLE ◄───────┘
```

**UV reuse on odd rows:** YUV420 has one chroma sample per 2×2 luma block. Even rows fetch new `u_buf`/`v_buf`; odd rows skip `S_FETCH_U`/`S_RECV_U`/`S_FETCH_V`/`S_RECV_V` and go directly to `S_FETCH_Y`, reusing the previous row's chroma. This halves chroma DDR3 bandwidth.

### Critical Timing Details

**Avalon read command acceptance:**
The DMA asserts `avl_read` in the state transition cycle (old value = 0). The guard to advance state is `avl_read && !avl_waitrequest` — because `avl_read` is a registered output, on the very first clock where it becomes 1, `avl_read` (old value) is still 0, so the guard does not fire prematurely. The advance fires on the first clock where both `avl_read=1` (previous cycle had asserted it) and `avl_waitrequest=0`.

**Avalon write data pre-loading:**
In `S_WRITE`, when the current beat is accepted (`avl_write && !avl_waitrequest`), the next beat's `avl_writedata` is pre-loaded in the same always block using `pack_beat(wr_beat + 1)`. Because non-blocking assignments take effect at the end of the time step, `wr_beat` used in `pack_beat(wr_beat + 1)` is the old (current) value — this correctly pre-loads beat N+1 when beat N is accepted.

**Pipeline collection:**
`S_PROCESS` feeds Y/U/V into the pipeline for `W` clocks, then continues collecting `yuv_to_rgb` output for `PIPE_LAT` more clocks before transitioning to `S_WRITE`. The `rgb_wr_px` counter increments on every `pipe_vout` pulse; when it reaches `W-1` the state transitions to `S_WRITE`.

**Chroma upsampling:** Nearest-neighbour: `u_buf[x >> 1]`, `v_buf[x >> 1]`.

---

## 6. `mp4_ctrl_regs.v` — AXI3 LW Slave Registers

**File:** `rtl/mp4_ctrl_regs.v`

*This module implements an AXI slave on the Lightweight HPS-to-FPGA bridge. See `memory_addressation_summary.md` for details on HPS-FPGA communication.*


### Address Map

| Offset | araddr[4:2] | Access | Description |
|---|---|---|---|
| `0x000` | `3'b000` | Read | Status: [2]=fb_vbl (VBlank), [3]=dma_done_latch |
| `0x008` | `3'b010` | R/W | Control: [0]=buf_sel, [1]=dma_trigger (auto-clears) |
| `0x010` | `3'b100` | R/W | yuv_y_base (default 0x3012C000) |
| `0x014` | `3'b101` | R/W | yuv_u_base (default 0x30177000) |
| `0x018` | `3'b110` | R/W | yuv_v_base (default 0x30189C00) |
| `0x01C` | `3'b111` | R/W | yuv_rgb_base (default 0x30000000) |

### `dma_done` Sticky Latch

The `done` output of `yuv_fb_dma` is a single-clock pulse (~10 ns at 100 MHz). The ARM polling loop cannot reliably catch a 10 ns pulse — the AXI read takes hundreds of nanoseconds.

The `dma_done_latch` register solves this:
```verilog
always @(posedge clk) begin
    if (!rst_n)
        dma_done_latch <= 1'b0;
    else begin
        if (dma_done)                                        // set on pulse
            dma_done_latch <= 1'b1;
        if (arvalid & arready & (araddr[4:2] == 3'b000))    // clear on ARM read
            dma_done_latch <= 1'b0;
    end
end
```

The latch is automatically cleared by the ARM's polling read — no explicit clear write is needed.

### `dma_trigger` Auto-Clear

Writing bit 1 of the Control register (`0x008`) raises `dma_trigger` for exactly one clock cycle. The write path sets it; the always block unconditionally clears it every cycle before the write path runs:
```verilog
dma_trigger <= 1'b0;          // auto-clear every cycle
// ...
if (aw_pend & w_pend) begin
    // ...
    dma_trigger <= wd_lat[1]; // one-clock pulse if bit was written 1
end
```

---

## 7. `sys_top.v` INTEGRATION

### New Wire Declarations
```verilog
wire        buf_sel;
wire        dma_trigger;
wire        dma_done;
wire [31:0] yuv_y_base, yuv_u_base, yuv_v_base, yuv_rgb_base;
```

### Updated `mp4_ctrl_regs` Instantiation
Added ports: `dma_done`, `dma_trigger`, `yuv_y_base`, `yuv_u_base`, `yuv_v_base`, `yuv_rgb_base`.

### `yuv_fb_dma` Instantiation
Connected directly to the existing `ram_*` Avalon wires that feed `fpga2sdram` RAM1 through `sysmem.sv`'s `f2sdram_safe_terminator`. No changes to `sysmem.sv` required.

```verilog
yuv_fb_dma yuv_dma (
    .clk              (clk_sys),
    .reset            (reset_req),
    .trigger          (dma_trigger),
    .done             (dma_done),
    .yuv_y_base       (yuv_y_base),
    .yuv_u_base       (yuv_u_base),
    .yuv_v_base       (yuv_v_base),
    .rgb_base         (yuv_rgb_base),
    .avl_address      (ram_address),
    .avl_burstcount   (ram_burstcount),
    .avl_waitrequest  (ram_waitrequest),
    .avl_readdata     (ram_readdata),
    .avl_readdatavalid(ram_readdatavalid),
    .avl_read         (ram_read),
    .avl_writedata    (ram_writedata),
    .avl_byteenable   (ram_byteenable),
    .avl_write        (ram_write)
);
assign ram_clk = clk_sys;
```

### Groovy Emu DDRAM Stubs
```verilog
// Emu DDRAM permanently stalled — ASCAL reads FB directly; emu decode unused
.DDRAM_BUSY      (1'b1),
.DDRAM_DOUT      (64'd0),
.DDRAM_DOUT_READY(1'b0),
// output ports routed to no-connect wires:
.DDRAM_CLK       (emu_ddram_clk_nc),
.DDRAM_ADDR      (emu_ddram_addr_nc),
// ... etc.
```

---

## 8. ARM DAEMON CHANGES (`h264-daemon/main.cpp`)

### New Memory Regions
```cpp
// YUV planes — written by ARM, read by FPGA DMA
#define YUV_Y_PHYS   (FB_PHYS + FB_TOTAL)            // 0x3012C000
#define YUV_Y_SIZE   (FB_W * FB_H)                    // 307200
#define YUV_U_SIZE   ((FB_W/2) * (FB_H/2))            // 76800
#define YUV_V_SIZE   ((FB_W/2) * (FB_H/2))            // 76800
#define YUV_TOTAL    (YUV_Y_SIZE + YUV_U_SIZE + YUV_V_SIZE)  // 460800

void* yuv_map = mmap(NULL, YUV_TOTAL, PROT_READ|PROT_WRITE,
                     MAP_SHARED, mem_fd, YUV_Y_PHYS);
uint8_t* yuv_y = (uint8_t*)yuv_map;
uint8_t* yuv_u = yuv_y + YUV_Y_SIZE;
uint8_t* yuv_v = yuv_u + YUV_U_SIZE;
```

### New AXI Constants
```cpp
#define AXI_STATUS_IDX   0     // word offset 0x000
#define AXI_CTRL_IDX     2     // word offset 0x008
#define AXI_YUV_Y_IDX    4     // word offset 0x010
#define AXI_YUV_U_IDX    5     // word offset 0x014
#define AXI_YUV_V_IDX    6     // word offset 0x018
#define AXI_RGB_BASE_IDX 7     // word offset 0x01C
#define AXI_VBL_BIT      (1u << 2)
#define AXI_DMA_DONE_BIT (1u << 3)
#define AXI_DMA_TRIG_BIT (1u << 1)
```

### One-Time Register Initialization
```cpp
axi[AXI_CTRL_IDX]     = 0;
axi[AXI_YUV_Y_IDX]    = (uint32_t)YUV_Y_PHYS;
axi[AXI_YUV_U_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE;
axi[AXI_YUV_V_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE + YUV_U_SIZE;
axi[AXI_RGB_BASE_IDX] = (uint32_t)FB_PHYS;   // initial: Buffer A
```

### Per-Frame: `write_yuv_and_dma()`

Replaces the old `yuv420p_to_fb()` software pixel conversion:

```cpp
static void write_yuv_and_dma(const AVFrame* f,
                               uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                               uint32_t rgb_back_phys,
                               volatile uint32_t* axi)
{
    // 1. Nearest-neighbour scale Y to 640×480
    static uint8_t tmp_y[FB_W * FB_H];
    for (int dy = 0; dy < FB_H; dy++) {
        int sy = dy * f->height / FB_H;
        const uint8_t* src_row = f->data[0] + sy * f->linesize[0];
        uint8_t* dst_row = tmp_y + dy * FB_W;
        for (int dx = 0; dx < FB_W; dx++)
            dst_row[dx] = src_row[dx * f->width / FB_W];
    }
    memcpy(yuv_y, tmp_y, YUV_Y_SIZE);

    // 2. Scale U/V to 320×240
    static uint8_t tmp_u[YUV_U_SIZE], tmp_v[YUV_V_SIZE];
    const int uvW = FB_W/2, uvH = FB_H/2;
    for (int dy = 0; dy < uvH; dy++) {
        int sy = dy * (f->height/2) / uvH;
        const uint8_t* usrc = f->data[1] + sy * f->linesize[1];
        const uint8_t* vsrc = f->data[2] + sy * f->linesize[2];
        uint8_t* udst = tmp_u + dy * uvW;
        uint8_t* vdst = tmp_v + dy * uvW;
        for (int dx = 0; dx < uvW; dx++) {
            udst[dx] = usrc[dx * (f->width/2) / uvW];
            vdst[dx] = vsrc[dx * (f->width/2) / uvW];
        }
    }
    memcpy(yuv_u, tmp_u, YUV_U_SIZE);
    memcpy(yuv_v, tmp_v, YUV_V_SIZE);

    // 3. Trigger FPGA DMA
    axi[AXI_RGB_BASE_IDX] = rgb_back_phys;
    axi[AXI_CTRL_IDX] = (axi[AXI_CTRL_IDX] & 1u) | AXI_DMA_TRIG_BIT;

    // 4. Poll for completion (sticky latch — never missed)
    while (!(axi[AXI_STATUS_IDX] & AXI_DMA_DONE_BIT)) { /* spin */ }
}
```

### Decode Loop Integration (step E)
```cpp
// ── E. Scale YUV planes, write to DDR3, trigger FPGA DMA ─────────────────
{
    const int64_t tc0 = now_us();
    const uint32_t rgb_back_phys = FB_PHYS + (uint32_t)back * FB_SIZE;
    write_yuv_and_dma(frame, yuv_y, yuv_u, yuv_v, rgb_back_phys, axi);
    t_convert_us += now_us() - tc0;
}
```

Timing is reported as `YUV+DMA avg` in the end-of-file statistics.

---

## 9. EXPECTED PERFORMANCE

| Operation | Estimated time | Notes |
|---|---|---|
| ARM nearest-neighbour Y scale | ~2 ms | 640×480 = 307,200 reads/writes |
| ARM nearest-neighbour UV scale | ~0.5 ms | 2 × 320×240 |
| ARM `memcpy` YUV to DDR3 | ~0.5 ms | 460 KB via O_SYNC mmap |
| FPGA DMA (read YUV + convert + write RGB) | ~3–5 ms | ~3 DDR3 reads + 1 write per row × 480 rows |
| Total `write_yuv_and_dma` | ~6–8 ms | vs ~18–25 ms for ARM-only software conversion |

ARM CPU is now free for ~8–12 ms of its ~33 ms frame budget (30fps), leaving headroom for H.264 decode of higher-bitrate or higher-resolution sources.

---

## 10. KNOWN LIMITATIONS

| Limitation | Impact | Future fix |
|---|---|---|
| No Avalon arbiter | yuv_fb_dma has exclusive ownership of RAM1; Groovy emu DDRAM permanently stalled | Phase 4: proper arbiter when emu decode path is needed |
| Hardcoded 640×480 | YUV planes always scaled to 640×480 by ARM before FPGA DMA | Phase 4: parameterise DMA on run-time width/height |
| Nearest-neighbour scaling | Blocky output on videos with different aspect ratio | Future: bilinear scale in ARM or FPGA |
| Single-frame YUV buffer | ARM overwrites YUV planes before FPGA confirms DMA start | Current polling loop prevents collision; harmless in practice |
| No hw VPU | ARM Cortex-A9 software decode limits to ~480p @ 30fps | Hardware limitation |
