// ember-latency: Vulkan layer that estimates input latency for MangoHud.
//
// What it measures (same idea as PresentMon's "click to photon"):
//   kernel timestamp of a key press / mouse click (evdev, CLOCK_MONOTONIC)
//   -> first frame the game *starts* after that press
//   -> the moment that frame's first pixel goes out to the monitor
//      (VK_EXT_present_timing, i.e. the compositor's real flip time; frames the
//      compositor drops are skipped, so it follows the press to the frame you saw).
// It's an estimate: it assumes the game reads input at the start of a frame and it
// can't see the panel's own pixel response. Expect it to be within about one frame.
// Without present timing (other drivers/WSI layers) it falls back to when
// vkWaitForPresentKHR returns, and only in vsync (FIFO), where that is meaningful.
//
// Cost: no GPU work, and the game's threads never wait on anything. While you're not
// pressing anything a present costs one atomic load. Mouse motion is filtered out in
// the kernel (EVIOCSMASK), so only a press or click wakes the input thread. Only the
// frame after a press (plus any dropped frames after it) asks the driver for timing.
// The readout file is written by a worker thread, never on the present path.
//
// Active only when MangoHud is (manifest: enable_environment MANGOHUD=1).
// Off switch: DISABLE_EMBER_LATENCY=1. Readout: /dev/shm/ember-latency/<pid>
// (line 1 = average of the last 16 presses in ms, line 2 = details).
// Debug: EMBER_LATENCY_DEBUG=1 (2 = every timing report). Fallback only: EMBER_LATENCY_NO_TIMING=1.
// Test without a human: EMBER_LATENCY_TEST=<presses/s>.
//
// Build and install: `make install` (see the README). The manifest goes in
// ~/.local/share/vulkan/implicit_layer.d/ember_latency.x86_64.json.
// Reading /dev/input needs your user in the `input` group.
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <dirent.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define KEY(h) (*(void **)(h))
#define MAX_TINS 8         // presses tracked per measured frame
#define RING 256           // pending presses
#define HISTORY 16         // the readout averages the last 16 measurements
#define MAX_PEND 8         // presses (frames) being followed per swapchain
#define TIMING_QUEUE 32    // driver-side timing report queue we reserve
#define WAIT_SLICE_NS 10000000ull   // 10 ms per vkWaitForPresent call
#define WAIT_MAX_NS 300000000ull    // give up on a frame after 300 ms
#define MAX_LATENCY_NS 500000000ull // ignore anything slower (loading, pauses)

static int g_debug; // EMBER_LATENCY_DEBUG=1, or 2 for every timing report
#define DBG(...) do { if (g_debug) fprintf(stderr, "ember-latency: " __VA_ARGS__); } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------- presses
static uint64_t g_ring[RING];
static _Atomic uint64_t g_head; // written by the input thread only
static _Atomic uint64_t g_tail; // advanced by presents (under g_tail_mtx)
static pthread_mutex_t g_tail_mtx = PTHREAD_MUTEX_INITIALIZER;

static void push_press(uint64_t t)
{
    uint64_t h = atomic_load_explicit(&g_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&g_tail, memory_order_relaxed) >= RING)
        return; // nobody is presenting; drop
    g_ring[h % RING] = t;
    atomic_store_explicit(&g_head, h + 1, memory_order_release);
}

static bool want_device(int fd)
{
    unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
        return false;
#define HAS(b) (keys[(b) / (8 * sizeof(long))] & (1ul << ((b) % (8 * sizeof(long)))))
    return HAS(BTN_LEFT) || (HAS(KEY_A) && HAS(KEY_SPACE));
#undef HAS
}

#define MAX_DEVS 64
static int g_dev_fd[MAX_DEVS];
static dev_t g_dev_rdev[MAX_DEVS];
static int g_ndevs;

static void drop_device(int ep, int fd)
{
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    for (int i = 0; i < g_ndevs; i++)
        if (g_dev_fd[i] == fd) {
            g_dev_fd[i] = g_dev_fd[--g_ndevs];
            g_dev_rdev[i] = g_dev_rdev[g_ndevs];
            break;
        }
}

static void add_device(int ep, const char *name)
{
    if (strncmp(name, "event", 5) != 0)
        return;
    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", name);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    bool seen = fstat(fd, &st) != 0 || g_ndevs >= MAX_DEVS;
    for (int i = 0; !seen && i < g_ndevs; i++)
        seen = g_dev_rdev[i] == st.st_rdev; // IN_ATTRIB after IN_CREATE: already watching it
    if (seen || !want_device(fd)) {
        close(fd);
        return;
    }
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    // Kernel-side filter: only EV_KEY reaches us, so mouse motion never wakes this thread.
    unsigned long types[EV_CNT / (8 * sizeof(long)) + 1] = {0};
    types[EV_KEY / (8 * sizeof(long))] |= 1ul << (EV_KEY % (8 * sizeof(long)));
    struct input_mask m = { .type = EV_SYN, .codes_size = sizeof types, .codes_ptr = (uint64_t)(uintptr_t)types };
    if (ioctl(fd, EVIOCSMASK, &m) < 0)
        DBG("EVIOCSMASK failed on %s (%s); motion will wake us\n", path, strerror(errno));
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd);
        return;
    }
    g_dev_fd[g_ndevs] = fd;
    g_dev_rdev[g_ndevs++] = st.st_rdev;
    DBG("watching %s\n", path);
}

static void *input_thread(void *arg)
{
    (void)arg;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0)
        return NULL;
    const char *test = getenv("EMBER_LATENCY_TEST");
    int ino = -1, tfd = -1;
    if (test && atof(test) > 0) {
        // Synthetic presses at a fixed rate (for checking the pipeline without a human).
        double hz = atof(test);
        uint64_t period = (uint64_t)(1e9 / hz);
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        struct itimerspec its = { .it_interval = { (time_t)(period / 1000000000ull), (long)(period % 1000000000ull) } };
        its.it_value = its.it_interval;
        timerfd_settime(tfd, 0, &its, NULL);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
        epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    } else {
        ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ino >= 0) {
            inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB);
            struct epoll_event ev = { .events = EPOLLIN, .data.fd = ino };
            epoll_ctl(ep, EPOLL_CTL_ADD, ino, &ev);
        }
        DIR *d = opendir("/dev/input");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)))
                add_device(ep, e->d_name);
            closedir(d);
        }
    }
    struct epoll_event evs[8];
    for (;;) {
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0 && errno != EINTR)
            break;
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == tfd) {
                uint64_t exp;
                if (read(tfd, &exp, sizeof exp) > 0)
                    push_press(now_ns());
            } else if (fd == ino) {
                char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                while ((len = read(ino, buf, sizeof buf)) > 0)
                    for (char *p = buf; p < buf + len;) {
                        struct inotify_event *ie = (struct inotify_event *)p;
                        // IN_ATTRIB: udev fixes permissions just after IN_CREATE.
                        if (ie->len)
                            add_device(ep, ie->name);
                        p += sizeof *ie + ie->len;
                    }
            } else {
                struct input_event ie[32];
                ssize_t len = read(fd, ie, sizeof ie);
                if (len < 0 && errno != EAGAIN) { // unplugged
                    drop_device(ep, fd);
                    continue;
                }
                for (ssize_t k = 0; k < len / (ssize_t)sizeof ie[0]; k++)
                    if (ie[k].type == EV_KEY && ie[k].value == 1) // press, not release/repeat
                        push_press((uint64_t)ie[k].input_event_sec * 1000000000ull +
                                   (uint64_t)ie[k].input_event_usec * 1000ull);
            }
        }
    }
    return NULL;
}

static pthread_once_t g_input_once = PTHREAD_ONCE_INIT;
static void start_input_thread(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, input_thread, NULL) == 0)
        pthread_setname_np(t, "ember-lat-in");
    pthread_attr_destroy(&a);
}

// ---------------------------------------------------------------- readout
static pthread_mutex_t g_stats_mtx = PTHREAD_MUTEX_INITIALIZER;
static double g_hist[HISTORY];
static unsigned g_hist_n;
static unsigned long g_total;
static char g_out_path[64], g_tmp_path[72];

static void record(const uint64_t *tins, int n, uint64_t shown, const char *src)
{
    pthread_mutex_lock(&g_stats_mtx);
    double last = -1;
    for (int i = 0; i < n; i++) {
        if (shown < tins[i] || shown - tins[i] > MAX_LATENCY_NS)
            continue;
        last = (double)(shown - tins[i]) / 1e6;
        g_hist[g_hist_n++ % HISTORY] = last;
        g_total++;
    }
    if (last < 0) {
        pthread_mutex_unlock(&g_stats_mtx);
        return;
    }
    unsigned cnt = g_hist_n < HISTORY ? g_hist_n : HISTORY;
    double sum = 0, mn = 1e9, mx = 0;
    for (unsigned i = 0; i < cnt; i++) {
        sum += g_hist[i];
        if (g_hist[i] < mn) mn = g_hist[i];
        if (g_hist[i] > mx) mx = g_hist[i];
    }
    if (!g_out_path[0]) {
        mkdir("/dev/shm/ember-latency", 0755);
        // Sweep readouts left by games that crashed or were killed.
        DIR *dir = opendir("/dev/shm/ember-latency");
        if (dir) {
            struct dirent *e;
            while ((e = readdir(dir))) {
                int pid = atoi(e->d_name);
                if (pid > 0 && kill(pid, 0) < 0 && errno == ESRCH) {
                    char old[300];
                    snprintf(old, sizeof old, "/dev/shm/ember-latency/%s", e->d_name);
                    unlink(old);
                }
            }
            closedir(dir);
        }
        snprintf(g_out_path, sizeof g_out_path, "/dev/shm/ember-latency/%d", (int)getpid());
        snprintf(g_tmp_path, sizeof g_tmp_path, "%s.tmp", g_out_path);
    }
    FILE *f = fopen(g_tmp_path, "w");
    if (f) {
        // Line 1 is what MangoHud shows; line 2 is for humans (cat the file).
        fprintf(f, "%.1f\nlast=%.1f min=%.1f max=%.1f avg_of=%u total=%lu src=%s\n",
                sum / cnt, last, mn, mx, cnt, g_total, src);
        fclose(f);
        rename(g_tmp_path, g_out_path);
    }
    DBG("press -> shown %.2f ms (avg %.2f, %s)\n", last, sum / cnt, src);
    pthread_mutex_unlock(&g_stats_mtx);
}

// ---------------------------------------------------------------- dispatch
typedef struct inst_s {
    void *key;
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumerateDeviceExtensionProperties enum_ext;
    PFN_vkGetPhysicalDeviceFeatures2 feat2;
    struct inst_s *next;
} inst_t;

enum { SC_NONE, SC_V1, SC_V2 };

typedef struct {
    uint64_t seq;         // our timing request on the first frame that started after the press(es)
    uint64_t presented;   // when we handed that frame to the driver
    bool fifo;            // vsync'd present (XWayland's report is only real then)
    int n;
    uint64_t tins[MAX_TINS];
} pend_t;

typedef struct sc_s {
    VkSwapchainKHR h;
    int mode;                  // which present id flavour we can wait on
    VkPresentModeKHR create_mode;
    bool app_ids;              // the app tags presents itself: never insert ours
    uint64_t next_id;          // our own ids (only while !app_ids)
    bool timing;               // VK_EXT_present_timing usable: real scanout times
    bool x11;                  // reports lack first-pixel-out: this is XWayland
    uint64_t domain_id;
    uint32_t outstanding;      // our timing requests still in the driver's queue
    uint64_t req_issued, req_done; // reports come back in present order, one per request
    pend_t pend[MAX_PEND];     // presses waiting for their frame's scanout time
    int npend;
    bool hot;                  // npend || outstanding: presents must look at us
    int busy;                  // fallback waits in flight
    bool dead;                 // destroyed or retired: no new work
    struct sc_s *next;
} sc_t;

typedef struct job_s {         // work for the device's worker thread
    sc_t *sc;                  // fallback: wait for present `id` on this swapchain
    uint64_t id;
    uint64_t shown;            // timing path: already known, just record it
    const char *src;
    VkFence fence;             // XWayland without vsync: wait for the GPU to finish the frame
    VkSemaphore sem;
    int n;
    uint64_t tins[MAX_TINS];
    struct job_s *next;
} job_t;

typedef struct gc_s {          // our fence/semaphore, freed a while after use
    VkFence fence;
    VkSemaphore sem;
    uint64_t free_at;
    struct gc_s *next;
} gc_t;

typedef struct ldev_s {
    void *key;
    VkDevice dev;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy;
    PFN_vkQueuePresentKHR present;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkWaitForPresentKHR wait1;
    PFN_vkWaitForPresent2KHR wait2;
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT timing_qsize;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT timing_domains;
    PFN_vkGetPastPresentationTimingEXT timing_past;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkWaitForFences wait_fences;
    PFN_vkCreateSemaphore create_sem;
    PFN_vkDestroySemaphore destroy_sem;
    gc_t *gc;
    _Atomic bool active;
    bool x11;                  // a swapchain here turned out to be XWayland: new ones are too
    _Atomic int hot;           // swapchains that need a look on every present
    _Atomic uint64_t last_ret; // when the previous present returned = next frame's start
    _Atomic int errors;
    pthread_mutex_t mtx;
    pthread_cond_t cv;         // jobs / quit
    pthread_cond_t idle_cv;    // swapchain busy -> 0
    sc_t *scs;
    job_t *jobs, *jobs_tail;
    bool quit, has_thread;
    pthread_t thread;
    struct ldev_s *next;
} ldev_t;

static pthread_mutex_t g_map_mtx = PTHREAD_MUTEX_INITIALIZER;
static inst_t *g_insts;
static ldev_t *g_devs;

static inst_t *find_inst(void *key)
{
    pthread_mutex_lock(&g_map_mtx);
    inst_t *i = g_insts;
    while (i && i->key != key)
        i = i->next;
    pthread_mutex_unlock(&g_map_mtx);
    return i;
}

// Device records are never freed (a few hundred bytes per destroyed device), so the
// last-used one can be cached and read without a lock on every present.
static _Atomic(ldev_t *) g_dev_cache;

static ldev_t *find_dev(void *key)
{
    ldev_t *c = atomic_load_explicit(&g_dev_cache, memory_order_acquire);
    if (c && c->key == key)
        return c;
    pthread_mutex_lock(&g_map_mtx);
    ldev_t *d = g_devs;
    while (d && d->key != key)
        d = d->next;
    pthread_mutex_unlock(&g_map_mtx);
    if (d)
        atomic_store_explicit(&g_dev_cache, d, memory_order_release);
    return d;
}

static sc_t *find_sc(ldev_t *d, VkSwapchainKHR h) // d->mtx held
{
    sc_t *s = d->scs;
    while (s && s->h != h)
        s = s->next;
    return s;
}

static void update_hot(ldev_t *d, sc_t *s) // d->mtx held
{
    bool hot = !s->dead && s->timing && (s->npend > 0 || s->outstanding > 0);
    if (hot != s->hot) {
        s->hot = hot;
        atomic_fetch_add(&d->hot, hot ? 1 : -1);
    }
}

static void enqueue(ldev_t *d, const job_t *src) // d->mtx held
{
    job_t *j = malloc(sizeof *j);
    if (!j)
        return;
    *j = *src;
    j->next = NULL;
    if (d->jobs_tail)
        d->jobs_tail->next = j;
    else
        d->jobs = j;
    d->jobs_tail = j;
    pthread_cond_signal(&d->cv);
}

// Stop new work on a swapchain and wait out a fallback wait in flight (at most one 10 ms slice).
static void retire_sc(ldev_t *d, sc_t *s) // d->mtx held
{
    s->dead = true;
    s->npend = 0;
    update_hot(d, s);
    for (job_t **pp = &d->jobs; *pp;) {
        if ((*pp)->sc == s) {
            job_t *j = *pp;
            *pp = j->next;
            free(j);
        } else {
            pp = &(*pp)->next;
        }
    }
    d->jobs_tail = NULL;
    for (job_t *j = d->jobs; j; j = j->next)
        d->jobs_tail = j;
    while (s->busy)
        pthread_cond_wait(&d->idle_cv, &d->mtx);
}

// Collect finished timing reports. Reports come in present order; the first frame
// that actually reached the screen at or after a press's frame is when it was seen
// (a frame the compositor dropped is reported with time 0 and skipped).
static void drain_timing(ldev_t *d, sc_t *s, uint64_t now) // d->mtx held
{
    enum { N = 16 };
    VkPresentStageTimeEXT stages[N][4];
    VkPastPresentationTimingEXT recs[N];
    for (int i = 0; i < N; i++)
        recs[i] = (VkPastPresentationTimingEXT){ .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
                                                 .presentStageCount = 4, .pPresentStages = stages[i] };
    VkPastPresentationTimingPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
                                                    .presentationTimingCount = N, .pPresentationTimings = recs };
    VkPastPresentationTimingInfoEXT info = { .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
                                             .swapchain = s->h };
    VkResult r = d->timing_past(d->dev, &info, &props);
    if (r < 0) {
        DBG("vkGetPastPresentationTimingEXT failed (%d); timing off for this swapchain\n", r);
        s->timing = false;
        s->npend = 0;
        s->outstanding = 0;
        return;
    }
    uint32_t got = props.presentationTimingCount;
    s->outstanding -= got < s->outstanding ? got : s->outstanding;
    for (uint32_t k = 0; k < got && k < N; k++) {
        // Prefer first-pixel-out (Wayland: the compositor's flip time). XWayland only
        // reports "dequeued" (X Present complete), which Mesa fills with the same event.
        uint64_t t = 0, t_deq = 0;
        for (uint32_t st = 0; st < recs[k].presentStageCount && st < 4; st++) {
            if (stages[k][st].stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
                t = stages[k][st].time;
            else if (stages[k][st].stage & VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT)
                t_deq = stages[k][st].time;
        }
        bool xwl = false;
        if (!t && t_deq) {
            // XWayland: "complete" from X Present. With vsync that's the real flip; without
            // it, it's a stale vblank time, so those frames take the GPU-fence path instead.
            t = t_deq;
            xwl = true;
            s->x11 = d->x11 = true;
        }
        uint64_t seq = ++s->req_done;
        if (g_debug >= 2)
            DBG("report seq=%lu complete=%d stages=%u t=%lu now-t=%.2fms\n", (unsigned long)seq, recs[k].reportComplete, recs[k].presentStageCount, (unsigned long)t, t ? (double)(int64_t)(now - t) / 1e6 : 0.0);
        if (!t)
            continue; // dropped by the compositor: the press shows up in a later frame
        int keep = 0;
        for (int p = 0; p < s->npend; p++) {
            pend_t *pe = &s->pend[p];
            if (pe->seq > seq) {
                s->pend[keep++] = *pe;
                continue;
            }
            if (xwl && !pe->fifo)
                continue; // no usable time for this one (dropped)
            // Scanout can't be before we presented. It can be a little in the future
            // (gamescope reports its scheduled vblank), but not much: otherwise this
            // driver's clock isn't CLOCK_MONOTONIC and the numbers would be garbage.
            if (t + 1000000ull < pe->presented || t > now + 50000000ull) {
                DBG("timing clock doesn't match CLOCK_MONOTONIC; timing off\n");
                s->timing = false;
                s->npend = 0;
                s->outstanding = 0;
                return;
            }
            job_t j = { .shown = t, .n = pe->n, .src = xwl ? "xwayland-vsync" : "scanout" };
            memcpy(j.tins, pe->tins, sizeof j.tins);
            enqueue(d, &j);
        }
        s->npend = keep;
    }
}

static void expire_pend(sc_t *s, uint64_t now) // d->mtx held
{
    int keep = 0;
    for (int p = 0; p < s->npend; p++)
        if (now - s->pend[p].presented < MAX_LATENCY_NS)
            s->pend[keep++] = s->pend[p];
    s->npend = keep;
}

// ---------------------------------------------------------------- worker thread
// Writes the readout (so no file I/O ever happens on the game's present thread) and,
// on the fallback path, waits for presents with vkWaitForPresentKHR.
static void *worker_thread(void *arg)
{
    ldev_t *d = arg;
    pthread_mutex_lock(&d->mtx);
    for (;;) {
        while (!d->jobs && !d->quit) {
            if (!d->gc) {
                pthread_cond_wait(&d->cv, &d->mtx);
                continue;
            }
            // Free our old fences/semaphores once the present that waited on them is long done.
            uint64_t now = now_ns();
            gc_t *expired = NULL;
            for (gc_t **pp = &d->gc; *pp;) {
                if ((*pp)->free_at <= now) {
                    gc_t *g = *pp;
                    *pp = g->next;
                    g->next = expired;
                    expired = g;
                } else {
                    pp = &(*pp)->next;
                }
            }
            pthread_mutex_unlock(&d->mtx);
            while (expired) {
                gc_t *g = expired;
                expired = g->next;
                d->destroy_fence(d->dev, g->fence, NULL);
                d->destroy_sem(d->dev, g->sem, NULL);
                free(g);
            }
            pthread_mutex_lock(&d->mtx);
            bool timed = false;
            for (gc_t *g = d->gc; g; g = g->next)
                timed |= g->free_at != UINT64_MAX;
            if (!d->jobs && !d->quit && timed) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;
                pthread_cond_timedwait(&d->cv, &d->mtx, &ts);
            } else if (!d->jobs && !d->quit) {
                pthread_cond_wait(&d->cv, &d->mtx); // only never-signalled leftovers: freed with the device
            }
        }
        if (d->quit)
            break;
        job_t *j = d->jobs;
        d->jobs = j->next;
        if (!d->jobs)
            d->jobs_tail = NULL;
        if (j->fence) {
            pthread_mutex_unlock(&d->mtx);
            VkResult r = VK_TIMEOUT;
            for (uint64_t waited = 0; r == VK_TIMEOUT && waited < WAIT_MAX_NS && !d->quit; waited += WAIT_SLICE_NS)
                r = d->wait_fences(d->dev, 1, &j->fence, VK_TRUE, WAIT_SLICE_NS);
            uint64_t done = now_ns();
            if (r == VK_SUCCESS && j->n)
                record(j->tins, j->n, done, "xwayland-gpu");
            gc_t *g = malloc(sizeof *g);
            pthread_mutex_lock(&d->mtx);
            if (g) {
                // A fence that never signalled may still be in use: keep it until the device dies.
                *g = (gc_t){ j->fence, j->sem, r == VK_SUCCESS ? done + 1000000000ull : UINT64_MAX, d->gc };
                d->gc = g;
            }
            free(j);
            continue;
        }
        if (!j->sc) {
            pthread_mutex_unlock(&d->mtx);
            record(j->tins, j->n, j->shown, j->src);
            free(j);
            pthread_mutex_lock(&d->mtx);
            continue;
        }
        sc_t *s = j->sc;
        s->busy++;
        VkResult r = VK_TIMEOUT;
        for (uint64_t waited = 0; !s->dead && !d->quit && r == VK_TIMEOUT && waited < WAIT_MAX_NS; waited += WAIT_SLICE_NS) {
            pthread_mutex_unlock(&d->mtx);
            if (s->mode == SC_V2) {
                VkPresentWait2InfoKHR wi = { VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, NULL, j->id, WAIT_SLICE_NS };
                r = d->wait2(d->dev, s->h, &wi);
            } else {
                r = d->wait1(d->dev, s->h, j->id, WAIT_SLICE_NS);
            }
            pthread_mutex_lock(&d->mtx);
        }
        uint64_t shown = now_ns();
        if (--s->busy == 0)
            pthread_cond_broadcast(&d->idle_cv);
        pthread_mutex_unlock(&d->mtx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            record(j->tins, j->n, shown, "vsync-wait");
        } else if (r != VK_TIMEOUT && r != VK_ERROR_OUT_OF_DATE_KHR) {
            if (atomic_fetch_add(&d->errors, 1) == 20) {
                atomic_store(&d->active, false); // this WSI can't do it; go passive
                DBG("present wait keeps failing (%d); turning off\n", r);
            }
        }
        free(j);
        pthread_mutex_lock(&d->mtx);
    }
    pthread_mutex_unlock(&d->mtx);
    return NULL;
}

// ---------------------------------------------------------------- hooks
// XWayland without vsync reports no display time, so for the frame right after a press we
// put one empty submission in front of the present: it waits for the game's "rendering
// done" semaphores and signals our fence + a semaphore the present waits on instead. No
// GPU work; the worker wakes when the GPU finished the frame. With Hyprland tearing (the
// games-tear rule) the frame is flipped right after that.
static bool gpu_fence_hook(ldev_t *d, VkQueue queue, const VkPresentInfoKHR *pi, VkPresentInfoKHR *mine,
                           VkSemaphore *sem, VkFence *fence)
{
    if (pi->waitSemaphoreCount == 0 || pi->waitSemaphoreCount > 16)
        return false;
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->create_sem(d->dev, &sci, NULL, sem) != VK_SUCCESS)
        return false;
    if (d->create_fence(d->dev, &fci, NULL, fence) != VK_SUCCESS) {
        d->destroy_sem(d->dev, *sem, NULL);
        return false;
    }
    VkPipelineStageFlags stages[16];
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++)
        stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = pi->waitSemaphoreCount, .pWaitSemaphores = pi->pWaitSemaphores,
                        .pWaitDstStageMask = stages, .signalSemaphoreCount = 1, .pSignalSemaphores = sem };
    if (d->queue_submit(queue, 1, &si, *fence) != VK_SUCCESS) {
        // Nothing was queued, so the game's semaphores are untouched: present as asked.
        d->destroy_fence(d->dev, *fence, NULL);
        d->destroy_sem(d->dev, *sem, NULL);
        return false;
    }
    mine->waitSemaphoreCount = 1;
    mine->pWaitSemaphores = sem;
    return true;
}

static VKAPI_ATTR VkResult VKAPI_CALL el_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
    ldev_t *d = find_dev(KEY(queue));
    uint64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
    bool pressed = head != atomic_load_explicit(&g_tail, memory_order_relaxed);
    // Fast path (almost every frame): nothing pressed, nothing waiting for a report.
    if (!atomic_load_explicit(&d->active, memory_order_relaxed) ||
        (!pressed && !atomic_load_explicit(&d->hot, memory_order_relaxed)) ||
        pi->swapchainCount == 0 || pi->swapchainCount > 8) {
        VkResult r = d->present(queue, pi);
        atomic_store_explicit(&d->last_ret, now_ns(), memory_order_relaxed);
        return r;
    }
    uint64_t frame_start = atomic_load_explicit(&d->last_ret, memory_order_relaxed);
    uint64_t t0 = now_ns();

    // Presses that happened before this frame started belong to this frame.
    job_t j = { .n = 0 };
    if (pressed && frame_start) {
        pthread_mutex_lock(&g_tail_mtx);
        uint64_t tail = atomic_load_explicit(&g_tail, memory_order_relaxed);
        while (tail != head && g_ring[tail % RING] <= frame_start) {
            if (j.n < MAX_TINS)
                j.tins[j.n++] = g_ring[tail % RING];
            tail++;
        }
        atomic_store_explicit(&g_tail, tail, memory_order_relaxed);
        pthread_mutex_unlock(&g_tail_mtx);
    }

    const VkPresentIdKHR *app_id1 = NULL;
    const VkPresentId2KHR *app_id2 = NULL;
    const VkSwapchainPresentModeInfoKHR *app_mode = NULL;
    bool app_timing = false;
    for (const VkBaseInStructure *p = pi->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR)
            app_id1 = (const VkPresentIdKHR *)p;
        else if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR)
            app_id2 = (const VkPresentId2KHR *)p;
        else if (p->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR)
            app_mode = (const VkSwapchainPresentModeInfoKHR *)p;
        else if (p->sType == VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT)
            app_timing = true;
    }

    const void *chain = pi->pNext;
    uint64_t ids[8] = {0};
    VkPresentIdKHR our_id;
    VkPresentTimingInfoEXT tinfo;
    VkPresentTimingsInfoEXT tinfos;
    sc_t *wait_sc = NULL;
    bool gpu_path = false;

    pthread_mutex_lock(&d->mtx);
    sc_t *s = find_sc(d, pi->pSwapchains[0]);
    if (s && !s->dead) {
        if (app_id1 || app_id2)
            s->app_ids = true;
        if (s->timing && s->outstanding)
            drain_timing(d, s, t0);
        expire_pend(s, t0);
        VkPresentModeKHR m = app_mode && app_mode->pPresentModes ? app_mode->pPresentModes[0] : s->create_mode;
        bool fifo = m == VK_PRESENT_MODE_FIFO_KHR || m == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        bool use_timing = s->timing && pi->swapchainCount == 1 && !app_timing;
        if (j.n && s->x11 && !fifo && pi->swapchainCount == 1 && d->queue_submit) {
            gpu_path = true; // see gpu_fence_hook
        } else if (use_timing) {
            // Ask for this frame's scanout time if it carries new presses, or while an
            // earlier press's frame may have been dropped and we're following it.
            // The queue limit is a hard guarantee: a full queue would fail the game's present.
            if ((j.n || s->npend) && s->outstanding + 1 < TIMING_QUEUE) {
                tinfo = (VkPresentTimingInfoEXT){ .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
                                                  .timeDomainId = s->domain_id,
                                                  .presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT |
                                                                         VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT };
                tinfos = (VkPresentTimingsInfoEXT){ .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
                                                    .pNext = chain, .swapchainCount = 1, .pTimingInfos = &tinfo };
                chain = &tinfos;
                s->outstanding++;
                uint64_t seq = ++s->req_issued;
                if (j.n && s->npend < MAX_PEND) {
                    pend_t *pe = &s->pend[s->npend++];
                    pe->seq = seq;
                    pe->presented = t0;
                    pe->fifo = fifo;
                    pe->n = j.n;
                    memcpy(pe->tins, j.tins, sizeof pe->tins);
                }
            }
        }
        // Fallback without scanout times: when a present wait returns is only meaningful
        // with vsync (FIFO). In mailbox/immediate the compositor's dropped frames count as
        // "done" too, so the number would be wrong; skip those.
        bool can_wait = (s->mode == SC_V1 && d->wait1) || (s->mode == SC_V2 && d->wait2);
        if (j.n && !use_timing && !gpu_path && can_wait && fifo) {
            if (s->mode == SC_V1 && app_id1 && app_id1->pPresentIds)
                j.id = app_id1->pPresentIds[0];
            else if (s->mode == SC_V2 && app_id2 && app_id2->pPresentIds)
                j.id = app_id2->pPresentIds[0];
            else if (s->mode == SC_V1 && !s->app_ids) {
                j.id = ids[0] = ++s->next_id;
                our_id = (VkPresentIdKHR){ VK_STRUCTURE_TYPE_PRESENT_ID_KHR, chain, pi->swapchainCount, ids };
                chain = &our_id;
            }
            if (j.id)
                wait_sc = s;
        }
        update_hot(d, s);
    }
    pthread_mutex_unlock(&d->mtx);

    VkPresentInfoKHR mine = *pi;
    mine.pNext = chain;
    VkSemaphore gsem = VK_NULL_HANDLE;
    VkFence gfence = VK_NULL_HANDLE;
    if (gpu_path)
        gpu_path = gpu_fence_hook(d, queue, pi, &mine, &gsem, &gfence);
    VkResult r = d->present(queue, (chain == pi->pNext && !gpu_path) ? pi : &mine);
    atomic_store_explicit(&d->last_ret, now_ns(), memory_order_relaxed);

    if (gpu_path) {
        job_t gj = j;
        gj.fence = gfence;
        gj.sem = gsem;
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            gj.n = 0; // not shown: just clean up
        pthread_mutex_lock(&d->mtx);
        enqueue(d, &gj);
        pthread_mutex_unlock(&d->mtx);
    }

    if (wait_sc && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
        pthread_mutex_lock(&d->mtx);
        if (!wait_sc->dead) {
            j.sc = wait_sc;
            enqueue(d, &j);
        }
        pthread_mutex_unlock(&d->mtx);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL el_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    ldev_t *d = find_dev(KEY(device));
    if (ci->oldSwapchain != VK_NULL_HANDLE) { // the old one is retired by this call
        pthread_mutex_lock(&d->mtx);
        sc_t *old = find_sc(d, ci->oldSwapchain);
        if (old)
            retire_sc(d, old);
        pthread_mutex_unlock(&d->mtx);
    }
    // Ask for scanout timing on the swapchain, unless the app uses present timing itself
    // (then the reports are its own and we must not consume them).
    bool timing = d->timing_past && !(ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) &&
                  !getenv("EMBER_LATENCY_NO_TIMING");
    VkSwapchainCreateInfoKHR mod = *ci;
    mod.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
    VkResult r = d->create_sc(device, timing ? &mod : ci, alloc, out);
    if (timing && r != VK_SUCCESS) {
        // Be invisible: retry exactly as asked (the old swapchain is already retired).
        DBG("swapchain with present timing failed (%d), retrying untouched\n", r);
        timing = false;
        mod = *ci;
        mod.oldSwapchain = VK_NULL_HANDLE;
        r = d->create_sc(device, &mod, alloc, out);
    }
    if (r != VK_SUCCESS)
        return r;
    sc_t *s = calloc(1, sizeof *s);
    if (!s)
        return r;
    s->h = *out;
    s->create_mode = ci->presentMode;
    const VkSwapchainCreateFlagsKHR v2 = VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    if ((ci->flags & v2) == v2 && d->wait2)
        s->mode = SC_V2;
    else if (!(ci->flags & VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR))
        s->mode = SC_V1;
    else
        s->mode = SC_NONE;
    if (timing && d->timing_qsize(device, *out, TIMING_QUEUE) == VK_SUCCESS) {
        VkTimeDomainKHR dom = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
        uint64_t dom_id = 0;
        VkSwapchainTimeDomainPropertiesEXT p = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
                                                 .timeDomainCount = 1, .pTimeDomains = &dom, .pTimeDomainIds = &dom_id };
        if (d->timing_domains(device, *out, &p, NULL) >= 0) {
            s->timing = true;
            s->domain_id = dom_id;
        }
    }
    pthread_mutex_lock(&d->mtx);
    s->x11 = d->x11;
    s->next = d->scs;
    d->scs = s;
    pthread_mutex_unlock(&d->mtx);
    DBG("swapchain %p mode %d timing %d present_mode %d\n", (void *)s->h, s->mode, s->timing, ci->presentMode);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL el_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc)
{
    ldev_t *d = find_dev(KEY(device));
    pthread_mutex_lock(&d->mtx);
    for (sc_t **pp = &d->scs; *pp; pp = &(*pp)->next) {
        if ((*pp)->h == sc) {
            sc_t *s = *pp;
            retire_sc(d, s);
            *pp = s->next;
            free(s);
            break;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    d->destroy_sc(device, sc, alloc);
}

static bool has_ext(const char *const *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (strcmp(list[i], name) == 0)
            return true;
    return false;
}

// One device feature we switch on: reuse the app's struct if it has one (duplicates in
// a pNext chain are invalid), else put ours in front of the chain.
typedef struct {
    VkStructureType stype;
    size_t offset;    // of the VkBool32 we need
    void *ours;       // our struct (sType/pNext/... zeroed + flag set)
    VkBool32 *app;    // the app's flag, if it chained the struct itself
    VkBool32 app_old;
} feat_t;

static const void *enable_features(feat_t *f, int nf, const void *chain)
{
    for (int i = 0; i < nf; i++) {
        for (VkBaseOutStructure *p = (VkBaseOutStructure *)chain; p; p = p->pNext)
            if (p->sType == f[i].stype)
                f[i].app = (VkBool32 *)((char *)p + f[i].offset);
    }
    for (int i = 0; i < nf; i++) {
        if (f[i].app) {
            f[i].app_old = *f[i].app;
            *f[i].app = VK_TRUE;
        } else {
            ((VkBaseOutStructure *)f[i].ours)->pNext = (VkBaseOutStructure *)chain;
            chain = f[i].ours;
        }
    }
    return chain;
}

static VKAPI_ATTR VkResult VKAPI_CALL el_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    inst_t *in = find_inst(KEY(pd));
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(in ? in->inst : VK_NULL_HANDLE, "vkCreateDevice");
    if (!next_create)
        return VK_ERROR_INITIALIZATION_FAILED;

    // What can this GPU do? We need present ids; scanout timing is the good path,
    // present wait the fallback.
    bool has_id = false, has_wait = false, has_timing = false;
    if (in && in->enum_ext && in->feat2 && !getenv("DISABLE_EMBER_LATENCY") &&
        has_ext(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        uint32_t n = 0;
        in->enum_ext(pd, NULL, &n, NULL);
        VkExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
        bool e_id = false, e_wait = false, e_timing = false;
        if (props && in->enum_ext(pd, NULL, &n, props) >= 0)
            for (uint32_t i = 0; i < n; i++) {
                e_id |= !strcmp(props[i].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME);
                e_wait |= !strcmp(props[i].extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                e_timing |= !strcmp(props[i].extensionName, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
            }
        free(props);
        VkPhysicalDevicePresentTimingFeaturesEXT ft = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
        VkPhysicalDevicePresentWaitFeaturesKHR fw = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                      .pNext = e_timing ? &ft : NULL };
        VkPhysicalDevicePresentIdFeaturesKHR fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .pNext = &fw };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &fi };
        if (e_id)
            in->feat2(pd, &f2);
        has_id = e_id && fi.presentId;
        has_wait = has_id && e_wait && fw.presentWait;
        has_timing = has_id && e_timing && ft.presentTiming;
    }
    bool want = has_id && (has_wait || has_timing);

    VkDeviceCreateInfo mod = *ci;
    const char **exts = NULL;
    VkPhysicalDevicePresentIdFeaturesKHR add_id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .presentId = VK_TRUE };
    VkPhysicalDevicePresentWaitFeaturesKHR add_wait = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, .presentWait = VK_TRUE };
    VkPhysicalDevicePresentTimingFeaturesEXT add_timing = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, .presentTiming = VK_TRUE };
    feat_t feats[3];
    int nf = 0;
    if (want) {
        exts = malloc((ci->enabledExtensionCount + 3) * sizeof *exts);
        if (!exts) {
            want = false;
        } else {
            uint32_t n = ci->enabledExtensionCount;
            memcpy(exts, ci->ppEnabledExtensionNames, n * sizeof *exts);
            if (!has_ext(exts, n, VK_KHR_PRESENT_ID_EXTENSION_NAME))
                exts[n++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            feats[nf++] = (feat_t){ .stype = add_id.sType, .offset = offsetof(VkPhysicalDevicePresentIdFeaturesKHR, presentId), .ours = &add_id };
            if (has_wait) {
                if (!has_ext(exts, n, VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
                    exts[n++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
                feats[nf++] = (feat_t){ .stype = add_wait.sType, .offset = offsetof(VkPhysicalDevicePresentWaitFeaturesKHR, presentWait), .ours = &add_wait };
            }
            if (has_timing) {
                if (!has_ext(exts, n, VK_EXT_PRESENT_TIMING_EXTENSION_NAME))
                    exts[n++] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
                feats[nf++] = (feat_t){ .stype = add_timing.sType, .offset = offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming), .ours = &add_timing };
            }
            mod.enabledExtensionCount = n;
            mod.ppEnabledExtensionNames = exts;
            mod.pNext = enable_features(feats, nf, ci->pNext);
        }
    }

    VkResult r = next_create(pd, want ? &mod : ci, alloc, out);
    for (int i = 0; i < nf; i++)
        if (feats[i].app)
            *feats[i].app = feats[i].app_old;
    free(exts);
    if (want && r != VK_SUCCESS) { // be invisible if the driver didn't like it
        DBG("device creation with present extensions failed (%d), retrying untouched\n", r);
        want = false;
        r = next_create(pd, ci, alloc, out);
    }
    if (r != VK_SUCCESS)
        return r;

    ldev_t *d = calloc(1, sizeof *d);
    if (!d) {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
        destroy(*out, alloc);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    d->key = KEY(*out);
    d->dev = *out;
    d->gdpa = gdpa;
#define GET(field, name) d->field = (void *)gdpa(*out, name)
    GET(destroy, "vkDestroyDevice");
    GET(present, "vkQueuePresentKHR");
    GET(create_sc, "vkCreateSwapchainKHR");
    GET(destroy_sc, "vkDestroySwapchainKHR");
    if (want && has_wait)
        GET(wait1, "vkWaitForPresentKHR");
    if (want && has_ext(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME))
        GET(wait2, "vkWaitForPresent2KHR");
    if (want) {
        GET(queue_submit, "vkQueueSubmit");
        GET(create_fence, "vkCreateFence");
        GET(destroy_fence, "vkDestroyFence");
        GET(wait_fences, "vkWaitForFences");
        GET(create_sem, "vkCreateSemaphore");
        GET(destroy_sem, "vkDestroySemaphore");
        if (!d->create_fence || !d->destroy_fence || !d->wait_fences || !d->create_sem || !d->destroy_sem)
            d->queue_submit = NULL;
    }
    if (want && has_timing) {
        GET(timing_qsize, "vkSetSwapchainPresentTimingQueueSizeEXT");
        GET(timing_domains, "vkGetSwapchainTimeDomainPropertiesEXT");
        GET(timing_past, "vkGetPastPresentationTimingEXT");
        if (!d->timing_qsize || !d->timing_domains || !d->timing_past)
            d->timing_past = NULL;
    }
#undef GET
    bool active = want && (d->wait1 || d->timing_past) && d->present && d->create_sc && d->destroy_sc;
    pthread_mutex_init(&d->mtx, NULL);
    pthread_cond_init(&d->cv, NULL);
    pthread_cond_init(&d->idle_cv, NULL);
    if (active && pthread_create(&d->thread, NULL, worker_thread, d) == 0) {
        pthread_setname_np(d->thread, "ember-lat-work");
        d->has_thread = true;
        pthread_once(&g_input_once, start_input_thread);
    } else {
        active = false;
    }
    atomic_store(&d->active, active);
    DBG("device %p active=%d wait=%d timing=%d\n", (void *)*out, active, d->wait1 != NULL, d->timing_past != NULL);
    pthread_mutex_lock(&g_map_mtx);
    d->next = g_devs;
    g_devs = d;
    pthread_mutex_unlock(&g_map_mtx);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL el_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
    if (!device)
        return;
    pthread_mutex_lock(&g_map_mtx);
    ldev_t *d = NULL;
    for (ldev_t **pp = &g_devs; *pp; pp = &(*pp)->next)
        if ((*pp)->key == KEY(device)) {
            d = *pp;
            *pp = d->next;
            break;
        }
    pthread_mutex_unlock(&g_map_mtx);
    if (!d)
        return;
    if (d->has_thread) {
        pthread_mutex_lock(&d->mtx);
        d->quit = true;
        pthread_cond_broadcast(&d->cv);
        pthread_mutex_unlock(&d->mtx);
        pthread_join(d->thread, NULL);
    }
    // The app made the device idle before destroying it, so our objects are free now.
    while (d->jobs) {
        job_t *j = d->jobs;
        d->jobs = j->next;
        if (j->fence) {
            d->destroy_fence(d->dev, j->fence, NULL);
            d->destroy_sem(d->dev, j->sem, NULL);
        }
        free(j);
    }
    while (d->gc) {
        gc_t *g = d->gc;
        d->gc = g->next;
        d->destroy_fence(d->dev, g->fence, NULL);
        d->destroy_sem(d->dev, g->sem, NULL);
        free(g);
    }
    while (d->scs) {
        sc_t *s = d->scs;
        d->scs = s->next;
        free(s);
    }
    PFN_vkDestroyDevice destroy = d->destroy;
    bool was_active = atomic_load(&d->active);
    // Not freed (see find_dev): just make sure no lookup can match it again.
    atomic_store(&d->active, false);
    d->key = NULL;
    if (was_active && g_out_path[0])
        unlink(g_out_path);
    destroy(device, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL el_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                         VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateInstance next = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = next(ci, alloc, out);
    if (r != VK_SUCCESS)
        return r;
    g_debug = getenv("EMBER_LATENCY_DEBUG") ? atoi(getenv("EMBER_LATENCY_DEBUG")) : 0;
    inst_t *i = calloc(1, sizeof *i);
    if (!i)
        return r; // vkCreateDevice then runs untouched (no instance data = no checks)
    i->key = KEY(*out);
    i->inst = *out;
    i->gipa = gipa;
    i->destroy = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    i->enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
    i->feat2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2");
    if (!i->feat2)
        i->feat2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
    pthread_mutex_lock(&g_map_mtx);
    i->next = g_insts;
    g_insts = i;
    pthread_mutex_unlock(&g_map_mtx);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL el_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
    if (!instance)
        return;
    pthread_mutex_lock(&g_map_mtx);
    inst_t *i = NULL;
    for (inst_t **pp = &g_insts; *pp; pp = &(*pp)->next)
        if ((*pp)->key == KEY(instance)) {
            i = *pp;
            *pp = i->next;
            break;
        }
    pthread_mutex_unlock(&g_map_mtx);
    if (!i)
        return;
    PFN_vkDestroyInstance destroy = i->destroy;
    free(i);
    destroy(instance, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL el_GetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction device_hook(const char *name, bool active)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)el_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)el_DestroyDevice;
    if (!active)
        return NULL;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)el_QueuePresentKHR;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)el_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)el_DestroySwapchainKHR;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL el_GetDeviceProcAddr(VkDevice device, const char *name)
{
    ldev_t *d = find_dev(KEY(device));
    if (!d)
        return NULL;
    // An inactive device gets the next layer's functions directly: zero overhead.
    PFN_vkVoidFunction f = device_hook(name, atomic_load(&d->active));
    return f ? f : d->gdpa(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL el_GetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)el_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)el_CreateInstance;
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)el_DestroyInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)el_CreateDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)el_GetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)el_DestroyDevice;
    if (!instance)
        return NULL;
    inst_t *i = find_inst(KEY(instance));
    return i ? i->gipa(instance, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
    if (!v || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = el_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = el_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
