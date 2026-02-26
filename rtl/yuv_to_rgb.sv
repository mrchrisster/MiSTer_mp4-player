// yuv_to_rgb.sv
//
// BT.601 limited-range YUV420P → RBG565 conversion pipeline.
// Hardware-confirmed output format for MiSTer ASCAL: R[15:11] B[10:5] G[4:0]
// (B and G channels are swapped vs standard RGB565 — see arm_dma_architecture.md)
//
// Pipeline latency: 4 clock cycles from valid input to valid output.
// Throughput:       1 pixel per clock (fully pipelined).
//
// BT.601 limited-range coefficients (same as ARM software path):
//   c = Y  - 16
//   d = Cb - 128
//   e = Cr - 128
//   R = (298*c         + 409*e + 128) >> 8   clamp [0,255]
//   G = (298*c - 100*d - 208*e + 128) >> 8   clamp [0,255]
//   B = (298*c + 516*d         + 128) >> 8   clamp [0,255]
//
// Quartus will infer Cyclone V DSP blocks for the constant multiplications.

module yuv_to_rgb (
    input  wire        clk,
    input  wire        reset,

    // Pixel inputs (one YUV triplet per clock when data_valid_in=1)
    input  wire [7:0]  Y,          // luma
    input  wire [7:0]  U,          // Cb (blue-difference chroma)
    input  wire [7:0]  V,          // Cr (red-difference chroma)
    input  wire        data_valid_in,

    // Pixel output — valid 4 cycles after corresponding input
    output reg  [15:0] rgb565,     // RBG565: R[15:11] B[10:5] G[4:0]
    output reg         data_valid_out
);

// ── Stage 0: register raw inputs ─────────────────────────────────────────────
reg [7:0] s0_Y, s0_U, s0_V;
reg       s0_valid;

always @(posedge clk) begin
    s0_Y     <= Y;
    s0_U     <= U;
    s0_V     <= V;
    s0_valid <= reset ? 1'b0 : data_valid_in;
end

// ── Stage 1: subtract BT.601 offsets, produce signed operands ────────────────
// c: Y-16   range [-16, 219]  → signed 9-bit
// d: U-128  range [-128, 112] → signed 9-bit
// e: V-128  range [-128, 112] → signed 9-bit
reg signed [8:0] s1_c, s1_d, s1_e;
reg              s1_valid;

always @(posedge clk) begin
    s1_c     <= $signed({1'b0, s0_Y}) - 9'sd16;
    s1_d     <= $signed({1'b0, s0_U}) - 9'sd128;
    s1_e     <= $signed({1'b0, s0_V}) - 9'sd128;
    s1_valid <= reset ? 1'b0 : s0_valid;
end

// ── Stage 2: constant multiplications → Cyclone V DSP blocks ─────────────────
// yy = 298*c  shared by all three channels
// p1 = 409*e  R term
// p2 = 100*d  G term (negative)
// p3 = 208*e  G term (negative)
// p4 = 516*d  B term
//
// Widths: 9-bit signed × 10-bit constant → 19-bit signed product (use 20-bit)
reg signed [19:0] s2_yy, s2_p1, s2_p2, s2_p3, s2_p4;
reg               s2_valid;

always @(posedge clk) begin
    s2_yy    <= 20'sd298 * s1_c;
    s2_p1    <= 20'sd409 * s1_e;
    s2_p2    <= 20'sd100 * s1_d;
    s2_p3    <= 20'sd208 * s1_e;
    s2_p4    <= 20'sd516 * s1_d;
    s2_valid <= reset ? 1'b0 : s1_valid;
end

// ── Stage 3: accumulate with rounding constant (+128 before >>8) ─────────────
// Max unsigned magnitudes:
//   yy:  298*219 = 65262    p1: 409*128 = 52352
//   p2:  100*128 = 12800    p3: 208*128 = 26624   p4: 516*128 = 66048
// R_acc range: [~-57120,  ~117742]  → fits in signed 20-bit (±524287)
// G_acc range: [~-42464,  ~101486]  → fits in signed 20-bit
// B_acc range: [~-70688,  ~123182]  → fits in signed 20-bit
reg signed [19:0] s3_R_acc, s3_G_acc, s3_B_acc;
reg               s3_valid;

always @(posedge clk) begin
    s3_R_acc <= s2_yy + s2_p1 + 20'sd128;
    s3_G_acc <= s2_yy - s2_p2 - s2_p3 + 20'sd128;
    s3_B_acc <= s2_yy + s2_p4 + 20'sd128;
    s3_valid <= reset ? 1'b0 : s2_valid;
end

// ── Stage 4: arithmetic shift right 8, clamp to [0,255], pack RBG565 ─────────
// s3_*_acc >> 8 gives a signed 12-bit result (bits [19:8] with sign extension).
// Clamp: negative → 0, > 255 → 255, else take [7:0].
wire signed [11:0] R_s12 = s3_R_acc[19:8];
wire signed [11:0] G_s12 = s3_G_acc[19:8];
wire signed [11:0] B_s12 = s3_B_acc[19:8];

wire [7:0] R8 = R_s12[11] ? 8'd0 : (R_s12 > 12'sd255 ? 8'd255 : R_s12[7:0]);
wire [7:0] G8 = G_s12[11] ? 8'd0 : (G_s12 > 12'sd255 ? 8'd255 : G_s12[7:0]);
wire [7:0] B8 = B_s12[11] ? 8'd0 : (B_s12 > 12'sd255 ? 8'd255 : B_s12[7:0]);

always @(posedge clk) begin
    // Pack as RBG565: R[15:11] B[10:5] G[4:0]
    rgb565        <= {R8[7:3], B8[7:2], G8[7:3]};
    data_valid_out <= reset ? 1'b0 : s3_valid;
end

endmodule
