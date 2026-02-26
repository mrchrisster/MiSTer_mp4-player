`timescale 1ns / 1ps

//=============================================================================
//  mp4_debug_uart.v
//  Periodic FPGA-side debug UART — sends one status line per second to the
//  HPS UART (/dev/ttyS1) via sys_top.v → uart_rxd.
//
//  Output format (115200 8N1, one line per second, 40 chars):
//    T=HHHH D=HHHH V=HHHH R=HHHH W=HHHH B=H\r\n
//
//    T = dma_trigger pulses in the last second    (hex 16-bit, expect 30 at 30fps)
//    D = dma_done   pulses in the last second    (hex 16-bit, should == T)
//    V = fb_vbl     pulses in the last second    (hex 16-bit, expect 0x003C at 60Hz)
//    R = Avalon READ-stall cycles (waitrequest & read)   (hex 16-bit)
//    W = Avalon WRITE-stall cycles (waitrequest & write) (hex 16-bit)
//    B = buf_sel current value                   (0 or 1)
//
//  R and W together reveal which phase the DMA is stuck in:
//    R high, W=0  → frozen waiting for DDR3 read data (S_READ_Y/U/V)
//    W high, R=0  → frozen waiting for DDR3 write accept (S_WRITE)
//    Both 0       → DMA completes normally; look at T vs D
//
//  Usage:
//    Connect tx_pin → sys_top.v uart_rxd → HPS peripheral UART rxd.
//    On MiSTer ARM: microcom /dev/ttyS1 -s 115200
//
//  NOTE: CLK_FRE must match the actual clk_sys frequency for correct baud
//  rate. Groovy's PLL is dynamically reprogrammed; set CLK_FRE to the
//  frequency in use when debugging.  If output is garbled try 25, 50, or 100.
//=============================================================================

module mp4_debug_uart #(
    parameter CLK_FRE = 50    // system clock frequency in MHz
) (
    input  wire clk,
    input  wire rst_n,

    // Signals to observe
    input  wire dma_trigger,      // one-clock pulse: DMA triggered
    input  wire dma_done,         // one-clock pulse: DMA complete
    input  wire fb_vbl,           // one-clock pulse: VBlank (clk_sys domain)
    input  wire ram_waitrequest,  // Avalon MM waitrequest from fpga2sdram
    input  wire ram_read,         // Avalon MM read strobe
    input  wire ram_write,        // Avalon MM write strobe
    input  wire buf_sel,          // current display buffer selection

    // UART TX (8N1, 115200)
    output wire tx_pin
);

//=============================================================================
//  1-second pulse counter and accumulator
//=============================================================================
localparam integer SEC_TICKS = CLK_FRE * 1000000;   // e.g. 50,000,000

// 27-bit counter: 2^27 = 134M, sufficient for up to 134 MHz
reg [26:0] sec_cnt;
reg [15:0] trig_cnt, done_cnt, vbl_cnt, read_cnt, wait_cnt;
reg        sec_tick;

// Snapshot registers (frozen while transmitting)
reg [15:0] snap_trig, snap_done, snap_vbl, snap_read, snap_wait;
reg        snap_buf;

always @(posedge clk) begin
    if (!rst_n) begin
        sec_cnt   <= 27'd0;
        trig_cnt  <= 16'd0;
        done_cnt  <= 16'd0;
        vbl_cnt   <= 16'd0;
        read_cnt  <= 16'd0;
        wait_cnt  <= 16'd0;
        sec_tick  <= 1'b0;
        snap_trig <= 16'd0;
        snap_done <= 16'd0;
        snap_vbl  <= 16'd0;
        snap_read <= 16'd0;
        snap_wait <= 16'd0;
        snap_buf  <= 1'b0;
    end else begin
        sec_tick <= 1'b0;
        if (sec_cnt == SEC_TICKS - 1) begin
            // Snapshot counters before clearing
            snap_trig <= trig_cnt;
            snap_done <= done_cnt;
            snap_vbl  <= vbl_cnt;
            snap_read <= read_cnt;
            snap_wait <= wait_cnt;
            snap_buf  <= buf_sel;
            // Reset
            trig_cnt  <= 16'd0;
            done_cnt  <= 16'd0;
            vbl_cnt   <= 16'd0;
            read_cnt  <= 16'd0;
            wait_cnt  <= 16'd0;
            sec_cnt   <= 27'd0;
            sec_tick  <= 1'b1;
        end else begin
            sec_cnt <= sec_cnt + 27'd1;
            if (dma_trigger)                  trig_cnt <= trig_cnt + 16'd1;
            if (dma_done)                     done_cnt <= done_cnt + 16'd1;
            if (fb_vbl)                       vbl_cnt  <= vbl_cnt  + 16'd1;
            if (ram_waitrequest & ram_read)   read_cnt <= read_cnt + 16'd1;
            if (ram_waitrequest & ram_write)  wait_cnt <= wait_cnt + 16'd1;
        end
    end
end

//=============================================================================
//  TX sequencer — sends 40-character line per second
//  Format: T=HHHH D=HHHH V=HHHH R=HHHH W=HHHH B=H\r\n
//  Characters 0..39 (40 total), idle state = char_idx == 63
//=============================================================================
reg [5:0] char_idx;      // 0–39 = active, 63 = idle
reg [7:0] tx_data;
reg       tx_valid;
reg       tx_just_sent;  // suppresses re-fire on the clock after presenting data
wire      tx_ready;

uart_tx #(
    .CLK_FRE  (CLK_FRE),
    .BAUD_RATE (115200)
) u_uart_tx (
    .clk          (clk),
    .rst_n        (rst_n),
    .tx_data      (tx_data),
    .tx_data_valid(tx_valid),
    .tx_data_ready(tx_ready),
    .tx_pin       (tx_pin)
);

// Convert 4-bit nibble to ASCII hex digit ('0'..'9', 'A'..'F')
function [7:0] hex_ch;
    input [3:0] nibble;
    begin
        // '0'=48, 'A'=65=55+10
        hex_ch = (nibble < 4'd10) ? (8'd48 + {4'd0, nibble})
                                  : (8'd55 + {4'd0, nibble});
    end
endfunction

always @(posedge clk) begin
    if (!rst_n) begin
        char_idx     <= 6'd63;
        tx_valid     <= 1'b0;
        tx_data      <= 8'd0;
        tx_just_sent <= 1'b0;
    end else begin
        tx_valid     <= 1'b0;   // default: idle
        tx_just_sent <= 1'b0;   // clear one cycle after firing

        if (sec_tick && char_idx == 6'd63) begin
            // Start new line when idle and 1-second tick fires
            char_idx <= 6'd0;
        end else if (char_idx != 6'd63 && tx_ready && !tx_just_sent) begin
            // Send next character — guard with tx_just_sent so we don't
            // double-fire: uart_tx takes one clock to de-assert tx_ready
            // after we present data, so without the guard every odd char
            // is silently dropped.
            tx_valid     <= 1'b1;
            tx_just_sent <= 1'b1;
            case (char_idx)
                // T=HHHH
                6'd0:  tx_data <= 8'd84;                        // 'T'
                6'd1:  tx_data <= 8'd61;                        // '='
                6'd2:  tx_data <= hex_ch(snap_trig[15:12]);
                6'd3:  tx_data <= hex_ch(snap_trig[11:8]);
                6'd4:  tx_data <= hex_ch(snap_trig[7:4]);
                6'd5:  tx_data <= hex_ch(snap_trig[3:0]);
                // ' D=HHHH'
                6'd6:  tx_data <= 8'd32;                        // ' '
                6'd7:  tx_data <= 8'd68;                        // 'D'
                6'd8:  tx_data <= 8'd61;                        // '='
                6'd9:  tx_data <= hex_ch(snap_done[15:12]);
                6'd10: tx_data <= hex_ch(snap_done[11:8]);
                6'd11: tx_data <= hex_ch(snap_done[7:4]);
                6'd12: tx_data <= hex_ch(snap_done[3:0]);
                // ' V=HHHH'
                6'd13: tx_data <= 8'd32;                        // ' '
                6'd14: tx_data <= 8'd86;                        // 'V'
                6'd15: tx_data <= 8'd61;                        // '='
                6'd16: tx_data <= hex_ch(snap_vbl[15:12]);
                6'd17: tx_data <= hex_ch(snap_vbl[11:8]);
                6'd18: tx_data <= hex_ch(snap_vbl[7:4]);
                6'd19: tx_data <= hex_ch(snap_vbl[3:0]);
                // ' R=HHHH'  (Avalon read stalls)
                6'd20: tx_data <= 8'd32;                        // ' '
                6'd21: tx_data <= 8'd82;                        // 'R'
                6'd22: tx_data <= 8'd61;                        // '='
                6'd23: tx_data <= hex_ch(snap_read[15:12]);
                6'd24: tx_data <= hex_ch(snap_read[11:8]);
                6'd25: tx_data <= hex_ch(snap_read[7:4]);
                6'd26: tx_data <= hex_ch(snap_read[3:0]);
                // ' W=HHHH'  (Avalon write stalls)
                6'd27: tx_data <= 8'd32;                        // ' '
                6'd28: tx_data <= 8'd87;                        // 'W'
                6'd29: tx_data <= 8'd61;                        // '='
                6'd30: tx_data <= hex_ch(snap_wait[15:12]);
                6'd31: tx_data <= hex_ch(snap_wait[11:8]);
                6'd32: tx_data <= hex_ch(snap_wait[7:4]);
                6'd33: tx_data <= hex_ch(snap_wait[3:0]);
                // ' B=H'
                6'd34: tx_data <= 8'd32;                        // ' '
                6'd35: tx_data <= 8'd66;                        // 'B'
                6'd36: tx_data <= 8'd61;                        // '='
                6'd37: tx_data <= hex_ch({3'b000, snap_buf});
                // \r\n
                6'd38: tx_data <= 8'd13;                        // CR
                6'd39: tx_data <= 8'd10;                        // LF
                default: tx_data <= 8'd32;
            endcase
            char_idx <= (char_idx == 6'd39) ? 6'd63 : (char_idx + 6'd1);
        end
    end
end

endmodule
