`timescale 1ns / 1ps
//=============================================================================
//  fb_arb.sv — 2-to-1 Avalon MM burst arbiter
//
//  Shares one 64-bit DDR3 Avalon port (ram1) between two masters:
//    Master A = fb_scan_out  (read-only,  high priority)
//    Master B = yuv_fb_dma   (read+write, lower priority)
//
//  Both masters and this arbiter run at the same clock (clk_vid).
//
//  Arbitration rules:
//    • A burst in progress is NEVER preempted — the current owner keeps the
//      bus until its burst is fully complete.
//    • In IDLE: A wins if both request simultaneously (timing-critical: CRT
//      scan-out must complete its HBlank line fetch before HActive begins).
//    • Burst completion:
//        Read  — when all readdata beats have been received (cnt → 0 on rdv).
//        Write — when all write beats have been consumed   (cnt → 0 on w&&!wreq).
//
//  Notes:
//    • There is a 1-cycle grant latency from IDLE — both masters re-assert
//      their requests every cycle until accepted (fb_scan_out F_FETCH loop;
//      yuv_fb_dma write loop), so 1-cycle latency is harmless.
//    • readdata is broadcast to both masters; routing is via a_rdv / b_rdv.
//=============================================================================

module fb_arb (
    input  wire        clk,
    input  wire        reset,

    // ── Master A — fb_scan_out (read only, high priority) ──────────────────
    input  wire [28:0] a_addr,
    input  wire  [7:0] a_burst,
    input  wire        a_read,
    output wire        a_waitreq,
    output wire [63:0] a_rdata,
    output wire        a_rdv,

    // ── Master B — yuv_fb_dma (read + write, lower priority) ───────────────
    input  wire [28:0] b_addr,
    input  wire  [7:0] b_burst,
    input  wire        b_read,
    input  wire        b_write,
    input  wire [63:0] b_wdata,
    input  wire  [7:0] b_be,
    output wire        b_waitreq,
    output wire [63:0] b_rdata,
    output wire        b_rdv,

    // ── Shared DDR3 Avalon MM port ──────────────────────────────────────────
    output wire [28:0] ram_addr,
    output wire  [7:0] ram_burst,
    output wire        ram_read,
    output wire        ram_write,
    output wire [63:0] ram_wdata,
    output wire  [7:0] ram_be,
    input  wire        ram_waitreq,
    input  wire [63:0] ram_rdata,
    input  wire        ram_rdv
);

// ── State machine ─────────────────────────────────────────────────────────
localparam ST_IDLE  = 2'd0;
localparam ST_A     = 2'd1;   // A owns bus (read burst)
localparam ST_B_RD  = 2'd2;   // B owns bus (read burst)
localparam ST_B_WR  = 2'd3;   // B owns bus (write burst)

reg [1:0] st;
reg [7:0] cnt;   // beats remaining until burst complete

// Grant signals (combinational from st)
wire grant_a = (st == ST_A);
wire grant_b = (st == ST_B_RD || st == ST_B_WR);

// ── Combinational bus mux ─────────────────────────────────────────────────
assign ram_addr  = grant_a ? a_addr  : (grant_b ? b_addr  : 29'd0);
assign ram_burst = grant_a ? a_burst : (grant_b ? b_burst : 8'd1 );
assign ram_read  = grant_a ? a_read  : (grant_b ? b_read  : 1'b0 );
assign ram_write = grant_b ? b_write : 1'b0;
assign ram_wdata = grant_b ? b_wdata : 64'd0;
assign ram_be    = grant_b ? b_be    : 8'hFF;

// waitrequest: pass-through to the active master, stall the idle one
assign a_waitreq = grant_a ? ram_waitreq : 1'b1;
assign b_waitreq = grant_b ? ram_waitreq : 1'b1;

// readdata broadcast; rdv routed to the correct master
assign a_rdata = ram_rdata;
assign b_rdata = ram_rdata;
assign a_rdv   = (st == ST_A)     ? ram_rdv : 1'b0;
assign b_rdv   = (st == ST_B_RD)  ? ram_rdv : 1'b0;

// ── Arbitration state machine (sequential) ────────────────────────────────
always @(posedge clk) begin
    if (reset) begin
        st  <= ST_IDLE;
        cnt <= 8'd0;
    end else begin
        case (st)

        ST_IDLE: begin
            // A has priority (timing-critical CRT line fetch)
            if (a_read) begin
                st  <= ST_A;
                cnt <= a_burst;
            end else if (b_read) begin
                st  <= ST_B_RD;
                cnt <= b_burst;
            end else if (b_write) begin
                st  <= ST_B_WR;
                cnt <= b_burst;
            end
        end

        // Read bursts: count incoming readdata beats.
        // cnt is initialised to burst_count in the IDLE→state transition,
        // so it is always valid before the first rdv arrives.
        ST_A: begin
            if (ram_rdv) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        ST_B_RD: begin
            if (ram_rdv) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        // Write burst: count beats accepted by DDR3 (ram_write && !ram_waitreq).
        ST_B_WR: begin
            if (b_write && !ram_waitreq) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        default: st <= ST_IDLE;
        endcase
    end
end

endmodule
