// test_yuv_dump.cpp - Dump RGB output after YUV→RGB DMA to diagnose shift
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

    // Map YUV region
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

    // Fill YUV with simple test pattern: all Y=128 (mid-grey), U=V=128
    uint8_t* y_plane = yuv;
    uint8_t* u_plane = yuv + (FB_W * FB_H);
    uint8_t* v_plane = u_plane + (FB_W/2 * FB_H/2);

    printf("Writing simple Y pattern: rows 0-9 = Y=50, rows 10-19 = Y=150...\n");
    for (int row = 0; row < FB_H; row++) {
        uint8_t y_val = 50 + ((row / 10) % 10) * 20;  // 50, 70, 90, 110, ...
        for (int col = 0; col < FB_W; col++) {
            y_plane[row * FB_W + col] = y_val;
        }
    }
    memset(u_plane, 200, FB_W/2 * FB_H/2);  // Changed from 128 to 200 to trace bug
    memset(v_plane, 200, FB_W/2 * FB_H/2);  // Changed from 128 to 200 to trace bug

    // VERIFY: Read back Y to confirm ARM wrote correctly (check boundary at 620)
    printf("Verifying ARM wrote Y correctly:\n");
    printf("  Y[0..19]     = ");
    for (int i = 0; i < 20; i++) printf("%d ", y_plane[i]);
    printf("\n  Y[610..629]  = ");
    for (int i = 610; i < 630; i++) printf("%d ", y_plane[i]);
    printf("\n  Y[630..639]  = ");
    for (int i = 630; i < 640; i++) printf("%d ", y_plane[i]);
    printf("\n");

    // Clear RGB to detect any pixels not written by FPGA
    memset(rgb, 0xFF, FB_W * FB_H * 2);

    // Trigger FPGA DMA
    printf("Triggering FPGA DMA...\n");
    axi[2] = 0;  // buf_sel = 0
    axi[4] = YUV_Y_PHYS;
    axi[5] = YUV_Y_PHYS + (FB_W * FB_H);
    axi[6] = YUV_Y_PHYS + (FB_W * FB_H) + (FB_W/2 * FB_H/2);
    axi[7] = RGB_PHYS;

    // DSB: wait for all writes to complete (flush cache to DDR3) before DMA
    __asm__ volatile ("dsb sy" ::: "memory");
    axi[2] = 2;  // trigger DMA

    // Wait for DMA done
    int timeout = 0;
    while (!(axi[0] & 8) && timeout++ < 100000);

    if (timeout >= 100000) {
        printf("ERROR: DMA timeout!\n");
        return 1;
    }

    printf("DMA complete. Dumping RGB output for rows 0-11...\n\n");

    // Dump first 4 rows - show first 100 pixels AND last 20 pixels
    for (int row = 0; row < 4; row++) {
        printf("Row %2d (%s) first 100 px: ", row, (row % 2 == 0) ? "EVEN" : "ODD ");
        for (int col = 0; col < 100; col++) {
            uint16_t px = rgb[row * FB_W + col];
            uint8_t r = (px & 0x001F) << 3;
            uint8_t g = ((px >> 5) & 0x3F) << 2;
            uint8_t b = (px >> 11) << 3;
            uint8_t grey = (r + g + b) / 3;
            if (col % 10 == 0) printf("\n  [%3d]: ", col);
            printf("%3d ", grey);
        }
        printf("\n  Last 20 px [620-639]: ");
        for (int col = 620; col < 640; col++) {
            uint16_t px = rgb[row * FB_W + col];
            uint8_t r = (px & 0x001F) << 3;
            uint8_t g = ((px >> 5) & 0x3F) << 2;
            uint8_t b = (px >> 11) << 3;
            uint8_t grey = (r + g + b) / 3;
            printf("%3d ", grey);
        }
        printf("\n\n");
    }

    printf("\nExpected: All rows should have same greyscale value (grouped by 10).\n");
    printf("If EVEN rows show different/shifted values, that confirms the bug.\n");

    munmap((void*)axi, 4096);
    munmap(yuv, 460800);
    munmap(rgb, FB_W * FB_H * 2);
    close(fd);

    return 0;
}
