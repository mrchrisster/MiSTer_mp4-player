# Ghosting Bug Investigation

## Date: 2026-03-03

## Symptom
Video playback shows horizontal ghosting/shift artifacts on alternating rows.
- **EVEN rows** (0, 2, 4...): shifted right by ~8-10 pixels
- **ODD rows** (1, 3, 5...): display correctly
- Pattern is consistent and repeatable

## Diagnostic Tests

### 1. test_stripe.cpp (RGB direct write)
**Result**: Displays perfectly - 8 vertical color stripes, sharp and aligned.
**Conclusion**: FB_WIDTH, FB_STRIDE, ASCAL display chain all correct. Bug is NOT in display path.

### 2. test_yuv_fpga.cpp (FPGA YUV→RGB DMA)
**Pattern**: Vertical greyscale stripes (Y gradient, U/V=128 neutral)
**Result**: Visible shift on even rows - stripe boundaries show sawtooth/comb pattern.
**Conclusion**: Bug is in YUV DMA pipeline, not ARM write or display.

### 3. test_yuv_dump.cpp (Detailed pixel analysis)
**Pattern**: Simple Y fill (rows 0-9 = Y=50, U/V=128)
**Results**:
```
ARM write verification: Y[0..19] = 50 50 50 ... ✓ CORRECT

FPGA output:
Row 0 (EVEN): pixels 0-619 = greyscale 128 ✗ WRONG
              pixels 620-639 = greyscale 40 ✓ CORRECT
Row 1 (ODD):  pixels 0-639 = greyscale 40 ✓ ALL CORRECT
Row 2 (EVEN): pixels 0-619 = greyscale 128 ✗ WRONG
              pixels 620-639 = greyscale 40 ✓ CORRECT
Row 3 (ODD):  pixels 0-639 = greyscale 40 ✓ ALL CORRECT
```

**Key Finding**:
- ARM writes Y=50 correctly to DDR3 ✓
- Even rows read Y=128 for first 620 pixels (WRONG - matches U/V fill value!)
- Even rows read Y=50 for last 20 pixels (CORRECT)
- Odd rows read Y=50 for all pixels (CORRECT)

**620-byte offset** = 640 - 20, very specific and consistent

### 4. Memory verification
```bash
devmem 0xFF200010  # Y base = 0x3012C000 ✓
devmem 0xFF200014  # U base = 0x30177000 ✓
devmem 0xFF200018  # V base = 0x30189C00 ✓

devmem 0x3012BD94  # (Y_base - 620) = 0x00000000 (not 128!)
```

Base addresses are correct. Memory at (Y_base - 620) is zero, not 128.

### 5. sys_top.v wiring check
```verilog
yuv_fb_dma yuv_dma (
    .yuv_y_base  (yuv_y_base),  ✓ correct
    .yuv_u_base  (yuv_u_base),  ✓ correct
    .yuv_v_base  (yuv_v_base),  ✓ correct
    ...
```
No wiring errors in top-level instantiation.

## Analysis

### State Machine Flow
**Even rows** (fetch U/V first):
```
S_IDLE → S_FETCH_U → S_RECV_U → S_FETCH_V → S_RECV_V →
S_GUARD → S_FETCH_Y → S_RECV_Y → S_PROCESS → S_WRITE → S_NEXT_ROW
```

**Odd rows** (skip U/V, reuse from previous row):
```
S_IDLE → S_FETCH_Y → S_RECV_Y → S_PROCESS → S_WRITE → S_NEXT_ROW
```

### Hypothesis
On **even rows**, the Y buffer (`y_buf`) is either:
1. **Not being filled** - retains stale data with value 128
2. **Partially filled** - only last 20 bytes written, first 620 bytes stale
3. **Being filled from wrong source** - reading from U/V plane instead of Y plane

The value **128** matches the U/V fill value, suggesting `y_buf` contains stale data from the previous U/V fetch.

### Why 620 bytes?
- 620 = 640 - 20 (almost exactly one row minus 20 bytes)
- 620 bytes = 77.5 Avalon beats (can't be fractional!)
- 20 bytes = 2.5 beats (also can't be fractional!)
- Pattern doesn't cleanly align with beat boundaries

## Attempted Fixes (all failed)

### Fix 1: S_GUARD state (isolation)
Added guard state between S_RECV_V and S_FETCH_Y to ensure clean separation.
**Result**: No change. Bug persists.

### Fix 2: Bit-shift arithmetic (synthesis)
Replaced `row * W` multiplication with explicit bit shifts `(row<<9)+(row<<7)`.
**Result**: No change. Bug persists.

### Fix 3: Explicit address register (timing)
Added `y_addr_reg` to latch Y address before use.
**Result**: Recompiled and tested — **NO IMPROVEMENT**. Bug persists.

## Fix 4: UART Debug Output (diagnostic approach)

All three logic fixes failed, suggesting the problem is NOT in address calculation or timing.
Created comprehensive UART debug to observe actual FPGA behavior:

### Files Added/Modified
- **`rtl/yuv_dma_debug.v`** (new): UART logger that monitors yuv_fb_dma state machine
  - Logs Y fetch address when S_FETCH_Y starts
  - Logs y_buf sample values when Y fetch completes (indices 0, 624, 639)
  - Only logs rows 0-3 to minimize UART overhead
  - Output format: ASCII lines, e.g., "R0 FY addr=3012C000\r\n"

- **`rtl/yuv_fb_dma.v`**:
  - Added `uart_debug_tx` output port
  - Instantiated yuv_dma_debug module internally
  - Exports y_buf[0], y_buf[624], y_buf[639] to debug module

- **`sys/sys_top.v`**:
  - Wired yuv_dma.uart_debug_tx → uart_rxd
  - Commented out old mp4_debug_uart instantiation

- **`files.qip`**: Added yuv_dma_debug.v to build

### Expected Debug Output
For rows 0-3, ARM should see (via `microcom /dev/ttyS1 -s 115200`):
```
R0 FY addr=3012C000
R0 YDONE y[000]=32 y[270]=32 y[27F]=32
R1 FY addr=3012C280
R1 YDONE y[000]=32 y[270]=32 y[27F]=32
R2 FY addr=3012C500
R2 YDONE y[000]=32 y[270]=32 y[27F]=32
R3 FY addr=3012C780
R3 YDONE y[000]=32 y[270]=32 y[27F]=32
```

If even rows show different y_buf values than odd rows, that proves y_buf is not being written correctly.

### Next Steps
1. Recompile FPGA (compile.bat)
2. Run test_yuv_dump on hardware
3. Monitor /dev/ttyS1 for debug output
4. Compare y_buf samples between even/odd rows
5. Identify root cause based on actual FPGA behavior

## Open Questions
1. Why does the bug only affect even rows (U/V fetch path)?
2. Where does the value 128 come from if not from (Y_base - 620)?
   - **Current hypothesis**: y_buf retains stale data from U/V writes
   - **Evidence**: 128 matches U/V fill value; 620 bytes = nearly u_buf + v_buf size (640 bytes)
3. Why is the offset exactly 620 bytes (not aligned to beat boundaries)?
   - 620 = 640 - 20; suggests partial overlap or indexing offset
4. Why are the LAST 20 pixels correct but not the FIRST 620?
   - Suggests y_buf is being written, but only the last 20 bytes reach correct storage

## Current Hypothesis
**Buffer synthesis overlap**: Quartus may be synthesizing y_buf, u_buf, v_buf into shared memory
with physical address aliasing. When S_RECV_U and S_RECV_V write to u_buf/v_buf (total 640 bytes
with value 128), they may be overwriting the first 620 bytes of y_buf's physical storage.

**Why 620 instead of 640?**
- y_buf indices 0-619 overlap with u/v storage
- y_buf indices 620-639 map to separate physical addresses (correct)

**Why only even rows?**
- Even rows: fetch U/V (writes 128) → fetch Y (last 20 bytes overwrite 128, first 620 remain)
- Odd rows: skip U/V fetch → y_buf retains previous row's correct Y data

The UART debug will show if y_buf actually contains the expected values immediately after S_RECV_Y,
which will prove or disprove this synthesis hypothesis.
