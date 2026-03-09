// test_switchres_pattern.cpp — Test Switchres + fill framebuffer with pattern
//
// Compile: arm-linux-gnueabihf-g++ -O2 test_switchres_pattern.cpp -o test_switchres_pattern
// Run: ./test_switchres_pattern
//
// Expected result: CRT shows 480i with colored stripes

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

// Framebuffer
#define FB_BASE          0x30000000
#define FB_WIDTH         640
#define FB_HEIGHT        480
#define FB_SIZE          (FB_WIDTH * FB_HEIGHT * 2)  // 614,400 bytes

// Switchres header location (DDR3 offset 8)
#define SWITCHRES_HEADER_ADDR 0x30000008
#define SWITCHRES_HEADER_SIZE 20

// Groovy Switchres header for 640×480i @ 59.94Hz NTSC
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
    printf("=== Switchres 480i + Test Pattern ===\n\n");

    // Open /dev/mem
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Failed to open /dev/mem: %s\n", strerror(errno));
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

    // Check magic register
    printf("Step 1: Checking magic register...\n");
    uint32_t magic = axi[AXI_MAGIC_IDX];
    if (magic != 0xA1EC0001) {
        printf("        ✗ Wrong core! (got 0x%08X, expected 0xA1EC0001)\n", magic);
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }
    printf("        ✓ Groovy MP4 core detected\n\n");

    // Map framebuffer
    printf("Step 2: Mapping framebuffer @ 0x%08X (%d KB)...\n", FB_BASE, FB_SIZE / 1024);
    uint16_t* fb = (uint16_t*)mmap(NULL, FB_SIZE,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd, FB_BASE);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap framebuffer: %s\n", strerror(errno));
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }
    printf("        ✓ Framebuffer mapped\n\n");

    // Fill framebuffer with vertical color stripes (BGR565 format)
    printf("Step 3: Drawing test pattern...\n");
    for (int y = 0; y < FB_HEIGHT; y++) {
        for (int x = 0; x < FB_WIDTH; x++) {
            uint16_t color;
            int stripe = (x / 80) % 8;  // 8 stripes, 80 pixels each

            switch (stripe) {
                case 0: color = 0xFFFF; break;  // White
                case 1: color = 0xF800; break;  // Blue (B=31)
                case 2: color = 0x07E0; break;  // Green (G=63)
                case 3: color = 0x001F; break;  // Red (R=31)
                case 4: color = 0xFFE0; break;  // Cyan (B+G)
                case 5: color = 0xF81F; break;  // Magenta (B+R)
                case 6: color = 0x07FF; break;  // Yellow (G+R)
                case 7: color = 0x0000; break;  // Black
                default: color = 0xFFFF;
            }

            fb[y * FB_WIDTH + x] = color;
        }
    }
    printf("        ✓ Test pattern drawn (8 vertical stripes)\n\n");

    // Map Switchres header region (page-aligned)
    printf("Step 4: Writing 480i header to DDR3 @ 0x%08X...\n", SWITCHRES_HEADER_ADDR);
    const uint32_t PAGE_SIZE = 4096;
    const uint32_t BASE_ADDR = 0x30000000;
    const uint32_t OFFSET_IN_PAGE = 8;

    uint8_t* page = (uint8_t*)mmap(NULL, PAGE_SIZE,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd, BASE_ADDR);
    if (page == MAP_FAILED) {
        fprintf(stderr, "ERROR: Failed to mmap DDR3 page: %s\n", strerror(errno));
        munmap(fb, FB_SIZE);
        munmap((void*)axi, AXI_SIZE);
        close(fd);
        return 1;
    }

    memcpy(page + OFFSET_IN_PAGE, groovy_480i_modeline, SWITCHRES_HEADER_SIZE);
    munmap(page, PAGE_SIZE);
    printf("        ✓ Header written\n\n");

    // Trigger Switchres
    printf("Step 5: Triggering Switchres...\n");
    axi[AXI_SWITCHRES_IDX] = 0x1;  // bit 0 = trigger, frame 0
    printf("        ✓ Trigger sent\n\n");

    // Wait for PLL to stabilize
    printf("Step 6: Waiting for PLL to stabilize (100ms)...\n");
    usleep(100000);
    printf("        ✓ Done\n\n");

    printf("=== Test Complete ===\n\n");
    printf("Expected result:\n");
    printf("  - CRT should display 8 vertical color stripes:\n");
    printf("    White, Blue, Green, Red, Cyan, Magenta, Yellow, Black\n");
    printf("  - Resolution: 640×480i @ 59.94Hz (15.734 kHz horizontal)\n\n");
    printf("If you see the stripes, Switchres is working!\n");
    printf("If black screen, check OSD 'Video Mode' is set to 'MP4'\n");

    // Cleanup
    munmap(fb, FB_SIZE);
    munmap((void*)axi, AXI_SIZE);
    close(fd);

    return 0;
}
