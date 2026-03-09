// test_switchres.cpp — Test Switchres 480i trigger from ARM
//
// Compile: arm-linux-gnueabihf-g++ -O2 test_switchres.cpp -o test_switchres
// Run: ./test_switchres
//
// Expected result: VGA output switches to 480i @ 59.94Hz (15.734 kHz horizontal)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

// AXI register offsets
#define AXI_BASE         0xFF200000
#define AXI_SIZE         4096
#define AXI_STATUS_IDX   0   // 0x000
#define AXI_CTRL_IDX     2   // 0x008
#define AXI_SWITCHRES_IDX 3  // 0x00C
#define AXI_MAGIC_IDX    8   // 0x020

// Switchres header location (DDR3 offset 8)
#define SWITCHRES_HEADER_ADDR 0x30000008
#define SWITCHRES_HEADER_SIZE 20

// Groovy Switchres header for 640×480i @ 59.94Hz NTSC
// (From doc/switchres_format.md)
const uint8_t groovy_480i_modeline[20] = {
    // Horizontal timing
    0x80, 0x02,  // H = 640 (0x0280)
    0x10,        // HFP = 16
    0x60,        // HS = 96
    0x6A,        // HBP = 106

    // Vertical timing (per field)
    0xF0, 0x00,  // V = 240 (0x00F0)
    0x04,        // VFP = 4
    0x03,        // VS = 3
    0x0F,        // VBP = 15

    // PLL dividers (13.5 MHz from 50 MHz)
    0x1B,        // PLL_M0 = 27
    0x00,        // PLL_M1 = 0
    0x64,        // PLL_C0 = 100
    0x00,        // PLL_C1 = 0

    // PLL K (fractional, little-endian uint32)
    0x01, 0x00, 0x00, 0x00,  // PLL_K = 1

    0x01,        // CE_PIX = 1 (no division)
    0x01         // INTERLACED = 1 (interlaced framebuffer)
};

int main() {
    printf("=== Switchres 480i Test ===\n\n");

    // Open /dev/mem
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open /dev/mem: %s\n", strerror(errno));
        fprintf(stderr, "       Try running with sudo\n");
        return 1;
    }

    // Map AXI registers
    volatile uint32_t* axi = (uint32_t*)mmap(NULL, AXI_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED,
                                              fd, AXI_BASE);
    if (axi == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap AXI registers: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    // Check magic register (verify Groovy MP4 core is loaded)
    printf("Step 1: Checking magic register...\n");
    uint32_t magic = axi[AXI_MAGIC_IDX];
    printf("        Magic = 0x%08X ", magic);
    if (magic == 0xA1EC0001) {
        printf("✓ Groovy MP4 core detected\n\n");
    } else {
        printf("✗ Wrong core!\n");
        printf("        Expected: 0xA1EC0001 (Groovy MP4)\n");
        printf("        Got:      0x%08X\n", magic);
        printf("        Make sure Groovy core is loaded and MP4 mode is enabled in OSD\n");
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }

    // Map Switchres header region (DDR3 offset 8)
    // NOTE: mmap requires page-aligned offset, so map the whole page starting at 0x30000000
    printf("Step 2: Writing 480i header to DDR3 @ 0x%08X...\n", SWITCHRES_HEADER_ADDR);

    const uint32_t PAGE_SIZE = 4096;
    const uint32_t BASE_ADDR = 0x30000000;  // Page-aligned base
    const uint32_t OFFSET_IN_PAGE = 8;      // Header is 8 bytes into the page

    uint8_t* page = (uint8_t*)mmap(NULL, PAGE_SIZE,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd, BASE_ADDR);
    if (page == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap DDR3 page: %s\n", strerror(errno));
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }

    // Write 480i modeline to offset 8 within the page
    memcpy(page + OFFSET_IN_PAGE, groovy_480i_modeline, SWITCHRES_HEADER_SIZE);
    munmap(page, PAGE_SIZE);
    printf("        ✓ Header written\n\n");

    // Verify header was written correctly
    printf("Step 3: Verifying header...\n");
    page = (uint8_t*)mmap(NULL, PAGE_SIZE,
                          PROT_READ,
                          MAP_SHARED,
                          fd, BASE_ADDR);
    if (page == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap DDR3 page for readback: %s\n", strerror(errno));
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }

    bool header_ok = true;
    uint8_t* switchres_hdr = page + OFFSET_IN_PAGE;
    for (int i = 0; i < SWITCHRES_HEADER_SIZE; i++) {
        if (switchres_hdr[i] != groovy_480i_modeline[i]) {
            printf("        ✗ Mismatch at byte %d: wrote 0x%02X, read 0x%02X\n",
                   i, groovy_480i_modeline[i], switchres_hdr[i]);
            header_ok = false;
        }
    }
    munmap(page, PAGE_SIZE);

    if (!header_ok) {
        fprintf(stderr, "ERROR: Header verification failed!\n");
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }
    printf("        ✓ Header verified\n\n");

    // Trigger Switchres (frame 0 = apply immediately)
    printf("Step 4: Triggering Switchres...\n");
    printf("        Writing 0x00000001 to AXI register 0x00C\n");
    axi[AXI_SWITCHRES_IDX] = 0x1;  // bit 0 = trigger, bits[31:1] = frame 0
    printf("        ✓ Trigger sent\n\n");

    // Wait for PLL to stabilize
    printf("Step 5: Waiting for PLL to stabilize (100ms)...\n");
    usleep(100000);
    printf("        ✓ Done\n\n");

    // Read back switchres register (should be 0 after auto-clear)
    uint32_t switchres_reg = axi[AXI_SWITCHRES_IDX];
    printf("Step 6: Verifying trigger cleared...\n");
    printf("        Switchres register = 0x%08X ", switchres_reg);
    if (switchres_reg == 0) {
        printf("✓ (auto-cleared)\n\n");
    } else {
        printf("? (expected 0, but got 0x%08X)\n", switchres_reg);
        printf("        This might be normal if trigger hasn't cleared yet\n\n");
    }

    printf("=== Test Complete ===\n\n");
    printf("Expected result:\n");
    printf("  - VGA output: 640×480i @ 59.94Hz\n");
    printf("  - Horizontal frequency: 15.734 kHz\n");
    printf("  - Pixel clock: 13.5 MHz\n");
    printf("  - Interlaced fields (240 lines each)\n\n");
    printf("If connected to a CRT:\n");
    printf("  - Display should sync to NTSC timing\n");
    printf("  - May show black screen (no active framebuffer yet)\n");
    printf("  - Use oscilloscope to measure horizontal frequency\n\n");
    printf("Next: Run mp4_play to display actual video\n");

    // Cleanup
    munmap((void*)axi, AXI_SIZE);
    close(fd);

    return 0;
}
