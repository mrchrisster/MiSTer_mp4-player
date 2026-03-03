`timescale 1ns / 1ps

//=============================================================================
//  yuv_dma_debug.v  —  UART debug logger for yuv_fb_dma state machine
//
//  Logs diagnostic info when processing rows 0-3 to help debug ghosting issue.
//  Output format (ASCII, one line per event):
//    "R0 FY beat=00 addr=12345678\r\n"  (row 0, fetch Y, beat_cnt, address)
//    "R0 RY beat=4F valid\r\n"          (row 0, recv Y, beat 79, readdatavalid)
//    "R0 YDONE y[000]=32 y[270]=32 y[27F]=32\r\n"  (row 0 Y complete, sample indices)
//
//  Sends at most ~12 lines for 4 rows (rows 0-3), ~600 bytes total.
//  At 115200 baud: 600 bytes × 10 bits/byte = 6000 bits = 52ms.
//=============================================================================

module yuv_dma_debug (
    input  wire        clk,
    input  wire        reset,

    // Signals from yuv_fb_dma FSM
    input  wire [3:0]  state,
    input  wire [9:0]  row,
    input  wire [7:0]  beat_cnt,
    input  wire [31:0] avl_address_word,  // Address (word, not beat) for current operation
    input  wire        avl_read,          // Avalon read strobe
    input  wire        avl_waitrequest,   // Avalon waitrequest
    input  wire        avl_readdatavalid,
    input  wire        state_changed,     // Pulse when state transitions

    // Sample y_buf values (connected from yuv_fb_dma)
    input  wire [7:0]  y_buf_0,           // y_buf[0]
    input  wire [7:0]  y_buf_270,         // y_buf[624] = 0x270
    input  wire [7:0]  y_buf_27F,         // y_buf[639] = 0x27F

    // UART TX
    output wire        uart_tx
);

// State encoding (must match yuv_fb_dma.v)
localparam S_IDLE      = 4'd0;
localparam S_FETCH_U   = 4'd1;
localparam S_RECV_U    = 4'd2;
localparam S_FETCH_V   = 4'd3;
localparam S_RECV_V    = 4'd4;
localparam S_FETCH_Y   = 4'd5;
localparam S_RECV_Y    = 4'd6;
localparam S_PROCESS   = 4'd7;
localparam S_WRITE     = 4'd8;
localparam S_NEXT_ROW  = 4'd9;
localparam S_DONE_ST   = 4'd10;
localparam S_GUARD     = 4'd11;

// UART transmitter
reg  [7:0] uart_data;
reg        uart_valid;
wire       uart_ready;

uart_tx #(
    .CLK_FRE(83),           // 83 MHz for Groovy
    .BAUD_RATE(115200)
) uart (
    .clk            (clk),
    .rst_n          (~reset),
    .tx_data        (uart_data),
    .tx_data_valid  (uart_valid),
    .tx_data_ready  (uart_ready),
    .tx_pin         (uart_tx)
);

// ─────────────────────────────────────────────────────────────────────────────
// Message buffer and transmission state machine
// ─────────────────────────────────────────────────────────────────────────────
reg [7:0] msg_buf [0:63];   // Max message length 64 bytes
reg [5:0] msg_len;          // Current message length
reg [5:0] msg_idx;          // Byte index being transmitted
reg       msg_active;       // Transmission in progress

// Hex digit conversion
function [7:0] hex_digit;
    input [3:0] val;
    begin
        hex_digit = (val < 10) ? (8'd48 + val) : (8'd55 + val);  // '0'-'9' or 'A'-'F'
    end
endfunction

// ─────────────────────────────────────────────────────────────────────────────
// Event detection — only log rows 0-3
// ─────────────────────────────────────────────────────────────────────────────
reg [3:0] state_prev;
reg       log_this_row;

always @(posedge clk) begin
    if (reset) begin
        state_prev  <= S_IDLE;
        log_this_row <= 1'b0;
        msg_active  <= 1'b0;
        msg_idx     <= 6'd0;
        uart_valid  <= 1'b0;
    end else begin
        state_prev <= state;

        // Determine if we should log this row
        if (state == S_FETCH_U && state_prev != S_FETCH_U)
            log_this_row <= (row < 10'd4);  // Log rows 0-3 only

        // ── Message transmission state machine ────────────────────────────
        if (!msg_active) begin
            uart_valid <= 1'b0;

            // Trigger message when Y read is actually accepted (address is stable)
            if (state == S_FETCH_Y && avl_read && !avl_waitrequest && log_this_row) begin
                // Format: "R0 FY addr=3012C000\r\n"
                msg_buf[0]  <= 8'd82;  // 'R'
                msg_buf[1]  <= 8'd48 + row[3:0];  // '0'-'3'
                msg_buf[2]  <= 8'd32;  // ' '
                msg_buf[3]  <= 8'd70;  // 'F'
                msg_buf[4]  <= 8'd89;  // 'Y'
                msg_buf[5]  <= 8'd32;  // ' '
                msg_buf[6]  <= 8'd97;  // 'a'
                msg_buf[7]  <= 8'd100; // 'd'
                msg_buf[8]  <= 8'd100; // 'd'
                msg_buf[9]  <= 8'd114; // 'r'
                msg_buf[10] <= 8'd61;  // '='
                msg_buf[11] <= hex_digit(avl_address_word[31:28]);
                msg_buf[12] <= hex_digit(avl_address_word[27:24]);
                msg_buf[13] <= hex_digit(avl_address_word[23:20]);
                msg_buf[14] <= hex_digit(avl_address_word[19:16]);
                msg_buf[15] <= hex_digit(avl_address_word[15:12]);
                msg_buf[16] <= hex_digit(avl_address_word[11:8]);
                msg_buf[17] <= hex_digit(avl_address_word[7:4]);
                msg_buf[18] <= hex_digit(avl_address_word[3:0]);
                msg_buf[19] <= 8'd13;  // '\r'
                msg_buf[20] <= 8'd10;  // '\n'
                msg_len     <= 6'd21;
                msg_active  <= 1'b1;
                msg_idx     <= 6'd0;
            end

            // Trigger message when Y fetch completes (last beat received)
            else if (state == S_RECV_Y && avl_readdatavalid && beat_cnt == 8'd79 && log_this_row) begin
                // Format: "R0 YDONE y[000]=32 y[270]=80 y[27F]=32\r\n"
                msg_buf[0]  <= 8'd82;  // 'R'
                msg_buf[1]  <= 8'd48 + row[3:0];
                msg_buf[2]  <= 8'd32;  // ' '
                msg_buf[3]  <= 8'd89;  // 'Y'
                msg_buf[4]  <= 8'd68;  // 'D'
                msg_buf[5]  <= 8'd79;  // 'O'
                msg_buf[6]  <= 8'd78;  // 'N'
                msg_buf[7]  <= 8'd69;  // 'E'
                msg_buf[8]  <= 8'd32;  // ' '
                msg_buf[9]  <= 8'd121; // 'y'
                msg_buf[10] <= 8'd91;  // '['
                msg_buf[11] <= 8'd48;  // '0'
                msg_buf[12] <= 8'd48;  // '0'
                msg_buf[13] <= 8'd48;  // '0'
                msg_buf[14] <= 8'd93;  // ']'
                msg_buf[15] <= 8'd61;  // '='
                msg_buf[16] <= hex_digit(y_buf_0[7:4]);
                msg_buf[17] <= hex_digit(y_buf_0[3:0]);
                msg_buf[18] <= 8'd32;  // ' '
                msg_buf[19] <= 8'd121; // 'y'
                msg_buf[20] <= 8'd91;  // '['
                msg_buf[21] <= 8'd50;  // '2'
                msg_buf[22] <= 8'd55;  // '7'
                msg_buf[23] <= 8'd48;  // '0'
                msg_buf[24] <= 8'd93;  // ']'
                msg_buf[25] <= 8'd61;  // '='
                msg_buf[26] <= hex_digit(y_buf_270[7:4]);
                msg_buf[27] <= hex_digit(y_buf_270[3:0]);
                msg_buf[28] <= 8'd32;  // ' '
                msg_buf[29] <= 8'd121; // 'y'
                msg_buf[30] <= 8'd91;  // '['
                msg_buf[31] <= 8'd50;  // '2'
                msg_buf[32] <= 8'd55;  // '7'
                msg_buf[33] <= 8'd70;  // 'F'
                msg_buf[34] <= 8'd93;  // ']'
                msg_buf[35] <= 8'd61;  // '='
                msg_buf[36] <= hex_digit(y_buf_27F[7:4]);
                msg_buf[37] <= hex_digit(y_buf_27F[3:0]);
                msg_buf[38] <= 8'd13;  // '\r'
                msg_buf[39] <= 8'd10;  // '\n'
                msg_len     <= 6'd40;
                msg_active  <= 1'b1;
                msg_idx     <= 6'd0;
            end
        end

        // ── Transmit bytes one at a time ──────────────────────────────────
        else begin  // msg_active = 1
            if (uart_ready && !uart_valid) begin
                uart_data  <= msg_buf[msg_idx];
                uart_valid <= 1'b1;
            end
            else if (uart_valid && uart_ready) begin
                uart_valid <= 1'b0;
                if (msg_idx == msg_len - 1) begin
                    msg_active <= 1'b0;  // Message complete
                end else begin
                    msg_idx <= msg_idx + 6'd1;
                end
            end
        end
    end
end

endmodule
