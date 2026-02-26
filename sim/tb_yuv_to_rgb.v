`timescale 1ns/1ps

// tb_yuv_to_rgb.v — iverilog testbench for yuv_to_rgb.sv
//
// Verifies BT.601 YUV→RGB conversion and RGB565 channel packing.
// Critical check: green (0x07E0) and blue (0x001F) expose the old B/G swap bug.
//   Old wrong RBG565: green→0x001F (appears blue), blue→0x07E0 (appears green)
//   Correct RGB565:   green→0x07E0, blue→0x001F
//
// Run:
//   iverilog -g2012 -o sim/tb_yuv_to_rgb.vvp sim/tb_yuv_to_rgb.v rtl/yuv_to_rgb.sv
//   vvp sim/tb_yuv_to_rgb.vvp

module tb_yuv_to_rgb;

reg        clk = 1'b0;
reg        reset = 1'b1;
reg  [7:0] Y = 8'd16, U = 8'd128, V = 8'd128;
reg        data_valid_in = 1'b0;
wire [15:0] rgb565;
wire        data_valid_out;

always #5 clk = ~clk;   // 100 MHz

yuv_to_rgb dut (
    .clk           (clk),
    .reset         (reset),
    .Y             (Y),
    .U             (U),
    .V             (V),
    .data_valid_in (data_valid_in),
    .rgb565        (rgb565),
    .data_valid_out(data_valid_out)
);

integer pass_cnt = 0, fail_cnt = 0;

// Feed one YUV pixel (one-clock pulse on data_valid_in), wait for
// data_valid_out, then compare rgb565 to expected.
task test_pixel;
    input [7:0]  ty, tu, tv;
    input [15:0] expected;
    begin
        @(negedge clk);
        Y = ty; U = tu; V = tv; data_valid_in = 1'b1;
        @(negedge clk);
        Y = 8'd16; U = 8'd128; V = 8'd128; data_valid_in = 1'b0;
        @(posedge data_valid_out); #1;
        if (rgb565 === expected) begin
            $display("PASS  Y=%3d U=%3d V=%3d  ->  rgb565=0x%04X",
                     ty, tu, tv, rgb565);
            pass_cnt = pass_cnt + 1;
        end else begin
            $display("FAIL  Y=%3d U=%3d V=%3d  ->  got=0x%04X  exp=0x%04X",
                     ty, tu, tv, rgb565, expected);
            fail_cnt = fail_cnt + 1;
        end
    end
endtask

initial begin
    $display("=== yuv_to_rgb testbench ===");
    $display("Standard RGB565: R[15:11] G[10:5] B[4:0]");
    $display("");

    // Hold reset for 3 clocks
    repeat(3) @(posedge clk);
    @(negedge clk); reset = 1'b0;
    @(negedge clk);  // one idle cycle

    // ── BT.601 limited-range formulas ────────────────────────────────────────
    // c = Y-16, d = U-128, e = V-128
    // R = clamp((298c + 409e       + 128) >> 8, 0, 255)
    // G = clamp((298c - 100d - 208e + 128) >> 8, 0, 255)
    // B = clamp((298c + 516d       + 128) >> 8, 0, 255)
    // RGB565 = {R[7:3], G[7:2], B[7:3]}

    // Black: Y=16,U=128,V=128  c=d=e=0  R=G=B=0  → 0x0000
    test_pixel(8'd16,  8'd128, 8'd128, 16'h0000);

    // White: Y=235,U=128,V=128  c=219,d=e=0
    //   R=G=B=(298*219+128)>>8=65390>>8=255  → 0xFFFF
    test_pixel(8'd235, 8'd128, 8'd128, 16'hFFFF);

    // Red: Y=81,U=90,V=240  c=65,d=-38,e=112
    //   R=(298*65+409*112+128)>>8=65306>>8=255
    //   G=(298*65+3800-23296+128)>>8=2>>8=0
    //   B=(298*65-19608+128)>>8=-110>>8→0(clamped)
    //   → 0xF800
    test_pixel(8'd81,  8'd90,  8'd240, 16'hF800);

    // Green: Y=145,U=54,V=34  c=129,d=-74,e=-94
    //   R=(298*129-38446+128)>>8=124>>8=0
    //   G=(298*129+7400+19552+128)>>8=65522>>8=255
    //   B=(298*129-38184+128)>>8=386>>8=1  → B5=0
    //   Correct RGB565 → 0x07E0
    //   Wrong  RBG565 → 0x001F  (what old code produced — showed BLUE on screen)
    test_pixel(8'd145, 8'd54,  8'd34,  16'h07E0);

    // Blue: Y=41,U=240,V=110  c=25,d=112,e=-18
    //   R=(298*25-7362+128)>>8=216>>8=0
    //   G=(298*25-11200+3744+128)>>8=122>>8=0
    //   B=(298*25+57792+128)>>8=65370>>8=255
    //   Correct RGB565 → 0x001F
    //   Wrong  RBG565 → 0x07E0  (what old code produced — showed GREEN on screen)
    test_pixel(8'd41,  8'd240, 8'd110, 16'h001F);

    // ── Summary ───────────────────────────────────────────────────────────────
    $display("");
    $display("Results: %0d passed, %0d failed", pass_cnt, fail_cnt);
    if (fail_cnt == 0)
        $display("ALL PASS");
    else
        $display("FAILURES DETECTED");
    $finish;
end

// Safety watchdog
initial begin #50000; $display("TIMEOUT — check pipeline latency"); $finish; end

endmodule
