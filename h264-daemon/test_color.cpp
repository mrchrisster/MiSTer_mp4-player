#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Copied from main.cpp
#define FB_PHYS   0x30000000UL
#define FB_W      640
#define FB_H      480
#define FB_SIZE   (FB_W * FB_H * 2)
#define FB_TOTAL  (FB_SIZE * 2)

#define YUV_Y_PHYS  (FB_PHYS + FB_TOTAL)
#define YUV_Y_SIZE  (FB_W * FB_H)
#define YUV_U_SIZE  ((FB_W / 2) * (FB_H / 2))
#define YUV_V_SIZE  ((FB_W / 2) * (FB_H / 2))
#define YUV_TOTAL   (YUV_Y_SIZE + YUV_U_SIZE + YUV_V_SIZE)

#define AXI_PHYS  0xFF200000UL
#define AXI_SIZE  4096
#define AXI_STATUS_IDX   0
#define AXI_CTRL_IDX     2
#define AXI_YUV_Y_IDX    4
#define AXI_YUV_U_IDX    5
#define AXI_YUV_V_IDX    6
#define AXI_RGB_BASE_IDX 7

#define AXI_VBL_BIT      (1u << 2)
#define AXI_DMA_DONE_BIT (1u << 3)
#define AXI_DMA_TRIG_BIT (1u << 1)

void test_color(volatile uint32_t* axi, uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v) {
    printf("Writing YUV gradient pattern...\n");

    // Write a gradient to Y to test byte ordering (0, 1, 2, 3...)
    for (int i = 0; i < YUV_Y_SIZE; i++) {
        yuv_y[i] = i % 256;
    }

    // Write solid U/V (grey/neutral chroma)
    memset(yuv_u, 128, YUV_U_SIZE);
    memset(yuv_v, 128, YUV_V_SIZE);
    printf("YUV data written. Y=%d bytes, U=%d, V=%d\n", YUV_Y_SIZE, YUV_U_SIZE, YUV_V_SIZE);

    __asm__ volatile ("dmb sy" ::: "memory");

    axi[AXI_RGB_BASE_IDX] = FB_PHYS; // Use buffer A
    printf("RGB base = 0x%08X\n", (unsigned int)FB_PHYS);

    axi[AXI_CTRL_IDX] = (axi[AXI_CTRL_IDX] & 1u) | AXI_DMA_TRIG_BIT;
    printf("DMA triggered, waiting for done...\n");

    int timeout = 0;
    while (!(axi[AXI_STATUS_IDX] & AXI_DMA_DONE_BIT)) {
        if (++timeout > 1000000) {
            printf("ERROR: DMA timeout! status=0x%08X\n", axi[AXI_STATUS_IDX]);
            return;
        }
    }
    printf("DMA done! status=0x%08X\n", axi[AXI_STATUS_IDX]);

    // Page flip to buffer A
    axi[AXI_CTRL_IDX] = 0;
    printf("Page flip to buffer A (buf_sel=0)\n");

    printf("Gradient pattern written and DMA triggered.\n");
}

int main(int argc, char** argv) {
// ... inside main ...

    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd == -1) { perror("open /dev/mem"); return 1; }

    void* axi_map = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, AXI_PHYS);
    volatile uint32_t* axi = (volatile uint32_t*)axi_map;

    void* yuv_map = mmap(NULL, YUV_TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, YUV_Y_PHYS);
    uint8_t* yuv_y = (uint8_t*)yuv_map;
    uint8_t* yuv_u = yuv_y + YUV_Y_SIZE;
    uint8_t* yuv_v = yuv_u + YUV_U_SIZE;

    // Init AXI regs just in case
    axi[AXI_CTRL_IDX]     = 0;
    axi[AXI_YUV_Y_IDX]    = (uint32_t)YUV_Y_PHYS;
    axi[AXI_YUV_U_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE;
    axi[AXI_YUV_V_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE + YUV_U_SIZE;
    axi[AXI_RGB_BASE_IDX] = (uint32_t)FB_PHYS;

    test_color(axi, yuv_y, yuv_u, yuv_v);

    munmap(yuv_map, YUV_TOTAL);
    munmap(axi_map, AXI_SIZE);
    close(mem_fd);
    return 0;
}
