/*
 * bridge_core.c  —  aarch64 stub side
 *
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>

#include <stdbool.h>

#ifdef DEBUG
#include <GLES3/gl32.h>
#include <EGL/egl.h>
#endif

#include "../deps/cJSON.h"

#define LOG_PREFIX "[bridge_core]"
#include "../bridge/bridge_shm.h"
#include "../bridge/shared_util.h"
#include "bridge_core.h"
#include "gles_bridge_protocol.h"

#ifndef GLES_PROXY_DEFAULT_PATH
#define GLES_PROXY_DEFAULT_PATH "/media/developer/temp/gles_proxy"
#endif

#define BP_SLEEP_DURATION 5000 /* 5us */

static BridgeShm g_ctrl_shm;
static BridgeShm g_data_shm;
static BridgeShm g_ctrl_wl_shm;
static BridgeShm g_data_wl_shm;

/* ── Internal state ──────────────────────────────────────────────────────── */
static uint8_t *g_data = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_proxy_pid = -1;

static int req_efd = -1;
static int resp_efd = -1;

static int req_wl_efd = -1;
static int resp_wl_efd = -1;
static WLBridge g_wl;

/* ── libEGL needs to notify libGLES of a change in context ─────────── */
__thread unsigned int g_stub_current_ctx;
unsigned int g_stub_new_ctx = 0;

/* ── Ring state (stub/producer side) ─────────────────────────────────── */
static BridgeRing *g_ring = NULL;
static uint32_t g_ring_slots = 0;
static uint32_t g_ring_mask = 0;
/* Per thread: bridge_send_call() drops g_lock before the caller reads its
 * results through BRIDGE_CTRL(), and another thread's bridge_begin() must not
 * repoint the slot it reads them from. */
static __thread BridgeCtrl *g_cur_slot = NULL;
static __thread uint64_t g_cur_seq = 0;

/* Process-local — there is only ever one producer (serialized by g_lock),
 * so these don't need to live in shared memory or be atomic. */
static uint64_t g_local_seq = 0;
static uint64_t g_local_data_head = 0;
static pid_t g_client_pid = 0;

#ifdef DEBUG_OPCODES
static uint64_t g_ring_full_stalls = 0;
static uint64_t g_data_full_stalls = 0;

void bridge_dump_backpressure_stats(void)
{
  log_always("=== bridge backpressure: ring_full_stalls=%llu "
             "data_full_stalls=%llu ===",
             (unsigned long long)g_ring_full_stalls,
             (unsigned long long)g_data_full_stalls);
}
#endif

static void print_environment_version(void)
{
  static char pretty[128];

  FILE *f = fopen("/var/run/nyx/os_info.json", "r");

  if (f)
  {
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root)
      return;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "webos_name");
    const cJSON *release =
        cJSON_GetObjectItemCaseSensitive(root, "webos_release");

    const char *name_str =
        (cJSON_IsString(name) && name->valuestring) ? name->valuestring : NULL;
    const char *release_str = (cJSON_IsString(release) && release->valuestring)
                                  ? release->valuestring
                                  : NULL;

    if (name_str && release_str)
      snprintf(pretty, sizeof(pretty), "%s %s", name_str, release_str);
    else if (name_str)
      snprintf(pretty, sizeof(pretty), "%s", name_str);
    else
      snprintf(pretty, sizeof(pretty), "webOS (unknown)");

    cJSON_Delete(root);
  }
  else
  {
    snprintf(pretty, sizeof(pretty), "(unknown)");
  }

  struct utsname u;
  uname(&u);

  const char *kernel = u.release;
  const char *arch = u.machine;

  log_always("Running on: %s (kernel=%s, arch=%s)", pretty, kernel, arch);

#ifdef DEBUG
  // log_always("GL_VERSION: %s", glGetString(GL_VERSION));
  // log_always("GL_RENDERER: %s", glGetString(GL_RENDERER));
#endif
}

/* ── eventfd helpers ────────────────────────────────────────────────────── */
static void efd_post(int fd)
{
  uint64_t one = 1;
  if (write(fd, &one, sizeof(one)) != sizeof(one))
  {
    log_console("efd_post(%d) FAILED: %s", fd, strerror(errno));
    abort();
  }
}

static void efd_wait(int fd)
{
  uint64_t val;
  ssize_t n;
  do
  {
    n = read(fd, &val, sizeof(val));
  } while (n < 0 && errno == EINTR);
  if (n != (ssize_t)sizeof(val))
  {
    log_console("efd_wait(%d) FAILED: %s", fd, strerror(errno));
    abort();
  }
}

/* ── Cleanup — called via atexit ────────────────────────────────────────── */
static void bridge_cleanup(void)
{
  /* Kill the proxy so it doesn't linger after client exits */
  if (g_proxy_pid > 0)
  {
    kill(g_proxy_pid, SIGTERM);
    /* Brief wait so the proxy can flush its logs */
    int status;
    waitpid(g_proxy_pid, &status, 0);
    g_proxy_pid = -1;
  }

  /* Remove shared memory so the next run always starts clean */
  shm_destroy(GLES_BRIDGE_CTRL_SHM, &g_ctrl_shm);
  shm_destroy(GLES_BRIDGE_DATA_SHM, &g_data_shm);

  shm_destroy(GLES_BRIDGE_WL_CTRL_SHM, &g_ctrl_wl_shm);
  shm_destroy(GLES_BRIDGE_WL_DATA_SHM, &g_data_wl_shm);

  log_console("cleanup done");
}

/* ── SIGCHLD handler ─────────────────────────────────────────────────────── */
static void proxy_sigchld(int sig)
{
  (void)sig;
  int status;
  pid_t pid = waitpid(-1, &status, WNOHANG);
  if (pid == g_proxy_pid)
  {
    log_console("proxy (pid %d) exited with status %d", (int)pid,
                WEXITSTATUS(status));
    g_proxy_pid = -1;
  }
}

/* ── Start the armv7a proxy ─────────────────────────────────────────────── */
static int start_proxy(void)
{
  log_console("start_proxy()");

  const char *bin = getenv("GLES_PROXY_BIN");
  if (!bin)
    bin = GLES_PROXY_DEFAULT_PATH;

  signal(SIGCHLD, proxy_sigchld);

  char req_str[32], resp_str[32];
  snprintf(req_str, sizeof(req_str), "%d", req_efd);
  snprintf(resp_str, sizeof(resp_str), "%d", resp_efd);

  char req_wl_str[32], resp_wl_str[32];
  snprintf(req_wl_str, sizeof(req_wl_str), "%d", req_wl_efd);
  snprintf(resp_wl_str, sizeof(resp_wl_str), "%d", resp_wl_efd);

  const char *appId = getenv("APPID") ? getenv("APPID") : "";

  /* Dummy placeholders kept so argv indices match proxy's main() */
  pid_t pid = fork();
  if (pid < 0)
  {
    log_console("fork: %s", strerror(errno));
    return -1;
  }

  if (pid == 0)
  {
    /* Child — will become the proxy process */
    execl(bin, bin, req_str, resp_str, req_wl_str, resp_wl_str, appId, NULL);
    log_console("execl(%s) FAILED: %s", bin, strerror(errno));
    _exit(1);
  }

  g_proxy_pid = pid;
  g_ring->proxy_pid = (int32_t)pid;
  log_console("proxy started, pid=%d", (int)pid);
  return 0;
}

/* ── Initialisation (lazy, called on first GL/EGL stub call) ───────────── */
static void bridge_init(void)
{
  // reset log file
  delete_log_file();

  log_console("initialising bridge on first GL/EGL stub call");

  print_environment_version();

  g_ring_slots = bridge_config_ring_slots();
  g_ring_mask = bridge_config_ring_mask();

  // dump opcodes for comparison
  /*for (int i = 0; i < OP_MAX; i++) {
    log_console("%d -> %s", i, opcode_to_string(i));
  }*/

  g_ctrl_shm = shm_create(GLES_BRIDGE_CTRL_SHM, bridge_ctrl_shm_size());

  g_data_shm = shm_create(GLES_BRIDGE_DATA_SHM, GLES_BRIDGE_DATA_SIZE);

  g_ring = (BridgeRing *)g_ctrl_shm.ptr;
  g_data = (uint8_t *)g_data_shm.ptr;

  if (!g_ring)
  {
    log_console("g_ring is NULL!!");
    exit(1);
  }

  if (!g_data)
  {
    log_console("g_data is NULL!!");
    exit(1);
  }

#ifdef DEBUG
  log_console("g_ring=%p g_data=%p ctrl_shm.shmid=%d "
              "data_shm.shmid=%d",
              g_ring, g_data, g_ctrl_shm.shmid, g_data_shm.shmid);

  log_console("sizeof(BridgeCtrl)=%zu "
              "client_pid offset=%zu "
              "ctrlsize=%u",
              sizeof(BridgeRing), offsetof(BridgeCtrl, client_pid),
              bridge_ctrl_shm_size());
#endif

  /* Plain (non-semaphore) eventfds — one write per command, one read per
   * command */
  req_efd = eventfd(0, 0);
  resp_efd = eventfd(0, 0);
  if (req_efd < 0 || resp_efd < 0)
  {
    log_console("eventfd: %s", strerror(errno));
    abort();
  }
  log_console("req_efd=%d resp_efd=%d", req_efd, resp_efd);

  req_wl_efd = eventfd(0, 0);
  resp_wl_efd = eventfd(0, 0);
  if (req_wl_efd < 0 || resp_wl_efd < 0)
  {
    log_console("wl eventfd: %s", strerror(errno));
    abort();
  }
  log_console("req_wl_efd=%d resp_wl_efd=%d", req_wl_efd, resp_wl_efd);

  wl_bridge_init();

  log_console("bridge_init() - setting up bridge_cleanup");

  /* Register cleanup so SHM and proxy are torn down on normal exit */
  if (atexit(bridge_cleanup) != 0)
    log_console("atexit returned a non zero value");

#ifdef DEBUG_VERBOSE
  log_console("before getpid");
#endif

  pid_t p = getpid();

#ifdef DEBUG_VERBOSE
  log_console("after getpid pid=%d", p);
  log_console("before write");
#endif

  g_client_pid = p;

#ifdef DEBUG_VERBOSE
  log_console("after write");
#endif

  log_console("bridge_init() - storing client pid: %d", g_client_pid);

  if (start_proxy() < 0)
    abort();
  /* Give proxy time to map the SHM and enter its dispatch loop */
  usleep(150000);

  log_console("ready");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

/* Async swap: seq of the last eglSwapBuffers slot that may not have been
 * presented yet. Only touched under g_lock. */
static uint64_t g_pending_swap_seq = 0;
static int g_have_pending_swap = 0;

#ifdef DEBUG_FRAMETIME
/* Per-frame counters, reset at every swap */
static uint64_t g_ft_last_swap_ns = 0;
static uint32_t g_ft_calls = 0;
static uint32_t g_ft_sync_calls = 0;
static uint64_t g_ft_sync_ns = 0;
/* time spent waiting for free ring slots / data arena space */
static uint64_t g_ft_stall_ns = 0;
/* stub-side cost of each call, BRIDGE_BEGIN -> publish (excludes stalls
 * and waiting for replies) */
static uint64_t g_ft_begin_ns = 0;
static uint64_t g_ft_stub_ns = 0;
static uint64_t g_ft_stub_op_ns[OP_MAX];
static uint32_t g_ft_stub_op_cnt[OP_MAX];
#define FT_STUB_PROFILE_NS (5u * 1000000u)
#define FT_PROFILE_TOP 10
/* WL calls run under g_wl.lock, not g_lock, so these are atomic */
static _Atomic uint32_t g_ft_wl_calls = 0;
static _Atomic uint64_t g_ft_wl_ns = 0;
static uint64_t g_ft_frames = 0;
static uint64_t g_ft_win_start_ns = 0;
static uint64_t g_ft_win_min_ns = UINT64_MAX;
static uint64_t g_ft_win_max_ns = 0;
#define FT_WINDOW_FRAMES 60
#endif

/* Wait until the proxy has completed `seq`.  The slot must have been sent
 * with needs_response != 0 so the proxy posts resp_efd once it is done.
 * Spins for spin_us first: an eventfd sleep/wake costs tens of us on the
 * TV, which is often longer than the proxy needs.  Caller holds g_lock. */
static void wait_completed(uint64_t seq)
{
  if (atomic_load_explicit(&g_ring->completed_seq, memory_order_acquire) > seq)
    return;

  uint32_t spin_us = bridge_config_spin_us();
  if (spin_us)
  {
    uint64_t deadline = bridge_now_ns() + (uint64_t)spin_us * 1000u;
    do
    {
      for (int i = 0; i < 64; i++)
      {
        if (atomic_load_explicit(&g_ring->completed_seq,
                                 memory_order_acquire) > seq)
          return;
        bridge_cpu_relax();
      }
    } while (bridge_now_ns() < deadline);
  }

  /* A post left over from an earlier spin-completed call can wake us
   * early, so re-check after every read. */
  for (;;)
  {
    if (atomic_load_explicit(&g_ring->completed_seq, memory_order_acquire) >
        seq)
      break;
    efd_wait(resp_efd);
  }
}

/* ── Slot reservation ─────────────────────────────────────────────────── */
static BridgeCtrl *ring_reserve_slot(uint64_t *out_seq)
{
  uint64_t seq = g_local_seq;

  for (;;)
  {
    uint64_t completed =
        atomic_load_explicit(&g_ring->completed_seq, memory_order_acquire);

    if (seq - completed < g_ring_slots)
      break;

    /* Ring full */
#ifdef DEBUG_OPCODES
    g_ring_full_stalls++;
#endif
#ifdef DEBUG_FRAMETIME
    uint64_t t0 = bridge_now_ns();
#endif
    struct timespec ts = {0, BP_SLEEP_DURATION};
    nanosleep(&ts, NULL);
#ifdef DEBUG_FRAMETIME
    g_ft_stall_ns += bridge_now_ns() - t0;
#endif
  }

  g_local_seq = seq + 1;
  *out_seq = seq;
  return &g_ring->slots[seq & g_ring_mask];
}

/* ── Data-arena ring allocator ────────────────────────────────────────── */
static uint32_t ring_data_alloc(size_t size)
{
  if (size > GLES_BRIDGE_DATA_SIZE)
  {
    log_error("bridge_data_write OVERFLOW: %zu > %u", size,
              GLES_BRIDGE_DATA_SIZE);
    abort();
  }

  size = (size + 3u) & ~3u;

  for (;;)
  {
    uint32_t pos = (uint32_t)(g_local_data_head % GLES_BRIDGE_DATA_SIZE);
    uint32_t space_to_end = GLES_BRIDGE_DATA_SIZE - pos;
    uint64_t new_head;
    uint32_t offset;

    if (size <= space_to_end)
    {
      offset = pos;
      new_head = g_local_data_head + size;
    }
    else
    {
      // Doesn't fit before the wrap point
      offset = 0;
      new_head = g_local_data_head + space_to_end + size;
    }

    uint64_t tail =
        atomic_load_explicit(&g_ring->data_tail, memory_order_acquire);

    if (new_head - tail > GLES_BRIDGE_DATA_SIZE)
    {
#ifdef DEBUG_OPCODES
      g_data_full_stalls++;
#endif
#ifdef DEBUG_FRAMETIME
      uint64_t t0 = bridge_now_ns();
#endif
      // full
      struct timespec ts = {0, BP_SLEEP_DURATION};
      nanosleep(&ts, NULL);
#ifdef DEBUG_FRAMETIME
      g_ft_stall_ns += bridge_now_ns() - t0;
#endif
      continue;
    }

    g_local_data_head = new_head;
    return offset;
  }
}

void bridge_begin(void)
{
  pthread_mutex_lock(&g_lock);
  if (!g_ring)
    bridge_init();

#ifdef DEBUG_FRAMETIME
  g_ft_begin_ns = bridge_now_ns();
#endif
  g_cur_slot = ring_reserve_slot(&g_cur_seq);
  g_cur_slot->client_pid = g_client_pid;
  g_cur_slot->target_ctx = g_stub_current_ctx;
  g_cur_slot->data_watermark = g_local_data_head;
  g_cur_slot->data_watermark_valid = 0;
}

uint32_t bridge_data_write(const void *src, size_t size)
{
  uint32_t offset = ring_data_alloc(size);

  if (size && src)
    memcpy(g_data + offset, src, size);

  g_cur_slot->data_watermark = g_local_data_head;
  g_cur_slot->data_watermark_valid = 1;
  return offset;
}

void bridge_data_read(void *dst, uint32_t offset, size_t size)
{
  if ((uint64_t)offset + size > GLES_BRIDGE_DATA_SIZE)
  {
    log_error("bridge_data_read OVERFLOW: %zu > %u, offset: %d\n", size,
              GLES_BRIDGE_DATA_SIZE, offset);
    abort();
  }

  memcpy(dst, g_data + offset, size);
}

uint8_t *bridge_data_ptr(uint32_t offset)
{
  return g_data + offset;
}

static uint64_t publish_cur(uint32_t needs_response);

void bridge_send_void(void)
{
#ifdef DEBUG_FRAMETIME
  g_ft_calls++;
#endif
  publish_cur(0);
  pthread_mutex_unlock(&g_lock);
}

/* Publish the current slot to the proxy, waking it if it is asleep */
static uint64_t publish_cur(uint32_t needs_response)
{
  g_cur_slot->needs_response = needs_response;

#ifdef DEBUG_FRAMETIME
  {
    uint64_t dt = bridge_now_ns() - g_ft_begin_ns;
    uint32_t opc = g_cur_slot->opcode;
    /* ring/data stalls happen inside this window; frametime_log
     * subtracts them (they are reported as ring_full) */
    g_ft_stub_ns += dt;
    if (opc < OP_MAX)
    {
      g_ft_stub_op_ns[opc] += dt;
      g_ft_stub_op_cnt[opc]++;
    }
  }
#endif

  uint64_t seq = g_cur_seq;
  atomic_store_explicit(&g_ring->published_seq, seq + 1, memory_order_release);

  if (atomic_load_explicit(&g_ring->consumer_waiting, memory_order_seq_cst))
    efd_post(req_efd);

  return seq;
}

uint64_t bridge_send_call(void)
{
  uint64_t seq = publish_cur(1);

#ifdef DEBUG_FRAMETIME
  uint64_t t0 = bridge_now_ns();
#endif

  // wait until dispatched
  wait_completed(seq);

#ifdef DEBUG_FRAMETIME
  g_ft_calls++;
  g_ft_sync_calls++;
  g_ft_sync_ns += bridge_now_ns() - t0;
#endif

  uint64_t r = g_cur_slot->result;
  pthread_mutex_unlock(&g_lock);
  return r;
}

#ifdef DEBUG_FRAMETIME
static void ft_dump_stub_profile(void)
{
  int top[FT_PROFILE_TOP];
  int n = 0;

  for (int i = 1; i < OP_MAX; i++)
  {
    if (!g_ft_stub_op_cnt[i])
      continue;
    /* keep top[] sorted by time, descending */
    int j;
    if (n < FT_PROFILE_TOP)
      j = n++;
    else if (g_ft_stub_op_ns[i] > g_ft_stub_op_ns[top[n - 1]])
      j = n - 1;
    else
      continue;
    while (j > 0 && g_ft_stub_op_ns[top[j - 1]] < g_ft_stub_op_ns[i])
    {
      top[j] = top[j - 1];
      j--;
    }
    top[j] = i;
  }

  log_always("[frametime] stub top calls:");
  for (int k = 0; k < n; k++)
  {
    int op = top[k];
    log_always("[frametime]   %-32s %8.2fms  n=%-5u avg=%.1fus",
               opcode_to_string(op), g_ft_stub_op_ns[op] / 1e6,
               g_ft_stub_op_cnt[op],
               g_ft_stub_op_ns[op] / 1e3 / g_ft_stub_op_cnt[op]);
  }
}

static void frametime_log(uint64_t swap_wait_ns)
{
  uint64_t now = bridge_now_ns();

  if (g_ft_last_swap_ns)
  {
    uint64_t frame_ns = now - g_ft_last_swap_ns;
    uint32_t wl_calls =
        atomic_exchange_explicit(&g_ft_wl_calls, 0, memory_order_relaxed);
    uint64_t wl_ns =
        atomic_exchange_explicit(&g_ft_wl_ns, 0, memory_order_relaxed);

    /* stub = encoding calls; ring_full stalls happen inside that window */
    uint64_t stub_ns =
        g_ft_stub_ns > g_ft_stall_ns ? g_ft_stub_ns - g_ft_stall_ns : 0;

    /* app = time outside the bridge (the core / RetroArch itself) */
    uint64_t blocked =
        g_ft_sync_ns + wl_ns + swap_wait_ns + g_ft_stall_ns + stub_ns;
    uint64_t app = frame_ns > blocked ? frame_ns - blocked : 0;

    log_always("[frametime] frame=%" PRIu64 " %.2fms app=%.2fms stub=%.2fms "
               "gl_sync=%.2fms(%u calls) ring_full=%.2fms "
               "wl=%.2fms(%u calls) swap_wait=%.2fms gl_calls=%u",
               g_ft_frames, frame_ns / 1e6, app / 1e6, stub_ns / 1e6,
               g_ft_sync_ns / 1e6, g_ft_sync_calls, g_ft_stall_ns / 1e6,
               wl_ns / 1e6, wl_calls, swap_wait_ns / 1e6, g_ft_calls);

    if (stub_ns > FT_STUB_PROFILE_NS || g_ft_frames % 300 == 0)
      ft_dump_stub_profile();

    if (frame_ns < g_ft_win_min_ns)
      g_ft_win_min_ns = frame_ns;
    if (frame_ns > g_ft_win_max_ns)
      g_ft_win_max_ns = frame_ns;

    if (g_ft_frames % FT_WINDOW_FRAMES == 0)
    {
      double avg_ms = (now - g_ft_win_start_ns) / 1e6 / FT_WINDOW_FRAMES;
      log_always("[frametime] last %u frames: avg=%.2fms (%.1f fps) "
                 "min=%.2fms max=%.2fms",
                 FT_WINDOW_FRAMES, avg_ms, avg_ms > 0 ? 1000.0 / avg_ms : 0.0,
                 g_ft_win_min_ns / 1e6, g_ft_win_max_ns / 1e6);
      g_ft_win_start_ns = now;
      g_ft_win_min_ns = UINT64_MAX;
      g_ft_win_max_ns = 0;
    }
  }
  else
  {
    g_ft_win_start_ns = now;
  }

  g_ft_frames++;
  g_ft_last_swap_ns = now;
  g_ft_calls = 0;
  g_ft_sync_calls = 0;
  g_ft_sync_ns = 0;
  g_ft_stall_ns = 0;
  g_ft_stub_ns = 0;
  memset(g_ft_stub_op_ns, 0, sizeof(g_ft_stub_op_ns));
  memset(g_ft_stub_op_cnt, 0, sizeof(g_ft_stub_op_cnt));
}
#endif

/* eglSwapBuffers.  With async_swap the stub does not wait for this swap:
 * it publishes it and then only waits for the *previous* swap, so the app
 * can build frame N+1 while the proxy is still submitting and presenting
 * frame N, with at most one frame in flight. */
uint64_t bridge_send_swap(void)
{
  int async = bridge_config_async_swap();

  /* 2 = proxy posts resp_efd when done, but nobody waits for it yet */
  uint64_t seq = publish_cur(async ? 2 : 1);

#ifdef DEBUG_FRAMETIME
  g_ft_calls++;
  uint64_t t0 = bridge_now_ns();
#endif

  uint64_t r = 1; /* EGL_TRUE; a failed async swap shows in eglGetError() */
  if (async)
  {
    if (g_have_pending_swap)
      wait_completed(g_pending_swap_seq);
    g_pending_swap_seq = seq;
    g_have_pending_swap = 1;
  }
  else
  {
    wait_completed(seq);
    r = g_cur_slot->result;
  }

#ifdef DEBUG_FRAMETIME
  frametime_log(bridge_now_ns() - t0);
#endif

  pthread_mutex_unlock(&g_lock);
  return r;
}

BridgeCtrl *bridge_ctrl(void)
{
  return g_cur_slot;
}

/* ── WL state ───────────────────────────────────── */
static WLBridge g_wl;

/* ── WL allocator ───────────────────────────────── */

static uint32_t wl_data_alloc(size_t size)
{
  uint32_t off = g_wl.data_pos;

  g_wl.data_pos = (g_wl.data_pos + (uint32_t)size + 3u) & ~3u;

  if (g_wl.data_pos > GLES_BRIDGE_DATA_SIZE)
  {
    log_console("FATAL wl shm overflow (%u bytes)", g_wl.data_pos);
    abort();
  }

  return off;
}

/* ── Init ────────────────────────────────────────── */
void wl_bridge_init(void)
{
  log_console("initialising wl bridge");

  g_ctrl_wl_shm = shm_create(GLES_BRIDGE_WL_CTRL_SHM, bridge_ctrl_shm_size());

  g_data_wl_shm = shm_create(GLES_BRIDGE_WL_DATA_SHM, GLES_BRIDGE_DATA_SIZE);

  g_wl.ctrl = (BridgeCtrl *)g_ctrl_wl_shm.ptr;

  g_wl.data = (uint8_t *)g_data_wl_shm.ptr;

  if (!g_wl.ctrl || !g_wl.data)
    abort();

  pthread_mutex_init(&g_wl.lock, NULL);

  g_wl.data_pos = 0;
}

/* ── Begin/end ───────────────────────────────────── */

void wl_bridge_begin(void)
{
  pthread_mutex_lock(&g_wl.lock);

  if (!g_ring)
  {
    bridge_init();
    g_wl.ctrl->client_pid = g_client_pid;
  }

  g_wl.data_pos = 0;
}

void wl_bridge_end(void)
{
  pthread_mutex_unlock(&g_wl.lock);
}

/* ── Data access ─────────────────────────────────── */

void bridge_data_read_wl(void *dst, uint32_t offset, size_t size)
{
  if ((uint64_t)offset + size > GLES_BRIDGE_DATA_SIZE)
  {
    log_error("bridge_data_read_wl OVERFLOW: %zu > %u, offset: %u", size,
              GLES_BRIDGE_DATA_SIZE, offset);
    abort();
  }

  memcpy(dst, g_wl.data + offset, size);
}

uint32_t bridge_data_write_wl(const void *src, size_t size)
{
  uint32_t off = wl_data_alloc(size);

  memcpy(g_wl.data + off, src, size);

  return off;
}

uint8_t *bridge_data_ptr_wl(uint32_t offset)
{
  return g_wl.data + offset;
}

/* ── Send ────────────────────────────────────────── */

#ifdef DEBUG_FRAMETIME
static void wl_frametime_account(uint64_t t0)
{
  atomic_fetch_add_explicit(&g_ft_wl_calls, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_ft_wl_ns, bridge_now_ns() - t0,
                            memory_order_relaxed);
}
#endif

void bridge_send_void_wl(void)
{
#ifdef DEBUG_BRIDGE_VERBOSE
  log_console("bridge_send_void_wl opcode=%u", g_wl.ctrl->opcode);
#endif

  g_wl.ctrl->needs_response = 1;

#ifdef DEBUG_FRAMETIME
  uint64_t t0 = bridge_now_ns();
#endif

  efd_post(req_wl_efd);
  efd_wait(resp_wl_efd);

#ifdef DEBUG_FRAMETIME
  wl_frametime_account(t0);
#endif

  wl_bridge_end();
}

uint64_t bridge_send_call_wl(void)
{
#ifdef DEBUG_BRIDGE_VERBOSE
  log_console("bridge_send_call_wl opcode=%u", g_wl.ctrl->opcode);
#endif

  g_wl.ctrl->needs_response = 1;

#ifdef DEBUG_FRAMETIME
  uint64_t t0 = bridge_now_ns();
#endif

  efd_post(req_wl_efd);
  efd_wait(resp_wl_efd);

#ifdef DEBUG_FRAMETIME
  wl_frametime_account(t0);
#endif

  uint64_t r = g_wl.ctrl->result;

  wl_bridge_end();

  return r;
}

BridgeCtrl *bridge_ctrl_wl(void)
{
  return g_wl.ctrl;
}
