# Plan: SDRAM Framebuffer for MP4 Player (Simplified - FPGA Only)

## Goal:
Move RGB framebuffers from DDR3 to dedicated SDRAM to eliminate DDR3 contention and reduce DMA latency. YUV buffers remain in DDR3 (ARM accessible). ARM has NO access to SDRAM.

---

## Architecture Overview

### **Data Flow:**
```
1. ARM decodes → YUV in DDR3 (460KB/frame)
2. FPGA DMA reads YUV from DDR3 (via ram1)
3. FPGA DMA converts YUV→RGB (yuv_to_rgb.sv pipeline)
4. FPGA DMA writes RGB to SDRAM (614KB/frame)
5. ASCAL reads RGB from SDRAM for display
```

### **Benefits:**
- **No ARM-to-SDRAM bridge needed** - ARM never touches SDRAM
- **Reduces DDR3 traffic by 58%:**
  - Old: 2.1 MB/frame (YUV write + YUV read + RGB write + RGB read)
  - New: 920 KB/frame (YUV write + YUV read only)
- **ASCAL gets dedicated SDRAM bandwidth** - no contention
- **FPGA DMA writes to fast SDRAM** - no DDR3 arbiter delays

---

## Phase 1: Memory Layout

### **DDR3 (ARM accessible, 768MB offset):**
```
Address      | Size       | Description
-------------|------------|---------------------------
0x3012C000   | 307,200 B  | YUV Y plane (640×480)
0x30177000   | 76,800 B   | YUV U plane (320×240)
0x30189C00   | 76,800 B   | YUV V plane (320×240)
```
**Total: 460 KB** (unchanged from current)

### **SDRAM (FPGA only, 128MB):**
```
SDRAM Offset | Size       | Description
-------------|------------|---------------------------
0x00000000   | 614,400 B  | RGB Buffer A (front/back)
0x00096000   | 614,400 B  | RGB Buffer B (front/back)
0x0012C000   | ~126 MB    | Reserved/unused
```
**Total used: 1.2 MB** (plenty of headroom)

---

## Phase 2: FPGA Architecture Changes

### 2.1 - SDRAM Arbiter: 2-to-1 (DMA + ASCAL)

**Masters accessing SDRAM:**
1. **FPGA DMA** (yuv_fb_dma) - writes RGB
2. **ASCAL** - reads RGB for display

**No HPS master needed!**

```verilog
// rtl/sdram_arbiter_2to1.v - New file
module sdram_arbiter_2to1 (
    input wire        clk,
    input wire        reset,

    // Master 0: FPGA DMA (yuv_fb_dma RGB writes)
    input  wire [28:0] m0_addr,
    input  wire [63:0] m0_writedata,
    input  wire        m0_write,
    input  wire        m0_read,
    input  wire [7:0]  m0_burstcount,
    output wire [63:0] m0_readdata,
    output wire        m0_waitrequest,

    // Master 1: ASCAL (RGB framebuffer reads)
    input  wire [28:0] m1_addr,
    input  wire        m1_read,
    input  wire [7:0]  m1_burstcount,
    output wire [63:0] m1_readdata,
    output wire        m1_waitrequest,

    // Slave: SDRAM controller
    output reg  [28:0] s_addr,
    output reg  [63:0] s_writedata,
    output reg         s_write,
    output reg         s_read,
    output reg  [7:0]  s_burstcount,
    input  wire [63:0] s_readdata,
    input  wire        s_waitrequest
);

    // Arbiter logic: ASCAL has priority (time-critical display)
    // Round-robin or priority-based arbitration
    // See implementation details below

endmodule
```

**Arbiter Priority:**
1. **ASCAL (highest)** - time-critical for display, must not stall
2. **FPGA DMA (lower)** - can tolerate some latency

---

### 2.2 - yuv_fb_dma.v: Dual Avalon Ports

**Current:** Single Avalon master port to DDR3 (reads YUV, writes RGB)

**New:** Split into two Avalon master ports:
- **Port A (DDR3):** Read YUV only
- **Port B (SDRAM):** Write RGB only

```verilog
// rtl/yuv_fb_dma.v modifications
module yuv_fb_dma (
    input  wire        clk,
    input  wire        reset,

    // Control interface (unchanged)
    input  wire        dma_trigger,
    input  wire [31:0] yuv_y_base,   // DDR3 address (0x3012C000)
    input  wire [31:0] yuv_u_base,   // DDR3 address (0x30177000)
    input  wire [31:0] yuv_v_base,   // DDR3 address (0x30189C00)
    input  wire [31:0] rgb_base,     // SDRAM address (0x00000000 or 0x00096000)
    output wire        dma_done,

    // Avalon Master Port A: DDR3 reads (YUV)
    output wire [31:0] ddr3_address,
    output wire        ddr3_read,
    output wire [7:0]  ddr3_burstcount,
    input  wire [63:0] ddr3_readdata,
    input  wire        ddr3_readdatavalid,
    input  wire        ddr3_waitrequest,

    // Avalon Master Port B: SDRAM writes (RGB)
    output wire [28:0] sdram_address,
    output wire [63:0] sdram_writedata,
    output wire        sdram_write,
    output wire [7:0]  sdram_burstcount,
    input  wire        sdram_waitrequest
);

    // FSM: Read Y from DDR3 → Read U from DDR3 → Read V from DDR3
    //      → Convert YUV→RGB (yuv_to_rgb pipeline)
    //      → Write RGB to SDRAM

    // Internal YUV buffers (same as before)
    reg [7:0] y_buf [0:640*480-1];
    reg [7:0] u_buf [0:320*240-1];
    reg [7:0] v_buf [0:320*240-1];

    // RGB output buffer
    reg [15:0] rgb_buf [0:640*480-1];

    // State machine reads Y/U/V from DDR3, writes RGB to SDRAM
    // (existing logic adapted for dual ports)

endmodule
```

---

### 2.3 - sys_top.v: Wire Dual Ports

```verilog
// sys/sys_top.v additions

// ============ SDRAM Arbiter Wiring ============

// FPGA DMA → SDRAM (RGB writes)
wire [28:0] dma_sdram_addr;
wire [63:0] dma_sdram_writedata;
wire        dma_sdram_write;
wire [7:0]  dma_sdram_burstcount;
wire        dma_sdram_waitrequest;

// ASCAL → SDRAM (RGB reads)
wire [28:0] ascal_sdram_addr;
wire        ascal_sdram_read;
wire [7:0]  ascal_sdram_burstcount;
wire [63:0] ascal_sdram_readdata;
wire        ascal_sdram_waitrequest;

// SDRAM arbiter output
wire [28:0] sdram_arb_addr;
wire [63:0] sdram_arb_writedata;
wire        sdram_arb_write;
wire        sdram_arb_read;
wire [7:0]  sdram_arb_burstcount;
wire [63:0] sdram_arb_readdata;
wire        sdram_arb_waitrequest;

// Instantiate SDRAM arbiter
sdram_arbiter_2to1 sdram_arb (
    .clk            (clk_sys),
    .reset          (reset),

    // Master 0: FPGA DMA
    .m0_addr        (dma_sdram_addr),
    .m0_writedata   (dma_sdram_writedata),
    .m0_write       (dma_sdram_write),
    .m0_read        (1'b0),  // DMA only writes to SDRAM
    .m0_burstcount  (dma_sdram_burstcount),
    .m0_readdata    (),
    .m0_waitrequest (dma_sdram_waitrequest),

    // Master 1: ASCAL
    .m1_addr        (ascal_sdram_addr),
    .m1_read        (ascal_sdram_read),
    .m1_burstcount  (ascal_sdram_burstcount),
    .m1_readdata    (ascal_sdram_readdata),
    .m1_waitrequest (ascal_sdram_waitrequest),

    // Slave: SDRAM controller (existing emu SDRAM interface)
    .s_addr         (sdram_arb_addr),
    .s_writedata    (sdram_arb_writedata),
    .s_write        (sdram_arb_write),
    .s_read         (sdram_arb_read),
    .s_burstcount   (sdram_arb_burstcount),
    .s_readdata     (sdram_arb_readdata),
    .s_waitrequest  (sdram_arb_waitrequest)
);

// Connect arbiter to emu SDRAM port
assign SDRAM_ADDR = sdram_arb_addr;
assign SDRAM_DIN  = sdram_arb_writedata;
assign SDRAM_WE   = sdram_arb_write;
assign SDRAM_RD   = sdram_arb_read;
// ... (other SDRAM signals)

// yuv_fb_dma instantiation with dual ports
yuv_fb_dma yuv_fb_dma_inst (
    .clk                (clk_sys),
    .reset              (reset),

    // Control
    .dma_trigger        (dma_trigger),
    .yuv_y_base         (yuv_y_base),
    .yuv_u_base         (yuv_u_base),
    .yuv_v_base         (yuv_v_base),
    .rgb_base           (yuv_rgb_base),  // SDRAM address (0x0 or 0x96000)
    .dma_done           (dma_done),

    // Port A: DDR3 reads (YUV)
    .ddr3_address       (dma_ddr3_addr),
    .ddr3_read          (dma_ddr3_read),
    .ddr3_burstcount    (dma_ddr3_burstcount),
    .ddr3_readdata      (ram1_readdata),  // Connect to existing ram1
    .ddr3_readdatavalid (ram1_readdatavalid),
    .ddr3_waitrequest   (ram1_waitrequest),

    // Port B: SDRAM writes (RGB)
    .sdram_address      (dma_sdram_addr),
    .sdram_writedata    (dma_sdram_writedata),
    .sdram_write        (dma_sdram_write),
    .sdram_burstcount   (dma_sdram_burstcount),
    .sdram_waitrequest  (dma_sdram_waitrequest)
);
```

---

### 2.4 - ASCAL: Connect to SDRAM

**sys_top.v ASCAL connections:**

```verilog
// ASCAL Avalon master reads from SDRAM arbiter
assign ascal_sdram_addr       = ASCAL_avl_address[28:0];  // Convert to SDRAM address space
assign ascal_sdram_read       = ASCAL_avl_read;
assign ascal_sdram_burstcount = ASCAL_avl_burstcount;
assign ASCAL_avl_readdata     = ascal_sdram_readdata;
assign ASCAL_avl_waitrequest  = ascal_sdram_waitrequest;
```

**Groovy.sv FB_BASE update:**

```verilog
`ifdef MISTER_FB
assign FB_EN         = status[60];
assign FB_FORMAT     = 5'd4;        // BGR565
assign FB_WIDTH      = 12'd640;
assign FB_HEIGHT     = 12'd480;
assign FB_BASE       = 32'h00000000; // SDRAM base (was 0x30000000 DDR3)
assign FB_STRIDE     = 14'd1280;
assign FB_FORCE_BLANK = 1'b0;
`endif
```

**Critical:** ASCAL must be configured to use SDRAM address space (0x00000000) instead of DDR3 (0x30000000).

---

### 2.5 - Groovy.sv: SDRAM Port Connection

**Add SDRAM port to emu module:**

```verilog
// Groovy.sv
module emu (
    // ... existing ports ...

`ifdef MISTER_FB
    // Framebuffer ports (ASCAL connection)
    output [12:0] FB_WIDTH,
    output [12:0] FB_HEIGHT,
    output [31:0] FB_BASE,
    // ... other FB ports ...
`endif

    // SDRAM interface (for RGB framebuffer)
    output [28:0] SDRAM_ADDR,
    output [63:0] SDRAM_DIN,
    output        SDRAM_WE,
    output        SDRAM_RD,
    input  [63:0] SDRAM_DOUT,
    input         SDRAM_READY
    // ... other SDRAM signals ...
);
```

**Note:** Groovy core may already have SDRAM ports for game ROMs. We'll share the same SDRAM but with arbiter managing access.

---

## Phase 3: ARM Software Changes

### 3.1 - Minimal Changes to main.cpp

**YUV buffers stay in DDR3 (no changes to mmap):**

```cpp
// h264-daemon/main.cpp

// YUV planes in DDR3 (unchanged)
#define YUV_Y_BASE_PHYS   0x3012C000  // DDR3
#define YUV_U_BASE_PHYS   0x30177000  // DDR3
#define YUV_V_BASE_PHYS   0x30189C00  // DDR3

// RGB buffers now in SDRAM (FPGA writes, ARM never accesses)
// Update AXI register to tell FPGA where to write
#define SDRAM_RGB_BUF_A   0x00000000  // SDRAM space
#define SDRAM_RGB_BUF_B   0x00096000  // SDRAM space

// In play_video():
// ARM writes YUV to DDR3 (unchanged)
uint8_t* yuv_y = (uint8_t*)mmap(NULL, 307200, PROT_WRITE, MAP_SHARED | MAP_SYNC, fd, YUV_Y_BASE_PHYS);
uint8_t* yuv_u = (uint8_t*)mmap(NULL, 76800,  PROT_WRITE, MAP_SHARED | MAP_SYNC, fd, YUV_U_BASE_PHYS);
uint8_t* yuv_v = (uint8_t*)mmap(NULL, 76800,  PROT_WRITE, MAP_SHARED | MAP_SYNC, fd, YUV_V_BASE_PHYS);

// Set RGB base to SDRAM address (FPGA will write there)
volatile uint32_t* axi = (uint32_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFF200000);
axi[AXI_YUV_RGB_BASE_IDX] = back_buffer ? SDRAM_RGB_BUF_B : SDRAM_RGB_BUF_A;

// Trigger FPGA DMA (reads YUV from DDR3, writes RGB to SDRAM)
axi[AXI_CTRL_IDX] |= 0x2;  // dma_trigger
```

**No direct RGB buffer access by ARM anymore** - FPGA handles everything!

---

## Phase 4: Implementation Details

### 4.1 - SDRAM Arbiter Priority Logic

**Simple priority arbiter:**

```verilog
// rtl/sdram_arbiter_2to1.v
always @(*) begin
    // ASCAL has priority (display is time-critical)
    if (m1_read && !m1_waitrequest) begin
        s_addr       = m1_addr;
        s_read       = 1'b1;
        s_write      = 1'b0;
        s_burstcount = m1_burstcount;
        m1_waitrequest = s_waitrequest;
        m0_waitrequest = 1'b1;  // Stall DMA
    end
    // FPGA DMA gets access when ASCAL idle
    else if (m0_write && !m0_waitrequest) begin
        s_addr       = m0_addr;
        s_write      = 1'b1;
        s_read       = 1'b0;
        s_writedata  = m0_writedata;
        s_burstcount = m0_burstcount;
        m0_waitrequest = s_waitrequest;
        m1_waitrequest = 1'b1;  // Stall ASCAL if it tries to read
    end
    else begin
        // Idle
        s_read  = 1'b0;
        s_write = 1'b0;
        m0_waitrequest = 1'b0;
        m1_waitrequest = 1'b0;
    end
end

// Read data routing
assign m0_readdata = s_readdata;  // (DMA doesn't read from SDRAM)
assign m1_readdata = s_readdata;  // ASCAL reads
```

---

### 4.2 - yuv_fb_dma FSM Changes

**Pseudo-code for dual-port DMA:**

```verilog
// State machine
localparam S_IDLE        = 0;
localparam S_READ_Y      = 1;
localparam S_READ_U      = 2;
localparam S_READ_V      = 3;
localparam S_CONVERT_RGB = 4;
localparam S_WRITE_RGB   = 5;
localparam S_DONE        = 6;

always @(posedge clk) begin
    case (state)
        S_IDLE: begin
            if (dma_trigger) state <= S_READ_Y;
        end

        S_READ_Y: begin
            // Issue DDR3 read burst for Y plane
            ddr3_address <= yuv_y_base + offset;
            ddr3_read    <= 1'b1;
            // Store readdatavalid into y_buf[]
            if (y_done) state <= S_READ_U;
        end

        S_READ_U: begin
            // Issue DDR3 read burst for U plane
            ddr3_address <= yuv_u_base + offset;
            ddr3_read    <= 1'b1;
            if (u_done) state <= S_READ_V;
        end

        S_READ_V: begin
            // Issue DDR3 read burst for V plane
            ddr3_address <= yuv_v_base + offset;
            ddr3_read    <= 1'b1;
            if (v_done) state <= S_CONVERT_RGB;
        end

        S_CONVERT_RGB: begin
            // Feed Y/U/V through yuv_to_rgb pipeline
            // Store results in rgb_buf[]
            if (rgb_ready) state <= S_WRITE_RGB;
        end

        S_WRITE_RGB: begin
            // Issue SDRAM write bursts for RGB
            sdram_address   <= rgb_base + offset;
            sdram_writedata <= rgb_buf[offset];
            sdram_write     <= 1'b1;
            if (rgb_write_done) state <= S_DONE;
        end

        S_DONE: begin
            dma_done <= 1'b1;
            state    <= S_IDLE;
        end
    endcase
end
```

---

## Phase 5: Testing & Validation

### 5.1 - FPGA Compilation Checklist
- [ ] Add `rtl/sdram_arbiter_2to1.v` to files.qip
- [ ] Modify `rtl/yuv_fb_dma.v` for dual Avalon ports
- [ ] Update `sys/sys_top.v` with arbiter and wiring
- [ ] Update `Groovy.sv` FB_BASE to 0x00000000
- [ ] Compile and verify timing closure (especially SDRAM @ 100-133 MHz)

### 5.2 - ARM Testing Steps
1. **Test YUV write to DDR3:** Write test pattern, verify with devmem
2. **Test FPGA DMA trigger:** Trigger DMA, check dma_done bit
3. **Test video playback:** Play simple video, verify display
4. **Measure DMA timing:** Check if 9-15ms → ~4-6ms improvement

### 5.3 - Performance Expectations

**DDR3 Traffic Reduction:**
| Metric | Before (DDR3 only) | After (SDRAM RGB) | Reduction |
|--------|-------------------|-------------------|-----------|
| ARM YUV write | 460 KB × 30fps = 13.8 MB/s | 13.8 MB/s | 0% |
| FPGA YUV read | 460 KB × 30fps = 13.8 MB/s | 13.8 MB/s | 0% |
| FPGA RGB write | 614 KB × 30fps = 18.4 MB/s | **0** (→SDRAM) | **-100%** |
| ASCAL RGB read | 614 KB × 60Hz = 36.8 MB/s | **0** (→SDRAM) | **-100%** |
| **Total DDR3** | **82.8 MB/s** | **27.6 MB/s** | **-67%** |

**SDRAM Traffic:**
| Operation | Bandwidth |
|-----------|-----------|
| FPGA RGB write | 18.4 MB/s |
| ASCAL RGB read | 36.8 MB/s |
| **Total SDRAM** | **55.2 MB/s** |
| SDRAM capacity | 200-400 MB/s |
| **Utilization** | **14-28%** ✓ |

**Expected DMA Time:**
- Current: 9-15ms (DDR3 contention)
- Target: **4-6ms** (SDRAM write, reduced DDR3 contention on read)

**Expected Decode Time:**
- Current: 24-43ms
- Target: **20-38ms** (15% improvement from extra DDR3 bandwidth)

**Expected Result:**
- **Eliminate or greatly reduce lag on ALF intro** ✓

---

## Phase 6: Implementation Order

### Step 1: Create SDRAM Arbiter
- Write `rtl/sdram_arbiter_2to1.v`
- Simple priority arbiter: ASCAL > DMA
- Test: Simulate with dummy traffic

### Step 2: Modify yuv_fb_dma
- Split into dual Avalon ports (DDR3 read + SDRAM write)
- Update FSM for separate read/write paths
- Test: Simulate DMA flow

### Step 3: Wire sys_top.v
- Instantiate arbiter
- Connect yuv_fb_dma dual ports
- Connect ASCAL to SDRAM
- Connect arbiter to emu SDRAM port

### Step 4: Update Groovy.sv
- Change FB_BASE from 0x30000000 → 0x00000000
- Verify SDRAM port wiring

### Step 5: Update ARM Code
- Change yuv_rgb_base to SDRAM addresses (0x0, 0x96000)
- Remove direct RGB buffer access (no longer needed)

### Step 6: Compile & Test
- Compile FPGA (verify timing)
- Test on hardware
- Measure performance improvements

---

## Challenges & Mitigations

### Challenge 1: SDRAM Bandwidth
- **Risk:** SDRAM might be too slow for 55 MB/s traffic
- **Mitigation:** SDRAM @ 133 MHz with 16-bit bus = 266 MB/s theoretical, ~200 MB/s practical. We need 55 MB/s. **Safe margin.**

### Challenge 2: ASCAL SDRAM Compatibility
- **Risk:** ASCAL might not work with SDRAM (expects DDR3)
- **Mitigation:** ASCAL uses generic Avalon MM interface. SDRAM arbiter provides Avalon slave. **Should work.**

### Challenge 3: Arbiter Latency
- **Risk:** Arbiter adds latency, slowing ASCAL display
- **Mitigation:** Priority-based arbiter gives ASCAL immediate access. Latency < 1 clock cycle. **Negligible.**

### Challenge 4: yuv_fb_dma Dual Port Complexity
- **Risk:** Splitting into dual ports is complex, might have bugs
- **Mitigation:** Careful FSM design, thorough simulation, incremental testing

---

## Architecture Diagrams

### **Before (DDR3 Contention):**
```
┌─────────────────────────────────────────────────┐
│                    DDR3 (1GB)                   │
│  ┌──────────────────────────────────────────┐   │
│  │ ARM: YUV write (13.8 MB/s)              │   │
│  │ FPGA DMA: YUV read (13.8 MB/s)          │   │
│  │ FPGA DMA: RGB write (18.4 MB/s)         │   │
│  │ ASCAL: RGB read (36.8 MB/s)             │   │
│  │ TOTAL: 82.8 MB/s                        │   │
│  └──────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
         ↑ 4 masters = contention = slow DMA
```

### **After (SDRAM Offload):**
```
┌──────────────────────┐  ┌──────────────────────┐
│  DDR3 (1GB)          │  │  SDRAM (128MB)       │
│  ┌────────────────┐  │  │  ┌────────────────┐  │
│  │ ARM: YUV write │  │  │  │ FPGA: RGB write│  │
│  │   (13.8 MB/s)  │  │  │  │   (18.4 MB/s)  │  │
│  │                │  │  │  │                │  │
│  │ FPGA: YUV read │  │  │  │ ASCAL: RGB read│  │
│  │   (13.8 MB/s)  │  │  │  │   (36.8 MB/s)  │  │
│  │                │  │  │  │                │  │
│  │ TOTAL: 27.6 MB/s│ │  │  │ TOTAL: 55.2 MB/s│ │
│  └────────────────┘  │  │  └────────────────┘  │
└──────────────────────┘  └──────────────────────┘
   ↑ 67% less traffic      ↑ Dedicated access
```

---

## Current Status
- **Phase:** Planning complete
- **Next:** Implement SDRAM arbiter
- **Blocker:** None - ready to start implementation

---

## Success Criteria
- [ ] FPGA compiles with SDRAM arbiter
- [ ] DMA time: 9-15ms → 4-6ms
- [ ] Decode time: 24-43ms → 20-38ms
- [ ] ALF intro plays smoothly with no lag
- [ ] Frame drop rate: 0.5% → 0%
