extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <alsa/asoundlib.h>
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

// CPU frequency control for performance boost
#define CPUFREQ_FILE "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define STOCK_FREQ   800000   // 800 MHz (stock)
#define BOOST_FREQ   1000000  // 1000 MHz (safe overclock with cooling)

static volatile sig_atomic_t g_stop = 0;

static void set_cpu_freq(int freq_khz) {
    FILE* fp = fopen(CPUFREQ_FILE, "w");
    if (fp) {
        fprintf(fp, "%d", freq_khz);
        fclose(fp);
        fprintf(stderr, "[mp4_play] CPU freq set to %d kHz (%.0f MHz)\n",
                freq_khz, freq_khz / 1000.0);
    }
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    set_cpu_freq(STOCK_FREQ);  // Restore stock frequency on exit
}

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
#define AXI_SWITCHRES_IDX 3  // Switchres trigger
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
// Ring buffer of depth 4 (increased from 2 to reduce drops with audio enabled):
//   Gives decode thread more headroom for audio decode spikes
//
// Ownership protocol:
//   - Each slot always holds a valid AVFrame* (either empty or filled).
//   - Decoder:  av_frame_move_ref into slot  (slot must be empty)
//   - Display:  swap slot AVFrame* with a fresh av_frame_alloc(), then
//               av_frame_free() after use.  The fresh frame is immediately
//               available for the decoder to fill.
struct FrameQueue {
    AVFrame*        f[4];       // ring of 4 pre-allocated AVFrame structs
    double          pts_s[4];   // PTS seconds for each slot
    int             head;       // display thread reads from f[head]
    int             count;      // 0..4 frames ready
    bool            eof;        // decoder has finished all frames
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;  // signalled when count 0 -> 1
    pthread_cond_t  not_full;   // signalled when count becomes < 4
};

// ── Audio frame queue ─────────────────────────────────────────────────────────
// Ring buffer for decoded audio frames. Audio thread pulls frames and writes
// to ALSA. Audio PTS becomes the master clock for video sync.
struct AudioQueue {
    AVFrame*        f[64];      // ring of 64 audio frames (~1.3s buffer @ 1024 samples/frame)
    double          pts_s[64];  // PTS seconds for each slot
    int             head;       // audio thread reads from f[head]
    int             count;      // 0..4 frames ready
    bool            eof;        // decoder finished all audio
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
};

struct DecodeArgs {
    FrameQueue*       vq;       // video queue
    AudioQueue*       aq;       // audio queue (NULL if no audio)
    AVFormatContext*  fmt;
    AVCodecContext*   vdec;     // video decoder
    AVCodecContext*   adec;     // audio decoder (NULL if no audio)
    int               vstream;
    int               astream;  // -1 if no audio
    AVRational        vtb;      // video timebase
    AVRational        atb;      // audio timebase
    volatile int64_t* t_decode_us;
    volatile int*     total_frames;
};

static void* decode_thread_fn(void* arg)
{
    DecodeArgs*  a    = (DecodeArgs*)arg;
    FrameQueue*  vq   = a->vq;
    AudioQueue*  aq   = a->aq;
    AVPacket*    pkt  = av_packet_alloc();
    AVFrame*     vfrm = av_frame_alloc();
    AVFrame*     afrm = (aq != NULL) ? av_frame_alloc() : NULL;

    while (!g_stop) {
        if (av_read_frame(a->fmt, pkt) < 0) break;

        // ── Video packet ───────────────────────────────────────────────────────
        if (pkt->stream_index == a->vstream) {
            const int64_t t0 = now_us();
            avcodec_send_packet(a->vdec, pkt);
            av_packet_unref(pkt);

            while (!g_stop && avcodec_receive_frame(a->vdec, vfrm) == 0) {
                *a->t_decode_us += now_us() - t0;
                (*a->total_frames)++;

                if (vfrm->pts == AV_NOPTS_VALUE) { av_frame_unref(vfrm); continue; }

                const double pts = vfrm->pts * av_q2d(a->vtb);

                pthread_mutex_lock(&vq->mu);
                while (vq->count == 4 && !g_stop)
                    pthread_cond_wait(&vq->not_full, &vq->mu);
                if (g_stop) { pthread_mutex_unlock(&vq->mu); goto done; }

                const int slot = (vq->head + vq->count) & 3;
                av_frame_move_ref(vq->f[slot], vfrm);
                vq->pts_s[slot] = pts;
                vq->count++;
                pthread_cond_signal(&vq->not_empty);
                pthread_mutex_unlock(&vq->mu);
            }
        }
        // ── Audio packet ───────────────────────────────────────────────────────
        else if (aq != NULL && pkt->stream_index == a->astream) {
            avcodec_send_packet(a->adec, pkt);
            av_packet_unref(pkt);

            while (!g_stop && avcodec_receive_frame(a->adec, afrm) == 0) {
                if (afrm->pts == AV_NOPTS_VALUE) { av_frame_unref(afrm); continue; }

                const double pts = afrm->pts * av_q2d(a->atb);

                pthread_mutex_lock(&aq->mu);
                while (aq->count == 64 && !g_stop)
                    pthread_cond_wait(&aq->not_full, &aq->mu);
                if (g_stop) { pthread_mutex_unlock(&aq->mu); goto done; }

                const int slot = (aq->head + aq->count) & 63;
                av_frame_move_ref(aq->f[slot], afrm);
                aq->pts_s[slot] = pts;
                aq->count++;
                pthread_cond_signal(&aq->not_empty);
                pthread_mutex_unlock(&aq->mu);
            }
        } else {
            av_packet_unref(pkt);
        }
    }

done:
    pthread_mutex_lock(&vq->mu);
    vq->eof = true;
    pthread_cond_signal(&vq->not_empty);
    pthread_mutex_unlock(&vq->mu);

    if (aq != NULL) {
        pthread_mutex_lock(&aq->mu);
        aq->eof = true;
        pthread_cond_signal(&aq->not_empty);
        pthread_mutex_unlock(&aq->mu);
    }

    if (afrm) av_frame_free(&afrm);
    av_frame_free(&vfrm);
    av_packet_free(&pkt);
    return NULL;
}

// ── Audio playback arguments and thread ───────────────────────────────────────
struct AudioPlayArgs {
    AudioQueue*            aq;
    AVCodecContext*        adec;
    volatile double*       audio_clock_s;       // updated by audio thread, read by video
    volatile bool*         audio_clock_valid;   // true after first frame played
    volatile bool*         start_playback;      // signal from video thread to start ALSA output
    const char*            alsa_device;
};

// Pulls decoded audio frames from AudioQueue, resamples to 48kHz stereo if
// needed, and writes to ALSA. Audio PTS becomes the master clock.
static void* audio_play_thread_fn(void* arg)
{
    AudioPlayArgs* a = (AudioPlayArgs*)arg;
    AudioQueue*    aq = a->aq;

    // Pin audio playback to Core 0 (display core) - audio is I/O-bound, won't interfere much
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    // ── Pre-playback: consume frames without output (prevents queue deadlock) ─
    fprintf(stderr, "[audio] thread ready, consuming frames until first video frame...\n");
    while (!*a->start_playback && !g_stop) {
        pthread_mutex_lock(&aq->mu);
        if (aq->count > 0) {
            // Discard frame (keep queue flowing to prevent decode thread deadlock)
            AVFrame* frame = aq->f[aq->head];
            AVFrame* fresh = av_frame_alloc();
            if (fresh) {
                aq->f[aq->head] = fresh;
                aq->head = (aq->head + 1) & 63;  // 64-element queue
                aq->count--;
                pthread_cond_signal(&aq->not_full);
                av_frame_free(&frame);
            }
        }
        pthread_mutex_unlock(&aq->mu);
        sleep_us(1000);  // 1ms poll
    }
    if (g_stop) {
        return NULL;
    }
    fprintf(stderr, "[audio] first video frame ready, initializing ALSA...\n");

    // ── ALSA setup ─────────────────────────────────────────────────────────────
    snd_pcm_t* pcm = NULL;
    if (snd_pcm_open(&pcm, a->alsa_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "[audio] FATAL: snd_pcm_open(%s) failed\n", a->alsa_device);
        return NULL;
    }

    const unsigned int rate     = 48000;
    const unsigned int channels = 2;
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           channels, rate, 1, 100000) < 0) {
        fprintf(stderr, "[audio] FATAL: snd_pcm_set_params failed\n");
        snd_pcm_close(pcm);
        return NULL;
    }

    fprintf(stderr, "[audio] ALSA opened: %s  48kHz stereo S16_LE\n", a->alsa_device);

    // ── Resampler setup (only if source format differs) ───────────────────────
    SwrContext* swr = NULL;
    if (a->adec->sample_rate != (int)rate || a->adec->channels != (int)channels
        || a->adec->sample_fmt != AV_SAMPLE_FMT_S16) {
        swr = swr_alloc();
        av_opt_set_int(swr, "in_channel_layout",  a->adec->channel_layout, 0);
        av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swr, "in_sample_rate",     a->adec->sample_rate, 0);
        av_opt_set_int(swr, "out_sample_rate",    rate, 0);
        av_opt_set_sample_fmt(swr, "in_sample_fmt",  a->adec->sample_fmt, 0);
        av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        swr_init(swr);
        fprintf(stderr, "[audio] resampler: %d Hz %d ch -> 48000 Hz 2 ch\n",
                a->adec->sample_rate, a->adec->channels);
    }

    uint8_t* resample_buf = NULL;
    int      resample_capacity = 0;
    int64_t  total_samples_written = 0;  // cumulative samples for clock tracking
    int      frame_count = 0;
    double   first_pts_s = -1.0;  // PTS of first audio frame (for clock offset)

    // ── Playback loop ──────────────────────────────────────────────────────────
    while (!g_stop) {
        pthread_mutex_lock(&aq->mu);
        while (aq->count == 0 && !aq->eof && !g_stop)
            pthread_cond_wait(&aq->not_empty, &aq->mu);

        if ((aq->count == 0 && aq->eof) || g_stop) {
            pthread_mutex_unlock(&aq->mu);
            break;
        }

        AVFrame* frame = aq->f[aq->head];
        double   pts_s = aq->pts_s[aq->head];

        // Capture first PTS to align audio clock with stream
        if (first_pts_s < 0.0) {
            first_pts_s = pts_s;
            fprintf(stderr, "[audio] first frame PTS=%.3f (clock offset)\n", first_pts_s);
        }
        AVFrame* fresh = av_frame_alloc();
        if (!fresh) {
            pthread_mutex_unlock(&aq->mu);
            fprintf(stderr, "[audio] FATAL: av_frame_alloc OOM\n");
            break;
        }
        aq->f[aq->head] = fresh;
        aq->head = (aq->head + 1) & 63;
        aq->count--;
        pthread_cond_signal(&aq->not_full);
        pthread_mutex_unlock(&aq->mu);

        // Debug: check input frame samples BEFORE resampling
        if (frame_count < 3) {
            const float* in_samples = (const float*)frame->data[0];  // fltp format
            fprintf(stderr, "[audio] PRE-resample frame %d: format=%d nb_samples=%d  in[0]=%.6f in[1]=%.6f\n",
                    frame_count, frame->format, frame->nb_samples, in_samples[0], in_samples[1]);
        }

        // ── Resample or direct copy ────────────────────────────────────────────
        const uint8_t* samples;
        int nb_samples;

        if (swr != NULL) {
            const int needed = av_samples_get_buffer_size(NULL, channels, frame->nb_samples,
                                                          AV_SAMPLE_FMT_S16, 0);
            if (needed > resample_capacity) {
                resample_buf = (uint8_t*)realloc(resample_buf, needed);
                resample_capacity = needed;
            }
            uint8_t* out_ptr = resample_buf;
            nb_samples = swr_convert(swr, &out_ptr, frame->nb_samples,
                                     (const uint8_t**)frame->data, frame->nb_samples);
            samples = resample_buf;
        } else {
            samples = frame->data[0];
            nb_samples = frame->nb_samples;
        }

        // ── Write to ALSA ──────────────────────────────────────────────────────
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, samples, nb_samples);
        if (written < 0) {
            snd_pcm_recover(pcm, written, 0);
        } else {
            total_samples_written += written;
        }

        // ── Update master audio clock ──────────────────────────────────────────
        // Clock = first_pts + (samples_played / rate)
        // This aligns the audio clock with the stream's PTS timeline
        snd_pcm_sframes_t delay = 0;
        snd_pcm_delay(pcm, &delay);
        if (delay < 0) delay = 0;

        int64_t samples_actually_played = total_samples_written - delay;
        if (samples_actually_played < 0) samples_actually_played = 0;

        *a->audio_clock_s = first_pts_s + (double)samples_actually_played / rate;

        // Mark audio clock as valid after first frame (allows video sync to start)
        if (!*a->audio_clock_valid) {
            *a->audio_clock_valid = true;
            fprintf(stderr, "[audio] clock now valid (video can sync)\n");
        }

        // Debug: show first few frames
        if (frame_count < 3) {
            const int16_t* s16 = (const int16_t*)samples;
            fprintf(stderr, "[audio] frame %d: pts=%.3f nb_samples=%d written=%ld delay=%ld clock=%.3f  sample[0]=%d sample[1]=%d\n",
                    frame_count, pts_s, nb_samples, (long)written, (long)delay, *a->audio_clock_s, s16[0], s16[1]);
        }
        frame_count++;

        av_frame_free(&frame);
    }

    // ── Cleanup ────────────────────────────────────────────────────────────────
    if (swr) swr_free(&swr);
    free(resample_buf);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    fprintf(stderr, "[audio] playback thread done\n");
    return NULL;
}

// ── FFmpeg decode + display loop ──────────────────────────────────────────────
static void play_video(const char* path,
                       volatile uint32_t* axi,
                       uint8_t* yuv_y, uint8_t* yuv_u, uint8_t* yuv_v,
                       bool benchmark, int threads, double seek_s, bool no_audio)
{
    int front = 0, back = 1;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open: %s\n", path);
        return;
    }
    avformat_find_stream_info(fmt, NULL);

    // ── Find video and audio streams ──────────────────────────────────────────
    int vstream = -1, astream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vstream == -1)
            vstream = i;
        else if (!no_audio && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && astream == -1)
            astream = i;
    }
    if (vstream == -1) {
        fprintf(stderr, "No video stream\n");
        avformat_close_input(&fmt);
        return;
    }

    // ── Open video decoder ─────────────────────────────────────────────────────
    AVCodecParameters* vpar   = fmt->streams[vstream]->codecpar;
    const AVCodec*     vcodec = avcodec_find_decoder(vpar->codec_id);
    AVCodecContext*    vdec   = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(vdec, vpar);
    vdec->thread_count      = threads;
    vdec->skip_loop_filter  = AVDISCARD_ALL;
    vdec->flags2           |= AV_CODEC_FLAG2_FAST;
    if (avcodec_open2(vdec, vcodec, NULL) < 0) {
        fprintf(stderr, "Could not open video codec\n");
        avcodec_free_context(&vdec);
        avformat_close_input(&fmt);
        return;
    }

    // ── Open audio decoder (if audio stream exists) ────────────────────────────
    AVCodecContext* adec = NULL;
    if (astream != -1) {
        AVCodecParameters* apar   = fmt->streams[astream]->codecpar;
        fprintf(stderr, "[audio] found stream #%d, codec_id=%d\n", astream, apar->codec_id);
        const AVCodec*     acodec = avcodec_find_decoder(apar->codec_id);
        if (acodec) {
            adec = avcodec_alloc_context3(acodec);
            avcodec_parameters_to_context(adec, apar);
            if (avcodec_open2(adec, acodec, NULL) < 0) {
                fprintf(stderr, "[audio] could not open codec, disabling audio\n");
                avcodec_free_context(&adec);
                adec = NULL;
                astream = -1;
            } else {
                fprintf(stderr, "[audio] codec: %s  %d Hz  %d ch\n",
                        acodec->name, adec->sample_rate, adec->channels);
            }
        } else {
            fprintf(stderr, "[audio] decoder not available for codec_id=%d, disabling audio\n", apar->codec_id);
            astream = -1;
        }
    } else {
        fprintf(stderr, "[audio] no audio stream found\n");
    }

    if (seek_s != 0.0) {
        double eff = seek_s;
        if (seek_s < 0.0 && fmt->duration != AV_NOPTS_VALUE)
            eff = fmt->duration / (double)AV_TIME_BASE + seek_s;
        if (eff > 0.0) {
            avformat_seek_file(fmt, -1, INT64_MIN,
                               (int64_t)(eff * AV_TIME_BASE), INT64_MAX, 0);
            avcodec_flush_buffers(vdec);
            if (adec) avcodec_flush_buffers(adec);
            fprintf(stderr, "[mp4_play] seeked to %.1f s\n", eff);
        }
    }

    fprintf(stderr, "[mp4_play] %s  %dx%d  threads=%d  audio=%s  mode=%s\n",
            path, vdec->width, vdec->height, vdec->thread_count,
            no_audio ? "disabled" : (adec ? "yes" : "no"),
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
            avcodec_send_packet(vdec, pkt);
            av_packet_unref(pkt);
            while (!g_stop && !done && avcodec_receive_frame(vdec, frm) == 0) {
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
            fprintf(stderr, "  Decode thread cnt : %d\n", vdec->thread_count);
            fprintf(stderr, "  Total wall time   : %.2f s\n", elapsed / 1e6);
        }

        av_frame_free(&frm);
        av_packet_free(&pkt);
        if (adec) avcodec_free_context(&adec);
        avcodec_free_context(&vdec);
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

    AVRational       vtb          = fmt->streams[vstream]->time_base;
    AVRational       atb          = (adec != NULL) ? fmt->streams[astream]->time_base : (AVRational){1, 1};
    volatile int64_t t_decode_us  = 0;
    volatile int     total_frames = 0;
    int64_t          t_convert_us = 0;
    int64_t          t_vbl_us     = 0;
    int              disp_count   = 0;
    int              drop_count   = 0;

    // ── Initialise video queue ────────────────────────────────────────────────
    FrameQueue vq;
    vq.f[0]     = av_frame_alloc();
    vq.f[1]     = av_frame_alloc();
    vq.f[2]     = av_frame_alloc();
    vq.f[3]     = av_frame_alloc();
    vq.pts_s[0] = vq.pts_s[1] = vq.pts_s[2] = vq.pts_s[3] = 0.0;
    vq.head     = 0;
    vq.count    = 0;
    vq.eof      = false;
    pthread_mutex_init(&vq.mu,        NULL);
    pthread_cond_init (&vq.not_empty, NULL);
    pthread_cond_init (&vq.not_full,  NULL);

    // ── Initialise audio queue (if audio present) ─────────────────────────────
    AudioQueue aq;
    AudioQueue* aq_ptr = NULL;
    volatile double audio_clock_s = 0.0;
    volatile bool   audio_clock_valid = false;       // set true by audio thread after first frame
    volatile bool   audio_playback_start = false;
    pthread_t       athr;
    AudioPlayArgs   aargs;  // must be in outer scope (audio thread holds pointer)
    if (adec != NULL) {
        for (int i = 0; i < 64; i++) {
            aq.f[i] = av_frame_alloc();
            aq.pts_s[i] = 0.0;
        }
        aq.head  = 0;
        aq.count = 0;
        aq.eof   = false;
        pthread_mutex_init(&aq.mu,        NULL);
        pthread_cond_init (&aq.not_empty, NULL);
        pthread_cond_init (&aq.not_full,  NULL);
        aq_ptr = &aq;

        // Set MiSTer volume for audio playback (Groovy core defaults to muted)
        FILE* cmd_fp = fopen("/dev/MiSTer_cmd", "w");
        if (cmd_fp) {
            fprintf(cmd_fp, "volume 6\n");
            fclose(cmd_fp);
            fprintf(stderr, "[audio] set MiSTer volume to 6\n");
        }

        // Start audio thread immediately (to consume frames), but it waits for signal
        aargs.aq                = &aq;
        aargs.adec              = adec;
        aargs.audio_clock_s     = &audio_clock_s;
        aargs.audio_clock_valid = &audio_clock_valid;
        aargs.start_playback    = &audio_playback_start;
        aargs.alsa_device       = "default";  // Same device BGM uses
        pthread_create(&athr, NULL, audio_play_thread_fn, &aargs);
        fprintf(stderr, "[audio] thread started (will wait for video sync)\n");
    }

    DecodeArgs dargs = { &vq, aq_ptr, fmt, vdec, adec, vstream, astream, vtb, atb,
                         &t_decode_us, &total_frames };
    pthread_t  dthr;
    pthread_create(&dthr, NULL, decode_thread_fn, &dargs);

    // ── Display loop ───────────────────────────────────────────────────────────
    // Timing strategy:
    //   - WITH AUDIO:    video syncs to audio_clock_s (audio = master clock)
    //   - WITHOUT AUDIO: wall-clock timing (old behavior)
    int64_t start_wall_us  = 0;
    double  start_pts_s    = 0.0;
    bool    clk_init       = false;
    double  prev_pts_s     = -1.0;
    double  frame_period_s = 1.0 / 30.0;

    while (!g_stop) {
        // ── A. Get next decoded frame (blocks if queue empty) ─────────────────
        pthread_mutex_lock(&vq.mu);
        while (vq.count == 0 && !vq.eof && !g_stop)
            pthread_cond_wait(&vq.not_empty, &vq.mu);

        if ((vq.count == 0 && vq.eof) || g_stop) {
            pthread_mutex_unlock(&vq.mu);
            break;
        }

        AVFrame* frame  = vq.f[vq.head];
        double   pts_s  = vq.pts_s[vq.head];
        AVFrame* fresh  = av_frame_alloc();
        if (!fresh) {
            pthread_mutex_unlock(&vq.mu);
            fprintf(stderr, "[mp4_play] FATAL: av_frame_alloc OOM\n");
            g_stop = 1;
            av_frame_free(&frame);
            break;
        }
        vq.f[vq.head] = fresh;
        vq.head       = (vq.head + 1) & 3;
        vq.count--;
        pthread_cond_signal(&vq.not_full);
        pthread_mutex_unlock(&vq.mu);

        // ── B. Signal audio thread to start playback (on first video frame) ──
        if (!clk_init && adec != NULL) {
            audio_playback_start = true;  // Signal audio thread to start ALSA output
        }

        // ── C. Initialise master clock on first displayed frame ───────────────
        if (!clk_init) {
            if (adec == NULL) {
                // No audio: use wall-clock timing
                start_wall_us = now_us();
                start_pts_s   = pts_s;
            }
            clk_init = true;
        }

        // ── D. Update frame-period estimate ──────────────────────────────────
        if (prev_pts_s >= 0.0) {
            const double dp = pts_s - prev_pts_s;
            if (dp > 0.001 && dp < 0.2) frame_period_s = dp;
        }
        prev_pts_s = pts_s;

        // ── E. Compute target display time ────────────────────────────────────
        int64_t target_us;
        double  drop_thresh_s = frame_period_s * 1.0;

        if (adec != NULL) {
            // WITH AUDIO: video PTS must not exceed audio clock
            // Wait until audio_clock_s >= pts_s
            // Drop if video is more than 1 frame period behind audio
            // BUT: only sync if audio clock is valid (audio has played first frame)
            if (audio_clock_valid) {
                const double audio_clk = audio_clock_s;
                if (pts_s < audio_clk - drop_thresh_s) {
                    // Video is too far behind audio — drop frame
                    fprintf(stderr, "[mp4_play] drop  video_pts=%.3f  audio_clk=%.3f  "
                                    "behind=%.1f ms\n",
                            pts_s, audio_clk, (audio_clk - pts_s) * 1000.0);
                    av_frame_free(&frame);
                    drop_count++;
                    continue;
                }
                // Wait until audio catches up to video PTS
                // (In practice, video decode is slower than audio, so this rarely blocks)
                while (audio_clock_s < pts_s && !g_stop) {
                    sleep_us(1000);  // 1 ms spin
                }
            }
            target_us = now_us();  // display immediately (audio clock reached or not yet valid)
        } else {
            // WITHOUT AUDIO: wall-clock timing (old behavior)
            const double  elapsed_pts_s = pts_s - start_pts_s;
            target_us = start_wall_us + (int64_t)(elapsed_pts_s * 1e6);
            const int64_t drop_thresh_us = (int64_t)(drop_thresh_s * 1e6);

            if (now_us() > target_us + drop_thresh_us) {
                const int64_t late_us = now_us() - target_us;
                fprintf(stderr, "[mp4_play] drop  pts=%.3f  late=%lld ms\n",
                        pts_s, (long long)late_us / 1000);
                av_frame_free(&frame);
                drop_count++;
                start_wall_us = now_us();
                start_pts_s   = pts_s;
                continue;
            }
        }

        // ── F. Scale YUV, write to DDR3, trigger + await FPGA DMA ─────────────
        {
            const int64_t  tc0           = now_us();
            const uint32_t rgb_back_phys = FB_PHYS + (uint32_t)back * FB_SIZE;
            write_yuv_and_dma(frame, yuv_y, yuv_u, yuv_v, rgb_back_phys, axi);
            t_convert_us += now_us() - tc0;
        }
        av_frame_free(&frame);
        if (g_stop) break;

        // ── G. Sleep until 1 ms before target (wall-clock mode only) ──────────
        if (adec == NULL) {
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

        // ── J. Wait for next VBL to ensure ASCAL switched to new front ────────
        // Prevents ghosting: old front becomes new back, but ASCAL might still
        // be reading from it for a few scanlines after buf_sel flip. Waiting
        // for the next VBL ensures ASCAL has fully switched before we write.
        {
            const int64_t tv1 = now_us();
            while (!g_stop && !(axi[AXI_STATUS_IDX] & AXI_VBL_BIT)) {
                if (now_us() - tv1 > 50000LL) break;
            }
        }

        disp_count++;
    }

    // ── Shutdown decoder and audio threads ────────────────────────────────────
    g_stop = 1;
    pthread_mutex_lock(&vq.mu);
    pthread_cond_signal(&vq.not_full);
    pthread_cond_signal(&vq.not_empty);
    pthread_mutex_unlock(&vq.mu);
    if (aq_ptr != NULL) {
        pthread_mutex_lock(&aq.mu);
        pthread_cond_signal(&aq.not_full);
        pthread_cond_signal(&aq.not_empty);
        pthread_mutex_unlock(&aq.mu);
    }
    pthread_join(dthr, NULL);
    if (adec != NULL) pthread_join(athr, NULL);

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
    for (int i = 0; i < 4; i++)
        if (vq.f[i]) av_frame_free(&vq.f[i]);
    pthread_cond_destroy(&vq.not_empty);
    pthread_cond_destroy(&vq.not_full);
    pthread_mutex_destroy(&vq.mu);

    if (aq_ptr != NULL) {
        for (int i = 0; i < 4; i++)
            if (aq.f[i]) av_frame_free(&aq.f[i]);
        pthread_cond_destroy(&aq.not_empty);
        pthread_cond_destroy(&aq.not_full);
        pthread_mutex_destroy(&aq.mu);
    }

    if (adec) avcodec_free_context(&adec);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    fprintf(stderr, "[mp4_play v" MP4_PLAY_VERSION "] start\n");
    if (argc < 2) {
        fprintf(stderr, "usage: mp4_play <file> [-b] [-t N] [-ss N] [--no-audio]\n");
        fprintf(stderr, "  -b           benchmark: decode 200 frames as fast as possible\n");
        fprintf(stderr, "  -t N         FFmpeg decoder thread count (default 1)\n");
        fprintf(stderr, "  -ss N        seek to N seconds (negative = from end)\n");
        fprintf(stderr, "               e.g. -ss -60 seeks to 1 minute before EOF\n");
        fprintf(stderr, "  --no-audio   disable audio (video only, wall-clock timing)\n");
        return 1;
    }
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // Overclock CPU for better H.264 decode performance
    // 1000 MHz = safe with heatsink + fan
    set_cpu_freq(BOOST_FREQ);

    bool   benchmark = false;
    bool   no_audio  = false;
    int    threads   = 1;
    double seek_s    = 0.0;
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        if (strcmp(argv[i], "--no-audio") == 0) {
            no_audio = true;
        } else {
            switch (argv[i][1]) {
                case 'b': benchmark = true; break;
                case 't': if (i + 1 < argc) threads = atoi(argv[++i]); break;
                case 's': if (argv[i][2] == 's' && i + 1 < argc) seek_s = atof(argv[++i]); break;
            }
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

    // ── Trigger Switchres 480i for CRT output ─────────────────────────────────────
    fprintf(stderr, "[mp4_play] triggering Switchres 480i for CRT...\n");
    {
        // Groovy Switchres header for 640×480i @ 59.94Hz NTSC
        const uint8_t groovy_480i_modeline[20] = {
            0x80, 0x02,  // H = 640
            0x10,        // HFP = 16
            0x60,        // HS = 96
            0x6A,        // HBP = 106
            0xF0, 0x00,  // V = 240 (per field)
            0x04,        // VFP = 4
            0x03,        // VS = 3
            0x0F,        // VBP = 15
            0x1B,        // PLL_M0 = 27
            0x00,        // PLL_M1 = 0
            0x64,        // PLL_C0 = 100
            0x00,        // PLL_C1 = 0
            0x01, 0x00, 0x00, 0x00,  // PLL_K = 1
            0x01,        // CE_PIX = 1
            0x01         // INTERLACED = 1
        };

        // Map DDR3 page containing Switchres header (offset 8 from framebuffer base)
        const uint32_t SWITCHRES_HDR_PHYS = FB_PHYS + 8;
        const uint32_t PAGE_SIZE = 4096;
        const uint32_t PAGE_BASE = FB_PHYS & ~(PAGE_SIZE - 1);
        const uint32_t OFFSET_IN_PAGE = (FB_PHYS & (PAGE_SIZE - 1)) + 8;

        uint8_t* page = (uint8_t*)mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, mem_fd, PAGE_BASE);
        if (page == MAP_FAILED) {
            perror("mmap Switchres header page");
        } else {
            memcpy(page + OFFSET_IN_PAGE, groovy_480i_modeline, 20);
            munmap(page, PAGE_SIZE);

            // Trigger Switchres via AXI register 0x00C
            axi[AXI_SWITCHRES_IDX] = 0x1;  // bit 0 = trigger, frame 0
            usleep(100000);  // Wait 100ms for PLL to stabilize
            fprintf(stderr, "[mp4_play] Switchres triggered, waiting for PLL...\n");
        }
    }

    play_video(argv[1], axi, yuv_y, yuv_u, yuv_v, benchmark, threads, seek_s, no_audio);

    // Restore stock CPU frequency
    set_cpu_freq(STOCK_FREQ);

    munmap(yuv_map, YUV_TOTAL);
    munmap(axi_map, AXI_SIZE);
    close(mem_fd);
    return 0;
}