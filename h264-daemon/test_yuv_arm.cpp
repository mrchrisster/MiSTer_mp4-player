#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define FB_PHYS   0x30000000UL
#define FB_W      1280
#define FB_H      720
#define FB_SIZE   (FB_W * FB_H * 2)

// BT.601 limited-range YUV420P to BGR565 conversion (ARM software)
uint16_t yuv_to_bgr565(uint8_t Y, uint8_t U, uint8_t V) {
    int c = (int)Y - 16;
    int d = (int)U - 128;
    int e = (int)V - 128;

    int r = (298*c + 409*e + 128) >> 8;
    int g = (298*c - 100*d - 208*e + 128) >> 8;
    int b = (298*c + 516*d + 128) >> 8;

    // Clamp to [0, 255]
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;

    // Pack as BGR565: B[15:11] G[10:5] R[4:0]
    return ((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3);
}

int main(int argc, char** argv) {
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd == -1) { perror("open /dev/mem"); return 1; }

    // Map framebuffer
    void* fb_map = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, mem_fd, FB_PHYS);
    if (fb_map == MAP_FAILED) { perror("mmap framebuffer"); return 1; }

    uint16_t* fb = (uint16_t*)fb_map;

    printf("Generating YUV gradient and converting to BGR565 in ARM software...\n");

    // Generate gradient: Y increments per pixel, U=128, V=128 (neutral chroma)
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            int pixel_idx = y * FB_W + x;
            uint8_t Y_val = pixel_idx % 256;  // Gradient wraps at 256
            uint8_t U_val = 128;              // Neutral chroma
            uint8_t V_val = 128;              // Neutral chroma

            uint16_t bgr565 = yuv_to_bgr565(Y_val, U_val, V_val);

            // Write directly to framebuffer (ARM little-endian write)
            // ASCAL reads as big-endian, so bytes get swapped automatically
            fb[pixel_idx] = bgr565;
        }
    }

    printf("Done! Framebuffer filled with YUV gradient converted to BGR565.\n");
    printf("Expected: smooth grayscale gradient (should wrap from dark to bright every 256 pixels)\n");
    printf("If you see wrong colors, the BT.601 math or BGR565 packing is incorrect.\n");

    munmap(fb_map, FB_SIZE);
    close(mem_fd);
    return 0;
}
