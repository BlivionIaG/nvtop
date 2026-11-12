/*
 * Hipmac backend: an AMD Vega 10 card driven from macOS through USB4/Thunderbolt by the hipmac stack (ut4g-usbgpu).
 *
 * macOS has no amdgpu driver, so no sysfs, no fdinfo and no DRM node exist for such a card. The process that owns it
 * (a client of libut4gdev, or the ut4gd daemon that several clients share) publishes its SMU telemetry and its usage
 * accounting into a small file; this backend only reads that file (see nvtop/hipmac_page.h for the layout and the
 * reader protocol). It never touches the card, it does not need root, and it does not link any hipmac library.
 *
 * The page lives in $UT4G_METRICS_PAGE (default $TMPDIR/ut4g-metrics.page). The device is listed when a page exists
 * when nvtop starts, or when NVTOP_HIPMAC_ALWAYS is set in the environment (then it shows N/A until a client
 * publishes). While the page is stale (writer gone or hung) every dynamic field is reported as unavailable.
 */

#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/hipmac_page.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HIPMAC_MIN_FRESH_WINDOW_NS UINT64_C(3000000000)
#define HIPMAC_SEQLOCK_TRIES 100

struct gpu_info_hipmac {
  struct gpu_info base;
  char path[PATH_MAX];
  struct hipmac_page page; // the last snapshot
  bool have_page;          // page holds a consistent, fresh snapshot of this refresh cycle
  // The names of the clients: a fallback for gpu_process.cmdline when the process cannot be inspected
  char proc_names[HIPMAC_PAGE_MAX_PROCS][HIPMAC_PAGE_NAME_BYTES + 1];
};

static bool gpuinfo_hipmac_init(void);
static void gpuinfo_hipmac_shutdown(void);
static const char *gpuinfo_hipmac_last_error_string(void);
static bool gpuinfo_hipmac_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_hipmac_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_hipmac_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_hipmac_get_running_processes(struct gpu_info *_gpu_info);

static struct gpu_vendor gpu_vendor_hipmac = {
    .init = gpuinfo_hipmac_init,
    .shutdown = gpuinfo_hipmac_shutdown,
    .last_error_string = gpuinfo_hipmac_last_error_string,
    .get_device_handles = gpuinfo_hipmac_get_device_handles,
    .populate_static_info = gpuinfo_hipmac_populate_static_info,
    .refresh_dynamic_info = gpuinfo_hipmac_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_hipmac_get_running_processes,
    .name = "hipmac",
};

static struct gpu_info_hipmac *gpu_info_hipmac; // one device: the card behind the page
static char last_error[160] = "hipmac: no metrics page";

__attribute__((constructor)) static void init_extract_gpuinfo_hipmac(void) { register_gpu_vendor(&gpu_vendor_hipmac); }

static bool gpuinfo_hipmac_init(void) {
  gpu_info_hipmac = NULL;
  return true;
}

static void gpuinfo_hipmac_shutdown(void) {
  free(gpu_info_hipmac);
  gpu_info_hipmac = NULL;
}

static const char *gpuinfo_hipmac_last_error_string(void) { return last_error; }

// $UT4G_METRICS_PAGE or $TMPDIR/ut4g-metrics.page, the way libut4gdev finds it
static void hipmac_page_path(char *buf, size_t size) {
  const char *env = getenv("UT4G_METRICS_PAGE");
  if (env && env[0]) {
    snprintf(buf, size, "%s", env);
    return;
  }
  const char *tmp = getenv("TMPDIR");
  if (!tmp || !tmp[0])
    tmp = "/tmp";
  size_t len = strlen(tmp);
  while (len > 1 && tmp[len - 1] == '/')
    --len;
  snprintf(buf, size, "%.*s/ut4g-metrics.page", (int)len, tmp);
}

static uint64_t realtime_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

enum hipmac_read_status {
  hipmac_page_ok = 0,
  hipmac_page_missing,  // no such file
  hipmac_page_invalid,  // not a page of a known version, truncated, or torn for too long
  hipmac_page_stale,    // a consistent page whose writer is gone or not updating it
};

// A consistent snapshot of the page into *out (zeroed first: a version 1 page leaves the extension zero)
static enum hipmac_read_status hipmac_read_page(const char *path, struct hipmac_page *out) {
  memset(out, 0, sizeof(*out));
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return hipmac_page_missing;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < (off_t)HIPMAC_PAGE_V1_SIZE) {
    close(fd); // not (yet) a page; never map less than the base: that would be a SIGBUS
    return hipmac_page_invalid;
  }
  // never map more than the file has (a version 1 page is the base only)
  size_t map_len = (uint64_t)st.st_size < sizeof(*out) ? (size_t)st.st_size : sizeof(*out);
  void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return hipmac_page_invalid;

  const struct hipmac_page_base *live = map;
  bool consistent = false;
  for (int tries = 0; tries < HIPMAC_SEQLOCK_TRIES; ++tries) {
    uint32_t seq1 = __atomic_load_n(&live->seq, __ATOMIC_ACQUIRE);
    if (seq1 & 1) {
      usleep(200); // the writer is rewriting the page
      continue;
    }
    memcpy(out, map, map_len); // racy by design, validated by the second read of seq
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&live->seq, __ATOMIC_RELAXED) == seq1) {
      consistent = true;
      break;
    }
  }
  munmap(map, map_len);
  if (!consistent)
    return hipmac_page_invalid;

  const struct hipmac_page_base *base = &out->base;
  if (base->magic != HIPMAC_PAGE_MAGIC)
    return hipmac_page_invalid;
  if (base->version == 1) {
    if (base->size != HIPMAC_PAGE_V1_SIZE)
      return hipmac_page_invalid;
  } else if (base->version == HIPMAC_PAGE_VERSION) {
    // the base says how big the page is: a shorter file or an extension without its header is not a page
    if (base->size != sizeof(*out) || map_len != sizeof(*out))
      return hipmac_page_invalid;
    if (out->ext.magic != HIPMAC_PAGE_EXT_MAGIC || out->ext.size != sizeof(out->ext))
      return hipmac_page_invalid;
  } else {
    return hipmac_page_invalid;
  }

  uint64_t window = (uint64_t)(base->interval_ms ? base->interval_ms : 1000) * UINT64_C(3000000);
  if (window < HIPMAC_MIN_FRESH_WINDOW_NS)
    window = HIPMAC_MIN_FRESH_WINDOW_NS;
  uint64_t now = realtime_ns();
  bool alive = base->pid != 0 && (kill((pid_t)base->pid, 0) == 0 || errno == EPERM);
  if (!(base->running != 0 && alive && now >= base->updated_ns && now - base->updated_ns < window))
    return hipmac_page_stale;
  return hipmac_page_ok;
}

static void hipmac_set_error(enum hipmac_read_status status, const char *path) {
  switch (status) {
  case hipmac_page_missing:
    snprintf(last_error, sizeof(last_error), "hipmac: no metrics page at %s (start the card's client with UT4G_METRICS=1)",
             path);
    break;
  case hipmac_page_invalid:
    snprintf(last_error, sizeof(last_error), "hipmac: %s is not a metrics page of a known version", path);
    break;
  case hipmac_page_stale:
    snprintf(last_error, sizeof(last_error), "hipmac: the metrics page %s is not being updated", path);
    break;
  case hipmac_page_ok:
    break;
  }
}

static bool gpuinfo_hipmac_get_device_handles(struct list_head *devices, unsigned *count) {
  *count = 0;
  char path[PATH_MAX];
  hipmac_page_path(path, sizeof(path));

  struct hipmac_page *probe = malloc(sizeof(*probe));
  if (!probe) {
    perror("Could not allocate memory: ");
    exit(EXIT_FAILURE);
  }
  // a stale page is still a card that was in use: list it, its fields read N/A until the writer is back
  enum hipmac_read_status status = hipmac_read_page(path, probe);
  free(probe);
  const char *always = getenv("NVTOP_HIPMAC_ALWAYS");
  bool wanted = status == hipmac_page_ok || status == hipmac_page_stale || (always && always[0] && strcmp(always, "0") != 0);
  if (!wanted) {
    hipmac_set_error(status, path);
    return true;
  }

  gpu_info_hipmac = calloc(1, sizeof(*gpu_info_hipmac));
  if (!gpu_info_hipmac) {
    perror("Could not allocate memory: ");
    exit(EXIT_FAILURE);
  }
  gpu_info_hipmac->base.vendor = &gpu_vendor_hipmac;
  snprintf(gpu_info_hipmac->path, sizeof(gpu_info_hipmac->path), "%s", path);
  snprintf(gpu_info_hipmac->base.pdev, sizeof(gpu_info_hipmac->base.pdev), "hipmac0");
  list_add_tail(&gpu_info_hipmac->base.list, devices);
  *count = 1;
  return true;
}

static void hipmac_copy_string(char *dst, size_t dst_size, const char *src, size_t src_size) {
  size_t n = strnlen(src, src_size);
  if (n >= dst_size)
    n = dst_size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void gpuinfo_hipmac_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_hipmac *gpu_info = container_of(_gpu_info, struct gpu_info_hipmac, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  RESET_ALL(static_info->valid);

  // The device may exist without a fresh page (NVTOP_HIPMAC_ALWAYS, or a client that just went away): read what is there
  enum hipmac_read_status status = hipmac_read_page(gpu_info->path, &gpu_info->page);
  const struct hipmac_dev_info *info = &gpu_info->page.base.info;
  bool have_info = (status == hipmac_page_ok || status == hipmac_page_stale) && info->name[0] != '\0';

  if (have_info)
    hipmac_copy_string(static_info->device_name, sizeof(static_info->device_name), info->name, sizeof(info->name));
  else
    snprintf(static_info->device_name, sizeof(static_info->device_name), "AMD Radeon (hipmac)");
  SET_VALID(gpuinfo_device_name_valid, static_info->valid);

  if (have_info && info->arch_name[0] != '\0') {
    hipmac_copy_string(static_info->device_architecture, sizeof(static_info->device_architecture), info->arch_name,
                       sizeof(info->arch_name));
    SET_VALID(gpuinfo_device_architecture_valid, static_info->valid);
  }
  if (have_info && info->num_cu != 0)
    SET_GPUINFO_STATIC(static_info, n_shared_cores, info->num_cu);
  if (have_info && info->l2_cache_size != 0)
    SET_GPUINFO_STATIC(static_info, l2cache_size, info->l2_cache_size);
  if (have_info && info->mem_bus_width != 0) {
    // Vega 10 carries HBM2 (the 2048 bit bus); anything else is reported without a memory type
    if (info->mem_bus_width == 2048) {
      snprintf(static_info->memory_type, sizeof(static_info->memory_type), "HBM2");
      SET_VALID(gpuinfo_memory_type_valid, static_info->valid);
    }
  }
  static_info->integrated_graphics = false;
  static_info->encode_decode_shared = false;
  static_info->memory_shared_with_host = false;
}

static void gpuinfo_hipmac_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_hipmac *gpu_info = container_of(_gpu_info, struct gpu_info_hipmac, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;
  RESET_ALL(dynamic_info->valid);

  enum hipmac_read_status status = hipmac_read_page(gpu_info->path, &gpu_info->page);
  gpu_info->have_page = status == hipmac_page_ok;
  if (!gpu_info->have_page) {
    hipmac_set_error(status, gpu_info->path);
    return;
  }

  const struct hipmac_page_base *base = &gpu_info->page.base;
  const struct hipmac_page_ext *ext = &gpu_info->page.ext;
  const struct hipmac_metrics *m = &base->m;
  const struct hipmac_dev_info *info = &base->info;

  if (m->valid & HIPMAC_MET_SCLK)
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, m->sclk_mhz);
  if (info->max_clock_khz != 0)
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed_max, info->max_clock_khz / 1000);
  if (m->valid & HIPMAC_MET_MCLK)
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed, m->mclk_mhz);
  if (info->mem_clock_khz != 0)
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed_max, info->mem_clock_khz / 1000);
  if (m->valid & HIPMAC_MET_SOCCLK)
    gpuinfo_add_extra_clock(dynamic_info, "SoC", m->socclk_mhz, true);

  if (m->valid & HIPMAC_MET_ACTIVITY)
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, m->activity_percent > 100 ? 100 : m->activity_percent);

  // Memory: the card's size is in the base; what is allocated comes with the usage extension (version 2 pages)
  unsigned long long total = (ext->valid & HIPMAC_EXT_MEM) && ext->vram_total != 0 ? ext->vram_total : info->vram_total;
  if (total != 0)
    SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, total);
  if ((ext->valid & HIPMAC_EXT_MEM) && total != 0 && ext->vram_used <= total) {
    SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, ext->vram_used);
    SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, total - ext->vram_used);
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate, (unsigned)(ext->vram_used * 100 / total));
  }

  // The link is USB4/Thunderbolt, its generation and width are not those of PCIe: only the throughput is reported.
  // Host side counters of the copies the client issued (the GPU receives what the host sends: rx = H2D), in KiB/s.
  if (ext->valid & HIPMAC_EXT_LINK) {
    uint64_t rx = ext->h2d_bps / 1024, tx = ext->d2h_bps / 1024;
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, rx > UINT_MAX ? UINT_MAX : (unsigned)rx);
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, tx > UINT_MAX ? UINT_MAX : (unsigned)tx);
  }

  if (m->valid & HIPMAC_MET_FAN_PCT)
    SET_GPUINFO_DYNAMIC(dynamic_info, fan_speed, m->fan_percent > 100 ? 100 : m->fan_percent);
  if (m->valid & HIPMAC_MET_FAN_RPM)
    SET_GPUINFO_DYNAMIC(dynamic_info, fan_rpm, m->fan_rpm);
  if ((m->valid & HIPMAC_MET_EDGE) && m->temp_edge_c >= 0)
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, (unsigned)m->temp_edge_c);

  // Package (ASIC socket) power in milliwatts, not board power
  if (m->valid & HIPMAC_MET_POWER)
    SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, m->power_w * 1000u);
  if (m->valid & HIPMAC_MET_POWER_LIMIT)
    SET_GPUINFO_DYNAMIC(dynamic_info, power_draw_max, m->power_limit_w * 1000u);
}

static void gpuinfo_hipmac_get_running_processes(struct gpu_info *_gpu_info) {
  struct gpu_info_hipmac *gpu_info = container_of(_gpu_info, struct gpu_info_hipmac, base);
  _gpu_info->processes_count = 0;

  // The page of this cycle (refresh_dynamic_info reads it); a stale or missing page has no clients to list
  if (!gpu_info->have_page || !(gpu_info->page.ext.valid & HIPMAC_EXT_PROCS))
    return;
  const struct hipmac_page_ext *ext = &gpu_info->page.ext;
  unsigned rows = ext->nprocs < HIPMAC_PAGE_MAX_PROCS ? ext->nprocs : HIPMAC_PAGE_MAX_PROCS;

  unsigned count = 0;
  for (unsigned i = 0; i < rows; ++i) {
    const struct hipmac_page_proc *row = &ext->procs[i];
    if (row->pid == 0)
      continue;

    if (_gpu_info->processes_array_size < count + 1) {
      _gpu_info->processes_array_size += COMMON_PROCESS_LINEAR_REALLOC_INC;
      _gpu_info->processes =
          reallocarray(_gpu_info->processes, _gpu_info->processes_array_size, sizeof(*_gpu_info->processes));
      if (!_gpu_info->processes) {
        perror("Could not allocate memory: ");
        exit(EXIT_FAILURE);
      }
    }
    struct gpu_process *process = &_gpu_info->processes[count];
    memset(process, 0, sizeof(*process));
    process->type = gpu_process_compute;
    process->pid = (pid_t)row->pid;

    if (ext->valid & HIPMAC_EXT_MEM)
      SET_GPUINFO_PROCESS(process, gpu_memory_usage, row->vram_bytes);
    if (ext->valid & HIPMAC_EXT_BUSY) {
      unsigned usage = row->compute_busy_pm / 10; // per mille -> percent
      SET_GPUINFO_PROCESS(process, gpu_usage, usage > 100 ? 100 : usage);
    }
    // used only when the process cannot be inspected (another user, or gone): the generic code replaces it otherwise
    if (row->name[0] != '\0') {
      hipmac_copy_string(gpu_info->proc_names[count], sizeof(gpu_info->proc_names[count]), row->name, sizeof(row->name));
      SET_GPUINFO_PROCESS(process, cmdline, gpu_info->proc_names[count]);
    }
    ++count;
  }
  _gpu_info->processes_count = count;
}
