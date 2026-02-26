`timescale 1ns / 1ps

//=============================================================================
//  mp4_ctrl_regs.v
//  AXI3 Lightweight H2F slave — Phase 1.5 registers
//
//  ADDRESS MAP  (H2F LW bridge base 0xFF200000, offsets below):
//
//    Offset 0x000  Status Register  (read-only from ARM):
//                    [2]    = fb_vbl       — sticky latch: set by ASCAL VBlank pulse,
//                                           cleared by reading this register
//                    [3]    = dma_done     — sticky latch: set by yuv_fb_dma done pulse,
//                                           cleared by reading this register
//                    [31:4] = 0 (reserved)
//
//    Offset 0x008  Control Register  (read/write):
//                    [0]    = buf_sel    — 0 = Buffer A, 1 = Buffer B
//                    [1]    = dma_start  — write 1 to start DMA (auto-clears)
//                    [31:2] = 0 (reserved)
//
//    Offset 0x010  YUV Y plane base address  (read/write, 32-bit byte addr)
//    Offset 0x014  YUV U plane base address  (read/write, 32-bit byte addr)
//    Offset 0x018  YUV V plane base address  (read/write, 32-bit byte addr)
//    Offset 0x01C  RGB output base address   (read/write, 32-bit byte addr)
//
//  Typical ARM usage (mmap region at 0xFF200000):
//    // Set up YUV planes once:
//    axi[4] = 0x3012C000;   // Y  base
//    axi[5] = 0x30177000;   // U  base
//    axi[6] = 0x30189C00;   // V  base
//    axi[7] = 0x30000000;   // RGB output (back buffer)
//    // Per-frame:
//    // 1. Write YUV data to DDR3 at the above addresses
//    // 2. Write dma_start:  axi[2] |= 2;
//    // 3. Poll dma_done:    while (!(axi[0] & 8)) {}
//    // 4. Flip display buffer: axi[2] = (axi[2] ^ 1) & ~2;
//=============================================================================

module mp4_ctrl_regs (
    input  wire        clk,
    input  wire        rst_n,

    // ── Write Address Channel  (HPS master → slave) ──────────────────────────
    input  wire [20:0] awaddr,
    input  wire [11:0] awid,
    input  wire  [3:0] awlen,
    input  wire  [2:0] awsize,
    input  wire  [1:0] awburst,
    input  wire  [1:0] awlock,
    input  wire  [3:0] awcache,
    input  wire  [2:0] awprot,
    input  wire        awvalid,
    output wire        awready,

    // ── Write Data Channel ────────────────────────────────────────────────────
    input  wire [31:0] wdata,
    input  wire [11:0] wid,
    input  wire  [3:0] wstrb,
    input  wire        wlast,
    input  wire        wvalid,
    output wire        wready,

    // ── Write Response Channel  (slave → HPS master) ─────────────────────────
    output reg  [11:0] bid,
    output wire  [1:0] bresp,
    output reg         bvalid,
    input  wire        bready,

    // ── Read Address Channel  (HPS master → slave) ───────────────────────────
    input  wire [20:0] araddr,
    input  wire [11:0] arid,
    input  wire  [3:0] arlen,
    input  wire  [2:0] arsize,
    input  wire  [1:0] arburst,
    input  wire  [1:0] arlock,
    input  wire  [3:0] arcache,
    input  wire  [2:0] arprot,
    input  wire        arvalid,
    output wire        arready,

    // ── Read Data Channel  (slave → HPS master) ──────────────────────────────
    output reg  [31:0] rdata,
    output reg  [11:0] rid,
    output wire  [1:0] rresp,
    output wire        rlast,
    output reg         rvalid,
    input  wire        rready,

    // ── Application signals ───────────────────────────────────────────────────
    input  wire        fb_vbl,      // vertical blank, clk_sys domain
    input  wire        dma_done,    // FPGA DMA complete, clk_sys domain

    output reg         buf_sel,     // 0 = Buffer A, 1 = Buffer B
    output reg         dma_trigger, // one-clock pulse → start DMA

    output reg  [31:0] yuv_y_base,  // Y plane DDR3 byte address
    output reg  [31:0] yuv_u_base,  // U plane DDR3 byte address
    output reg  [31:0] yuv_v_base,  // V plane DDR3 byte address
    output reg  [31:0] yuv_rgb_base // RGB output DDR3 byte address
);

// Constant AXI responses
assign bresp = 2'b00;   // OKAY
assign rresp = 2'b00;   // OKAY
assign rlast = 1'b1;    // Always single-beat

//=============================================================================
//  Sticky latches for one-clock pulses
//
//  Both fb_vbl and dma_done are single-clock pulses (~10–20 ns at 100 MHz).
//  The ARM's AXI read path cannot reliably catch pulses that short.  Each
//  latch is set by the pulse and cleared automatically when the ARM reads
//  the status register (offset 0x000).
//
//  Interaction between the two bits: the dma_done polling loop reads the
//  status register many times and clears both latches on each read.  By the
//  time dma_done is seen (and the loop exits), any stale fb_vbl_latch is
//  also cleared.  The subsequent VBlank wait therefore blocks on the next
//  actual VBlank edge, which is at most one frame period (~16.7 ms) away.
//=============================================================================
reg dma_done_latch;
reg fb_vbl_latch;

always @(posedge clk) begin
    if (!rst_n) begin
        dma_done_latch <= 1'b0;
        fb_vbl_latch   <= 1'b0;
    end else begin
        // SET has priority over CLEAR: if the pulse fires on the exact same
        // clock as the ARM's status read, the latch is SET (not cleared).
        // The ARM will see it on the next poll.  Using two independent 'if'
        // blocks would let the clear (being last in source order) win the
        // Verilog NB race, silently discarding the pulse → DMA timeout bug.
        if (dma_done)
            dma_done_latch <= 1'b1;
        else if (arvalid & arready & (araddr[4:2] == 3'b000))
            dma_done_latch <= 1'b0;

        if (fb_vbl)
            fb_vbl_latch   <= 1'b1;
        else if (arvalid & arready & (araddr[4:2] == 3'b000))
            fb_vbl_latch   <= 1'b0;
    end
end

//=============================================================================
//  Read path
//=============================================================================
assign arready = ~rvalid;

always @(posedge clk) begin
    if (!rst_n) begin
        rvalid <= 1'b0;
        rdata  <= 32'b0;
        rid    <= 12'b0;
    end else begin
        if (arvalid & arready) begin
            rid <= arid;
            case (araddr[4:2])
                3'b000: rdata <= {28'b0, dma_done_latch, fb_vbl_latch, 2'b00}; // 0x000
                3'b010: rdata <= {30'b0, dma_trigger, buf_sel};            // 0x008
                3'b100: rdata <= yuv_y_base;                               // 0x010
                3'b101: rdata <= yuv_u_base;                               // 0x014
                3'b110: rdata <= yuv_v_base;                               // 0x018
                3'b111: rdata <= yuv_rgb_base;                             // 0x01C
                default: rdata <= 32'b0;
            endcase
            rvalid <= 1'b1;
        end else if (rvalid & rready) begin
            rvalid <= 1'b0;
        end
    end
end

//=============================================================================
//  Write path
//=============================================================================
reg        aw_pend  = 1'b0;
reg [20:0] aw_lat   = 21'b0;
reg [11:0] awid_lat = 12'b0;
reg        w_pend   = 1'b0;
reg [31:0] wd_lat   = 32'b0;

assign awready = !aw_pend && !bvalid;
assign wready  = !w_pend  && !bvalid;

always @(posedge clk) begin
    if (!rst_n) begin
        aw_pend     <= 1'b0;
        w_pend      <= 1'b0;
        bvalid      <= 1'b0;
        bid         <= 12'b0;
        buf_sel     <= 1'b0;
        dma_trigger <= 1'b0;
        yuv_y_base  <= 32'h3012C000;  // default layout after RGB buffers
        yuv_u_base  <= 32'h30177000;
        yuv_v_base  <= 32'h30189C00;
        yuv_rgb_base<= 32'h30000000;  // default: write to Buffer A
    end else begin

        // dma_trigger is a one-clock pulse; auto-clear every cycle
        dma_trigger <= 1'b0;

        // Latch write address
        if (awvalid & awready) begin
            aw_lat   <= awaddr;
            awid_lat <= awid;
            aw_pend  <= 1'b1;
        end

        // Latch write data
        if (wvalid & wready) begin
            wd_lat  <= wdata;
            w_pend  <= 1'b1;
        end

        // Perform register write when both AW and W received
        if (aw_pend & w_pend) begin
            case (aw_lat[4:2])
                3'b010: begin                   // 0x008 Control
                    buf_sel     <= wd_lat[0];
                    dma_trigger <= wd_lat[1];   // one-clock pulse
                end
                3'b100: yuv_y_base   <= wd_lat; // 0x010
                3'b101: yuv_u_base   <= wd_lat; // 0x014
                3'b110: yuv_v_base   <= wd_lat; // 0x018
                3'b111: yuv_rgb_base <= wd_lat; // 0x01C
                default: ;
            endcase
            aw_pend <= 1'b0;
            w_pend  <= 1'b0;
            bid     <= awid_lat;
            bvalid  <= 1'b1;
        end

        if (bvalid & bready)
            bvalid <= 1'b0;

    end
end

endmodule
