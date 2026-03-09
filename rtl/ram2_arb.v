`timescale 1ns / 1ps
//=============================================================================
//  ram2_arb.v — 2-to-1 Avalon MM arbiter for ram2 (DDR3 port)
//
//  Shares ram2 DDR3 port between:
//    Master A = ddr_svc     (ALSA audio + OSD palette, high priority)
//    Master B = fb_scan_out (CRT framebuffer readout, lower priority)
//
//  Arbitration rules:
//    • ddr_svc has priority (audio glitches are audible; it uses <1% bandwidth)
//    • fb_scan_out can tolerate arbitration latency (fetches during HBlank)
//    • Bursts are atomic — no preemption mid-burst
//    • 1-cycle grant latency from IDLE (both masters re-request until granted)
//
//  Clocking:
//    • All modules run at clk_sys for synchronous operation
//    • ddr_svc: moved from clk_audio to clk_sys (no impact on ALSA/palette)
//    • fb_scan_out: already runs at clk_sys (= clk_vid)
//    • This arbiter: runs at clk_sys
//    • ram2 sysmem port: moved from clk_audio to clk_sys
//=============================================================================

module ram2_arb (
    input  wire        clk,           // clk_sys (all modules synchronous)
    input  wire        reset,

    // ── Master A — ddr_svc (read+write, high priority) ─────────────────────
    input  wire [28:0] a_addr,
    input  wire  [7:0] a_burst,
    input  wire        a_read,
    input  wire        a_write,
    input  wire [63:0] a_wdata,
    input  wire  [7:0] a_be,
    output wire        a_waitreq,
    output wire [63:0] a_rdata,
    output wire        a_rdv,

    // ── Master B — fb_scan_out (read-only, lower priority) ─────────────────
    input  wire [28:0] b_addr,
    input  wire  [7:0] b_burst,
    input  wire        b_read,
    output wire        b_waitreq,
    output wire [63:0] b_rdata,
    output wire        b_rdv,

    // ── Shared ram2 DDR3 Avalon port ───────────────────────────────────────
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
localparam ST_A_RD  = 2'd1;   // A owns bus (read burst)
localparam ST_A_WR  = 2'd2;   // A owns bus (write burst)
localparam ST_B     = 2'd3;   // B owns bus (read burst)

reg [1:0] st;
reg [7:0] cnt;   // beats remaining in current burst

// Grant signals (combinational)
wire grant_a = (st == ST_A_RD || st == ST_A_WR);
wire grant_b = (st == ST_B);

// ── Combinational bus mux ─────────────────────────────────────────────────
assign ram_addr  = grant_a ? a_addr  : (grant_b ? b_addr  : 29'd0);
assign ram_burst = grant_a ? a_burst : (grant_b ? b_burst : 8'd1 );
assign ram_read  = grant_a ? a_read  : (grant_b ? b_read  : 1'b0 );
assign ram_write = grant_a ? a_write : 1'b0;
assign ram_wdata = grant_a ? a_wdata : 64'd0;
assign ram_be    = grant_a ? a_be    : 8'hFF;

// waitrequest: pass-through to active master, stall the idle one
assign a_waitreq = grant_a ? ram_waitreq : 1'b1;
assign b_waitreq = grant_b ? ram_waitreq : 1'b1;

// readdata broadcast; readdatavalid routed to correct master
assign a_rdata = ram_rdata;
assign b_rdata = ram_rdata;
assign a_rdv   = (st == ST_A_RD) ? ram_rdv : 1'b0;
assign b_rdv   = (st == ST_B)    ? ram_rdv : 1'b0;

// ── Arbitration FSM ───────────────────────────────────────────────────────
always @(posedge clk) begin
    if (reset) begin
        st  <= ST_IDLE;
        cnt <= 8'd0;
    end else begin
        case (st)

        ST_IDLE: begin
            // ddr_svc (A) has priority over fb_scan_out (B)
            if (a_read) begin
                st  <= ST_A_RD;
                cnt <= a_burst;
            end else if (a_write) begin
                st  <= ST_A_WR;
                cnt <= a_burst;
            end else if (b_read) begin
                st  <= ST_B;
                cnt <= b_burst;
            end
        end

        // A read burst: count incoming readdata beats
        ST_A_RD: begin
            if (ram_rdv) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        // A write burst: count accepted writes
        ST_A_WR: begin
            if (a_write && !ram_waitreq) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        // B read burst: count incoming readdata beats
        ST_B: begin
            if (ram_rdv) begin
                if (cnt == 8'd1) st <= ST_IDLE;
                else             cnt <= cnt - 8'd1;
            end
        end

        default: st <= ST_IDLE;
        endcase
    end
end

endmodule
