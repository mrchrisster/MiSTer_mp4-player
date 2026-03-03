`timescale 1ns / 1ps
//=============================================================================
//  fb_scan_out.sv — Framebuffer Scan-Out for CRT-Compatible Video Output
//
//  Reads the front RGB565 framebuffer from DDR3 one line at a time,
//  synchronized to the core's native video timing (DE/VS/CE_PIXEL).
//  Outputs unpacked R/G/B 8-bit signals for the VGA native video path so
//  the MiSTer I/O board VGA output (vga_scaler=0) carries the framebuffer
//  at whatever resolution/timing the Groovy emu generates — no video_mode
//  override, no ASCAL dependency, proper CRT timings.
//
//  TIMING MODEL
//    • VS rising edge (sync-pulse end)     → reset frame, fetch line 0
//    • DE falling edge (end of visible line N) → fetch line N+1
//    • DE rising edge  (start of visible line N+1) → switch read buffer
//    • CE_PIXEL during DE=1                → output pixels from read buffer
//
//  OUTPUT PIPELINE LATENCY: 2 clk cycles (1 BRAM read + 1 output register).
//  The integrator in sys_top.v must delay the companion HS/VS/DE signals by
//  the same 2 cycles so they stay aligned with r_out/g_out/b_out.
//  Use two chained registers on hs_in/vs_in/de_in before driving VGA pins.
//
//  AVALON MM (identical protocol to yuv_fb_dma.v):
//    64-bit data, 29-bit word address (byte_addr >> 3).
//    Burst reads: 160 beats × 8 bytes = 1280 bytes = one 640-pixel line.
//    Big-endian pixels in DDR3 (high byte at lower address):
//      readdata[7:0]   = p0 high byte (R[4:0] G[5:3])
//      readdata[15:8]  = p0 low byte  (G[2:0] B[4:0])
//      readdata[23:16] = p1 high byte  … etc.
//
//  TIMING BUDGET (640×480@60 Hz, 50 MHz clk, 25 MHz pixel clock):
//    HBlank = 160 pixel clocks = 320 clk50 cycles ≈ 6.4 µs
//    Line fetch = 160 beats; DDR3 must sustain ≤2 clk/beat → 320 cycles max.
//    Budget is tight — ensure fpga2sdram port priority is set appropriately.
//
//  QSYS NOTE:
//    This module needs its own dedicated fpga2sdram Avalon MM master port.
//    It does NOT share the port used by yuv_fb_dma (they may be active
//    simultaneously: yuv_fb_dma writes back buffer while scan-out reads
//    front buffer). Add a second RAM port in Platform Designer and wire
//    the avl_* signals to it in sys_top.v.
//=============================================================================

module fb_scan_out #(
    parameter W     = 640,      // Frame width  (pixels)
    parameter H     = 480,      // Frame height (lines)
    parameter BEATS = 8'd160    // Avalon beats per line = W*2/8
) (
    input  wire        clk,         // System clock (50 MHz)
    input  wire        reset,

    // ── Core video timing (clk domain, CE_PIXEL as pixel enable) ─────────
    input  wire        ce_pixel,    // Pixel clock enable
    input  wire        de_in,       // Data enable: 1 = visible pixel region
    input  wire        vs_in,       // VSync from emu (active-low standard VGA)

    // ── Framebuffer control ───────────────────────────────────────────────
    input  wire        fb_active,   // 1 = output framebuffer; 0 = black
    input  wire [31:0] fb_base,     // DDR3 byte address of active (front) buffer

    // ── Pixel output (2-cycle latency — see note above) ───────────────────
    output reg  [7:0]  r_out,
    output reg  [7:0]  g_out,
    output reg  [7:0]  b_out,

    // ── Avalon MM master (64-bit, 29-bit word address) ────────────────────
    output reg  [28:0] avl_address,
    output reg  [7:0]  avl_burstcount,
    output reg         avl_read,
    input  wire        avl_waitrequest,
    input  wire [63:0] avl_readdata,
    input  wire        avl_readdatavalid
);

// ── Ping-pong pixel line buffers ──────────────────────────────────────────
// Two buffers of 640 × 16-bit.  Quartus infers as M9K BRAM (synchronous
// read, 1-cycle latency).  buf_wr holds the line currently being fetched;
// buf_rd holds the completed line being displayed.
reg [15:0] buf0 [0:W-1];   // ping
reg [15:0] buf1 [0:W-1];   // pong

reg         wr_sel;         // 0 = fetch writes buf0; 1 = fetch writes buf1

// ── Fetch FSM ─────────────────────────────────────────────────────────────
localparam F_IDLE  = 2'd0;
localparam F_FETCH = 2'd1;
localparam F_RECV  = 2'd2;

reg [1:0]  f_state;
reg [7:0]  beat_cnt;        // Beat index within current burst (0..BEATS-1)
reg [7:0]  wr_beat;         // Pixel-group index into write buffer (0..BEATS-1)
reg        fetch_done;      // One-cycle pulse: fetch wrote into wr_sel buffer

// Handshake from display → fetch FSM
reg [9:0]  fetch_line;      // Line to fetch (set before asserting fetch_req)
reg        fetch_req;       // One-cycle pulse: start fetching fetch_line
reg        fetch_pending;   // Sticky latch: request waiting for FSM to go idle

// Line DDR3 byte address (combinational from fetch_line)
wire [31:0] line_byte_addr = fb_base + ({22'd0, fetch_line} * (W * 2));

// ── Avalon Fetch FSM ───────────────────────────────────────────────────────
always @(posedge clk) begin
    if (reset) begin
        f_state        <= F_IDLE;
        avl_read       <= 1'b0;
        avl_address    <= 29'd0;
        avl_burstcount <= BEATS;
        beat_cnt       <= 8'd0;
        wr_beat        <= 8'd0;
        wr_sel         <= 1'b0;
        fetch_done     <= 1'b0;
        fetch_pending  <= 1'b0;
    end else begin
        avl_read   <= 1'b0;
        fetch_done <= 1'b0;

        case (f_state)

        F_IDLE: begin
            // fetch_line is stable here (NB committed from display always block).
            // fetch_pending is set if a fetch_req arrived while FSM was busy.
            if (fetch_req || fetch_pending) begin
                avl_address    <= line_byte_addr[31:3];
                avl_burstcount <= BEATS;
                beat_cnt       <= 8'd0;
                wr_beat        <= 8'd0;
                fetch_pending  <= 1'b0;
                f_state        <= F_FETCH;
            end
        end

        // Issue burst read; re-assert avl_read each cycle until accepted.
        // Latch any incoming fetch_req so it isn't lost while we handshake.
        F_FETCH: begin
            if (fetch_req) fetch_pending <= 1'b1;
            avl_read <= 1'b1;
            if (avl_read && !avl_waitrequest)
                f_state <= F_RECV;
        end

        // Collect readdata beats — unpack 4 big-endian RGB565 pixels each beat.
        // Avalon little-endian: readdata[7:0] = lowest DDR3 byte address.
        // Big-endian pixel layout: high byte first, so:
        //   px = {readdata[7:0], readdata[15:8]}   (hi byte, lo byte)
        F_RECV: begin
            if (fetch_req) fetch_pending <= 1'b1;
            if (avl_readdatavalid) begin
                if (wr_sel == 1'b0) begin
                    buf0[{wr_beat, 2'b00}] <= {avl_readdata[ 7: 0], avl_readdata[15: 8]};
                    buf0[{wr_beat, 2'b01}] <= {avl_readdata[23:16], avl_readdata[31:24]};
                    buf0[{wr_beat, 2'b10}] <= {avl_readdata[39:32], avl_readdata[47:40]};
                    buf0[{wr_beat, 2'b11}] <= {avl_readdata[55:48], avl_readdata[63:56]};
                end else begin
                    buf1[{wr_beat, 2'b00}] <= {avl_readdata[ 7: 0], avl_readdata[15: 8]};
                    buf1[{wr_beat, 2'b01}] <= {avl_readdata[23:16], avl_readdata[31:24]};
                    buf1[{wr_beat, 2'b10}] <= {avl_readdata[39:32], avl_readdata[47:40]};
                    buf1[{wr_beat, 2'b11}] <= {avl_readdata[55:48], avl_readdata[63:56]};
                end

                if (beat_cnt == BEATS - 8'd1) begin
                    fetch_done <= 1'b1;
                    wr_sel     <= ~wr_sel;   // flip: next fetch uses other buffer
                    f_state    <= F_IDLE;
                end else begin
                    beat_cnt <= beat_cnt + 8'd1;
                    wr_beat  <= wr_beat  + 8'd1;
                end
            end
        end

        default: f_state <= F_IDLE;
        endcase
    end
end

// ── Display control + fetch trigger ───────────────────────────────────────
reg  [9:0]  pix_x;          // Pixel column being read from buffer (0..W-1)
reg  [9:0]  line_num;       // Line currently being displayed (0..H-1)
reg         de_prev;        // DE delayed 1 clock (edge detect)
reg         vs_prev;        // VS delayed 1 clock (edge detect)
reg         rd_sel;         // Which buffer display reads: 0=buf0, 1=buf1
reg         buf_ready;      // At least one line has been fetched (safe to display)

wire        de_fall = de_prev & ~de_in;    // End of visible line
wire        de_rise = ~de_prev & de_in;    // Start of visible line
wire        vs_rise = ~vs_prev & vs_in;    // VSync trailing edge (sync-pulse end)

always @(posedge clk) begin
    if (reset) begin
        pix_x      <= 10'd0;
        line_num   <= 10'd0;
        de_prev    <= 1'b0;
        vs_prev    <= 1'b0;
        rd_sel     <= 1'b1;    // buf1 = initial read side; buf0 filled first
        buf_ready  <= 1'b0;
        fetch_req  <= 1'b0;
        fetch_line <= 10'd0;
    end else begin
        fetch_req <= 1'b0;   // default: no request

        if (fetch_done)
            buf_ready <= 1'b1;   // latch once first fetch completes

        if (ce_pixel) begin
            de_prev <= de_in;
            vs_prev <= vs_in;

            // Priority: vs_rise > de_fall > de_rise > visible pixel
            if (vs_rise) begin
                // Frame start: reset counters, fetch line 0 into buf[wr_sel]
                line_num   <= 10'd0;
                pix_x      <= 10'd0;
                fetch_line <= 10'd0;
                buf_ready  <= 1'b0;
                fetch_req  <= 1'b1;
            end else if (de_fall) begin
                // End of visible line N: kick off fetch of line N+1
                pix_x    <= 10'd0;
                line_num <= line_num + 10'd1;
                if (line_num < H - 1) begin
                    fetch_line <= line_num + 10'd1;
                    fetch_req  <= 1'b1;
                end
            end else if (de_rise) begin
                // Start of line: switch to the buffer just completed during HBlank.
                // fetch_done has already flipped wr_sel, so ~wr_sel = completed side.
                rd_sel <= ~wr_sel;
                pix_x  <= 10'd0;
            end else if (de_in) begin
                // Visible pixel: advance read address
                pix_x <= pix_x + 10'd1;
            end
        end
    end
end

// ── Pixel output pipeline (2 cycles: BRAM read + output register) ─────────
// Stage 1: synchronous BRAM read (Quartus M9K, 1-cycle registered read)
reg [15:0] bram_out;
always @(posedge clk) begin
    if (ce_pixel)
        bram_out <= (rd_sel == 1'b0) ? buf0[pix_x] : buf1[pix_x];
end

// Stage 2: RGB565 → R8G8B8 expansion + output register
// RGB565: px[15:11]=R5, px[10:5]=G6, px[4:0]=B5
// Expand to 8 bits by replicating the MSBs into the vacated LSBs so that
// max-value (all 1s) maps to 0xFF and zero maps to 0x00.
always @(posedge clk) begin
    if (ce_pixel) begin
        if (fb_active && buf_ready) begin
            r_out <= {bram_out[15:11], bram_out[15:13]};  // R5 → R8
            g_out <= {bram_out[10: 5], bram_out[10: 9]};  // G6 → G8
            b_out <= {bram_out[ 4: 0], bram_out[ 4: 2]};  // B5 → B8
        end else begin
            r_out <= 8'd0;
            g_out <= 8'd0;
            b_out <= 8'd0;
        end
    end
end

endmodule
