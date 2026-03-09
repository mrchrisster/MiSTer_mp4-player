# CRT Output via DDR3 (Current Setup)

## Goal
Enable fb_scan_out to read RGB framebuffer from DDR3 and output to Groovy's VGA signals for CRT display.

## Architecture

```
DDR3 ram1: yuv_fb_dma (YUV read + RGB write)
DDR3 ram2: Audio + fb_scan_out (shared via arbiter)
           ├─→ Audio system (low priority, ~1-2 MB/s)
           └─→ fb_scan_out (high priority, ~37 MB/s)
```

## Implementation Steps

### Step 1: Add ram2_arbiter.v to files.qip

Edit `files.qip` and add:
```tcl
set_global_assignment -name SYSTEMVERILOG_FILE rtl/ram2_arbiter.v
```

### Step 2: Modify sys_top.v - Add Arbiter Wiring

**Location:** Around line 840 (where ram2 wires are declared)

**Add these new wires for fb_scan_out:**
```verilog
// fb_scan_out DDR3 interface (connects to ram2 arbiter)
wire [28:0] fbs_ddr3_address;
wire [7:0]  fbs_ddr3_burstcount;
wire        fbs_ddr3_read;
wire        fbs_ddr3_waitrequest;
wire [63:0] fbs_ddr3_readdata;
wire        fbs_ddr3_readdatavalid;

// Audio system (existing ram2 connection, now goes to arbiter)
wire [28:0] audio_ram2_address;
wire [7:0]  audio_ram2_burstcount;
wire [7:0]  audio_ram2_byteenable;
wire [63:0] audio_ram2_writedata;
wire        audio_ram2_read;
wire        audio_ram2_write;
wire        audio_ram2_waitrequest;
wire [63:0] audio_ram2_readdata;
wire        audio_ram2_readdatavalid;
```

**Rename existing ram2 connections to audio_ram2:**

Find the existing audio system instantiation (around line 854) and change:
```verilog
// OLD:
// .ram_waitrequest(ram2_waitrequest),
// .ram_burstcnt(ram2_burstcount),
// etc.

// NEW:
.ram_waitrequest(audio_ram2_waitrequest),
.ram_burstcnt(audio_ram2_burstcount),
.ram_addr(audio_ram2_address),
.ram_readdata(audio_ram2_readdata),
.ram_read_ready(audio_ram2_readdatavalid),
.ram_read(audio_ram2_read),
.ram_writedata(audio_ram2_writedata),
.ram_byteenable(audio_ram2_byteenable),
.ram_write(audio_ram2_write),
```

### Step 3: Change sysmem ram2_clk to clk_sys

**Location:** Around line 815 in sysmem instantiation

**Change:**
```verilog
// OLD:
// .ram2_clk(clk_audio),

// NEW:
.ram2_clk(clk_sys),     // Changed from clk_audio to avoid clock crossing with fb_scan_out
```

This allows both fb_scan_out and ram2_arbiter to run on clk_sys, avoiding clock domain issues.

### Step 4: Instantiate ram2_arbiter

**Location:** After audio system, before sysmem instantiation

```verilog
// ram2 arbiter: audio + fb_scan_out share DDR3 ram2 port
// NOTE: Using clk_sys to avoid clock domain crossing with fb_scan_out
ram2_arbiter ram2_arb (
    .clk                (clk_sys),   // Using clk_sys to match fb_scan_out!
    .reset              (reset),

    // Master 0: Audio system (low priority)
    .m0_address         (audio_ram2_address),
    .m0_burstcount      (audio_ram2_burstcount),
    .m0_byteenable      (audio_ram2_byteenable),
    .m0_writedata       (audio_ram2_writedata),
    .m0_read            (audio_ram2_read),
    .m0_write           (audio_ram2_write),
    .m0_waitrequest     (audio_ram2_waitrequest),
    .m0_readdata        (audio_ram2_readdata),
    .m0_readdatavalid   (audio_ram2_readdatavalid),

    // Master 1: fb_scan_out (high priority)
    .m1_address         (fbs_ddr3_address),
    .m1_burstcount      (fbs_ddr3_burstcount),
    .m1_read            (fbs_ddr3_read),
    .m1_waitrequest     (fbs_ddr3_waitrequest),
    .m1_readdata        (fbs_ddr3_readdata),
    .m1_readdatavalid   (fbs_ddr3_readdatavalid),

    // Slave: DDR3 ram2 port (to sysmem)
    .s_address          (ram2_address),
    .s_burstcount       (ram2_burstcount),
    .s_byteenable       (ram2_byteenable),
    .s_writedata        (ram2_writedata),
    .s_read             (ram2_read),
    .s_write            (ram2_write),
    .s_waitrequest      (ram2_waitrequest),
    .s_readdata         (ram2_readdata),
    .s_readdatavalid    (ram2_readdatavalid)
);
```

### Step 5: Connect fb_scan_out to Arbiter

**Location:** In the `ifdef MISTER_FB` section (around line 432-445 based on MEMORY.md)

**Current state (from MEMORY.md):**
```verilog
// fbs_avl_* wires stalled: fb_scan_out cannot fetch, outputs black
assign fbs_avl_waitrequest = 1'b1;  // ← Currently stalled!
```

**Change to:**
```verilog
// Connect fb_scan_out Avalon interface to DDR3 ram2 (via arbiter)
// Note: Clock domain crossing - fb_scan_out runs on clk_vid, arbiter on clk_audio
// Add clock domain crossing FIFO or ensure clocks are synchronous

fb_scan_out fb_scan_inst (
    .clk               (clk_vid),
    .reset             (reset),

    // Control
    .fb_en             (fb_en),
    .fb_base           (fb_base_sel),      // DDR3 front buffer (0x30000000 or 0x30096000)
    .fb_width          (FB_WIDTH),
    .fb_height         (FB_HEIGHT),
    .fb_stride         (FB_STRIDE),

    // Avalon master → ram2 arbiter
    // WARNING: Clock domain crossing if clk_vid != clk_audio!
    .avl_address       (fbs_ddr3_address),
    .avl_burstcount    (fbs_ddr3_burstcount),
    .avl_read          (fbs_ddr3_read),
    .avl_waitrequest   (fbs_ddr3_waitrequest),
    .avl_readdata      (fbs_ddr3_readdata),
    .avl_readdatavalid (fbs_ddr3_readdatavalid),

    // Core video timing (Groovy's native CRT timing)
    .ce_pix            (ce_pix),
    .de_in             (de_emu),
    .hs_in             (hs_emu),
    .vs_in             (vs_emu),

    // RGB output (goes to VGA mux, already in sys_top.v)
    .r_out             (fbs_r),
    .g_out             (fbs_g),
    .b_out             (fbs_b),
    .de_out            (fbs_de),
    .hs_out            (fbs_hs),
    .vs_out            (fbs_vs)
);
```

### Step 6: Verify VGA Mux (Should Already Exist)

**Location:** End of sys_top.v, VGA output assignment

Should already have:
```verilog
`ifdef MISTER_FB
assign VGA_R = fb_en ? fbs_r : emu_r;
assign VGA_G = fb_en ? fbs_g : emu_g;
assign VGA_B = fb_en ? fbs_b : emu_b;
assign VGA_HS = fb_en ? fbs_hs : emu_hs;
assign VGA_VS = fb_en ? fbs_vs : emu_vs;
`else
assign VGA_R = emu_r;
assign VGA_G = emu_g;
assign VGA_B = emu_b;
assign VGA_HS = emu_hs;
assign VGA_VS = emu_vs;
`endif
```

## Clock Domain Considerations

**CONFIRMED ISSUE:**
- `CLK_VIDEO = clk_sys` (~83 MHz)
- `CLK_AUDIO = 24.576 MHz`
- `ram2_clk = clk_audio` (in sysmem)
- fb_scan_out needs to run on clk_sys for video timing

**This IS a clock domain crossing!**

### Solution: Use Async DCFIFO

Add a Dual-Clock FIFO between fb_scan_out (clk_sys) and ram2_arbiter (clk_audio):

```verilog
// Clock domain crossing FIFO for fb_scan_out Avalon requests
dcfifo_avalon_adapter fbs_cdc_fifo (
    // Write side: fb_scan_out (clk_sys domain)
    .wrclk             (clk_sys),
    .wravl_address     (fbs_avl_address),      // From fb_scan_out
    .wravl_burstcount  (fbs_avl_burstcount),
    .wravl_read        (fbs_avl_read),
    .wravl_waitrequest (fbs_avl_waitrequest),
    .wravl_readdata    (fbs_avl_readdata),
    .wravl_readdatavalid(fbs_avl_readdatavalid),

    // Read side: ram2_arbiter (clk_audio domain)
    .rdclk             (clk_audio),
    .rdavl_address     (fbs_ddr3_address),     // To ram2_arbiter
    .rdavl_burstcount  (fbs_ddr3_burstcount),
    .rdavl_read        (fbs_ddr3_read),
    .rdavl_waitrequest (fbs_ddr3_waitrequest),
    .rdavl_readdata    (fbs_ddr3_readdata),
    .rdavl_readdatavalid(fbs_ddr3_readdatavalid)
);
```

**Alternatively (Simpler):** Just run ram2_arbiter on clk_sys instead of clk_audio:
- Change ram2_arbiter instantiation to use `clk_sys`
- fb_scan_out stays on `clk_sys` - no crossing!
- Audio system would need to cross from `clk_audio` to `clk_sys` (but it likely already does internally)

**Recommendation:** Try the simpler approach first (run arbiter on clk_sys).

## Expected Result

Once wired up:
1. Compile FPGA
2. Load Groovy core on MiSTer
3. Toggle OSD "Video Mode → MP4"
4. Launch mp4_play
5. **CRT should display video via VGA output!**

## Troubleshooting

### Issue: Black screen on CRT
- Check fb_scan_out is instantiated and connected
- Verify fb_en=1 when MP4 mode active
- Check fb_base points to correct DDR3 buffer

### Issue: Corrupted/garbled image
- Clock domain crossing issue - add async FIFO
- Check address calculation in fb_scan_out

### Issue: Audio glitches
- Audio losing DDR3 access to fb_scan_out priority
- Increase audio buffer size or adjust arbiter priority

## Performance Impact

**DDR3 ram2 bandwidth usage:**
- Audio: ~1-2 MB/s (low)
- fb_scan_out: 614KB × 60Hz = ~37 MB/s (medium)
- **Total: ~39 MB/s** (well within DDR3 capability)

Should work fine with simple priority arbiter.

## Next Steps

1. Add ram2_arbiter.v to files.qip
2. Modify sys_top.v per above
3. Resolve clock domain crossing
4. Compile and test on hardware
5. Verify CRT output works!
