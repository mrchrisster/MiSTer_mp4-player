// test_yuv_fpga.cpp - Test FPGA YUV→RGB DMA with known pattern
// Writes a simple YUV pattern, triggers FPGA DMA, checks RGB output
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#define AXI_PHYS    0xFF200000UL
#define YUV_Y_PHYS  0x3012C000UL
#define RGB_PHYS    0x30000000UL

#define FB_W 640
#define FB_H 480

int main() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    // Map AXI registers
    volatile uint32_t* axi = (volatile uint32_t*)mmap(NULL, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, AXI_PHYS);
    if (axi == MAP_FAILED) {
        perror("mmap AXI");
        return 1;
    }

    // Map YUV region (Y + U + V)
    uint8_t* yuv = (uint8_t*)mmap(NULL, 460800, PROT_READ | PROT_WRITE,
        MAP_SHARED, fd, YUV_Y_PHYS);
    if (yuv == MAP_FAILED) {
        perror("mmap YUV");
        return 1;
    }

    // Map RGB framebuffer
    uint16_t* rgb = (uint16_t*)mmap(NULL, FB_W * FB_H * 2,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, RGB_PHYS);
    if (rgb == MAP_FAILED) {
        perror("mmap RGB");
        return 1;
    }

    // Clear RGB framebuffer to black
    memset(rgb, 0, FB_W * FB_H * 2);

    // Fill YUV with a simple pattern:
    // - Y: horizontal gradient (0→255 left to right)
    // - U: 128 (neutral)
    // - V: 128 (neutral)
    // This should produce greyscale gradient if conversion works
    uint8_t* y_plane = yuv;
    uint8_t* u_plane = yuv + (FB_W * FB_H);
    uint8_t* v_plane = u_plane + (FB_W/2 * FB_H/2);

    printf("Writing YUV test pattern (vertical stripes)...\n");
    for (int row = 0; row < FB_H; row++) {
        for (int col = 0; col < FB_W; col++) {
            // Vertical stripes: 8 stripes of 80px each
            // Values: 0, 36, 72, 108, 144, 180, 216, 252
            uint8_t stripe = (col / 80) * 36;
            y_plane[row * FB_W + col] = stripe;
        }
    }

    // U/V neutral (128 = no chroma)
    memset(u_plane, 128, FB_W/2 * FB_H/2);
    memset(v_plane, 128, FB_W/2 * FB_H/2);

    // Trigger FPGA DMA
    printf("Triggering FPGA DMA...\n");
    axi[2] = 0;  // buf_sel = 0
    axi[4] = YUV_Y_PHYS;
    axi[5] = YUV_Y_PHYS + (FB_W * FB_H);
    axi[6] = YUV_Y_PHYS + (FB_W * FB_H) + (FB_W/2 * FB_H/2);
    axi[7] = RGB_PHYS;

    __asm__ volatile ("dmb sy" ::: "memory");
    axi[2] = 2;  // trigger DMA

    // Wait for DMA done
    int timeout = 0;
    while (!(axi[0] & 8) && timeout++ < 100000);

    if (timeout >= 100000) {
        printf("ERROR: DMA timeout!\n");
        return 1;
    }

    printf("DMA complete. Checking RGB output...\n");

    // Check first row of RGB output
    printf("First 10 pixels (should be dark to bright greyscale):\n");
    for (int i = 0; i < 10; i++) {
        uint16_t px = rgb[i];
        uint8_t r = (px & 0x001F) << 3;
        uint8_t g = ((px >> 5) & 0x3F) << 2;
        uint8_t b = (px >> 11) << 3;
        printf("  px[%d] = 0x%04X → R=%d G=%d B=%d\n", i, px, r, g, b);
    }

    // Check for duplicates/ghosts at pixel 640 (start of potential ghost region)
    printf("\nPixels at row 0, columns 640-650 (should be BLACK if no overflow):\n");
    for (int i = 640; i < 650; i++) {
        uint16_t px = rgb[i];
        if (px != 0) {
            printf("  WARNING: px[%d] = 0x%04X (expected 0x0000 black!)\n", i, px);
        }
    }

    munmap((void*)axi, 4096);
    munmap(yuv, 460800);
    munmap(rgb, FB_W * FB_H * 2);
    close(fd);

    printf("\nDone. Check screen - you should see 8 vertical greyscale stripes (80px wide each).\n");
    printf("Stripes should go from dark (left) to bright (right): 0, 36, 72, 108, 144, 180, 216, 252.\n");
    printf("If even rows (0,2,4...) are shifted right, there's a bug in the U/V fetch affecting Y read.\n");

    return 0;
}
