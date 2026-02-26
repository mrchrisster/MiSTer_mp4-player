extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// ── Framebuffer parameters ────────────────────────────────────────────────────
#define FB_PHYS   0x30000000UL  // Physical DDR3 address of Buffer A
#define FB_W      640
#define FB_H      480
#define FB_SIZE   (FB_W * FB_H * 2)   // 614400 bytes per buffer
#define FB_TOTAL  (FB_SIZE * 2)        // 1228800 bytes (double buffer)

// ── YUV plane region — written by ARM, read by FPGA DMA (Phase 1.5) ──────────
// Placed immediately after both RGB buffers in DDR3.
#define YUV_Y_PHYS  (FB_PHYS + FB_TOTAL)             // 0x3012C000
#define YUV_Y_SIZE  (FB_W * FB_H)                     // 307200 bytes
#define YUV_U_SIZE  ((FB_W / 2) * (FB_H / 2))         //  76800 bytes
#define YUV_V_SIZE  ((FB_W / 2) * (FB_H / 2))         //  76800 bytes
#define YUV_TOTAL   (YUV_Y_SIZE + YUV_U_SIZE + YUV_V_SIZE)  // 460800 bytes

// ── H2F Lightweight AXI registers (ARM side: 0xFF200000) ─────────────────────
#define AXI_PHYS  0xFF200000UL
#define AXI_SIZE  4096

// Register word indices (32-bit words, 4 bytes each)
#define AXI_STATUS_IDX   0   // offset 0x000: [2]=fb_vbl  [3]=dma_done
#define AXI_CTRL_IDX     2   // offset 0x008: [0]=buf_sel [1]=dma_trigger
#define AXI_YUV_Y_IDX    4   // offset 0x010: Y plane DDR3 byte address
#define AXI_YUV_U_IDX    5   // offset 0x014: U plane DDR3 byte address
#define AXI_YUV_V_IDX    6   // offset 0x018: V plane DDR3 byte address
#define AXI_RGB_BASE_IDX 7   // offset 0x01C: RGB output DDR3 byte address

#define AXI_VBL_BIT      (1u << 2)  // Status: vertical blank pulse
#define AXI_DMA_DONE_BIT (1u << 3)  // Status: FPGA YUV→RGB DMA complete
#define AXI_DMA_TRIG_BIT (1u << 1)  // Control: write 1 to start DMA (auto-clears)

// ── Timing helpers ────────────────────────────────────────────────────────────
// gettimeofday/usleep used instead of clock_gettime/nanosleep to avoid
// requiring GLIBC_2.17+ on the MiSTer (gettimeofday is GLIBC_2.0).
static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void sleep_us(long us) {
    if (us > 0) usleep((useconds_t)us);
}

// ── YUV420P → FPGA DMA path (Phase 1.5) ──────────────────────────────────────
// Scales the decoded frame to FB_W×FB_H, writes Y/U/V planes to the uncached
// DDR3 YUV region, then triggers the FPGA yuv_to_rgb DMA engine and waits for
// completion.  The FPGA writes RBG565 directly to the RGB back buffer.
// Scaling uses nearest-neighbour.  Intermediate cached buffers are used so the
// uncached DDR3 write is a single burst memcpy (avoids per-byte stall overhead).
static void write_yuv_and_dma(const AVFrame* f,
                               uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                               uint32_t rgb_back_phys,
                               volatile uint32_t* axi)
{
    const int src_w = f->width,  src_h = f->height;

    // First-frame diagnostic: confirm format and strides
    static bool first_call = true;
    if (first_call) {
        first_call = false;
        fprintf(stderr, "[mp4_play] frame fmt=%d  src=%dx%d  "
                        "strides Y=%d U=%d V=%d  data[2]=%p\n",
                f->format, src_w, src_h,
                f->linesize[0], f->linesize[1], f->linesize[2],
                (void*)f->data[2]);
        if (!f->data[0] || !f->data[1] || !f->data[2]) {
            fprintf(stderr, "[mp4_play] ERROR: frame plane is NULL — "
                            "decoder output format may not be YUV420P\n");
        } else {
        // Print center pixel YUV so we can verify colour math
        const int cy = src_h / 2, cx = src_w / 2;
        const int Y = f->data[0][cy * f->linesize[0] + cx];
        const int U = f->data[1][(cy/2) * f->linesize[1] + (cx/2)];
        const int V = f->data[2][(cy/2) * f->linesize[2] + (cx/2)];
        const int c = Y-16, d = U-128, e = V-128;
        int r = (298*c + 409*e + 128) >> 8; r = r<0?0:r>255?255:r;
        int g = (298*c - 100*d - 208*e + 128) >> 8; g = g<0?0:g>255?255:g;
        int b = (298*c + 516*d + 128) >> 8; b = b<0?0:b>255?255:b;
        fprintf(stderr, "[mp4_play] center YUV=(%d,%d,%d) -> RGB=(%d,%d,%d) "
                        "expected RBG565=0x%04X\n",
                Y, U, V, r, g, b,
                ((r&0xF8)<<8) | ((b&0xFC)<<3) | (g>>3));
        } // end else (all planes non-NULL)
    }

    // ── Scale and copy Y plane ────────────────────────────────────────────────
    static uint8_t tmp_y[FB_W * FB_H];
    for (int dy = 0; dy < FB_H; dy++) {
        const int sy = dy * src_h / FB_H;
        const uint8_t* src_row = f->data[0] + sy * f->linesize[0];
        uint8_t* dst_row = tmp_y + dy * FB_W;
        for (int dx = 0; dx < FB_W; dx++)
            dst_row[dx] = src_row[dx * src_w / FB_W];
    }
    memcpy(yuv_y, tmp_y, YUV_Y_SIZE);

    // ── Scale and copy U and V planes (at half chroma resolution) ────────────
    if (!f->data[1] || !f->data[2]) {
        fprintf(stderr, "[mp4_play] FATAL: chroma plane is NULL (fmt=%d) — aborting\n",
                f->format);
        g_stop = 1;
        return;
    }
    static uint8_t tmp_u[YUV_U_SIZE], tmp_v[YUV_V_SIZE];
    const int ch = FB_H / 2, cw = FB_W / 2;
    const int src_ch = (src_h + 1) / 2, src_cw = (src_w + 1) / 2;
    for (int dy = 0; dy < ch; dy++) {
        const int sy = dy * src_ch / ch;
        const uint8_t* su = f->data[1] + sy * f->linesize[1];
        const uint8_t* sv = f->data[2] + sy * f->linesize[2];
        uint8_t* du = tmp_u + dy * cw;
        uint8_t* dv = tmp_v + dy * cw;
        for (int dx = 0; dx < cw; dx++) {
            const int sx = dx * src_cw / cw;
            du[dx] = su[sx];
            dv[dx] = sv[sx];
        }
    }
    memcpy(yuv_u, tmp_u, YUV_U_SIZE);
    memcpy(yuv_v, tmp_v, YUV_V_SIZE);

    // ── Trigger FPGA DMA and wait for completion ──────────────────────────────
    static int s_frame = 0;
    ++s_frame;
    axi[AXI_RGB_BASE_IDX] = rgb_back_phys;
    axi[AXI_CTRL_IDX] = (axi[AXI_CTRL_IDX] & 1u) | AXI_DMA_TRIG_BIT;
    fprintf(stderr, "[mp4_play] frame %d: DMA triggered rgb_back=0x%08X "
                    "status=0x%08X\n",
            s_frame, rgb_back_phys, axi[AXI_STATUS_IDX]);
    {
        const int64_t dma_t0 = now_us();
        while (!(axi[AXI_STATUS_IDX] & AXI_DMA_DONE_BIT)) {
            if (now_us() - dma_t0 > 200000LL) {   // 200 ms timeout
                fprintf(stderr, "[mp4_play] DMA TIMEOUT frame %d! "
                                "status=0x%08X ctrl=0x%08X "
                                "Y=0x%08X U=0x%08X V=0x%08X RGB=0x%08X\n",
                        s_frame,
                        axi[AXI_STATUS_IDX], axi[AXI_CTRL_IDX],
                        axi[AXI_YUV_Y_IDX],  axi[AXI_YUV_U_IDX],
                        axi[AXI_YUV_V_IDX],  axi[AXI_RGB_BASE_IDX]);
                g_stop = 1;
                return;
            }
        }
        fprintf(stderr, "[mp4_play] frame %d: DMA done in %lld us\n",
                s_frame, (long long)(now_us() - dma_t0));
    }
}

// ── FFmpeg decode + display loop ──────────────────────────────────────────────
// Separated from main() so that local variable declarations never cross a
// cleanup label (avoids the C++ "goto crosses initialization" error).
//
// benchmark=true : decode as fast as possible, skip display entirely.
//                  Prints per-phase breakdown and fps after 200 frames.
// threads        : FFmpeg decoder thread count (1 = single-core, 2 = both cores)
// seek_s         : seek to this position before starting (negative = from end)
static void play_video(const char* path,
                       volatile uint32_t* axi,
                       uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                       bool benchmark, int threads, double seek_s) {
    int front = 0, back = 1;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open: %s\n", path);
        return;
    }
    avformat_find_stream_info(fmt, NULL);

    // ── find video stream ─────────────────────────────────────────────────────
    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            { vstream = i; break; }
    if (vstream == -1) {
        fprintf(stderr, "No video stream\n");
        avformat_close_input(&fmt);
        return;
    }

    AVCodecParameters* par  = fmt->streams[vstream]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext*    dec   = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    dec->thread_count = threads;   // use both Cortex-A9 cores when threads=2
    if (avcodec_open2(dec, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    // ── optional seek ─────────────────────────────────────────────────────────
    if (seek_s != 0.0) {
        double eff = seek_s;
        if (seek_s < 0.0 && fmt->duration != AV_NOPTS_VALUE)
            eff = fmt->duration / (double)AV_TIME_BASE + seek_s;
        if (eff > 0.0) {
            int64_t ts = (int64_t)(eff * AV_TIME_BASE);
            avformat_seek_file(fmt, -1, INT64_MIN, ts, INT64_MAX, 0);
            avcodec_flush_buffers(dec);
            fprintf(stderr, "[mp4_play] seeked to %.1f s\n", eff);
        }
    }

    fprintf(stderr, "[mp4_play] %s  %dx%d  threads=%d  mode=%s\n",
            path, dec->width, dec->height, dec->thread_count,
            benchmark ? "BENCHMARK" : "PLAY");

    // ── decode loop ───────────────────────────────────────────────────────────
    fprintf(stderr, "[mp4_play] entering decode loop\n");
    AVPacket*  pkt   = av_packet_alloc();
    AVFrame*   frame = av_frame_alloc();
    AVRational tb    = fmt->streams[vstream]->time_base;

    // Master clock (playback mode only)
    int64_t start_wall_us  = 0;
    double  start_pts_s    = 0.0;
    bool    clk_init       = false;
    double  prev_pts_s     = -1.0;
    double  frame_period_s = 1.0 / 30.0;
    int     frame_count    = 0;   // all frames (drops + displayed)
    int     disp_count     = 0;   // displayed frames only

    // Benchmark accumulators (us)
    int64_t t_decode_us = 0, t_convert_us = 0, t_vbl_us = 0;
    const int BM_FRAMES = 200;   // decode this many frames then stop
    int64_t bm_start_us = 0;

    while (!g_stop && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }

        const int64_t t0 = now_us();
        avcodec_send_packet(dec, pkt);

        while (avcodec_receive_frame(dec, frame) == 0) {
            const int64_t t_after_decode = now_us();
            t_decode_us += t_after_decode - t0;

            if (frame->pts == AV_NOPTS_VALUE) { continue; }

            // ── BENCHMARK MODE: skip all display, just measure decode ──────
            if (benchmark) {
                if (frame_count == 0)
                    bm_start_us = now_us();
                frame_count++;
                if (frame_count >= BM_FRAMES) goto bm_done;
                continue;
            }

            const double frame_pts_s = frame->pts * av_q2d(tb);

            // ── A. Initialise master clock on first frame ──────────────────
            if (!clk_init) {
                start_wall_us = now_us();
                start_pts_s   = frame_pts_s;
                clk_init      = true;
            }

            // ── B. Update frame period estimate ───────────────────────────
            if (prev_pts_s >= 0.0) {
                const double dp = frame_pts_s - prev_pts_s;
                if (dp > 0.001 && dp < 0.2)
                    frame_period_s = dp;
            }
            prev_pts_s = frame_pts_s;

            // ── C. Compute target display time ────────────────────────────
            const double  elapsed_pts_s = frame_pts_s - start_pts_s;
            const int64_t target_us     = start_wall_us + (int64_t)(elapsed_pts_s * 1e6);

            // ── D. Sync engine: drop if too late ──────────────────────────
            const int64_t drop_thresh_us = (int64_t)(frame_period_s * 0.5e6);
            if (now_us() > target_us + drop_thresh_us) {
                fprintf(stderr, "[mp4_play] drop #%d  pts=%.3f\n",
                        frame_count, frame_pts_s);
                frame_count++;
                continue;
            }

            // ── E. Scale YUV planes, write to DDR3, trigger FPGA DMA ─────
            // rgb_back_phys: physical address of the back RGB buffer.
            // FPGA reads YUV from yuv_y/u/v and writes RBG565 there.
            {
                const int64_t tc0 = now_us();
                const uint32_t rgb_back_phys = FB_PHYS + (uint32_t)back * FB_SIZE;
                write_yuv_and_dma(frame, yuv_y, yuv_u, yuv_v, rgb_back_phys, axi);
                t_convert_us += now_us() - tc0;
            }
            if (g_stop) goto bm_done;

            // ── F. Sleep until 1 ms before target ────────────────────────
            {
                const long to_sleep_us = (long)(target_us - now_us()) - 1000;
                if (to_sleep_us > 500)
                    sleep_us(to_sleep_us);
            }

            // ── G. Wait for VBlank (50 ms timeout — continue without vsync) ─
            {
                const int64_t tv0 = now_us();
                while (!g_stop && !(axi[AXI_STATUS_IDX] & AXI_VBL_BIT)) {
                    if (now_us() - tv0 > 50000LL) break;   // no VBL: proceed anyway
                }
                t_vbl_us += now_us() - tv0;
            }
            if (g_stop) goto bm_done;

            // ── H. Page flip ──────────────────────────────────────────────
            axi[AXI_CTRL_IDX] = (uint32_t)back;

            // ── I. Swap front / back ───────────────────────────────────────
            { int tmp = front; front = back; back = tmp; }

            frame_count++;
            disp_count++;
        }
        av_packet_unref(pkt);
    }

bm_done:
    av_packet_unref(pkt);

    if (benchmark && frame_count > 0) {
        const int64_t elapsed = now_us() - bm_start_us;
        const double fps = frame_count * 1e6 / elapsed;
        fprintf(stderr, "\n=== BENCHMARK RESULTS (%d frames) ===\n", frame_count);
        fprintf(stderr, "  Decode throughput : %.1f fps  (%.1f ms/frame avg)\n",
                fps, elapsed / 1000.0 / frame_count);
        fprintf(stderr, "  Decode thread cnt : %d\n", dec->thread_count);
        fprintf(stderr, "  Total wall time   : %.2f s\n", elapsed / 1e6);
    } else if (!benchmark && disp_count > 0) {
        const int drop_count = frame_count - disp_count;
        fprintf(stderr, "\n=== PHASE TIMING (over %d displayed / %d total frames) ===\n",
                disp_count, frame_count);
        fprintf(stderr, "  Drop rate   : %d/%d = %.0f%%\n",
                drop_count, frame_count, 100.0 * drop_count / frame_count);
        fprintf(stderr, "  Decode avg  : %.1f ms/frame  (all frames)\n",
                t_decode_us  / 1000.0 / frame_count);
        fprintf(stderr, "  YUV+DMA avg : %.1f ms/frame  (displayed only)\n",
                t_convert_us / 1000.0 / disp_count);
        fprintf(stderr, "  VBL wait avg: %.1f ms/frame  (displayed only)\n",
                t_vbl_us     / 1000.0 / disp_count);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

int main(int argc, char** argv) {
    fprintf(stderr, "[mp4_play] main() start\n");
    if (argc < 2) {
        fprintf(stderr, "usage: mp4_play <file> [-b] [-t N] [-ss N]\n");
        fprintf(stderr, "  -b      benchmark: decode 200 frames as fast as possible\n");
        fprintf(stderr, "  -t N    FFmpeg decoder thread count (default 1)\n");
        fprintf(stderr, "  -ss N   seek to N seconds (negative = from end)\n");
        fprintf(stderr, "          e.g. -ss -60 seeks to 1 minute before EOF\n");
        return 1;
    }
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    bool   benchmark  = false;
    int    threads    = 1;
    double seek_s     = 0.0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        switch (argv[i][1]) {
            case 'b': benchmark = true; break;
            case 't': if (i + 1 < argc) threads = atoi(argv[++i]); break;
            case 's': if (argv[i][2] == 's' && i + 1 < argc) seek_s = atof(argv[++i]); break;
        }
    }

    // ── mmap AXI control registers ────────────────────────────────────────────
    fprintf(stderr, "[mp4_play] opening /dev/mem\n");
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd == -1) { perror("open /dev/mem"); return 1; }

    fprintf(stderr, "[mp4_play] mmap AXI regs at 0x%08X\n", (unsigned)AXI_PHYS);
    void* axi_map = mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mem_fd, AXI_PHYS);
    if (axi_map == MAP_FAILED) { perror("mmap AXI regs"); close(mem_fd); return 1; }
    volatile uint32_t* axi = (volatile uint32_t*)axi_map;

    fprintf(stderr, "[mp4_play] mmap YUV region at 0x%08X size=%u\n",
            (unsigned)YUV_Y_PHYS, (unsigned)YUV_TOTAL);
    // ── mmap YUV plane region (Phase 1.5: ARM writes here, FPGA DMA reads) ────
    void* yuv_map = mmap(NULL, YUV_TOTAL, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mem_fd, YUV_Y_PHYS);
    if (yuv_map == MAP_FAILED) {
        perror("mmap YUV region");
        munmap(axi_map, AXI_SIZE);
        close(mem_fd);
        return 1;
    }
    uint8_t* yuv_y = (uint8_t*)yuv_map;
    uint8_t* yuv_u = yuv_y + YUV_Y_SIZE;
    uint8_t* yuv_v = yuv_u + YUV_U_SIZE;

    fprintf(stderr, "[mp4_play] writing AXI init registers\n");
    // ── Initialise AXI registers ──────────────────────────────────────────────
    axi[AXI_CTRL_IDX]     = 0;                              // buf_sel=A, no DMA
    axi[AXI_YUV_Y_IDX]    = (uint32_t)YUV_Y_PHYS;          // Y plane address
    axi[AXI_YUV_U_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE;  // U address
    axi[AXI_YUV_V_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE + YUV_U_SIZE; // V
    axi[AXI_RGB_BASE_IDX] = (uint32_t)FB_PHYS;              // default: write to A

    fprintf(stderr, "[mp4_play] YUV region: 0x%08X  Y=%u U=%u V=%u bytes\n",
            (unsigned)YUV_Y_PHYS, YUV_Y_SIZE, YUV_U_SIZE, YUV_V_SIZE);

    play_video(argv[1], axi, yuv_y, yuv_u, yuv_v, benchmark, threads, seek_s);

    munmap(yuv_map, YUV_TOTAL);
    munmap(axi_map, AXI_SIZE);
    close(mem_fd);
    return 0;
}
