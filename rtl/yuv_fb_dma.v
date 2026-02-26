`timescale 1ns / 1ps

//=============================================================================
//  yuv_fb_dma.v  —  Phase 1.5 YUV→RGB DMA engine
//
//  Reads YUV420P planes from DDR3 via fpga2sdram RAM1 port (64-bit Avalon),
//  converts through yuv_to_rgb pipeline, writes RBG565 to the RGB framebuffer.
//
//  MEMORY LAYOUT (byte-addressed, caller supplies base addresses):
//    yuv_y_base  Y plane   640×480 = 307 200 bytes
//    yuv_u_base  Cb plane  320×240 =  76 800 bytes
//    yuv_v_base  Cr plane  320×240 =  76 800 bytes
//    rgb_base    RBG565    640×480×2 = 614 400 bytes  (ASCAL reads here)
//
//  AVALON MASTER (fpga2sdram RAM1 port):
//    64-bit data, 29-bit address (8-byte granularity).
//    avl_address = byte_address[31:3]
//
//  PIXEL BYTE ORDER in DDR3:
//    ASCAL expects big-endian 16-bit pixels (high byte at lower address).
//    This matches the ARM path: *dst++ = px>>8; *dst++ = px&0xFF.
//    For 4 pixels packed into one 64-bit Avalon beat:
//      writedata[7:0]  = p0[15:8]   (p0 high byte → lowest address)
//      writedata[15:8] = p0[7:0]    (p0 low byte)
//      writedata[23:16]= p1[15:8]
//      ... etc.
//
//  PIPELINE:
//    pipe_Y/U/V are registered before entering yuv_to_rgb.
//    yuv_to_rgb has 4-stage latency.
//    Total: proc_x=N sends pixel N, data_valid_out fires at proc_x = N+5.
//
//  USAGE:
//    Assert trigger for one clock to start a frame conversion.
//    done goes high for one clock when the frame is complete.
//    Caller should then flip the display buffer (buf_sel register).
//=============================================================================

module yuv_fb_dma (
    input  wire        clk,
    input  wire        reset,

    // Control
    input  wire        trigger,     // one-clock pulse: start conversion
    output reg         done,        // one-clock pulse: frame complete

    // Plane base addresses (DDR3 physical byte address, 32-bit)
    input  wire [31:0] yuv_y_base,
    input  wire [31:0] yuv_u_base,
    input  wire [31:0] yuv_v_base,
    input  wire [31:0] rgb_base,

    // Avalon MM master — fpga2sdram RAM1 port (64-bit, 29-bit address)
    output reg  [28:0] avl_address,
    output reg   [7:0] avl_burstcount,
    input  wire        avl_waitrequest,
    input  wire [63:0] avl_readdata,
    input  wire        avl_readdatavalid,
    output reg         avl_read,
    output reg  [63:0] avl_writedata,
    output reg   [7:0] avl_byteenable,
    output reg         avl_write
);

// ─────────────────────────────────────────────────────────────────────────────
// Parameters  (overridable for simulation — testbench uses W=16, H=2)
// ─────────────────────────────────────────────────────────────────────────────
parameter W           = 640;   // frame width  (pixels)
parameter H           = 480;   // frame height (rows)

// Avalon burst lengths (beats of 8 bytes each).
// Must be consistent: Y_BEATS = W/8, UV_BEATS = W/16, RGB_BEATS = W/4.
parameter Y_BEATS     = 8'd80;    // 640 B / 8 = 80
parameter UV_BEATS    = 8'd40;    // 320 B / 8 = 40
parameter RGB_BEATS   = 8'd160;   // 1280 B / 8 = 160

// Total pipeline latency seen from proc_x:
//   pipe_Y/U/V are registered (1 clock) then 4-stage yuv_to_rgb = 5 total.
localparam PIPE_LAT    = 5;

// ─────────────────────────────────────────────────────────────────────────────
// Line buffers  (Quartus infers as RAM — M9K or distributed depending on size)
// ─────────────────────────────────────────────────────────────────────────────
reg [7:0] y_buf  [0:W-1];        // 640 B — one luma row
reg [7:0] u_buf  [0:(W/2)-1];    // 320 B — one Cb row  (reused 2× per UV pair)
reg [7:0] v_buf  [0:(W/2)-1];    // 320 B — one Cr row

// RGB output row buffer.  Index 2k = high byte of pixel k, 2k+1 = low byte.
reg [7:0] rgb_buf [0:(W*2)-1];   // 1280 B

// ─────────────────────────────────────────────────────────────────────────────
// FSM encoding
// ─────────────────────────────────────────────────────────────────────────────
localparam S_IDLE      = 4'd0;
localparam S_FETCH_U   = 4'd1;   // issue burst read of Cb row
localparam S_RECV_U    = 4'd2;   // collect Cb readdata beats
localparam S_FETCH_V   = 4'd3;   // issue burst read of Cr row
localparam S_RECV_V    = 4'd4;   // collect Cr readdata beats
localparam S_FETCH_Y   = 4'd5;   // issue burst read of Y row
localparam S_RECV_Y    = 4'd6;   // collect Y readdata beats
localparam S_PROCESS   = 4'd7;   // run conversion pipeline, fill rgb_buf
localparam S_WRITE     = 4'd8;   // burst-write rgb_buf row to DDR3
localparam S_NEXT_ROW  = 4'd9;   // advance row counter, decide next state
localparam S_DONE_ST   = 4'd10;  // pulse done, return to idle

reg [3:0] state;

// ─────────────────────────────────────────────────────────────────────────────
// Counters / bookkeeping
// ─────────────────────────────────────────────────────────────────────────────
reg  [9:0] row;          // current row being processed  (0..H-1)
reg  [7:0] beat_cnt;     // Avalon read beat counter in RECV states
reg  [9:0] proc_x;       // pixel feed counter in S_PROCESS  (0..W-1)
reg  [9:0] rgb_wr_px;    // pipeline-output collection counter (0..W-1)
reg  [7:0] wr_beat;      // write beat counter in S_WRITE  (0..RGB_BEATS-1)

// ─────────────────────────────────────────────────────────────────────────────
// Avalon row address helpers
// ─────────────────────────────────────────────────────────────────────────────
// Avalon address = DDR3_byte_address >> 3.
// Multiplications by row-stride constants are synthesised to adders/counters
// by Quartus (no dedicated multipliers needed at compile time since these only
// change once per row).
wire [31:0] y_row_byte   = yuv_y_base + {22'b0, row}        * W;
wire [31:0] u_row_byte   = yuv_u_base + {23'b0, row[9:1]}   * (W/2);
wire [31:0] v_row_byte   = yuv_v_base + {23'b0, row[9:1]}   * (W/2);
wire [31:0] rgb_row_byte = rgb_base   + {22'b0, row}        * (W*2);

// ─────────────────────────────────────────────────────────────────────────────
// yuv_to_rgb pipeline instance
// ─────────────────────────────────────────────────────────────────────────────
reg  [7:0] pipe_Y, pipe_U, pipe_V;
reg        pipe_vin;
wire [15:0] pipe_rgb;
wire        pipe_vout;

yuv_to_rgb yuv_conv (
    .clk            (clk),
    .reset          (reset),
    .Y              (pipe_Y),
    .U              (pipe_U),
    .V              (pipe_V),
    .data_valid_in  (pipe_vin),
    .rgb565         (pipe_rgb),
    .data_valid_out (pipe_vout)
);

// ─────────────────────────────────────────────────────────────────────────────
// Write-data packing helper  (combinational, used inside always block)
// ─────────────────────────────────────────────────────────────────────────────
// Pack 4 consecutive pixels from rgb_buf starting at byte offset b*8.
// Beat b contains pixels b*4 .. b*4+3.
// Byte layout in rgb_buf:  [b*8+0] = p0 high, [b*8+1] = p0 low, ...
// Avalon little-endian: writedata[7:0] → DDR3 byte at (addr+0), which must
// be the high byte of pixel 0 (ASCAL big-endian pixels).
// Result: writedata = {p3_lo, p3_hi,  p2_lo, p2_hi,  p1_lo, p1_hi,  p0_lo, p0_hi}
//   where _hi = rgb565[15:8], _lo = rgb565[7:0].
// Since rgb_buf stores [2k]=hi, [2k+1]=lo this simplifies to:
//   writedata[7:0]   = rgb_buf[b*8+0]   (p0 hi)
//   writedata[15:8]  = rgb_buf[b*8+1]   (p0 lo)
//   writedata[23:16] = rgb_buf[b*8+2]   (p1 hi)
//   writedata[31:24] = rgb_buf[b*8+3]   (p1 lo)
//   writedata[39:32] = rgb_buf[b*8+4]   (p2 hi)
//   writedata[47:40] = rgb_buf[b*8+5]   (p2 lo)
//   writedata[55:48] = rgb_buf[b*8+6]   (p3 hi)
//   writedata[63:56] = rgb_buf[b*8+7]   (p3 lo)
function [63:0] pack_beat;
    input [7:0] b;  // beat index 0..159
    reg  [10:0] base;
    begin
        base = {b, 3'b000};   // b * 8  (11-bit, max 159*8=1272)
        pack_beat = {rgb_buf[base+7], rgb_buf[base+6],
                     rgb_buf[base+5], rgb_buf[base+4],
                     rgb_buf[base+3], rgb_buf[base+2],
                     rgb_buf[base+1], rgb_buf[base+0]};
    end
endfunction

// ─────────────────────────────────────────────────────────────────────────────
// Main FSM
// ─────────────────────────────────────────────────────────────────────────────
always @(posedge clk) begin
    if (reset) begin
        state          <= S_IDLE;
        done           <= 1'b0;
        row            <= 10'd0;
        beat_cnt       <= 8'd0;
        proc_x         <= 10'd0;
        rgb_wr_px      <= 10'd0;
        wr_beat        <= 8'd0;
        avl_read       <= 1'b0;
        avl_write      <= 1'b0;
        avl_address    <= 29'd0;
        avl_burstcount <= 8'd1;
        avl_writedata  <= 64'd0;
        avl_byteenable <= 8'hFF;
        pipe_vin       <= 1'b0;
        pipe_Y         <= 8'd0;
        pipe_U         <= 8'd128;
        pipe_V         <= 8'd128;
    end else begin

        // Defaults — deassert strobes each cycle unless explicitly held
        avl_read  <= 1'b0;
        avl_write <= 1'b0;
        pipe_vin  <= 1'b0;
        done      <= 1'b0;

        case (state)

        // ── Idle ─────────────────────────────────────────────────────────────
        S_IDLE: begin
            if (trigger) begin
                row   <= 10'd0;
                state <= S_FETCH_U;
            end
        end

        // ── Fetch Cb (U) row ─────────────────────────────────────────────────
        // Avalon burst read protocol (registered outputs):
        //   Cycle 1: assert avl_read, present address/burstcount (avl_read was 0).
        //   Cycle 2: avl_read=1 visible on bus; check waitrequest.
        //            If waitrequest=0: command accepted, go to RECV.
        //            If waitrequest=1: hold (re-assert avl_read, same address).
        // Guard: only advance when avl_read is already 1 (i.e., bus saw it).
        S_FETCH_U: begin
            avl_address    <= u_row_byte[31:3];
            avl_burstcount <= UV_BEATS;
            avl_read       <= 1'b1;
            beat_cnt       <= 8'd0;
            // avl_read (old value) was 0 on first entry, so don't advance yet.
            // On subsequent cycles avl_read=1 (old), check waitrequest.
            if (avl_read && !avl_waitrequest)
                state <= S_RECV_U;
        end

        S_RECV_U: begin
            if (avl_readdatavalid) begin
                // Unpack 8 bytes from 64-bit beat (Avalon little-endian:
                // readdata[7:0] = byte at lowest address).
                // beat_cnt[5:0]: 0..39 (6 bits sufficient for 40 beats)
                u_buf[{beat_cnt[5:0], 3'd0}] <= avl_readdata[ 7: 0];
                u_buf[{beat_cnt[5:0], 3'd1}] <= avl_readdata[15: 8];
                u_buf[{beat_cnt[5:0], 3'd2}] <= avl_readdata[23:16];
                u_buf[{beat_cnt[5:0], 3'd3}] <= avl_readdata[31:24];
                u_buf[{beat_cnt[5:0], 3'd4}] <= avl_readdata[39:32];
                u_buf[{beat_cnt[5:0], 3'd5}] <= avl_readdata[47:40];
                u_buf[{beat_cnt[5:0], 3'd6}] <= avl_readdata[55:48];
                u_buf[{beat_cnt[5:0], 3'd7}] <= avl_readdata[63:56];
                if (beat_cnt == UV_BEATS - 8'd1)
                    state <= S_FETCH_V;
                else
                    beat_cnt <= beat_cnt + 8'd1;
            end
        end

        // ── Fetch Cr (V) row ─────────────────────────────────────────────────
        S_FETCH_V: begin
            avl_address    <= v_row_byte[31:3];
            avl_burstcount <= UV_BEATS;
            avl_read       <= 1'b1;
            beat_cnt       <= 8'd0;
            if (avl_read && !avl_waitrequest)
                state <= S_RECV_V;
        end

        S_RECV_V: begin
            if (avl_readdatavalid) begin
                v_buf[{beat_cnt[5:0], 3'd0}] <= avl_readdata[ 7: 0];
                v_buf[{beat_cnt[5:0], 3'd1}] <= avl_readdata[15: 8];
                v_buf[{beat_cnt[5:0], 3'd2}] <= avl_readdata[23:16];
                v_buf[{beat_cnt[5:0], 3'd3}] <= avl_readdata[31:24];
                v_buf[{beat_cnt[5:0], 3'd4}] <= avl_readdata[39:32];
                v_buf[{beat_cnt[5:0], 3'd5}] <= avl_readdata[47:40];
                v_buf[{beat_cnt[5:0], 3'd6}] <= avl_readdata[55:48];
                v_buf[{beat_cnt[5:0], 3'd7}] <= avl_readdata[63:56];
                if (beat_cnt == UV_BEATS - 8'd1)
                    state <= S_FETCH_Y;
                else
                    beat_cnt <= beat_cnt + 8'd1;
            end
        end

        // ── Fetch Y row ──────────────────────────────────────────────────────
        S_FETCH_Y: begin
            avl_address    <= y_row_byte[31:3];
            avl_burstcount <= Y_BEATS;
            avl_read       <= 1'b1;
            beat_cnt       <= 8'd0;
            if (avl_read && !avl_waitrequest)
                state <= S_RECV_Y;
        end

        S_RECV_Y: begin
            if (avl_readdatavalid) begin
                // beat_cnt[6:0]: 0..79 (7 bits sufficient for 80 beats)
                y_buf[{beat_cnt[6:0], 3'd0}] <= avl_readdata[ 7: 0];
                y_buf[{beat_cnt[6:0], 3'd1}] <= avl_readdata[15: 8];
                y_buf[{beat_cnt[6:0], 3'd2}] <= avl_readdata[23:16];
                y_buf[{beat_cnt[6:0], 3'd3}] <= avl_readdata[31:24];
                y_buf[{beat_cnt[6:0], 3'd4}] <= avl_readdata[39:32];
                y_buf[{beat_cnt[6:0], 3'd5}] <= avl_readdata[47:40];
                y_buf[{beat_cnt[6:0], 3'd6}] <= avl_readdata[55:48];
                y_buf[{beat_cnt[6:0], 3'd7}] <= avl_readdata[63:56];
                if (beat_cnt == Y_BEATS - 8'd1) begin
                    proc_x    <= 10'd0;
                    rgb_wr_px <= 10'd0;
                    state     <= S_PROCESS;
                end else begin
                    beat_cnt <= beat_cnt + 8'd1;
                end
            end
        end

        // ── Convert row through pipeline ──────────────────────────────────────
        // pipe_Y/U/V are registered (1-cycle), yuv_to_rgb is 4-cycle: total 5.
        // proc_x=0 feeds pixel 0; data_valid_out fires 5 cycles later.
        // Chroma upsampling: U[x>>1], V[x>>1] (nearest-neighbour, correct for 4:2:0).
        S_PROCESS: begin
            // Feed pipeline for proc_x = 0..W-1
            if (proc_x < W) begin
                pipe_Y   <= y_buf[proc_x[9:0]];
                pipe_U   <= u_buf[proc_x[9:1]];
                pipe_V   <= v_buf[proc_x[9:1]];
                pipe_vin <= 1'b1;
                proc_x   <= proc_x + 10'd1;
            end

            // Collect pipeline output into rgb_buf
            if (pipe_vout) begin
                // Big-endian: high byte at lower address
                rgb_buf[{rgb_wr_px[9:0], 1'b0}] <= pipe_rgb[15:8];
                rgb_buf[{rgb_wr_px[9:0], 1'b1}] <= pipe_rgb[ 7:0];
                if (rgb_wr_px == W - 1) begin
                    // Last pixel collected — move to write phase
                    wr_beat      <= 8'd0;
                    avl_writedata <= pack_beat(8'd0);  // pre-load beat 0
                    state        <= S_WRITE;
                end else begin
                    rgb_wr_px <= rgb_wr_px + 10'd1;
                end
            end
        end

        // ── Write rgb_buf row to DDR3 ─────────────────────────────────────────
        // Avalon burst write protocol (registered outputs):
        //   First beat: assert avl_write, present address, burstcount, writedata.
        //   Bus sees these signals 1 cycle later (registered).
        //   Guard: only advance beat when avl_write=1 (old) AND !waitrequest.
        //   Pre-load: when advancing beat N→N+1, load pack_beat(N+1) into
        //             avl_writedata so it appears on the bus next cycle.
        //
        //   IMPORTANT — last-beat deassert:
        //   avl_write <= 1 fires unconditionally at the top of this state.
        //   On the last accepted beat we override it with avl_write <= 0.
        //   Verilog NB semantics: the last scheduled NB assignment wins, so
        //   the override below takes precedence over the unconditional one above.
        //   Without this, avl_write would stay high for the first clock of
        //   S_NEXT_ROW, causing the Avalon slave to see a spurious extra write.
        S_WRITE: begin
            avl_write      <= 1'b1;
            avl_byteenable <= 8'hFF;

            // Address and burstcount only needed for the first beat
            if (!avl_write) begin   // avl_write old = 0 means first entry
                avl_address    <= rgb_row_byte[31:3];
                avl_burstcount <= RGB_BEATS;
            end

            // Advance beat when the current beat is accepted by the slave
            if (avl_write && !avl_waitrequest) begin
                if (wr_beat == RGB_BEATS - 8'd1) begin
                    state     <= S_NEXT_ROW;
                    avl_write <= 1'b0;   // deassert: overrides unconditional <= 1 above
                end else begin
                    wr_beat       <= wr_beat + 8'd1;
                    // Pre-load the NEXT beat's data so it appears on the bus
                    // the cycle after the current beat is accepted.
                    avl_writedata <= pack_beat(wr_beat + 8'd1);
                end
            end
            // (avl_writedata was loaded with pack_beat(0) before entering,
            //  and is kept up-to-date via the pre-load above)
        end

        // ── Advance row counter ───────────────────────────────────────────────
        S_NEXT_ROW: begin
            if (row == H - 1) begin
                state <= S_DONE_ST;
            end else begin
                row <= row + 10'd1;
                // row[0] is the CURRENT (just-finished) row's LSB.
                // Even row just finished (row[0]=0) → next row is odd → reuse UV.
                // Odd  row just finished (row[0]=1) → next row is even → fetch UV.
                if (row[0])
                    state <= S_FETCH_U;
                else
                    state <= S_FETCH_Y;
            end
        end

        // ── Done ─────────────────────────────────────────────────────────────
        S_DONE_ST: begin
            done  <= 1'b1;     // one-cycle pulse
            state <= S_IDLE;
        end

        default: state <= S_IDLE;

        endcase
    end
end

endmodule
