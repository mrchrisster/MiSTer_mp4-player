// ram2_arbiter.v - Simple 2-to-1 arbiter for DDR3 ram2 port
// Master 0: Audio system (low priority)
// Master 1: fb_scan_out (high priority - time-critical CRT scan)

module ram2_arbiter (
    input wire        clk,
    input wire        reset,

    // Master 0: Audio system
    input  wire [28:0] m0_address,
    input  wire [7:0]  m0_burstcount,
    input  wire [7:0]  m0_byteenable,
    input  wire [63:0] m0_writedata,
    input  wire        m0_read,
    input  wire        m0_write,
    output wire        m0_waitrequest,
    output wire [63:0] m0_readdata,
    output wire        m0_readdatavalid,

    // Master 1: fb_scan_out (high priority)
    input  wire [28:0] m1_address,
    input  wire [7:0]  m1_burstcount,
    input  wire        m1_read,
    output wire        m1_waitrequest,
    output wire [63:0] m1_readdata,
    output wire        m1_readdatavalid,

    // Slave: DDR3 ram2 port
    output reg  [28:0] s_address,
    output reg  [7:0]  s_burstcount,
    output reg  [7:0]  s_byteenable,
    output reg  [63:0] s_writedata,
    output reg         s_read,
    output reg         s_write,
    input  wire        s_waitrequest,
    input  wire [63:0] s_readdata,
    input  wire        s_readdatavalid
);

// Arbitration state
reg active_master;  // 0 = audio, 1 = fb_scan_out
reg grant_locked;   // Lock grant during burst

// Grant logic: fb_scan_out has priority when not locked
wire grant_m1 = (m1_read || m1_write) && !grant_locked;
wire grant_m0 = (m0_read || m0_write) && !grant_m1 && !grant_locked;

// Lock grant during burst (unlock when burst completes)
always @(posedge clk) begin
    if (reset) begin
        active_master <= 1'b0;
        grant_locked  <= 1'b0;
    end else begin
        // Lock on new grant
        if (!grant_locked && (grant_m0 || grant_m1)) begin
            active_master <= grant_m1 ? 1'b1 : 1'b0;
            grant_locked  <= 1'b1;
        end
        // Unlock when burst completes (readdatavalid or write accepted)
        else if (grant_locked && (s_readdatavalid || (s_write && !s_waitrequest))) begin
            grant_locked <= 1'b0;
        end
    end
end

// Mux master signals to slave based on active_master
always @(*) begin
    if (active_master == 1'b1) begin
        // fb_scan_out active
        s_address    = m1_address;
        s_burstcount = m1_burstcount;
        s_byteenable = 8'hFF;  // fb_scan_out reads full words
        s_writedata  = 64'h0;  // fb_scan_out never writes
        s_read       = m1_read;
        s_write      = 1'b0;
    end else begin
        // Audio active
        s_address    = m0_address;
        s_burstcount = m0_burstcount;
        s_byteenable = m0_byteenable;
        s_writedata  = m0_writedata;
        s_read       = m0_read;
        s_write      = m0_write;
    end
end

// Waitrequest routing
assign m0_waitrequest = (active_master == 1'b0) ? s_waitrequest : 1'b1;
assign m1_waitrequest = (active_master == 1'b1) ? s_waitrequest : 1'b1;

// Readdata routing
assign m0_readdata      = s_readdata;
assign m0_readdatavalid = (active_master == 1'b0) ? s_readdatavalid : 1'b0;
assign m1_readdata      = s_readdata;
assign m1_readdatavalid = (active_master == 1'b1) ? s_readdatavalid : 1'b0;

endmodule
