#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Direct RGB565 write test - bypasses YUV→RGB pipeline entirely
// Tests ONLY byte ordering and ASCAL pixel interpretation

#define FB_PHYS   0x30000000UL
#define FB_W      640
#define FB_H      480
#define FB_SIZE   (FB_W * FB_H * 2)

#define AXI_PHYS  0xFF200000UL
#define AXI_SIZE  4096
#define AXI_CTRL_IDX 2

int main() {
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd == -1) { perror("open /dev/mem"); return 1; }

    // mmap framebuffer (Buffer A)
    void* fb_map = mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, mem_fd, FB_PHYS);
    if (fb_map == MAP_FAILED) { perror("mmap framebuffer"); close(mem_fd); return 1; }
    uint8_t* fb = (uint8_t*)fb_map;

    // mmap AXI to set buf_sel
    void* axi_map = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mem_fd, AXI_PHYS);
    if (axi_map == MAP_FAILED) { perror("mmap AXI"); munmap(fb_map, FB_SIZE); close(mem_fd); return 1; }
    volatile uint32_t* axi = (volatile uint32_t*)axi_map;

    printf("Direct RGB565 write test\n");
    printf("Writing 4 vertical stripes:\n");
    printf("  Stripe 1 (x=0..159):   RED   (RGB565 = 0xF800)\n");
    printf("  Stripe 2 (x=160..319): GREEN (RGB565 = 0x07E0)\n");
    printf("  Stripe 3 (x=320..479): BLUE  (RGB565 = 0x001F)\n");
    printf("  Stripe 4 (x=480..639): WHITE (RGB565 = 0xFFFF)\n\n");

    // Write RGB565 pixels directly to DDR3 framebuffer
    // Test both byte orderings by trying each in sequence
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            uint16_t pixel;
            if      (x < 160) pixel = 0xF800;  // RED
            else if (x < 320) pixel = 0x07E0;  // GREEN
            else if (x < 480) pixel = 0x001F;  // BLUE
            else              pixel = 0xFFFF;  // WHITE

            int offset = (y * FB_W + x) * 2;

            // ARM is little-endian, so uint16_t write stores:
            //   fb[offset+0] = pixel & 0xFF   (low byte)
            //   fb[offset+1] = pixel >> 8     (high byte)
            *(uint16_t*)(&fb[offset]) = pixel;
        }
    }

    // Memory barrier + display Buffer A
    __asm__ volatile ("dmb sy" ::: "memory");
    axi[AXI_CTRL_IDX] = 0;  // buf_sel = 0 (Buffer A)

    printf("Pattern written.\n");
    printf("\nIf you see correct colors (RED|GREEN|BLUE|WHITE):\n");
    printf("  → ASCAL expects LITTLE-endian pixels (ARM native)\n");
    printf("  → yuv_fb_dma.v should write LOW byte first\n");
    printf("\nIf colors are swapped/wrong:\n");
    printf("  → ASCAL expects BIG-endian pixels\n");
    printf("  → yuv_fb_dma.v should write HIGH byte first\n");

    munmap(axi_map, AXI_SIZE);
    munmap(fb_map, FB_SIZE);
    close(mem_fd);
    return 0;
}
