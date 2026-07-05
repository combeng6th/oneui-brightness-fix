/*
 * brightd — Native brightness daemon for ported Samsung ROMs
 *
 * When running an S23U ROM on Note 20 Ultra hardware, the lights HAL
 * always writes max brightness (510) regardless of the requested value.
 * post-fs-data.sh locks the HAL out by setting root-only permissions
 * on the backlight sysfs node before the HAL starts. This daemon then
 * acts as the sole brightness controller.
 *
 * Sensor input: ASensorManager NDK API via dlopen (zero forks).
 *               Falls back to dumpsys sensorservice parsing if unavailable.
 * Filter chain: median-of-10 -> symmetric EMA -> hysteresis -> proportional ramp.
 *
 * Build: clang -O2 -Wall -o brightd brightd.c -ldl && llvm-strip brightd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <stdint.h>

#define BKLT_PATH  "/sys/class/backlight/panel0-backlight/brightness"
#define LIBANDROID "/system/lib64/libandroid.so"

/* Tuning — adjust these for your panel */
#define MAX_HW       510      /* max_brightness of your backlight device   */
#define MIN_HW       2        /* minimum visible backlight level           */
#define MAX_SW       255      /* Android settings brightness range         */
#define AUTO_K       80       /* curve midpoint: 50% backlight at this lux */
#define EMA_ALPHA    0.10f    /* EMA smoothing factor (0-1)                */
#define HYSTERESIS   15       /* ignore target changes smaller than this   */
#define MIN_STEP     1        /* minimum ramp step per animation frame     */
#define RAMP_DIV     15       /* ramp speed: delta/RAMP_DIV per frame      */
#define FRAME_MS     200      /* animation frame interval (ms)             */
#define MODE_CHECK_S 5        /* brightness mode poll interval (seconds)   */
#define MEDIAN_WIN   10       /* median filter window size                 */
#define SENSOR_RATE_US 250000 /* sensor sample interval (microseconds)     */
#define ASENSOR_TYPE_LIGHT 5

/* ASensorEvent struct (matches NDK ABI, 104 bytes) */
typedef struct {
    int32_t version, sensor, type, reserved0;
    int64_t timestamp;
    union { float data[16]; float light; };
    uint32_t flags;
    int32_t reserved1[3];
} SensorEvent;

typedef void ASensorManager;
typedef void ASensor;
typedef void ASensorEventQueue;
typedef void ALooper;

static ASensorManager*    (*fn_mgr_get)(void);
static const ASensor*     (*fn_get_sensor)(ASensorManager*, int);
static ASensorEventQueue* (*fn_create_queue)(ASensorManager*, ALooper*, int, void*, void*);
static int     (*fn_enable)(ASensorEventQueue*, const ASensor*);
static int     (*fn_set_rate)(ASensorEventQueue*, const ASensor*, int32_t);
static ssize_t (*fn_get_events)(ASensorEventQueue*, SensorEvent*, size_t);
static ALooper*(*fn_looper_prepare)(int);
static int     (*fn_looper_poll)(int, int*, int*, void**);

static int sensor_api_ok = 0;
static ASensorEventQueue *sensor_queue;

/* ── Median filter ──────────────────────────────────────────────── */

static int lux_ring[MEDIAN_WIN], lux_n = 0, lux_idx = 0;

static void push_lux(int v) {
    lux_ring[lux_idx] = v;
    lux_idx = (lux_idx + 1) % MEDIAN_WIN;
    if (lux_n < MEDIAN_WIN) lux_n++;
}

static int cmp_int(const void *a, const void *b) {
    return *(const int*)a - *(const int*)b;
}

static int median_lux(void) {
    if (lux_n == 0) return 0;
    int s[MEDIAN_WIN];
    memcpy(s, lux_ring, lux_n * sizeof(int));
    qsort(s, lux_n, sizeof(int), cmp_int);
    return (lux_n & 1) ? s[lux_n/2] : (s[lux_n/2-1] + s[lux_n/2]) / 2;
}

/* ── Helpers ────────────────────────────────────────────────────── */

static int read_sysfs_int(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    char b[32]; int n = read(fd, b, 31); close(fd);
    if (n <= 0) return -1; b[n] = 0; return atoi(b);
}

static void write_sysfs_int(const char *p, int v) {
    int fd = open(p, O_WRONLY);
    if (fd < 0) return;
    char b[16]; int n = snprintf(b, 16, "%d", v);
    write(fd, b, n); close(fd);
}

static int popen_int(const char *c) {
    FILE *f = popen(c, "r"); if (!f) return -1;
    char b[64]; int v = -1;
    if (fgets(b, 64, f)) v = atoi(b);
    pclose(f); return v;
}

static void msleep(int ms) {
    struct timespec t = { ms/1000, (ms%1000)*1000000L };
    nanosleep(&t, NULL);
}

/* ── Sensor API (zero-fork via dlopen) ──────────────────────────── */

static int init_sensor_api(void) {
    void *lib = dlopen(LIBANDROID, RTLD_NOW);
    if (!lib) return 0;
    fn_mgr_get       = dlsym(lib, "ASensorManager_getInstance");
    fn_get_sensor     = dlsym(lib, "ASensorManager_getDefaultSensor");
    fn_create_queue   = dlsym(lib, "ASensorManager_createEventQueue");
    fn_enable         = dlsym(lib, "ASensorEventQueue_enableSensor");
    fn_set_rate       = dlsym(lib, "ASensorEventQueue_setEventRate");
    fn_get_events     = dlsym(lib, "ASensorEventQueue_getEvents");
    fn_looper_prepare = dlsym(lib, "ALooper_prepare");
    fn_looper_poll    = dlsym(lib, "ALooper_pollAll");
    if (!fn_mgr_get || !fn_get_sensor || !fn_create_queue || !fn_enable ||
        !fn_set_rate || !fn_get_events || !fn_looper_prepare || !fn_looper_poll)
        return 0;
    ALooper *l = fn_looper_prepare(0); if (!l) return 0;
    ASensorManager *m = fn_mgr_get(); if (!m) return 0;
    const ASensor *s = fn_get_sensor(m, ASENSOR_TYPE_LIGHT); if (!s) return 0;
    sensor_queue = fn_create_queue(m, l, 0, NULL, NULL); if (!sensor_queue) return 0;
    if (fn_enable(sensor_queue, s) < 0) return 0;
    fn_set_rate(sensor_queue, s, SENSOR_RATE_US);
    return 1;
}

static void drain_sensor(void) {
    SensorEvent e;
    while (fn_get_events(sensor_queue, &e, 1) > 0)
        if (e.type == ASENSOR_TYPE_LIGHT)
            push_lux((int)(e.light + 0.5f));
}

/* ── Fallback: parse dumpsys sensorservice ──────────────────────── */

static void read_lux_dumpsys(void) {
    FILE *f = popen("dumpsys sensorservice 2>/dev/null", "r");
    if (!f) return;
    char l[512]; int found=0, v[64], n=0;
    while (fgets(l, 512, f)) {
        if (!found) { if (strstr(l,"Light Ambient") && strstr(l,"last 50")) found=1; continue; }
        if (l[0]!='\t' && l[0]!=' ') break;
        char *p = strstr(l, ") ");
        if (p && n<64) { p+=2; v[n++]=atoi(p); }
    }
    pclose(f);
    int s = n > MEDIAN_WIN ? n - MEDIAN_WIN : 0;
    for (int i = s; i < n; i++) push_lux(v[i]);
}

/* ── Main loop ──────────────────────────────────────────────────── */

int main(void) {
    /* Wait for boot */
    for (;;) {
        FILE *f = popen("getprop sys.boot_completed","r");
        if (f) { char b[8]; if (fgets(b,8,f)&&b[0]=='1') { pclose(f); break; } pclose(f); }
        msleep(2000);
    }
    msleep(3000);

    sensor_api_ok = init_sensor_api();

    float smooth = -1.0f;
    int cur_bl = -1, ramp = -1, mode = -1, msw = 128;
    int mctr = 0, dctr = 0, manctr = 0;
    int fpm = (MODE_CHECK_S*1000)/FRAME_MS;
    int dpf = 1000/FRAME_MS;

    for (;;) {
        mctr = (mctr+1) % fpm;

        /* Check brightness mode (manual vs auto) */
        if (mode < 0 || mctr == 0) {
            int m = popen_int("settings get system screen_brightness_mode");
            if (m < 0) m = 0;
            if (m != mode) { mode=m; smooth=-1; ramp=-1; lux_n=lux_idx=0; }
        }

        if (mode == 1) {
            /* ── Auto-brightness ───────────────────────────────── */
            if (sensor_api_ok) {
                fn_looper_poll(FRAME_MS, NULL, NULL, NULL);
                drain_sensor();
            } else {
                dctr = (dctr+1) % dpf;
                if (dctr == 0) read_lux_dumpsys();
                msleep(FRAME_MS);
            }

            int med = median_lux();
            if (smooth < 0) smooth = (float)med;
            else smooth += ((float)med - smooth) * EMA_ALPHA;

            int s = (int)(smooth + 0.5f);
            int tgt = (s <= 0) ? MIN_HW : s * MAX_HW / (s + AUTO_K);
            if (tgt < MIN_HW) tgt = MIN_HW;
            if (tgt > MAX_HW) tgt = MAX_HW;

            if (ramp < 0) ramp = tgt;
            else if (abs(tgt - ramp) > HYSTERESIS) ramp = tgt;

            if (cur_bl < 0) { cur_bl = ramp; write_sysfs_int(BKLT_PATH, cur_bl); }

            if (cur_bl != ramp) {
                int d = abs(ramp - cur_bl);
                int step = d / RAMP_DIV;
                if (step < MIN_STEP) step = MIN_STEP;
                if (ramp > cur_bl) { cur_bl += step; if (cur_bl > ramp) cur_bl = ramp; }
                else { cur_bl -= step; if (cur_bl < ramp) cur_bl = ramp; }
                write_sysfs_int(BKLT_PATH, cur_bl);
            }
        } else {
            /* ── Manual brightness ─────────────────────────────── */
            int hw = read_sysfs_int(BKLT_PATH);
            if (hw != cur_bl) {
                int s = popen_int("settings get system screen_brightness");
                if (s >= 0) msw = s;
            } else {
                manctr = (manctr+1) % 5;
                if (manctr == 0) { int s = popen_int("settings get system screen_brightness"); if (s>=0) msw=s; }
            }

            int tgt;
            if (msw >= MAX_SW) tgt = MAX_HW;
            else if (msw <= 0) tgt = MIN_HW;
            else tgt = MIN_HW + msw * (MAX_HW - MIN_HW) / MAX_SW;

            if (hw != tgt) { write_sysfs_int(BKLT_PATH, tgt); cur_bl = tgt; msleep(50); }
            else { cur_bl = tgt; msleep(FRAME_MS); }
        }
    }
}
