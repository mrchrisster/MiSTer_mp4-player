# FPGA Compilation History

Tracks every FPGA bitfile compiled for the MP4 player project, what changed,
and the test results. Use this to avoid revisiting dead ends.

---

## Pre-YUV DMA Era (Feb 23)

### Groovy_first (2026-02-23 10:23)
- **File:** `releases/old/Groovy_first_20260223_1023.rbf`
- **Changes:** Initial Groovy core with MISTER_FB=1, mp4_ctrl_regs AXI slave,
  double-framebuffer (A=0x30000000, B=0x30096000), buf_sel page flip
- **Test:** ARM software YUV→RGB565 conversion writing directly to framebuffer
- **Result:** Video displayed but colors wrong (rainbow/wrong hues)
- **Root cause (found later):** ARM was packing RGB565 but ASCAL expects BGR565

### Groovy_sec (2026-02-23 17:21)
- **File:** `releases/old/Groovy_sec_20260223_1721.rbf`
- **Changes:** Minor fixes to AXI bridge
- **Result:** Same color issues

---

## YUV→RGB FPGA DMA Era (Feb 25–26)

### yuv2rgb_1 (2026-02-25 07:30)
- **File:** `releases/old/Groovy_yuv2rgb_1_20260225_0730.rbf`
- **Changes:** First yuv_fb_dma.v + yuv_to_rgb.sv integration.
  FPGA reads YUV420P from DDR3, converts via 4-stage BT.601 pipeline,
  writes RGB565 to framebuffer
- **Result:** DMA timeout — never completed

### yuv2rgb_2 (2026-02-25 08:02)
- **File:** `releases/old/Groovy_yuv2rgb_2_20260225_0802.rbf`
- **Changes:** Avalon protocol fixes (read command acceptance guard)
- **Result:** DMA timeout — Avalon burst protocol still broken

### yuv2rgb_3 (2026-02-25 12:20)
- **File:** `releases/old/Groovy_yuv2rgb_3_20260225_1220.rbf`
- **Changes:** Fixed Avalon read/write protocol (registered outputs,
  waitrequest handling, write pre-loading)
- **Result:** DMA completed but image garbled / tripled horizontally, wrong colors

### yuv2rgb_4 (2026-02-25 13:21)
- **File:** `releases/old/Groovy_yuv2rgb_4_20260225_1321.rbf`
- **Changes:** Address formula fixes (`byte_address[31:3]`)
- **Result:** DMA completed, still wrong colors and tripled image

### yuv2rgb_5 (2026-02-25 15:11)
- **File:** `releases/old/Groovy_yuv2rgb_5_20260225_1511.rbf`
- **Changes:** fb_arb (2-to-1 arbiter) + fb_scan_out for CRT output
- **Result:** DMA stalled permanently — fb_arb caused starvation between
  fb_scan_out and yuv_fb_dma sharing the same DDR3 port.
  fb_scan_out needs a dedicated fpga2sdram port.

### yuv2rgb_6 (2026-02-25 21:52)
- **File:** `releases/Groovy_yuv2rgb_6_20260225_2152.rbf`
- **Changes:** fb_arb bypassed — yuv_fb_dma connected directly to ram1.
  fb_scan_out stalled (waitrequest=1 permanently)
- **Result:** DMA working again, still wrong colors

### yuv2rgb_8 (2026-02-26 19:35)
- **File:** `releases/Groovy_yuv2rgb_8_20260226_1935.rbf`
- **Changes:** mp4_debug_uart added (UART debug counters for T/D/V/R/W/B).
  dma_done sticky latch race condition fixed in mp4_ctrl_regs.v
- **Result:** DMA reliable (T=D counts match), VBL counting confirmed.
  Still wrong colors.

### yuv2rgb_9 (2026-02-26 23:17)
- **File:** `releases/Groovy_yuv2rgb_9_20260226_2317.rbf`
- **Changes:** Performance optimizations in daemon (decode-ahead threading,
  scale LUT, dmb sy barrier, clock reset on drop, drop threshold tuning)
- **Result:** Playback smooth (~2% drop rate), but colors still wrong.
  test_rgb_direct (ARM writes pixels directly) shows correct colors.
  YUV→RGB pipeline output has wrong colors and tripled image.

---

## H.264 / Color Fix Era (Mar 2)

### h264_1 (2026-03-02 11:00)
- **File:** `releases/Groovy_h264_1_20260302_1100.rbf`
- **Changes:** First attempt at BGR565 fix in yuv_to_rgb.sv
  (changed output packing from RGB565 to BGR565)
- **Result:** test_rgb_direct correct (blue/green/red/white).
  test_color still wrong (pink/green stripes). Zero improvement for YUV pipeline.

### h264_2 (2026-03-02 12:33)
- **File:** `releases/Groovy_h264_2_20260302_1233.rbf`
- **Changes:** VBlank edge detection fix in mp4_ctrl_regs.v
- **Result:** No change in color issues

### h264_3 (2026-03-02 18:33)
- **File:** `releases/Groovy_h264_3_20260302_1833.rbf`
- **Changes:** Unknown incremental fix
- **Result:** Colors still wrong

### h264_4 (2026-03-02 19:07)
- **File:** `releases/Groovy_h264_4_20260302_1907.rbf`
- **Changes:** BGR565 output packing confirmed in yuv_to_rgb.sv.
  `(* ramstyle = "M10K" *)` added to line buffers (later found to be wrong approach)
- **Result:** test_color still shows orange/green stripes instead of grayscale.
  DMA completes successfully. devmem shows FPGA computes correct grayscale
  values (pipe_rgb=0x8410 for Y≈126) but stored big-endian in DDR3,
  so ASCAL reads byte-swapped pixels (0x1084 = wrong colors).

### h264_5 (2026-03-02 22:22) — PENDING TEST
- **File:** `releases/Groovy_h264_5_20260302_2222.rbf`
- **Changes:**
  1. **Byte order fix (THE REAL FIX):** Swapped rgb_buf storage from big-endian
     to little-endian in yuv_fb_dma.v:
     ```verilog
     // BEFORE (wrong — big-endian):
     rgb_buf[{px, 1'b0}] <= pipe_rgb[15:8];  // high byte at even index
     rgb_buf[{px, 1'b1}] <= pipe_rgb[ 7:0];  // low byte at odd index

     // AFTER (correct — little-endian, matches ARM):
     rgb_buf[{px, 1'b0}] <= pipe_rgb[ 7:0];  // low byte at even index
     rgb_buf[{px, 1'b1}] <= pipe_rgb[15:8];  // high byte at odd index
     ```
  2. **Reverted M10K directives:** Removed `(* ramstyle = "M10K" *)` from all
     line buffers. M10K requires synchronous reads but pack_beat() does
     combinational reads — would cause 1-cycle stale data. Distributed
     logic (registers) is correct.
- **Expected result:** Grayscale gradient from test_color. Correct colors from video.
- **Actual result:** ✅ **COLORS FIXED!** test_color shows correct grayscale (2.5 gradient cycles across 640 pixels). Real video shows correct colors but **ghosting** (main image centered with ~5 squished copies left/right).

### h264_6 (2026-03-02 TBD) — PENDING TEST
- **File:** `releases/Groovy_h264_6_YYYYMMDD_HHMM.rbf`
- **Changes:** Fixed ASCAL input resolution mismatch
  1. **Changed Groovy core default video timing from 256×240 to 640×480:**
     - The ASCAL auto-detects input resolution from video timing signals (de_emu/hs_fix/vs_fix)
     - Old default: PoC_H=256, PoC_V=240 (Sega Master System native)
     - New default: PoC_H=640, PoC_V=480 (matches FB_WIDTH/FB_HEIGHT)
     - Updated PoC_HFP/PoC_HS/PoC_HBP and PoC_VFP/PoC_VS/PoC_VBP to VGA timings
     - Updated PoC_ce_pix from 16 to 4 (pixel clock 25 MHz for 640×480@60Hz)
  2. **Root cause of ghosting:** ASCAL detected input as 256×240 but framebuffer was 640×480.
     The scaler tried to fit a 640-pixel-wide framebuffer into a 256-pixel input area,
     creating 640/256 = 2.5 wrapping copies, which with edge artifacts appeared as ~5 copies.
- **Expected result:** No more ghosting. Clean video display.

---

## Key Diagnostic Tests

| Test | What it proves | Bypasses |
|------|---------------|----------|
| `test_rgb_direct` | BGR565 format + ASCAL byte order | YUV pipeline, DMA |
| `test_yuv_arm` | BT.601 formulas + BGR565 packing | FPGA DMA entirely |
| `test_color` | Full FPGA DMA pipeline (YUV read → convert → RGB write) | Nothing |
| `test_endian` | YUV input byte order (swap input bytes) | Nothing |
| UART debug | DMA trigger/done counts, VBL, Avalon stalls | Software |

## Lessons Learned

1. **Always trace the full byte path** from writer → DDR3 → reader. Don't assume
   big-endian or little-endian — verify empirically with test_rgb_direct.

2. **ARM uint16_t writes are the ground truth.** They produce correct display.
   The FPGA DMA must store bytes in the same order.

3. **Don't force M10K on buffers with combinational reads.** M10K block RAM
   requires synchronous reads (1-cycle latency). Use distributed logic instead.

4. **test_rgb_direct bypasses the YUV pipeline** — it only tests the pixel format
   and ASCAL path. Don't assume "test_rgb_direct works" means the YUV pipeline
   is correct.

5. **devmem reads are little-endian on ARM.** When interpreting devmem output,
   remember: result[7:0] = DDR3[addr+0], result[15:8] = DDR3[addr+1].

6. **ASCAL input resolution must match framebuffer resolution.** The ASCAL auto-detects
   input video resolution (iauto=1) from the core's timing signals. If the core outputs
   256×240 but the framebuffer is 640×480, the ASCAL creates wrapping/tiling artifacts.
   **Test patterns can hide this** — a repeating gradient looks fine even when tiled,
   but real video with distinct features shows ghosting/multiple copies.
