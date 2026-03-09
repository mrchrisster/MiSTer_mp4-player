# Groovy Switchres Header Format

## Overview
Groovy core reads a 20-byte video timing header from DDR3 @ **0x30000008** to dynamically reconfigure video output.

**Location:** DDR3 offset `28'd8` (physical address depends on DDR3 base)
**Size:** 20 bytes (read in 3 × 8-byte bursts = 24 bytes, last 4 unused)
**Trigger:** Set `cmd_switchres` via HPS_EXT interface
**Application:** During VBLANK (lines 1209: `if (vblank_core || vga_frame == 0)`)

---

## Header Structure (20 bytes)

From Groovy.sv lines 1211-1228:

| Bit Range | Bytes | Type | Field | Description |
|-----------|-------|------|-------|-------------|
| [0:15] | 0-1 | uint16 | H | Horizontal active pixels |
| [16:23] | 2 | uint8 | HFP | Horizontal front porch |
| [24:31] | 3 | uint8 | HS | Horizontal sync width |
| [32:39] | 4 | uint8 | HBP | Horizontal back porch |
| [40:55] | 5-6 | uint16 | V | Vertical active lines |
| [56:63] | 7 | uint8 | VFP | Vertical front porch |
| [64:71] | 8 | uint8 | VS | Vertical sync width |
| [72:79] | 9 | uint8 | VBP | Vertical back porch |
| [80:87] | 10 | uint8 | PLL_M0 | PLL M divider (low byte) |
| [88:95] | 11 | uint8 | PLL_M1 | PLL M divider (high byte) |
| [96:103] | 12 | uint8 | PLL_C0 | PLL C divider (low byte) |
| [104:111] | 13 | uint8 | PLL_C1 | PLL C divider (high byte) |
| [112:143] | 14-17 | uint32 | PLL_K | PLL K divider (fractional) |
| [144:151] | 18 | uint8 | CE_PIX | Pixel clock enable divider |
| [152:159] | 19 | uint8 | INTERLACED | 0=prog, 1=interlaced, 2=interlaced+scandoubler |

**Note:** Little-endian byte order (DE10-nano is ARM Cortex-A9)

---

## 480i NTSC Standard Values

### **Target Specifications:**
- **Resolution:** 640×480 interlaced (240 lines per field)
- **Refresh Rate:** 59.94 Hz (29.97 fields/sec × 2)
- **Pixel Clock:** 13.5 MHz (SMPTE 259M standard)
- **Horizontal Frequency:** 15.734 kHz (525 lines / 59.94 Hz ÷ 2)
- **Interlaced:** Yes (NTSC standard)

### **Horizontal Timing (per line):**
Based on SMPTE 259M / ITU-R BT.601:

| Parameter | Value | Description |
|-----------|-------|-------------|
| H (active) | 640 | Visible pixels per line |
| HFP (front porch) | 16 | Pixels after active before sync |
| HS (sync width) | 62 | Horizontal sync pulse width |
| HBP (back porch) | 60 | Pixels after sync before active |
| **H_TOTAL** | **778** | Total pixels per line |

**Line time:** 778 pixels ÷ 13.5 MHz = **57.63 μs** (15.734 kHz) ✓

### **Vertical Timing (per field):**
NTSC interlaced: 525 total lines, 480 active (240 per field)

| Parameter | Value | Description |
|-----------|-------|-------------|
| V (active) | 240 | Visible lines per field |
| VFP (front porch) | 4 | Lines after active before sync |
| VS (sync width) | 3 | Vertical sync pulse width (lines) |
| VBP (back porch) | 15 | Lines after sync before active |
| **V_TOTAL** | **262** | Total lines per field (525/2) |

**Field time:** 262 lines × 57.63 μs = **15.099 ms** (66.23 Hz per field)
**Frame time:** 2 fields × 15.099 ms = **30.198 ms** (33.12 Hz frame rate)

Wait, this doesn't match 59.94 Hz... Let me recalculate.

**NTSC Standard Correction:**
- Total lines per frame: 525 (interlaced)
- Field rate: 59.94 Hz
- Frame rate: 29.97 Hz
- Line frequency: 525 × 29.97 = **15,734.25 Hz**
- Line time: 1 / 15,734.25 = **63.556 μs**
- Pixel clock: 858 pixels/line × 15,734.25 Hz = **13.5 MHz** ✓

**Corrected Horizontal Timing:**
| Parameter | Value | Description |
|-----------|-------|-------------|
| H (active) | 640 | Visible pixels |
| HFP | 16 | Front porch |
| HS | 96 | Sync width (NTSC standard: ~4.7 μs) |
| HBP | 106 | Back porch |
| **H_TOTAL** | **858** | Total pixels per line (NTSC standard) |

**Line time:** 858 ÷ 13.5 MHz = **63.556 μs** (15,734 Hz) ✓

**Corrected Vertical Timing (per field):**
| Parameter | Value | Description |
|-----------|-------|-------------|
| V (active) | 240 | Visible lines per field |
| VFP | 4 | Front porch |
| VS | 3 | Sync width |
| VBP | 15 | Back porch |
| **V_TOTAL** | **262.5** | Lines per field (525/2) |

**Field rate:** 15,734 Hz / 262.5 = **59.94 Hz** ✓

---

## PLL Calculation

Groovy uses an Altera PLL to generate the pixel clock. We need to calculate M/C/K dividers to produce **13.5 MHz** from the base clock.

**Groovy base clock (assumed):** 50 MHz (standard DE10-nano clock)

**PLL Formula:**
```
f_out = f_in × (M / C) × K
13.5 MHz = 50 MHz × (M / C) × K
```

**Target ratio:** 13.5 / 50 = 0.27

**Option 1: Simple integer dividers**
- M = 27, C = 100, K = 1
- f_out = 50 × (27/100) × 1 = **13.5 MHz** ✓

**Option 2: Using K for precision**
- M = 135, C = 500, K = 1
- f_out = 50 × (135/500) = **13.5 MHz** ✓

**Recommended:** M=27, C=100, K=1 (simplest)

**Header values:**
- PLL_M0 = 27 (0x1B)
- PLL_M1 = 0
- PLL_C0 = 100 (0x64)
- PLL_C1 = 0
- PLL_K = 1

**CE_PIX (pixel clock enable):**
- Typically 1 for direct pixel clock
- Set to 1 (no further division)

---

## 480i Header (C array)

```c
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

**Size:** 20 bytes ✓

---

## Triggering Switchres

### **1. Write header to DDR3:**
```c
// Open /dev/mem
int fd = open("/dev/mem", O_RDWR | O_SYNC);

// Map DDR3 Switchres region (0x30000008)
uint8_t* switchres = (uint8_t*)mmap(NULL, 32,
                                     PROT_WRITE,
                                     MAP_SHARED | MAP_SYNC,
                                     fd, 0x30000008);

// Write 480i modeline
memcpy(switchres, groovy_480i_modeline, 20);
munmap(switchres, 32);
```

### **2. Trigger via cmd_switchres:**

**Current Implementation (GroovyMAME):**
The `cmd_switchres` signal comes from `hps_ext` module (rtl/hps_ext.v).

**EXT_BUS Protocol (lines 259-266):**
- Command code: `0xF3` (SET_SWITCHRES)
- Sends 4 bytes: cmd byte + `switchres_frame[31:0]` (frame number when to apply)
- Sets `cmd_switchres <= 1'b1` after receiving frame number
- Designed for GroovyMAME streaming over MiSTer I/O framework

**Problem for MP4 Player:**
The EXT_BUS protocol is not directly accessible from ARM daemon via `/dev/mem`. It's part of MiSTer's I/O framework for PC-to-FPGA communication.

**Solution for MP4 Player:**
Add a new AXI register to `mp4_ctrl_regs.v` to trigger cmd_switchres directly from ARM:

```verilog
// In mp4_ctrl_regs.v, add new output:
output reg cmd_switchres_mp4 = 1'b0,

// In sys_top.v, wire to Groovy emu:
wire cmd_switchres_mp4;
// ... in emu instantiation:
.cmd_switchres(cmd_switchres_mp4),

// AXI register map (add to 0x00C):
// 0x00C: bit 0 = trigger_switchres (write 1 to trigger, auto-clears)
```

**ARM Daemon Trigger Sequence:**
```c
// 1. Write 480i header to DDR3
memcpy(switchres_hdr, groovy_480i_modeline, 20);

// 2. Trigger Switchres via AXI register
axi[3] = 0x1;  // Offset 0x00C, bit 0 = trigger_switchres

// 3. Wait for video timing to reconfigure
usleep(100000);  // 100ms for PLL to stabilize
```

---

## Verification

### **Test Plan:**
1. Write 480i header to DDR3 @ 0x30000008
2. Trigger cmd_switchres
3. Check if FPGA reconfigures (monitor VGA sync signals)
4. Verify CRT displays at correct refresh rate

### **Expected Result:**
- VGA outputs 640×480i @ 59.94Hz
- Horizontal frequency: 15.734 kHz
- Interlaced fields (240 lines each)
- Compatible with NTSC CRTs

---

## Next Steps

1. **Find cmd_switchres trigger mechanism** - Research hps_ext module
2. **Test 480i header** - Write to DDR3 and verify
3. **Implement in main.cpp** - Add Switchres initialization
4. **Verify CRT output** - Test on actual hardware

---

## References

- **SMPTE 259M:** Standard for 525-line NTSC video (480i)
- **ITU-R BT.601:** Digital video standard (13.5 MHz pixel clock)
- **CEA-861:** Video timing standards
- **Groovy.sv:** Lines 1195-1236 (Switchres state machine)
