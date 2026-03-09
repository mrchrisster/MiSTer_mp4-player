`timescale 1ns / 1ps
//=============================================================================
//  fb_scan_out.sv — Framebuffer Scan-Out with Internal 480i Timing Generator
//
//  Reads RGB565 framebuffer from DDR3 and outputs to CRT at 640×480i @ 59.94Hz.
//  Generates its own video timing (HS/VS/DE) independent of the emu, so CRT
//  output is always 480i regardless of the emu's native video mode.
//
//  When fb_active=0, outputs black and timing stops.
//  When fb_active=1, generates 480i interlaced timing and reads framebuffer.
//
//  INTERLACED OUTPUT:
//    Field 0 (even): displays framebuffer lines 0, 2, 4, ..., 478
//    Field 1 (odd):  displays framebuffer lines 1, 3, 5, ..., 479
//    Each field has 262 total lines (240 active + 22 blanking).
//
//  480i NTSC TIMING (per field):
//    H: 858 total (640 active, 16 FP, 62 sync, 140 BP) — matches PSX core
//    V: 262 total (240 active, 4 FP, 3 sync, 15 BP)
//    Pixel clock: 13.5 MHz (from clk_sys ~83 MHz via CE divider)
//
//  OUTPUT PIPELINE LATENCY: 2 clk cycles (1 BRAM read + 1 output register).
//=============================================================================

module fb_scan_out #(
    parameter W     = 640,      // Frame width  (pixels)
    parameter H     = 480,      // Frame height (lines, both fields combined)
    parameter BEATS = 8'd160    // Avalon beats per line = W*2/8
) (
    input  wire        clk,         // System clock (83 MHz for Groovy)
    input  wire        reset,

    // ── Framebuffer control ───────────────────────────────────────────────
    input  wire        fb_active,   // 1 = output framebuffer at 480i; 0 = black
    input  wire [31:0] fb_base,     // DDR3 byte address of active (front) buffer

    // ── Video timing outputs (for VGA connector) ───────────────────────────
    output reg         hs_out,      // HSync (active-low standard VGA)
    output reg         vs_out,      // VSync (active-low standard VGA)
    output reg         de_out,      // Data enable (1 = visible pixel)

    // ── Pixel output (2-cycle latency) ─────────────────────────────────────
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

// ── 480i Timing Parameters ────────────────────────────────────────────────
// Standard NTSC 480i timing (matches PSX core)
localparam H_ACTIVE = 640;
localparam H_FP     = 16;   // Front porch
localparam H_SYNC   = 62;   // Sync pulse (standard NTSC)
localparam H_BP     = 140;  // Back porch (increased to shift image left vs PSX)
localparam H_TOTAL  = H_ACTIVE + H_FP + H_SYNC + H_BP;  // 858

localparam V_ACTIVE = 240;  // per field
localparam V_FP     = 4;
localparam V_SYNC   = 3;
localparam V_BP     = 15;
localparam V_TOTAL  = V_ACTIVE + V_FP + V_SYNC + V_BP;  // 262

// ── CE Pixel Divider (83 MHz → ~13.8 MHz pixel clock) ────────────────────
reg [2:0] ce_div;
wire      ce_pix = (ce_div == 3'd0);

always @(posedge clk) begin
    if (reset || !fb_active)
        ce_div <= 3'd0;
    else
        ce_div <= (ce_div == 3'd5) ? 3'd0 : (ce_div + 3'd1);
end

// ── Video Timing Counters ─────────────────────────────────────────────────
reg [9:0] h_cnt;       // 0..857 (H_TOTAL-1)
reg [8:0] v_cnt;       // 0..261 (V_TOTAL-1)
reg       field;       // 0 = even field (lines 0,2,4...), 1 = odd field (1,3,5...)

wire h_active = (h_cnt < H_ACTIVE);
wire v_active = (v_cnt < V_ACTIVE);
wire h_sync   = (h_cnt >= H_ACTIVE + H_FP) && (h_cnt < H_ACTIVE + H_FP + H_SYNC);
wire v_sync   = (v_cnt >= V_ACTIVE + V_FP) && (v_cnt < V_ACTIVE + V_FP + V_SYNC);

always @(posedge clk) begin
    if (reset || !fb_active) begin
        h_cnt <= 10'd0;
        v_cnt <= 9'd0;
        field <= 1'b0;
    end else if (ce_pix) begin
        if (h_cnt == H_TOTAL - 1) begin
            h_cnt <= 10'd0;
            if (v_cnt == V_TOTAL - 1) begin
                v_cnt <= 9'd0;
                field <= ~field;  // toggle field at end of each field
            end else begin
                v_cnt <= v_cnt + 9'd1;
            end
        end else begin
            h_cnt <= h_cnt + 10'd1;
        end
    end
end

// ── Output Timing Signals (registered for stable output) ──────────────────
always @(posedge clk) begin
    if (reset || !fb_active) begin
        hs_out <= 1'b1;   // idle high (active-low sync)
        vs_out <= 1'b1;
        de_out <= 1'b0;
    end else if (ce_pix) begin
        hs_out <= !h_sync;
        vs_out <= !v_sync;
        de_out <= h_active && v_active;
    end
end

// ── Ping-pong pixel line buffers ──────────────────────────────────────────
reg [15:0] buf0 [0:W-1];   // ping
reg [15:0] buf1 [0:W-1];   // pong

reg         wr_sel;         // 0 = fetch writes buf0; 1 = fetch writes buf1
reg         rd_sel;         // 0 = display reads buf0; 1 = display reads buf1

// ── Fetch FSM ─────────────────────────────────────────────────────────────
localparam F_IDLE  = 2'd0;
localparam F_FETCH = 2'd1;
localparam F_RECV  = 2'd2;

reg [1:0]  f_state;
reg [7:0]  beat_cnt;
reg [7:0]  wr_beat;
reg        fetch_done;

reg [9:0]  fetch_line;      // Framebuffer line to fetch (0..479)
reg        fetch_req;
reg        fetch_pending;

// Line DDR3 byte address (combinational)
wire [31:0] line_byte_addr = fb_base + ({22'd0, fetch_line} * (W * 2));

// ── Avalon Fetch FSM ──────────────────────────────────────────────────────
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
            if (fetch_req || fetch_pending) begin
                fetch_pending  <= 1'b0;
                avl_address    <= line_byte_addr[31:3];  // word address
                avl_burstcount <= BEATS;
                avl_read       <= 1'b1;
                beat_cnt       <= 8'd0;
                wr_beat        <= 8'd0;
                f_state        <= F_FETCH;
            end
        end

        F_FETCH: begin
            if (!avl_waitrequest) begin
                f_state <= F_RECV;
            end else begin
                avl_read <= 1'b1;  // hold request
            end
        end

        F_RECV: begin
            if (avl_readdatavalid) begin
                // Unpack 4 pixels from 64-bit readdata (little-endian BGR565)
                if (wr_sel == 1'b0) begin
                    buf0[{wr_beat, 2'b00}] <= avl_readdata[15:0];
                    buf0[{wr_beat, 2'b01}] <= avl_readdata[31:16];
                    buf0[{wr_beat, 2'b10}] <= avl_readdata[47:32];
                    buf0[{wr_beat, 2'b11}] <= avl_readdata[63:48];
                end else begin
                    buf1[{wr_beat, 2'b00}] <= avl_readdata[15:0];
                    buf1[{wr_beat, 2'b01}] <= avl_readdata[31:16];
                    buf1[{wr_beat, 2'b10}] <= avl_readdata[47:32];
                    buf1[{wr_beat, 2'b11}] <= avl_readdata[63:48];
                end
                wr_beat  <= wr_beat + 8'd1;
                beat_cnt <= beat_cnt + 8'd1;

                if (beat_cnt == BEATS - 1) begin
                    fetch_done <= 1'b1;
                    wr_sel     <= ~wr_sel;
                    f_state    <= F_IDLE;
                end
            end
        end

        default: f_state <= F_IDLE;
        endcase

        // Latch fetch_req if FSM is busy
        if (fetch_req && f_state != F_IDLE)
            fetch_pending <= 1'b1;
    end
end

// ── Display Logic (reads from buffer, outputs pixels) ─────────────────────
reg [9:0] rd_x;         // horizontal pixel counter during active display
reg [9:0] display_line; // current framebuffer line being displayed (0..479)

// Buffer read (1-cycle BRAM latency)
wire [15:0] pixel_bgr565 = (rd_sel == 1'b0) ? buf0[rd_x] : buf1[rd_x];

// Unpack BGR565 → 8-bit RGB (with 1-cycle register delay for output)
wire [7:0] b8 = {pixel_bgr565[15:11], 3'b000};
wire [7:0] g8 = {pixel_bgr565[10:5],  2'b00};
wire [7:0] r8 = {pixel_bgr565[4:0],   3'b000};

always @(posedge clk) begin
    if (reset || !fb_active) begin
        r_out <= 8'd0;
        g_out <= 8'd0;
        b_out <= 8'd0;
    end else if (ce_pix) begin
        if (de_out) begin
            // Output pixel from buffer (with 1-cycle delay from BRAM read)
            r_out <= r8;
            g_out <= g8;
            b_out <= b8;
        end else begin
            r_out <= 8'd0;
            g_out <= 8'd0;
            b_out <= 8'd0;
        end
    end
end

// ── Fetch Control Logic ───────────────────────────────────────────────────
// Fetch next line during HBlank, display it on the next active line.
// For interlacing: even field displays even lines (0,2,4...), odd field odd (1,3,5...).

reg prev_de;
reg prev_vs;

always @(posedge clk) begin
    if (reset || !fb_active) begin
        fetch_req    <= 1'b0;
        fetch_line   <= 10'd0;
        display_line <= 10'd0;
        rd_sel       <= 1'b0;
        rd_x         <= 10'd0;
        prev_de      <= 1'b0;
        prev_vs      <= 1'b1;
    end else if (ce_pix) begin
        prev_de  <= de_out;
        prev_vs  <= vs_out;
        fetch_req <= 1'b0;

        // At VSync rising edge: reset frame, prepare to fetch first line of field
        if (!prev_vs && vs_out) begin
            display_line <= {9'd0, field};  // field 0→line 0, field 1→line 1
            fetch_line   <= {9'd0, field};
            fetch_req    <= 1'b1;           // fetch first line immediately
        end

        // During active display: update rd_x, output pixels
        if (de_out) begin
            rd_x <= (h_cnt < W) ? h_cnt : 10'd0;
        end

        // At end of visible line (DE falling edge): fetch next line, swap buffers
        if (prev_de && !de_out && v_active) begin
            // Switch read buffer (display the line just fetched)
            rd_sel <= ~rd_sel;

            // Prepare next line for interlaced field (skip by 2)
            if (display_line + 10'd2 < H) begin
                display_line <= display_line + 10'd2;
                fetch_line   <= display_line + 10'd2;
                fetch_req    <= 1'b1;
            end
        end

        // Sync fetch_done with buffer swap
        if (fetch_done) begin
            // Fetch completed, buffer is ready for next display line
        end
    end
end

endmodule
