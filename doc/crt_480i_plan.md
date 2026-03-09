# Plan: 480i CRT Output for MP4 Player

## Goal
Output video to NTSC CRT displays at native 480i (640×480 interlaced @ 59.94Hz) using Groovy's built-in Switchres protocol for zero-lag, pixel-perfect analog video.

---

## Architecture Overview

### **Video Path:**
```
ARM Decode (640×480 YUV)
    → DDR3 YUV buffers
        → FPGA DMA (YUV→RGB, field-aware)
            → DDR3 RGB framebuffer
                → fb_scan_out (interlaced field fetch)
                    → VGA_R/G/B (480i @ 59.94Hz)
                        → CRT
```

### **Key Components:**
1. **Switchres Protocol** - Groovy's hardware video timing reconfiguration
2. **Interlaced Field Fetching** - Fetch only even/odd lines per field (halves DMA bandwidth!)
3. **Fixed 480i Modeline** - 640×480i @ 59.94Hz NTSC standard

---

## Phase 1: Research Groovy Switchres Protocol

### **What We Know:**
- Groovy has state machine: `S_Switchres_Header`, `S_Switchres_PLL`, `S_Switchres_Mode`
- Reads 160-byte payload from DDR3 @ **0x30000008**
- Contains PLL dividers (M/C/K) and timing parameters (HFP, HBP, VFP, VBP)
- Reconfigures FPGA video timing during VBLANK

### **What We Need to Find:**

#### **1.1 - Switchres Header Structure**

Search Groovy.sv for:
```verilog
parameter DDR_SW_HEADER = ...
```

Find the exact byte layout:
- Offset 0x00: ? (PLL M0/M1?)
- Offset 0x04: ? (PLL C0/C1?)
- Offset 0x08: ? (PLL K?)
- Offset 0x0C: ? (Horizontal front porch?)
- Offset 0x10: ? (Horizontal back porch?)
- ... etc (160 bytes total)

**Action:** Read Groovy.sv state machine to decode the structure.

#### **1.2 - 480i Modeline Values**

Calculate or find reference values for 640×480i @ 59.94Hz:

**Target Specifications:**
- **Resolution:** 640×480 interlaced
- **Refresh Rate:** 59.94Hz (NTSC)
- **Pixel Clock:** ~13.5 MHz (NTSC standard)
- **Horizontal Frequency:** ~15.734 kHz
- **Vertical Frequency:** 59.94 Hz (interlaced)

**Timing Parameters (to calculate):**
- Horizontal sync width
- Horizontal front porch (HFP)
- Horizontal back porch (HBP)
- Vertical sync width
- Vertical front porch (VFP)
- Vertical back porch (VBP)

**PLL Dividers (M/C/K):**
- Need to generate 13.5 MHz pixel clock from Groovy's base clock
- Calculate M0, M1, C0, C1, K values for PLL

**Reference:** SMPTE 259M (NTSC 480i standard) or CEA-861 specifications.

#### **1.3 - CMD_SWITCHRES Trigger**

Find the AXI register that triggers Switchres load:
```verilog
// Somewhere in Groovy.sv or sys_top.v:
if (cmd_switchres) begin
    state <= S_Switchres_Header;
end
```

**Action:** Search for `cmd_switchres` or similar trigger signal.

---

## Phase 2: Implement Interlaced Field Fetching

### **2.1 - Field Indicator**

**Find field signal in Groovy:**
```verilog
reg field;  // 0 = even field, 1 = odd field
always @(posedge clk_vid) begin
    if (vs_rising_edge)
        field <= ~field;  // Toggle every vertical sync
end
```

**Pass to fb_scan_out:**
```verilog
fb_scan_out fb_scan_inst (
    .field         (field),        // ← Add field input
    .fb_base       (fb_base_sel),
    // ... other ports
);
```

### **2.2 - Modify fb_scan_out.sv for Interlacing**

**Current behavior:** Fetches all 480 lines every frame

**New behavior:** Fetches only 240 lines per field

```verilog
// fb_scan_out.sv modifications
input wire field;  // 0 = even, 1 = odd

// Line address calculation
wire [9:0] line_num = v_count;  // 0-479
wire [9:0] fetch_line = {line_num[8:0], field};  // Even: 0,2,4... Odd: 1,3,5...

// Only fetch during active lines
wire fetch_enable = (line_num < 10'd240);  // Only 240 lines per field

// Address calculation
assign avl_address = fb_base + (fetch_line * fb_stride);
```

**Result:** DMA bandwidth drops from ~37 MB/s → **~18.5 MB/s** (halved!)

### **2.3 - Set Interlaced Output Flags**

Tell MiSTer framework this is interlaced:
```verilog
// In sys_top.v or Groovy.sv
assign VGA_F1 = field;      // Field indicator (0=even, 1=odd)
assign VGA_SL = 2'b00;      // Scanline mode: 00=interlaced, 01=scanlines, 10=half, 11=quarter
```

---

## Phase 3: ARM Daemon Integration

### **3.1 - Write Switchres Header on Startup**

```cpp
// h264-daemon/main.cpp

// 480i Switchres payload (160 bytes) - to be filled after research
const uint8_t groovy_480i_modeline[160] = {
    // PLL dividers (M0, M1, C0, C1, K)
    0x00, 0x00, 0x00, 0x00,  // M0/M1 (placeholder)
    0x00, 0x00, 0x00, 0x00,  // C0/C1 (placeholder)
    0x00, 0x00, 0x00, 0x00,  // K (placeholder)

    // Horizontal timing
    0x80, 0x02, 0x00, 0x00,  // H_ACTIVE = 640
    0x10, 0x00, 0x00, 0x00,  // H_FRONT_PORCH = 16 (example)
    0x60, 0x00, 0x00, 0x00,  // H_SYNC_WIDTH = 96 (example)
    0x30, 0x00, 0x00, 0x00,  // H_BACK_PORCH = 48 (example)

    // Vertical timing (interlaced - per field)
    0xF0, 0x00, 0x00, 0x00,  // V_ACTIVE = 240 (per field)
    0x03, 0x00, 0x00, 0x00,  // V_FRONT_PORCH = 3 (example)
    0x06, 0x00, 0x00, 0x00,  // V_SYNC_WIDTH = 6 (example)
    0x1E, 0x00, 0x00, 0x00,  // V_BACK_PORCH = 30 (example)

    // Flags
    0x01, 0x00, 0x00, 0x00,  // INTERLACED = 1

    // ... rest of 160 bytes (to be determined)
};

int main(int argc, char** argv) {
    // Open /dev/mem
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    // Map Switchres header region (0x30000008)
    uint8_t* switchres_hdr = (uint8_t*)mmap(NULL, 160,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED | MAP_SYNC,
                                             fd, 0x30000008);

    // Write 480i modeline
    memcpy(switchres_hdr, groovy_480i_modeline, 160);
    munmap(switchres_hdr, 160);

    // Map AXI registers (0xFF200000)
    volatile uint32_t* axi = (uint32_t*)mmap(NULL, 4096,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED,
                                              fd, 0xFF200000);

    // Trigger Switchres load (register offset TBD)
    axi[CMD_SWITCHRES_IDX] = 1;

    // Wait for Switchres to complete
    sleep_ms(100);  // Give FPGA time to reconfigure PLL

    // Continue with normal video playback...
}
```

### **3.2 - Decode at 640×480 (Phase 4 from g3mini)**

Force FFmpeg to decode at native CRT resolution:

```cpp
// Scale video to 640×480 if larger
AVFrame* scaled_frame = av_frame_alloc();
struct SwsContext* sws_ctx = sws_getContext(
    vdec->width, vdec->height, vdec->pix_fmt,
    640, 480, AV_PIX_FMT_YUV420P,
    SWS_FAST_BILINEAR, NULL, NULL, NULL
);

sws_scale(sws_ctx, frame->data, frame->linesize, 0, vdec->height,
          scaled_frame->data, scaled_frame->linesize);
```

**Benefits:**
- Reduces CPU decode overhead
- Reduces DDR3 bandwidth
- Prevents ALF intro stuttering
- Perfect for 480i CRT output

---

## Phase 4: FPGA Modifications

### **4.1 - Enable fb_scan_out (from crt_output_ddr3.md)**

Follow existing plan to connect fb_scan_out to DDR3 via ram2_arbiter:
1. Add ram2_arbiter.v to files.qip
2. Change ram2_clk to clk_sys
3. Wire fb_scan_out to arbiter
4. Remove waitrequest=1 stall

### **4.2 - Add Field Input to fb_scan_out**

Modify fb_scan_out.sv:
```verilog
module fb_scan_out (
    input wire        clk,
    input wire        reset,

    input wire        field,         // ← NEW: 0=even, 1=odd
    input wire        fb_en,
    // ... existing ports
);

// Interlaced line fetching
wire [9:0] line_num = v_count;
wire fetch_active = (line_num < 10'd240);  // Only 240 lines per field

// Address calculation (fetch every other line based on field)
wire [9:0] actual_line = {line_num[8:0], field};  // 0,2,4... or 1,3,5...
assign avl_address = fb_base + (actual_line * fb_stride);
assign avl_read = fetch_active && de_in;
```

### **4.3 - Export Field Signal to sys_top.v**

```verilog
// sys_top.v
wire field;  // From Groovy.sv video timing

// Pass field to fb_scan_out
fb_scan_out fb_scan_inst (
    .field         (field),
    // ... other ports
);

// Export to VGA for MiSTer framework
assign VGA_F1 = fb_en ? field : emu_field;
assign VGA_SL = fb_en ? 2'b00 : emu_scanlines;  // 00 = interlaced
```

---

## Phase 5: Testing & Validation

### **5.1 - Test Progressive First (Baseline)**

Before implementing interlacing, test basic CRT output:
1. Compile FPGA with fb_scan_out connected (no interlacing yet)
2. Run mp4_play without Switchres
3. Verify CRT displays *something* (even if wrong refresh rate)

### **5.2 - Test Switchres Loading**

1. Write dummy Switchres header to DDR3
2. Trigger cmd_switchres
3. Verify FPGA doesn't crash
4. Check if video timing changes

### **5.3 - Test 480i Output**

1. Write proper 480i modeline
2. Enable field fetching in fb_scan_out
3. Play video
4. Verify:
   - CRT displays interlaced video (no flickering)
   - Correct aspect ratio (4:3)
   - Smooth 59.94Hz motion
   - No tearing or judder

---

## Expected Benefits

### **Performance:**
- **DMA Bandwidth:** 37 MB/s → 18.5 MB/s (halved!)
- **CPU Decode:** Lighter (640×480 vs higher resolutions)
- **DDR3 Contention:** Reduced significantly

### **Video Quality:**
- **Zero lag** (direct video, no scaler)
- **Perfect 59.94Hz** NTSC timing
- **Correct aspect ratio** (4:3 letterbox if needed)
- **Smooth motion** (no frame pacing issues)

### **Compatibility:**
- Works with any NTSC CRT (composite, S-Video, RGB via SCART)
- Standard 480i output (SMPTE 259M compliant)
- Direct Video or Analog IO board

---

## Research Checklist

- [x] **Switchres header format** - ✓ Decoded 20-byte structure from Groovy.sv (doc/switchres_format.md)
- [x] **480i modeline values** - ✓ Calculated PLL (M=27, C=100, K=1) and timing parameters (doc/switchres_format.md)
- [x] **CMD_SWITCHRES trigger** - ✓ Found EXT_BUS protocol, need AXI register instead (doc/switchres_implementation.md)
- [ ] **Field signal location** - Find field toggle in Groovy.sv
- [ ] **VGA_F1 / VGA_SL usage** - Verify MiSTer framework interlace support
- [x] **SMPTE 259M spec** - ✓ Used standard 480i NTSC timings (858×525, 13.5 MHz)

---

## Implementation Order

1. **Research Phase** (1-2 hours)
   - Decode Switchres header from Groovy.sv
   - Find 480i reference timings
   - Locate cmd_switchres trigger

2. **FPGA Phase** (2-3 hours)
   - Connect fb_scan_out via ram2_arbiter (from crt_output_ddr3.md)
   - Add field input to fb_scan_out
   - Modify for interlaced fetching
   - Compile and test

3. **ARM Phase** (1-2 hours)
   - Add Switchres header write to main.cpp
   - Force 640×480 decode
   - Test on hardware

4. **Polish Phase** (variable)
   - Fine-tune 480i timings
   - Add aspect ratio management (letterbox 16:9 → 4:3)
   - Frame pacing improvements

---

## Current Status
- **Status:** Research phase complete ✓
- **Completed:**
  - Decoded Switchres 20-byte header format
  - Calculated 480i NTSC timing values (640×480i @ 59.94Hz, 13.5 MHz)
  - Found cmd_switchres trigger mechanism (EXT_BUS protocol)
  - Designed AXI register alternative for ARM daemon
- **Next:** Implement Switchres trigger in FPGA (mp4_ctrl_regs.v + sys_top.v)
- **Documentation:**
  - doc/switchres_format.md — Header specification + 480i values
  - doc/switchres_implementation.md — Complete implementation guide
