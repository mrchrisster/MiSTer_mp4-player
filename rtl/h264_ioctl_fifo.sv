`timescale 1ns / 1ps

//=============================================================================
//  h264_ioctl_fifo.sv
//  MP4 stream capture: ioctl write FIFO + AXI4-Lite read slave
//
//  ADDRESS MAP (H2F Lightweight AXI Bridge, base 0xFF200000):
//    Offset 0x000  Status Register (RO):
//                    [0] = ioctl_download active (stream is live)
//                    [1] = FIFO not empty  (data ready for ARM to read)
//                  [31:2] = reserved / zero
//    Offset 0x004  Data Register (RO, self-advancing):
//                    [7:0] = next byte from FIFO
//                    Reading this register pops one byte; FIFO advances
//                    automatically on every successful AXI read transaction.
//
//  THROTTLE:
//    ioctl_wait is asserted when the FIFO occupancy exceeds FIFO_HI_WATER
//    (490 of 512 bytes).  The MiSTer daemon will pause sending bytes until
//    the ARM daemon has drained the FIFO and ioctl_wait de-asserts.
//
//  DEBUG STATE VECTOR [31:0]  — Section 6 of mission brief:
//    [0]    ioctl_download active (stream is live)
//    [1]    ioctl_wait asserted   (FIFO nearly full, throttle engaged)
//    [2]    AXI read of Data register detected (ARM reading FIFO)
//    [3]    FIFO empty flag
//    [15:8] FIFO occupancy — lower 8 bits (0..255; saturates at 255 if >255)
//    [31:16] Reserved / zero
//
//  INTEGRATION NOTE — sys_top.v:
//    The axi_* ports connect to the signals exposed by the
//    cyclonev_hps_interface_masters primitive in sys_top.v (see patch in
//    doc/mp4_player_mission.md).  The emu module forwards them as LW_* ports.
//=============================================================================

module h264_ioctl_fifo (

    // -------------------------------------------------------------------------
    // Clock / Reset
    // -------------------------------------------------------------------------
    input  wire        clk,            // System clock — same as clk_sys in emu
    input  wire        reset,          // Synchronous active-high reset

    // -------------------------------------------------------------------------
    // MiSTer ioctl Write Interface  (data arrives FROM the Linux daemon)
    //   Driven by hps_io inside the emu module.
    //   Pass mp4_active (not raw ioctl_download) so only MP4 traffic enters.
    // -------------------------------------------------------------------------
    input  wire        ioctl_download, // High for the full duration of the download
    input  wire        ioctl_wr,       // One-cycle write strobe; valid with ioctl_dout
    input  wire [7:0]  ioctl_dout,     // 8-bit data byte from the ARM (WIDE=0 → 8-bit)
    output wire        ioctl_wait,     // Assert to PAUSE the MiSTer daemon (backpressure)

    // -------------------------------------------------------------------------
    // AXI4-Lite Slave Interface  (connected to H2F LW AXI Bridge in sys_top.v)
    //   Signal directions are from the perspective of this slave module.
    //   The C++ daemon accesses these via /dev/mem mmap at 0xFF200000.
    // -------------------------------------------------------------------------

    // — Read Address Channel —
    input  wire [20:0] axi_araddr,     // Byte address; only [3:2] are decoded
    input  wire        axi_arvalid,    // ARM: "my read address is valid"
    output reg         axi_arready,   // FPGA: "I accept the address"

    // — Read Data Channel —
    output reg  [31:0] axi_rdata,     // Data returned to ARM
    output reg   [1:0] axi_rresp,     // 2'b00 = OKAY
    output reg         axi_rvalid,    // FPGA: "rdata is valid"
    input  wire        axi_rready,    // ARM: "I accept rdata"

    // — Write Address Channel  (accepted, writes silently ignored) —
    input  wire [20:0] axi_awaddr,
    input  wire        axi_awvalid,
    output wire        axi_awready,

    // — Write Data Channel —
    input  wire [31:0] axi_wdata,
    input  wire  [3:0] axi_wstrb,
    input  wire        axi_wvalid,
    output wire        axi_wready,

    // — Write Response Channel —
    output wire  [1:0] axi_bresp,
    output reg         axi_bvalid,
    input  wire        axi_bready,

    // -------------------------------------------------------------------------
    // UART Debug State Output — Section 6 of Mission Brief
    //   Wire this port into your uart_debug module as an additional input group.
    //   The user is responsible for the wiring; this module only exposes it.
    // -------------------------------------------------------------------------
    output wire [31:0] debug_state
);

//=============================================================================
//  FIFO Parameters
//=============================================================================
localparam FIFO_DEPTH    = 10'd512;   // Total capacity in bytes (MUST be a power of 2)
localparam FIFO_ABITS    = 9;         // Address bits  →  2^9 = 512

// Assert ioctl_wait when occupancy exceeds this threshold.
// The 22-byte headroom covers worst-case pipeline latency before the daemon
// actually stops sending bytes.
localparam FIFO_HI_WATER = 10'd490;

//=============================================================================
//  FIFO Storage and Pointer Registers
//=============================================================================
(* ramstyle = "M10K" *)           // Request block RAM on Cyclone V
reg [7:0] fifo_mem [0:(1 << FIFO_ABITS) - 1];

reg [FIFO_ABITS-1:0] wr_ptr    = '0;   // Points to the next empty slot
reg [FIFO_ABITS-1:0] rd_ptr    = '0;   // Points to the oldest unread byte
reg [FIFO_ABITS:0]   occupancy = '0;   // Valid range: 0 .. FIFO_DEPTH (10 bits)

wire fifo_full  = (occupancy == FIFO_DEPTH);
wire fifo_empty = (occupancy == '0);

//=============================================================================
//  IOCTL Throttle
//=============================================================================
assign ioctl_wait = (occupancy >= FIFO_HI_WATER);

//=============================================================================
//  Internal Read Strobe (set by AXI state machine, consumed by FIFO block)
//=============================================================================
reg do_read;    // One-cycle pulse: pop one byte from the FIFO

//=============================================================================
//  FIFO Write / Read State Machine
//  Single synchronous clock domain — no CDC needed.
//=============================================================================
wire do_write = ioctl_download && ioctl_wr && !fifo_full;

always @(posedge clk) begin
    if (reset) begin
        wr_ptr    <= '0;
        rd_ptr    <= '0;
        occupancy <= '0;
    end else begin

        // ---- Write side (from ioctl) ----
        if (do_write) begin
            fifo_mem[wr_ptr] <= ioctl_dout;
            wr_ptr           <= wr_ptr + 1'b1;
        end

        // ---- Read side (from AXI) ----
        if (do_read && !fifo_empty)
            rd_ptr <= rd_ptr + 1'b1;

        // ---- Occupancy counter ----
        // Simultaneous read+write → no change (one in, one out).
        unique case ({do_write, do_read && !fifo_empty})
            2'b10 : occupancy <= occupancy + 1'b1;
            2'b01 : occupancy <= occupancy - 1'b1;
            default: ;    // 2'b00 or 2'b11 → no change
        endcase

    end
end

//=============================================================================
//  Register Read Values (combinatorial peeks)
//=============================================================================
wire [31:0] status_val = {30'd0, ~fifo_empty, ioctl_download};
wire [31:0] data_val   = {24'd0, fifo_mem[rd_ptr]};  // Peek at FIFO head

//=============================================================================
//  AXI4-Lite Read Slave State Machine
//
//  Three-state pipeline:
//    S_IDLE    — wait for a valid read address
//    S_RESPOND — register rdata; optionally pop the FIFO (data register)
//    S_WAIT    — hold rvalid until the ARM asserts rready
//
//  Address decode uses axi_araddr[3:2] (word-aligned offsets):
//    2'b00 → 0xFF200000  Status Register  (no side effect)
//    2'b01 → 0xFF200004  Data Register    (pops FIFO)
//=============================================================================
localparam AXI_IDLE    = 2'd0;
localparam AXI_RESPOND = 2'd1;
localparam AXI_WAIT    = 2'd2;

reg [1:0]  axi_state  = AXI_IDLE;
reg [20:0] axi_addr_r;              // Latched read address

always @(posedge clk) begin
    if (reset) begin
        axi_state   <= AXI_IDLE;
        axi_arready <= 1'b1;
        axi_rvalid  <= 1'b0;
        axi_rdata   <= 32'd0;
        axi_rresp   <= 2'b00;
        do_read     <= 1'b0;
    end else begin

        do_read <= 1'b0;            // Default: no FIFO pop this cycle

        case (axi_state)

            // -----------------------------------------------------------------
            AXI_IDLE: begin
                axi_rvalid  <= 1'b0;
                axi_arready <= 1'b1;    // Advertise that we can accept an address
                if (axi_arvalid) begin
                    axi_arready <= 1'b0;        // De-assert while processing
                    axi_addr_r  <= axi_araddr;  // Latch address
                    axi_state   <= AXI_RESPOND;
                end
            end

            // -----------------------------------------------------------------
            AXI_RESPOND: begin
                axi_rresp  <= 2'b00;    // OKAY
                axi_rvalid <= 1'b1;
                axi_state  <= AXI_WAIT;

                // Decode word-aligned register offset from bits [3:2]
                case (axi_addr_r[3:2])

                    2'b00: begin    // 0xFF200000 — Status Register (read-only, no side effect)
                        axi_rdata <= status_val;
                    end

                    2'b01: begin    // 0xFF200004 — Data Register (pops FIFO)
                        axi_rdata <= data_val;
                        do_read   <= ~fifo_empty;   // Only pop when data is present
                    end

                    default: begin  // Unmapped — return sentinel value
                        axi_rdata <= 32'hDEAD_BEEF;
                    end

                endcase
            end

            // -----------------------------------------------------------------
            AXI_WAIT: begin
                // Hold rvalid until the ARM acknowledges with rready.
                if (axi_rready) begin
                    axi_rvalid  <= 1'b0;
                    axi_arready <= 1'b1;    // Ready for the next transaction
                    axi_state   <= AXI_IDLE;
                end
            end

            default: axi_state <= AXI_IDLE;

        endcase
    end
end

//=============================================================================
//  Write Channels — Accept and Discard
//  We must respond with OKAY so the ARM interconnect never hangs.
//  The ARM daemon should only perform reads, but /dev/mem doesn't prevent
//  accidental writes.
//=============================================================================
assign axi_awready = 1'b1;     // Always accept write addresses immediately
assign axi_wready  = 1'b1;     // Always accept write data immediately
assign axi_bresp   = 2'b00;    // OKAY

// Issue one bvalid pulse per write address accepted.
always @(posedge clk) begin
    if (reset)
        axi_bvalid <= 1'b0;
    else if (axi_awvalid && !axi_bvalid)
        axi_bvalid <= 1'b1;     // Respond as soon as AW is seen
    else if (axi_bready)
        axi_bvalid <= 1'b0;     // Clear once ARM acknowledges
end

//=============================================================================
//  Debug State Vector — Section 6 of Mission Brief
//
//  Bit mapping (32-bit output):
//    [0]    ioctl_download  — stream is live
//    [1]    ioctl_wait      — throttle is asserted (FIFO almost full)
//    [2]    axi_rd_strobe   — ARM read the Data register (1-cycle latched pulse)
//    [3]    fifo_empty      — nothing in the FIFO yet
//    [15:8] occupancy[7:0]  — lower 8 bits of FIFO fill level
//    [31:16] reserved / zero
//
//  The uart_debug module captures these signals once per second; the
//  1-cycle AXI read strobe will be visible because it latches for one
//  full clock, which is long enough for the UART capture to catch it.
//=============================================================================
reg axi_rd_strobe_r;
always @(posedge clk) begin
    if (reset)
        axi_rd_strobe_r <= 1'b0;
    else
        // do_read fires in S_RESPOND; latch it one cycle so it's stable
        axi_rd_strobe_r <= do_read;
end

assign debug_state = {
    16'd0,                //  [31:16]  Reserved
    occupancy[7:0],       //  [15:8]   FIFO occupancy (lower 8 bits)
    4'd0,                 //  [7:4]    Reserved
    fifo_empty,           //  [3]      FIFO empty flag
    axi_rd_strobe_r,      //  [2]      ARM reading FIFO data (latched)
    ioctl_wait,           //  [1]      Throttle asserted
    ioctl_download        //  [0]      Stream active
};

endmodule
