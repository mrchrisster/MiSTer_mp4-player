// jtframe_hsize passthrough stub
// The real JTFRAME horizontal scaler is not included in this project.
// This stub passes video signals through unchanged so the design compiles.
module jtframe_hsize #(parameter COLORW=8) (
    input                clk,
    input                pxl_cen,
    input                pxl2_cen,
    input  [3:0]         scale,
    input  [4:0]         offset,
    input                enable,
    input  [COLORW-1:0]  r_in,
    input  [COLORW-1:0]  g_in,
    input  [COLORW-1:0]  b_in,
    input                HS_in,
    input                VS_in,
    input                HB_in,
    input                VB_in,
    output [COLORW-1:0]  r_out,
    output [COLORW-1:0]  g_out,
    output [COLORW-1:0]  b_out,
    output               HS_out,
    output               VS_out,
    output               HB_out,
    output               VB_out
);
    assign r_out  = r_in;
    assign g_out  = g_in;
    assign b_out  = b_in;
    assign HS_out = HS_in;
    assign VS_out = VS_in;
    assign HB_out = HB_in;
    assign VB_out = VB_in;
endmodule
