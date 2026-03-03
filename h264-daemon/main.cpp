extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define MP4_PLAY_VERSION "1"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// ── Framebuffer parameters ────────────────────────────────────────────────────
#define FB_PHYS   0x30000000UL  // Physical DDR3 address of Buffer A
#define FB_W      640
#define FB_H      480
#define FB_SIZE   (FB_W * FB_H * 2)   // 614400 bytes per buffer
#define FB_TOTAL  (FB_SIZE * 2)        // 1228800 bytes (double buffer)

// ── YUV plane region — written by ARM, read by FPGA DMA ──────────────────────
#define YUV_Y_PHYS  (FB_PHYS + FB_TOTAL)             // 0x3012C000
#define YUV_Y_SIZE  (FB_W * FB_H)                     // 307200 bytes
#define YUV_U_SIZE  ((FB_W / 2) * (FB_H / 2))         //  76800 bytes
#define YUV_V_SIZE  ((FB_W / 2) * (FB_H / 2))         //  76800 bytes
#define YUV_TOTAL   (YUV_Y_SIZE + YUV_U_SIZE + YUV_V_SIZE)  // 460800 bytes

// ── H2F Lightweight AXI registers ────────────────────────────────────────────
#define AXI_PHYS  0xFF200000UL
#define AXI_SIZE  4096
#define AXI_STATUS_IDX   0   // [2]=fb_vbl  [3]=dma_done_latch
#define AXI_CTRL_IDX     2   // [0]=buf_sel [1]=dma_trigger
#define AXI_YUV_Y_IDX    4
#define AXI_YUV_U_IDX    5
#define AXI_YUV_V_IDX    6
#define AXI_RGB_BASE_IDX 7

#define AXI_VBL_BIT      (1u << 2)
#define AXI_DMA_DONE_BIT (1u << 3)
#define AXI_DMA_TRIG_BIT (1u << 1)

// ── Timing helpers ────────────────────────────────────────────────────────────
static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
static void sleep_us(long us) { if (us > 0) usleep((useconds_t)us); }

// ── YUV420P -> FPGA DMA path ──────────────────────────────────────────────────
// Scales the decoded frame to FB_W x FB_H and writes Y/U/V planes to the
// uncached DDR3 YUV region, then triggers the FPGA yuv_to_rgb DMA and polls
// for completion.
//
// Scale optimisations:
//   - Coordinate lookup tables (uint16_t arrays) are precomputed once per
//     unique source resolution, eliminating per-pixel multiply+divide.
//   - Identity fast-path: when src == FB dimensions, copies each row directly
//     to the DDR3 YUV region via memcpy (no intermediate tmp buffer).
static void write_yuv_and_dma(const AVFrame* f,
                               uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                               uint32_t rgb_back_phys,
                               volatile uint32_t* axi)
{
    const int src_w  = f->width,  src_h  = f->height;
    const int src_cw = (src_w + 1) / 2, src_ch = (src_h + 1) / 2;
    const int cw     = FB_W / 2,  ch     = FB_H / 2;

    // ── First-frame diagnostic ────────────────────────────────────────────────
    static bool first_call = true;
    if (first_call) {
        first_call = false;
        fprintf(stderr, "[mp4_play] frame fmt=%d  src=%dx%d  "
                        "strides Y=%d U=%d V=%d\n",
                f->format, src_w, src_h,
                f->linesize[0], f->linesize[1], f->linesize[2]);
        if (f->data[0] && f->data[1] && f->data[2]) {
            const int cy = src_h / 2, cx = src_w / 2;
            int Y = f->data[0][cy * f->linesize[0] + cx];
            int U = f->data[1][(cy/2) * f->linesize[1] + (cx/2)];
            int V = f->data[2][(cy/2) * f->linesize[2] + (cx/2)];
            int c = Y-16, d = U-128, e = V-128;
            int r = (298*c + 409*e + 128) >> 8; r = r<0?0:r>255?255:r;
            int g = (298*c - 100*d - 208*e + 128) >> 8; g = g<0?0:g>255?255:g;
            int b = (298*c + 516*d + 128) >> 8; b = b<0?0:b>255?255:b;
            fprintf(stderr, "[mp4_play] center YUV=(%d,%d,%d) -> RGB=(%d,%d,%d) "
                            "expected RGB565=0x%04X\n",
                    Y, U, V, r, g, b,
                    ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3));
        }
    }

    // ── Coordinate lookup tables (rebuilt only on resolution change) ──────────
    // x_map[dx]  = source column for destination luma column dx
    // y_map[dy]  = source row    for destination luma row    dy
    // ux_map[dx] = source column for destination chroma column dx
    // uy_map[dy] = source row    for destination chroma row    dy
    static int      cached_src_w = -1, cached_src_h = -1;
    static uint16_t x_map[FB_W],   y_map[FB_H];
    static uint16_t ux_map[FB_W/2], uy_map[FB_H/2];

    if (src_w != cached_src_w || src_h != cached_src_h) {
        for (int i = 0; i < FB_W;   i++) x_map[i]  = (uint16_t)(i * src_w  / FB_W);
        for (int i = 0; i < FB_H;   i++) y_map[i]  = (uint16_t)(i * src_h  / FB_H);
        for (int i = 0; i < cw;     i++) ux_map[i] = (uint16_t)(i * src_cw / cw);
        for (int i = 0; i < ch;     i++) uy_map[i] = (uint16_t)(i * src_ch / ch);
        cached_src_w = src_w;
        cached_src_h = src_h;
        fprintf(stderr, "[mp4_play] scale LUTs: %dx%d -> %dx%d  (%s)\n",
                src_w, src_h, FB_W, FB_H,
                (src_w == FB_W && src_h == FB_H) ? "identity" : "scaled");
    }

    // ── Copy / scale Y plane ──────────────────────────────────────────────────
    if (src_w == FB_W && src_h == FB_H) {
        // Identity: copy each row directly to the DDR3 YUV region.
        // No tmp buffer needed — saves one full 307 KB copy.
        for (int dy = 0; dy < FB_H; dy++)
            memcpy(yuv_y + dy * FB_W,
                   f->data[0] + dy * f->linesize[0],
                   FB_W);
    } else {
        // Scaled: nearest-neighbour via lookup table -> tmp -> DDR3.
        static uint8_t tmp_y[FB_W * FB_H];
        for (int dy = 0; dy < FB_H; dy++) {
            const uint8_t* src_row = f->data[0] + y_map[dy] * f->linesize[0];
            uint8_t*       dst_row = tmp_y + dy * FB_W;
            for (int dx = 0; dx < FB_W; dx++)
                dst_row[dx] = src_row[x_map[dx]];
        }
        memcpy(yuv_y, tmp_y, YUV_Y_SIZE);
    }

    // ── Copy / scale UV planes ────────────────────────────────────────────────
    if (!f->data[1] || !f->data[2]) {
        fprintf(stderr, "[mp4_play] FATAL: chroma plane NULL (fmt=%d)\n", f->format);
        g_stop = 1;
        return;
    }
    if (src_cw == cw && src_ch == ch) {
        // Identity chroma: copy each row directly to DDR3
        for (int dy = 0; dy < ch; dy++) {
            memcpy(yuv_u + dy * cw, f->data[1] + dy * f->linesize[1], cw);
            memcpy(yuv_v + dy * cw, f->data[2] + dy * f->linesize[2], cw);
        }
    } else {
        static uint8_t tmp_u[YUV_U_SIZE], tmp_v[YUV_V_SIZE];
        for (int dy = 0; dy < ch; dy++) {
            const uint8_t* su = f->data[1] + uy_map[dy] * f->linesize[1];
            const uint8_t* sv = f->data[2] + uy_map[dy] * f->linesize[2];
            uint8_t*       du = tmp_u + dy * cw;
            uint8_t*       dv = tmp_v + dy * cw;
            for (int dx = 0; dx < cw; dx++) {
                du[dx] = su[ux_map[dx]];
                dv[dx] = sv[ux_map[dx]];
            }
        }
        memcpy(yuv_u, tmp_u, YUV_U_SIZE);
        memcpy(yuv_v, tmp_v, YUV_V_SIZE);
    }

    // ── Trigger FPGA DMA and poll for completion ──────────────────────────────
    // DMB SY: ensure all ARM stores to the DDR3 YUV region are visible to all
    // bus masters (including the FPGA DMA) before the DMA trigger write leaves
    // the CPU.  Without this, the AXI write buffer may still hold pending YUV
    // writes when the FPGA starts reading, causing a read-after-write hazard
    // at the DDR3 controller that manifests as 2-3x DMA slowdowns.
    __asm__ volatile ("dmb sy" ::: "memory");

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
            if (now_us() - dma_t0 > 200000LL) {
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

// ── Decode-ahead frame queue ──────────────────────────────────────────────────
// The decoder thread (Core 1) pre-decodes one frame into the queue while the
// display thread (Core 0) is doing scale+DMA+VBL work.  This hides the
// ~8 ms DMA+VBL wait behind concurrent decode, roughly halving the per-frame
// wall time for 30 fps content.
//
// Ring buffer of depth 2:
//   slot[head]         — frame currently consumed by display thread
//   slot[(head+1) & 1] — pre-decoded next frame (or empty if not ready yet)
//
// Ownership protocol:
//   - Each slot always holds a valid AVFrame* (either empty or filled).
//   - Decoder:  av_frame_move_ref into slot  (slot must be empty)
//   - Display:  swap slot AVFrame* with a fresh av_frame_alloc(), then
//               av_frame_free() after use.  The fresh frame is immediately
//               available for the decoder to fill.
struct FrameQueue {
    AVFrame*        f[2];       // ring of pre-allocated AVFrame structs
    double          pts_s[2];   // PTS seconds for each slot
    int             head;       // display thread reads from f[head]
    int             count;      // 0..2 frames ready
    bool            eof;        // decoder has finished all frames
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;  // signalled when count 0 -> 1
    pthread_cond_t  not_full;   // signalled when count 2 -> 1
};

struct DecodeArgs {
    FrameQueue*       q;
    AVFormatContext*  fmt;
    AVCodecContext*   dec;
    int               vstream;
    AVRational        tb;
    volatile int64_t* t_decode_us;  // accumulated by decoder, read at end
    volatile int*     total_frames; // all decoded frames (including drops)
};

static void* decode_thread_fn(void* arg)
{
    DecodeArgs*  a   = (DecodeArgs*)arg;
    FrameQueue*  q   = a->q;
    AVPacket*    pkt = av_packet_alloc();
    AVFrame*     frm = av_frame_alloc();

    while (!g_stop) {
        if (av_read_frame(a->fmt, pkt) < 0) break;
        if (pkt->stream_index != a->vstream) { av_packet_unref(pkt); continue; }

        const int64_t t0 = now_us();
        avcodec_send_packet(a->dec, pkt);
        av_packet_unref(pkt);

        while (!g_stop && avcodec_receive_frame(a->dec, frm) == 0) {
            *a->t_decode_us += now_us() - t0;
            (*a->total_frames)++;

            if (frm->pts == AV_NOPTS_VALUE) { av_frame_unref(frm); continue; }

            const double pts = frm->pts * av_q2d(a->tb);

            pthread_mutex_lock(&q->mu);
            while (q->count == 2 && !g_stop)
                pthread_cond_wait(&q->not_full, &q->mu);
            if (g_stop) { pthread_mutex_unlock(&q->mu); goto done; }

            // Move decoded refs into queue slot (slot is always empty here —
            // display thread guarantees it by replacing the taken slot with a
            // fresh av_frame_alloc before signalling not_full).
            const int slot = (q->head + q->count) & 1;
            av_frame_move_ref(q->f[slot], frm);   // frm is now empty
            q->pts_s[slot] = pts;
            q->count++;
            pthread_cond_signal(&q->not_empty);
            pthread_mutex_unlock(&q->mu);
        }
    }

done:
    pthread_mutex_lock(&q->mu);
    q->eof = true;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);

    av_frame_free(&frm);
    av_packet_free(&pkt);
    return NULL;
}

// ── FFmpeg decode + display loop ──────────────────────────────────────────────
static void play_video(const char* path,
                       volatile uint32_t* axi,
                       uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                       bool benchmark, int threads, double seek_s)
{
    int front = 0, back = 1;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open: %s\n", path);
        return;
    }
    avformat_find_stream_info(fmt, NULL);

    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            { vstream = i; break; }
    if (vstream == -1) {
        fprintf(stderr, "No video stream\n");
        avformat_close_input(&fmt);
        return;
    }

    AVCodecParameters* par   = fmt->streams[vstream]->codecpar;
    const AVCodec*     codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext*    dec   = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    dec->thread_count      = threads;
    dec->skip_loop_filter  = AVDISCARD_ALL;        // skip H.264 deblock: -15-25% decode time
    dec->flags2           |= AV_CODEC_FLAG2_FAST;  // non-spec-compliant fast paths: -5-10%
    if (avcodec_open2(dec, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    if (seek_s != 0.0) {
        double eff = seek_s;
        if (seek_s < 0.0 && fmt->duration != AV_NOPTS_VALUE)
            eff = fmt->duration / (double)AV_TIME_BASE + seek_s;
        if (eff > 0.0) {
            avformat_seek_file(fmt, -1, INT64_MIN,
                               (int64_t)(eff * AV_TIME_BASE), INT64_MAX, 0);
            avcodec_flush_buffers(dec);
            fprintf(stderr, "[mp4_play] seeked to %.1f s\n", eff);
        }
    }

    fprintf(stderr, "[mp4_play] %s  %dx%d  threads=%d  mode=%s\n",
            path, dec->width, dec->height, dec->thread_count,
            benchmark ? "BENCHMARK" : "PLAY");

    // ── BENCHMARK MODE: decode-only, no display ───────────────────────────────
    if (benchmark) {
        AVPacket* pkt       = av_packet_alloc();
        AVFrame*  frm       = av_frame_alloc();
        int       count     = 0;
        const int BM_FRAMES = 200;
        int64_t   bm_start  = 0;
        bool      done      = false;

        while (!g_stop && !done && av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            while (!g_stop && !done && avcodec_receive_frame(dec, frm) == 0) {
                if (count == 0) bm_start = now_us();
                av_frame_unref(frm);
                if (++count >= BM_FRAMES) done = true;
            }
        }

        if (count > 0) {
            const int64_t elapsed = now_us() - bm_start;
            fprintf(stderr, "\n=== BENCHMARK RESULTS (%d frames) ===\n", count);
            fprintf(stderr, "  Decode throughput : %.1f fps  (%.1f ms/frame avg)\n",
                    count * 1e6 / elapsed, elapsed / 1000.0 / count);
            fprintf(stderr, "  Decode thread cnt : %d\n", dec->thread_count);
            fprintf(stderr, "  Total wall time   : %.2f s\n", elapsed / 1e6);
        }

        av_frame_free(&frm);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return;
    }

    // ── PLAY MODE: decode-ahead thread (Core 1) + display thread (Core 0) ─────
    //
    // Critical path per frame (sequential):
    //   OLD: decode(~17ms) + scale+DMA(~16ms) + VBL(~3ms) = ~36ms  [drops at 30fps]
    //   NEW: max(decode=~17ms, scale+DMA+VBL=~19ms) = ~19ms  [52fps headroom]
    //
    // With threads=1 (default): Core 1 owned by decode thread; Core 0 does
    // scale+DMA+VBL.  This is the recommended setting for decode-ahead.
    // With threads=2: FFmpeg's second decode thread may contend with Core 0
    // during scale+copy; experiment if decode is still the bottleneck.
    fprintf(stderr, "[mp4_play] decode-ahead enabled  "
                    "(Core 1 = decode, Core 0 = display)\n");

    AVRational       tb          = fmt->streams[vstream]->time_base;
    volatile int64_t t_decode_us = 0;
    volatile int     total_frames = 0;
    int64_t          t_convert_us = 0;
    int64_t          t_vbl_us     = 0;
    int              disp_count   = 0;
    int              drop_count   = 0;

    // ── Initialise decode-ahead queue ─────────────────────────────────────────
    FrameQueue q;
    q.f[0]     = av_frame_alloc();
    q.f[1]     = av_frame_alloc();
    q.pts_s[0] = q.pts_s[1] = 0.0;
    q.head     = 0;
    q.count    = 0;
    q.eof      = false;
    pthread_mutex_init(&q.mu,        NULL);
    pthread_cond_init (&q.not_empty, NULL);
    pthread_cond_init (&q.not_full,  NULL);

    DecodeArgs dargs = { &q, fmt, dec, vstream, tb, &t_decode_us, &total_frames };
    pthread_t  dthr;
    pthread_create(&dthr, NULL, decode_thread_fn, &dargs);

    // ── Display loop ───────────────────────────────────────────────────────────
    int64_t start_wall_us  = 0;
    double  start_pts_s    = 0.0;
    bool    clk_init       = false;
    double  prev_pts_s     = -1.0;
    double  frame_period_s = 1.0 / 30.0;

    while (!g_stop) {
        // ── A. Get next decoded frame (blocks if queue empty) ─────────────────
        pthread_mutex_lock(&q.mu);
        while (q.count == 0 && !q.eof && !g_stop)
            pthread_cond_wait(&q.not_empty, &q.mu);

        if ((q.count == 0 && q.eof) || g_stop) {
            pthread_mutex_unlock(&q.mu);
            break;
        }

        // Take ownership of the frame at queue head.
        // Replace the slot with a fresh empty AVFrame so the decoder can
        // immediately reuse it.  All of this is under the mutex.
        AVFrame* frame  = q.f[q.head];
        double   pts_s  = q.pts_s[q.head];
        AVFrame* fresh  = av_frame_alloc();
        if (!fresh) {
            // OOM — extremely unlikely; bail out cleanly
            pthread_mutex_unlock(&q.mu);
            fprintf(stderr, "[mp4_play] FATAL: av_frame_alloc OOM\n");
            g_stop = 1;
            av_frame_free(&frame);
            break;
        }
        q.f[q.head] = fresh;
        q.head      = (q.head + 1) & 1;
        q.count--;
        pthread_cond_signal(&q.not_full);
        pthread_mutex_unlock(&q.mu);

        // ── B. Initialise master clock on first displayed frame ───────────────
        if (!clk_init) {
            start_wall_us = now_us();
            start_pts_s   = pts_s;
            clk_init      = true;
        }

        // ── C. Update frame-period estimate ──────────────────────────────────
        if (prev_pts_s >= 0.0) {
            const double dp = pts_s - prev_pts_s;
            if (dp > 0.001 && dp < 0.2) frame_period_s = dp;
        }
        prev_pts_s = pts_s;

        // ── D. Compute target display time ────────────────────────────────────
        const double  elapsed_pts_s = pts_s - start_pts_s;
        const int64_t target_us     = start_wall_us + (int64_t)(elapsed_pts_s * 1e6);
        // Drop threshold: 1 full frame period.
        // Frames that arrive 0–33 ms late are displayed (slight timing slip,
        // invisible or imperceptible).  Only frames that arrive more than a
        // full frame late are skipped; the clock then resets so the next
        // frame gets a fresh 33 ms budget.  Using 0.5× (16.67 ms) caused
        // frames that were only 18–27 ms late to be discarded unnecessarily,
        // producing visible freezes instead of the brief stutter a late
        // display would cause.
        const int64_t drop_thresh   = (int64_t)(frame_period_s * 1.0e6);

        // ── E. Drop if too late ───────────────────────────────────────────────
        if (now_us() > target_us + drop_thresh) {
            const int64_t late_us = now_us() - target_us;
            fprintf(stderr, "[mp4_play] drop  pts=%.3f  late=%lld ms\n",
                    pts_s, (long long)late_us / 1000);
            av_frame_free(&frame);
            drop_count++;

            // Re-anchor the master clock to prevent cascade drops.
            // Without this, one slow decode or DMA spike skews the clock and
            // every subsequent frame appears "late" until the backlog drains,
            // turning 1 real drop into a burst of 5-10 drops.
            // After reset, the next frame gets a fresh 33.3 ms budget.
            start_wall_us = now_us();
            start_pts_s   = pts_s;
            continue;
        }

        // ── F. Scale YUV, write to DDR3, trigger + await FPGA DMA ────────────
        {
            const int64_t  tc0           = now_us();
            const uint32_t rgb_back_phys = FB_PHYS + (uint32_t)back * FB_SIZE;
            write_yuv_and_dma(frame, yuv_y, yuv_u, yuv_v, rgb_back_phys, axi);
            t_convert_us += now_us() - tc0;
        }
        av_frame_free(&frame);
        if (g_stop) break;

        // ── G. Sleep until 1 ms before target ────────────────────────────────
        {
            const long to_sleep = (long)(target_us - now_us()) - 1000;
            if (to_sleep > 500) sleep_us(to_sleep);
        }

        // ── H. Wait for VBlank (50 ms timeout — proceed without vsync) ───────
        {
            const int64_t tv0 = now_us();
            while (!g_stop && !(axi[AXI_STATUS_IDX] & AXI_VBL_BIT)) {
                if (now_us() - tv0 > 50000LL) break;
            }
            t_vbl_us += now_us() - tv0;
        }
        if (g_stop) break;

        // ── I. Page flip ──────────────────────────────────────────────────────
        axi[AXI_CTRL_IDX] = (uint32_t)back;
        { int tmp = front; front = back; back = tmp; }

        disp_count++;
    }

    // ── Shutdown decoder thread ───────────────────────────────────────────────
    g_stop = 1;
    pthread_mutex_lock(&q.mu);
    pthread_cond_signal(&q.not_full);   // wake decoder if blocked on full
    pthread_cond_signal(&q.not_empty);  // wake display if blocked on empty
    pthread_mutex_unlock(&q.mu);
    pthread_join(dthr, NULL);

    // ── Summary statistics ────────────────────────────────────────────────────
    const int total = total_frames;   // snapshot after join — no more writes
    if (total > 0) {
        fprintf(stderr,
            "\n=== PHASE TIMING (%d displayed / %d total / %d dropped) ===\n",
            disp_count, total, drop_count);
        fprintf(stderr,
            "  NOTE: decode (Core 1) overlaps with DMA+VBL (Core 0)\n");
        fprintf(stderr, "  Drop rate   : %d/%d = %.0f%%\n",
                drop_count, total, 100.0 * drop_count / total);
        fprintf(stderr, "  Decode avg  : %.1f ms/frame  (decode thread, all frames)\n",
                (double)t_decode_us / 1000.0 / total);
        if (disp_count > 0) {
            fprintf(stderr, "  YUV+DMA avg : %.1f ms/frame  (display thread, shown only)\n",
                    t_convert_us / 1000.0 / disp_count);
            fprintf(stderr, "  VBL wait avg: %.1f ms/frame  (display thread, shown only)\n",
                    t_vbl_us     / 1000.0 / disp_count);
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (int i = 0; i < 2; i++)
        if (q.f[i]) av_frame_free(&q.f[i]);
    pthread_cond_destroy(&q.not_empty);
    pthread_cond_destroy(&q.not_full);
    pthread_mutex_destroy(&q.mu);

    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    fprintf(stderr, "[mp4_play v" MP4_PLAY_VERSION "] start\n");
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

    bool   benchmark = false;
    int    threads   = 1;
    double seek_s    = 0.0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        switch (argv[i][1]) {
            case 'b': benchmark = true; break;
            case 't': if (i + 1 < argc) threads = atoi(argv[++i]); break;
            case 's': if (argv[i][2] == 's' && i + 1 < argc) seek_s = atof(argv[++i]); break;
        }
    }

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
    axi[AXI_CTRL_IDX]     = 0;
    axi[AXI_YUV_Y_IDX]    = (uint32_t)YUV_Y_PHYS;
    axi[AXI_YUV_U_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE;
    axi[AXI_YUV_V_IDX]    = (uint32_t)YUV_Y_PHYS + YUV_Y_SIZE + YUV_U_SIZE;
    axi[AXI_RGB_BASE_IDX] = (uint32_t)FB_PHYS;

    fprintf(stderr, "[mp4_play] YUV region: 0x%08X  Y=%u U=%u V=%u bytes\n",
            (unsigned)YUV_Y_PHYS, YUV_Y_SIZE, YUV_U_SIZE, YUV_V_SIZE);

    play_video(argv[1], axi, yuv_y, yuv_u, yuv_v, benchmark, threads, seek_s);

    munmap(yuv_map, YUV_TOTAL);
    munmap(axi_map, AXI_SIZE);
    close(mem_fd);
    return 0;
}
