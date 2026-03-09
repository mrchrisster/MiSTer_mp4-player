# Switchres Implementation for MP4 Player

## Overview
This document describes how to add Switchres support to the MP4 player, allowing the ARM daemon to dynamically reconfigure video timing for 480i CRT output.

---

## Architecture

```
ARM Daemon (main.cpp)
    |
    | 1. Write 480i header to DDR3 @ 0x30000008
    |
    | 2. Write AXI register 0x00C bit 0 = 1
    |
    V
mp4_ctrl_regs.v (AXI slave)
    |
    | Sets cmd_switchres_mp4 = 1
    |
    V
sys_top.v
    |
    | Wires cmd_switchres_mp4 to emu
    |
    V
Groovy.sv (emu module)
    |
    | Reads 20-byte header from DDR3 @ 0x30000008
    | Reconfigures PLL dividers (M/C/K)
    | Reconfigures video timing (H/V sync, porches)
    | Switches to interlaced mode
    |
    V
480i @ 59.94Hz CRT output
```

---

## Code Changes

### 1. rtl/mp4_ctrl_regs.v — Add Switchres Trigger

**Add output port** (after line 20):
```verilog
output reg        cmd_switchres_mp4 = 1'b0,
output reg [31:0] switchres_frame_mp4 = 32'd0,
```

**Add register implementation** (in write logic, around line 80):
```verilog
// Register 0x00C: Switchres control
8'h03: begin
    if (h2f_awvalid && h2f_wvalid) begin
        cmd_switchres_mp4  <= h2f_wdata[0];      // bit 0: trigger switchres
        switchres_frame_mp4 <= h2f_wdata[31:1];  // bits[31:1]: frame number (optional)
    end
end
```

**Auto-clear cmd_switchres_mp4** (in always block, after dma_trigger clear):
```verilog
// Auto-clear cmd_switchres_mp4 after 1 cycle
if (cmd_switchres_mp4) begin
    cmd_switchres_mp4 <= 1'b0;
end
```

**Add to read logic** (around line 100):
```verilog
8'h03: h2f_rdata <= {switchres_frame_mp4[30:0], cmd_switchres_mp4};
```

---

### 2. sys/sys_top.v — Wire to Groovy emu

**Add wire declaration** (around line 400):
```verilog
wire cmd_switchres_mp4;
wire [31:0] switchres_frame_mp4;
```

**Connect mp4_ctrl_regs output** (in mp4_ctrl_regs instantiation):
```verilog
mp4_ctrl_regs mp4_ctrl (
    // ... existing ports ...
    .cmd_switchres_mp4(cmd_switchres_mp4),
    .switchres_frame_mp4(switchres_frame_mp4)
);
```

**Wire to emu** (in emu instantiation, around line 1200):
```verilog
emu emu (
    // ... existing ports ...
    .cmd_switchres(cmd_switchres_mp4),      // ← Use our AXI trigger instead of hps_ext
    .switchres_frame(switchres_frame_mp4),  // ← Frame number when to apply
    // ... rest of ports ...
);
```

**Important:** The emu already has `cmd_switchres` and `switchres_frame` ports connected to `hps_ext`. We need to **OR** our MP4 trigger with the existing one:

```verilog
// Combine MP4 trigger with GroovyMAME trigger
wire cmd_switchres_combined = cmd_switchres_hps_ext || cmd_switchres_mp4;

emu emu (
    .cmd_switchres(cmd_switchres_combined),
    .switchres_frame(cmd_switchres_mp4 ? switchres_frame_mp4 : switchres_frame_hps_ext),
    // ...
);
```

---

### 3. Groovy.sv — No Changes Needed!

The Switchres state machine (lines 1195-1236) already implements:
- Reads header from DDR3 @ `DDR_SW_HEADER` (offset 8)
- Parses 20-byte header into timing registers
- Reconfigures PLL during VBLANK
- Switches to interlaced mode if header specifies it

We just need to trigger it via `cmd_switchres`.

---

## AXI Register Map (Updated)

| Offset | Index | Bits | Meaning |
|---|---|---|---|
| 0x000 | axi[0] | bit 2 = fb_vbl | VBlank pulse |
| 0x000 | axi[0] | bit 3 = dma_done | DMA completion (sticky latch) |
| 0x008 | axi[2] | bit 0 = buf_sel | 0=Buffer A, 1=Buffer B |
| 0x008 | axi[2] | bit 1 = dma_trigger | Write 1 to trigger YUV→RGB DMA |
| **0x00C** | **axi[3]** | **bit 0 = trigger_switchres** | **Write 1 to trigger Switchres** |
| **0x00C** | **axi[3]** | **bits[31:1] = switchres_frame** | **Frame number when to apply (optional, 0=immediate)** |
| 0x010 | axi[4] | [31:0] = yuv_y_base | Y plane DDR3 address |
| 0x014 | axi[5] | [31:0] = yuv_u_base | U plane DDR3 address |
| 0x018 | axi[6] | [31:0] = yuv_v_base | V plane DDR3 address |
| 0x01C | axi[7] | [31:0] = yuv_rgb_base | RGB output DDR3 address |
| 0x020 | axi[8] | [31:0] = magic | Read-only, returns 0xA1EC0001 |

---

## ARM Daemon Integration

### 4. h264-daemon/main.cpp — Trigger Switchres on Startup

**Add constants** (after existing AXI defines):
```cpp
#define AXI_SWITCHRES_IDX 3        // Offset 0x00C

// Switchres header location (DDR3 offset 8)
#define SWITCHRES_HEADER_ADDR 0x30000008
#define SWITCHRES_HEADER_SIZE 20
```

**Add 480i modeline** (from doc/switchres_format.md):
```cpp
// Groovy Switchres header for 640×480i @ 59.94Hz NTSC
const uint8_t groovy_480i_modeline[20] = {
    // Horizontal timing
    0x80, 0x02,  // H = 640 (0x0280)
    0x10,        // HFP = 16
    0x60,        // HS = 96
    0x6A,        // HBP = 106

    // Vertical timing (per field)
    0xF0, 0x00,  // V = 240 (0x00F0)
    0x04,        // VFP = 4
    0x03,        // VS = 3
    0x0F,        // VBP = 15

    // PLL dividers (13.5 MHz from 50 MHz)
    0x1B,        // PLL_M0 = 27
    0x00,        // PLL_M1 = 0
    0x64,        // PLL_C0 = 100
    0x00,        // PLL_C1 = 0

    // PLL K (fractional, little-endian uint32)
    0x01, 0x00, 0x00, 0x00,  // PLL_K = 1

    0x01,        // CE_PIX = 1 (no division)
    0x01         // INTERLACED = 1 (interlaced framebuffer)
};
```

**Add Switchres initialization** (in main(), after mmap'ing AXI registers):
```cpp
int main(int argc, char** argv) {
    // ... existing mmap code ...

    // Map Switchres header region (DDR3 offset 8)
    uint8_t* switchres_hdr = (uint8_t*)mmap(NULL, SWITCHRES_HEADER_SIZE,
                                             PROT_WRITE,
                                             MAP_SHARED | MAP_SYNC,
                                             fd, SWITCHRES_HEADER_ADDR);
    if (switchres_hdr == MAP_FAILED) {
        fprintf(stderr, "Failed to mmap Switchres header: %s\n", strerror(errno));
        return 1;
    }

    // Write 480i modeline to DDR3
    memcpy(switchres_hdr, groovy_480i_modeline, SWITCHRES_HEADER_SIZE);
    munmap(switchres_hdr, SWITCHRES_HEADER_SIZE);

    // Trigger Switchres (frame 0 = apply immediately)
    printf("Triggering Switchres for 480i...\n");
    axi[AXI_SWITCHRES_IDX] = 0x1;  // bit 0 = trigger, bits[31:1] = frame 0

    // Wait for PLL to stabilize (100ms)
    usleep(100000);

    printf("Video reconfigured to 480i @ 59.94Hz\n");

    // Continue with normal video playback...
}
```

---

## Testing

### Test 1: Verify Header Write
```bash
# On MiSTer ARM, check if header was written:
devmem 0x30000008 32  # Should show 0x10020280 (H=640, HFP=16)
devmem 0x3000000C 32  # Should show 0x00F0606A (V=240, HS=96, HBP=106)
```

### Test 2: Trigger Switchres
```bash
# Write trigger bit:
devmem 0xFF20000C 32 0x1

# Check if video timing changed (monitor should sync to 15.7 kHz)
```

### Test 3: Full MP4 Player
```bash
# Load Groovy core with MISTER_FB enabled
# Toggle OSD "Video Mode → MP4"
# Launch daemon:
./mp4_play /media/fat/videos/test.mp4

# Expected: CRT displays 480i video @ 59.94Hz
```

---

## Expected Results

1. **Switchres Header Written:** DDR3 @ 0x30000008 contains 480i timing values
2. **PLL Reconfigured:** Pixel clock = 13.5 MHz (NTSC standard)
3. **Video Timing Changed:**
   - Horizontal frequency: 15.734 kHz
   - Vertical frequency: 59.94 Hz (interlaced)
   - Total lines: 525 (262.5 per field)
4. **CRT Displays:** Stable 480i video with proper NTSC timing

---

## Troubleshooting

### Issue: CRT doesn't sync
- Check if Switchres was triggered (read axi[3] back, should be 0 after trigger clears)
- Verify header was written to DDR3 @ 0x30000008
- Check PLL values are correct (M=27, C=100, K=1 for 13.5 MHz)

### Issue: FPGA doesn't reconfigure
- Verify cmd_switchres_mp4 wire is connected in sys_top.v
- Check Groovy.sv state machine enters S_Switchres_Header state
- Ensure fb_en=1 (MP4 mode active in OSD)

### Issue: Wrong refresh rate
- Verify PLL dividers (M/C/K) are correct for target pixel clock
- Check horizontal/vertical timing values match NTSC standard
- Use oscilloscope to measure actual horizontal frequency

---

## Next Steps

After implementing Switchres trigger:

1. **Implement Interlaced Field Fetching** (doc/crt_480i_plan.md Phase 2)
   - Modify fb_scan_out.sv to fetch only even/odd lines per field
   - Add field input from Groovy video timing
   - Halves DMA bandwidth (37 MB/s → 18.5 MB/s)

2. **Connect fb_scan_out to DDR3** (doc/crt_output_ddr3.md)
   - Add ram2_arbiter for DDR3 sharing
   - Remove fbs_avl_waitrequest=1 stall
   - Enable CRT output via VGA signals

3. **Test on Hardware**
   - Verify 480i CRT output works
   - Check interlaced field sync
   - Measure performance (frame rate, drops)

---

## References

- **doc/switchres_format.md** — 20-byte header specification
- **doc/crt_480i_plan.md** — Full implementation plan
- **rtl/hps_ext.v** — GroovyMAME EXT_BUS protocol (reference)
- **Groovy.sv lines 1195-1236** — Switchres state machine
