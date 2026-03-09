// mp4_launcher.c - Minimal daemon to launch mp4_play from OSD
// Polls FPGA status register and launches mp4_play when OSD trigger button pressed
//
// Build: arm-linux-gnueabihf-gcc -o mp4_launcher mp4_launcher.c -O2 -Wall
// Install: Copy to /media/fat/linux/
// Run: Add to /media/fat/linux/user-startup.sh:
//      /media/fat/linux/mp4_launcher &

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>

// MiSTer mp4_ctrl_regs AXI register addresses
#define AXI_BASE        0xFF200000
#define AXI_SIZE        4096
#define AXI_STATUS_IDX  0          // offset 0x000 / 4 = index 0
#define AXI_CTRL_IDX    2          // offset 0x008 / 4 = index 2

// Video paths (customize these as needed)
#define DEFAULT_VIDEO   "/media/fat/videos/demo.mp4"
#define VIDEO_DIR       "/media/fat/videos"

// CPU frequency control
#define CPUFREQ_FILE    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define STOCK_FREQ      800000     // 800 MHz (stock DE10-nano frequency)
#define OVERCLOCK_FREQ  1000000    // 1000 MHz (safe overclock with active cooling)

// Status bit definitions (from OSD triggers)
// These correspond to T options in CONF_STR
#define TRIGGER_PLAY    (1 << 0)   // T0 in OSD
#define TRIGGER_STOP    (1 << 1)   // T1 in OSD
#define TRIGGER_RANDOM  (1 << 2)   // T2 in OSD

static volatile int g_running = 1;
static volatile uint32_t* g_axi = NULL;

void signal_handler(int sig) {
    fprintf(stderr, "[mp4_launcher] Caught signal %d, exiting...\n", sig);
    g_running = 0;
}

// Set CPU frequency (for overclocking during video playback)
int set_cpu_freq(int freq_khz) {
    FILE* fp = fopen(CPUFREQ_FILE, "w");
    if (!fp) {
        fprintf(stderr, "[mp4_launcher] WARNING: Cannot open %s (need root or cpufreq driver)\n", CPUFREQ_FILE);
        return -1;
    }

    fprintf(fp, "%d", freq_khz);
    fclose(fp);

    fprintf(stderr, "[mp4_launcher] CPU frequency set to %d kHz (%.0f MHz)\n",
            freq_khz, freq_khz / 1000.0);
    return 0;
}

// Get current CPU frequency
int get_cpu_freq() {
    FILE* fp = fopen(CPUFREQ_FILE, "r");
    if (!fp) return STOCK_FREQ;

    int freq = STOCK_FREQ;
    fscanf(fp, "%d", &freq);
    fclose(fp);
    return freq;
}

// Get a random .mp4/.mkv/.mov file from video directory
void get_random_video(char* path_out, size_t max_len) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "find \"%s\" -type f \\( -iname '*.mp4' -o -iname '*.mkv' -o -iname '*.mov' -o -iname '*.m4v' \\) 2>/dev/null | shuf -n 1",
             VIDEO_DIR);

    FILE* fp = popen(cmd, "r");
    if (fp) {
        if (fgets(path_out, max_len, fp)) {
            // Strip newline
            size_t len = strlen(path_out);
            if (len > 0 && path_out[len-1] == '\n')
                path_out[len-1] = '\0';
        } else {
            // Fallback to default
            snprintf(path_out, max_len, "%s", DEFAULT_VIDEO);
        }
        pclose(fp);
    } else {
        snprintf(path_out, max_len, "%s", DEFAULT_VIDEO);
    }
}

// Stop video playback and restore stock CPU frequency
void stop_video() {
    fprintf(stderr, "[mp4_launcher] Stopping playback...\n");
    system("killall -9 mp4_play 2>/dev/null");
    usleep(100000);  // 100ms settle time

    // Restore stock frequency
    set_cpu_freq(STOCK_FREQ);
}

// Launch mp4_play with the given video file (with overclock)
void launch_video(const char* video_path, int threads) {
    char cmd[1024];

    // Stop any existing playback first
    stop_video();

    // Overclock CPU for better H.264 decode performance
    // WARNING: Requires active cooling! Disable if you experience instability.
    fprintf(stderr, "[mp4_launcher] Overclocking CPU for video playback...\n");
    set_cpu_freq(OVERCLOCK_FREQ);
    usleep(100000);  // Let frequency stabilize

    // Launch new instance
    snprintf(cmd, sizeof(cmd), "/media/fat/_DEV/mp4_play \"%s\" -t %d > /tmp/mp4_play.log 2>&1 &",
             video_path, threads);

    fprintf(stderr, "[mp4_launcher] Launching: %s\n", video_path);
    system(cmd);
}

int main() {
    int fd;
    uint32_t prev_status = 0;
    char video_path[512];

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "[mp4_launcher] Starting MP4 launcher daemon...\n");

    // Open /dev/mem and map AXI registers
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("[mp4_launcher] ERROR: Cannot open /dev/mem");
        return 1;
    }

    g_axi = (volatile uint32_t*)mmap(NULL, AXI_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, AXI_BASE);
    if (g_axi == MAP_FAILED) {
        perror("[mp4_launcher] ERROR: Cannot mmap AXI registers");
        close(fd);
        return 1;
    }

    fprintf(stderr, "[mp4_launcher] Mapped AXI registers at 0x%08X\n", AXI_BASE);
    fprintf(stderr, "[mp4_launcher] Watching for OSD triggers...\n");
    fprintf(stderr, "[mp4_launcher]   - T0: Play default video (%s)\n", DEFAULT_VIDEO);
    fprintf(stderr, "[mp4_launcher]   - T1: Stop playback\n");
    fprintf(stderr, "[mp4_launcher]   - T2: Play random video from %s\n", VIDEO_DIR);

    // Main polling loop
    while (g_running) {
        uint32_t status = g_axi[AXI_STATUS_IDX];

        // Detect rising edges on trigger bits
        uint32_t rising = status & ~prev_status;

        if (rising & TRIGGER_PLAY) {
            // T0: Play default video
            launch_video(DEFAULT_VIDEO, 2);
        }

        if (rising & TRIGGER_STOP) {
            // T1: Stop playback and restore stock frequency
            stop_video();
        }

        if (rising & TRIGGER_RANDOM) {
            // T2: Play random video
            get_random_video(video_path, sizeof(video_path));
            launch_video(video_path, 2);
        }

        prev_status = status;
        usleep(100000);  // Poll every 100ms (0.1% CPU usage)
    }

    // Cleanup
    fprintf(stderr, "[mp4_launcher] Shutting down...\n");
    stop_video();  // Stop playback and restore stock CPU frequency
    munmap((void*)g_axi, AXI_SIZE);
    close(fd);

    fprintf(stderr, "[mp4_launcher] Exited cleanly.\n");
    return 0;
}
