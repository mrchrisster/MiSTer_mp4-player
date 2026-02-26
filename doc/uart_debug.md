# FPGA Debug UART — Tracing Guide

## Overview

The `mp4_debug_uart` module sends one ASCII status line per second to the HPS UART
`/dev/ttyS1`.  This lets you observe FPGA internals without rebuilding software or
attaching a JTAG probe.

```
UART path:
  FPGA mp4_debug_uart.tx_pin
    → sys_top.v  uart_rxd
    → cyclonev_hps_interface_peripheral_uart .rxd
    → ARM Linux  /dev/ttyS1
```

## Reading Output on the MiSTer ARM

```bash
microcom /dev/ttyS1 -s 115200
# or
stty -F /dev/ttyS1 115200 raw && cat /dev/ttyS1
```

Press `Ctrl+X` to exit microcom.

## Output Format

One line per second, 40 characters, 115200 8N1:

```
T=HHHH D=HHHH V=HHHH R=HHHH W=HHHH B=H\r\n
```

| Field | Meaning | Expected value |
|---|---|---|
| `T` | `dma_trigger` pulses in last second | 30 at 30 fps |
| `D` | `dma_done` pulses in last second | Should equal T |
| `V` | `fb_vbl` (VBlank) pulses in last second | 0x003C = 60 at 60 Hz |
| `R` | Avalon **read**-stall cycles (`waitrequest & read`) | 0 = reads complete promptly |
| `W` | Avalon **write**-stall cycles (`waitrequest & write`) | 0 = writes complete promptly |
| `B` | Current `buf_sel` value | 0 or 1 |

All counts are 16-bit hex (zero-padded), saturating at 0xFFFF.

R and W together reveal which phase the DMA is stuck in:
- **R high, W=0** → frozen waiting for DDR3 read data (S_READ_Y/U/V state)
- **W high, R=0** → frozen waiting for DDR3 write accept (S_WRITE state)
- **Both 0** → DMA completes normally; compare T and D

### Example — healthy playback at 30 fps, 60 Hz display

```
T=001E D=001E V=003C R=0000 W=0000 B=1
T=001E D=001E V=003C R=0000 W=0000 B=0
```

### Example — DMA stuck in write phase (AXI write FIFO frozen)

```
T=0001 D=0000 V=003C R=0000 W=EA60 B=0
```

`W` is very high, `R=0`, `D=0`.  The DMA issued a write but the Avalon
interconnect never de-asserted `waitrequest`.  This is the classic
"64-write hang" — the AXI FIFO is full, likely from an out-of-bounds
address.  Power-cycle to recover.

### Example — DMA stuck in read phase

```
T=0001 D=0000 V=003C R=C350 W=0000 B=0
```

`R` is very high, `W=0`, `D=0`.  The DMA issued a burst read but the
Avalon interconnect is stalling it.  Possible causes: invalid read address,
f2sdram arbiter locked by another master, reset sequencing issue.

### Example — VBlank not firing

```
T=001E D=001E V=0000 R=0000 W=0000 B=1
```

`V=0` means `hdmi_vbl` is not reaching `clk_sys` domain.
Check that FB_EN is active (OSD "Video Mode: MP4") and the ASCAL is running.

### Example — no DMA activity at all

```
T=0000 D=0000 V=003C R=0000 W=0000 B=0
```

FPGA alive (VBlank present), but ARM daemon has not sent any `dma_trigger`.
Daemon may have crashed or is stuck waiting for `dma_done` / `fb_vbl`.

## Implementation Details

### Files

| File | Description |
|---|---|
| `rtl/uart_tx.v` | UART TX byte-sender (from mpeg2fpga-master). Params: CLK_FRE (MHz), BAUD_RATE |
| `rtl/mp4_debug_uart.v` | Counter + sequencer module |
| `sys/sys_top.v` lines 392–412 | Instantiation + `assign uart_rxd = mp4_debug_uart_tx` |

### Clock frequency

The baud rate depends on `CLK_FRE` (default: 50 MHz).  If output is garbled:

1. Determine actual `clk_sys` frequency (Groovy reprograms its PLL per video mode).
2. Change `#(.CLK_FRE(50))` in sys_top.v to the correct value and recompile.

Common values: 25, 48, 50, 100 (MHz).

### uart_rxd wiring

`uart_rxd` was previously driven by `emu.UART_TXD` (always 0 in Groovy.sv, which
looks like a UART break condition to the HPS).  It is now driven by the debug UART:

```verilog
// sys_top.v (inside `ifdef MISTER_FB)
assign uart_rxd = mp4_debug_uart_tx;

// emu instantiation
.UART_TXD(emu_uart_txd_nc),   // disconnected
```

Without MISTER_FB:
```verilog
assign uart_rxd = 1'b1;   // idle (no activity)
```

## Troubleshooting Reference

### System freeze after DMA trigger

**Symptom**: `T=0001 D=0000`, `W` very high, Ctrl+C does not kill daemon (process in D state).

**Cause**: Avalon write FIFO full (64-entry limit), bus permanently frozen.

**Investigation steps**:
1. Check `R` and `W` fields to determine which bus phase is stalling.
   - `W` high → write hang — check `yuv_rgb_base` address is valid.
   - `R` high → read hang — check `yuv_y/u/v_base` addresses are valid.
2. Verify all addresses are in the valid DDR3 window (0x30000000–0x30200000).
3. Check address formula: `avl_address = byte_addr[31:3]` — must be DENSE format.
4. Check `ram_address` bit width: must be 29 bits (Avalon word address).

### Other cores show out-of-sync HDMI after running mp4_play

**Cause**: fpga2sdram AXI bridge frozen (see above).  Power-cycle the DE10-nano to recover.

### dma_done count less than dma_trigger count

Triggers fired but not all completed.  DMA is taking longer than one frame period,
or the daemon triggers a second DMA before the first completes (race condition in
polling loop — verify `dma_done` sticky latch is cleared before re-triggering).

### VBlank count wrong (not ~60)

- `V=0`: FB_EN not active, or ASCAL not running (check OSD mode).
- `V` very high: synchroniser or latch firing spuriously.  Check `fb_vbl_sys`
  two-FF synchroniser in sys_top.v lines 294–296.
