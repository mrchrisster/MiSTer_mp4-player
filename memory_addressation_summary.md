# DE10-Nano Memory Addressation Summary

This document summarizes the memory addressing architecture of the Terasic DE10-Nano board, focusing on the interaction between the Hard Processor System (HPS) and the FPGA fabric. The information is distilled from the documentation provided at [https://github.com/zangman/de10-nano/tree/master/docs](https://github.com/zangman/de10-nano/tree/master/docs).

## 1. HPS-FPGA Communication Architecture

The DE10-Nano's Cyclone V SoC integrates a dual-core ARM Cortex-A9 Hard Processor System (HPS) and an FPGA fabric on a single chip. Communication between these two parts is primarily handled by ARM AMBA AXI bridges. Since the Intel/Altera FPGA ecosystem uses the Avalon Memory-Mapped (MM) interface standard, the Platform Designer tool automatically inserts the necessary logic to convert between AXI and Avalon protocols.

There are three key bridges for this communication:

*   **HPS-to-FPGA Bridge (Heavyweight):** A high-throughput, low-latency bridge designed for large data transfers. It exposes a 960MB address space from the HPS to the FPGA.
*   **Lightweight HPS-to-FPGA Bridge:** A lower-speed bridge intended for accessing control and status registers on FPGA components. It maps a 2MB address space.
*   **FPGA-to-HPS Bridge:** Allows FPGA components to initiate communication with the HPS.

## 2. Accessing FPGA Components from the HPS (Linux)

From the perspective of the Linux operating system running on the HPS, any custom logic built into the FPGA (as an Avalon Agent/Slave) appears as a set of memory-mapped I/O registers.

### Physical Base Addresses

The Linux device tree specifies the physical base addresses for the bridges. These are the starting points for all communication to the FPGA:

*   **Lightweight HPS-to-FPGA Bridge:** `0xff400000`
*   **HPS-to-FPGA (Heavyweight) Bridge:** `0xff500000`
*   **FPGA-to-HPS Bridge:** `0xff600000`

### Address Mapping

1.  When you add a custom component (e.g., a PIO module, a custom accelerator) to your FPGA design in Platform Designer, you connect it to one of the HPS-to-FPGA bridges.
2.  Platform Designer assigns this component an **offset** relative to the bridge's base address.
3.  To access this component from a C/C++ program in Linux, you must:
    a. Open the `/dev/mem` device file.
    b. Use the `mmap()` system call to map the physical address (`bridge_base_address` + `component_offset`) into your process's virtual address space.
    c. The `mmap()` call returns a pointer. You can now read from or write to this pointer to directly interact with the hardware registers of your component in the FPGA.

## 3. High-Speed FPGA Access to SDRAM

The 1GB DDR3 SDRAM on the DE10-Nano is physically wired to the HPS's memory controller. For the FPGA to achieve high-speed, low-latency access to this memory (e.g., for video processing or high-speed data acquisition), it uses a dedicated `FPGA-to-SDRAM` port.

### Architecture

*   The HPS SDRAM Controller acts as an **Avalon Agent (Slave)**.
*   To access it, you must design a custom **Avalon Host (Master)** component within your FPGA logic.
*   This connection bypasses the main L3 Interconnect Switch in the HPS, providing a more direct and faster path to memory compared to going through the general-purpose FPGA-to-HPS bridge.

### Avalon-MM Burst Interface

For maximum efficiency, the custom Avalon Host (Master) on the FPGA should implement the **Avalon-MM Burst** protocol. This allows the FPGA to read or write large, contiguous blocks of data by specifying a starting address and a `burstcount` (the number of data words to transfer). This is significantly more efficient than transferring one word at a time.

## 4. Enabling the Hardware via the Device Tree

The Linux kernel is unaware of the hardware configuration on the FPGA by default. The **Device Tree** is a data structure, loaded by the bootloader, that describes the system's hardware to the kernel.

*   By default, the HPS-FPGA bridges are marked as `disabled` in the DE10-Nano's standard device tree.
*   To use any of the bridges, you must create a custom device tree source file (`.dts`). This file includes the default configuration and adds new entries to override the bridge statuses.
*   For each bridge you intend to use, you must set its `status` property to `"okay"` and `bridge-enable` to `<1>`.
*   This custom `.dts` file is then compiled into a binary `.dtb` blob, which must be placed on the boot partition to be used by the kernel.
*   On boot, the kernel reads this configuration and loads the necessary drivers, making the bridges accessible (e.g., via `/dev/mem` and `/sys/class/fpga_bridge/`).
