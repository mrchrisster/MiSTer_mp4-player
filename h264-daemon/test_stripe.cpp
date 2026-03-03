// test_stripe.cpp - Write vertical color stripes to framebuffer to test ASCAL
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FB_PHYS 0x30000000UL
#define FB_SIZE (640 * 480 * 2)

int main() {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    uint16_t* fb = (uint16_t*)mmap(NULL, FB_SIZE, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, FB_PHYS);
    if (fb == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fd);
        return 1;
    }

    // Draw 8 vertical stripes of 80px each (640 / 8 = 80)
    // BGR565 format: B[15:11] G[10:5] R[4:0]
    uint16_t colors[8] = {
        0x001F,  // RED    (00000 000000 11111)
        0x07E0,  // GREEN  (00000 111111 00000)
        0xF800,  // BLUE   (11111 000000 00000)
        0xFFFF,  // WHITE
        0x0000,  // BLACK
        0xF81F,  // MAGENTA (B + R)
        0xFFE0,  // CYAN    (B + G)
        0x07FF,  // YELLOW  (G + R)
    };

    for (int y = 0; y < 480; y++) {
        for (int x = 0; x < 640; x++) {
            int stripe = x / 80;  // 0-7
            fb[y * 640 + x] = colors[stripe];
        }
    }

    printf("Wrote 8 vertical color stripes to framebuffer.\n");
    printf("Should see (left to right): RED GREEN BLUE WHITE BLACK MAGENTA CYAN YELLOW\n");
    printf("If you see duplicates or shifted colors, FB_WIDTH or stride is wrong.\n");

    munmap(fb, FB_SIZE);
    close(fd);
    return 0;
}
