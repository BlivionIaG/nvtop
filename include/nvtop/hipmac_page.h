/*
 * Layout of the metrics page of the hipmac stack (ut4g-usbgpu: an AMD Vega 10 card on a Mac over USB4/Thunderbolt).
 *
 * A process that owns the card (a client of libut4gdev with UT4G_METRICS=1, or the ut4gd daemon) publishes its telemetry into
 * a small file ($UT4G_METRICS_PAGE, default $TMPDIR/ut4g-metrics.page); this is the reader side. The file is the ABI: the
 * structures below mirror include/ut4g_dev.h of the hipmac tree (ut4g_metrics_page2), the static assertions keep them honest.
 *
 * Reader protocol (seqlock): read `seq` (retry if odd), copy the page, read `seq` again and retry if it changed.
 * Fresh = magic ok, running, the writer pid alive and now - updated_ns < max(3 * interval_ms, 3 s).
 * Version 1 is the first HIPMAC_PAGE_V1_SIZE bytes only; version 2 appends hipmac_page_ext (usage: memory, link, engines,
 * the clients of the card). A group of the extension is meaningful only if its HIPMAC_EXT_* bit is set in `valid`.
 */

#ifndef NVTOP_HIPMAC_PAGE_H_
#define NVTOP_HIPMAC_PAGE_H_

#include <stdint.h>

#define HIPMAC_PAGE_MAGIC UINT64_C(0x3150475455345447) // "GT4UTGP1" little endian
#define HIPMAC_PAGE_VERSION_MIN 1
#define HIPMAC_PAGE_VERSION 2
#define HIPMAC_PAGE_V1_SIZE 768
#define HIPMAC_PAGE_EXT_MAGIC 0x31545845u // "EXT1"
#define HIPMAC_PAGE_MAX_PROCS 8
#define HIPMAC_PAGE_NAME_BYTES 32

// hipmac_metrics.valid
enum {
  HIPMAC_MET_SCLK = 1u << 0,
  HIPMAC_MET_SCLK_AVG = 1u << 1,
  HIPMAC_MET_SCLK_TARGET = 1u << 2,
  HIPMAC_MET_GFX_LEVEL = 1u << 3,
  HIPMAC_MET_MCLK = 1u << 4,
  HIPMAC_MET_MCLK_AVG = 1u << 5,
  HIPMAC_MET_MEM_LEVEL = 1u << 6,
  HIPMAC_MET_SOCCLK = 1u << 7,
  HIPMAC_MET_SOC_LEVEL = 1u << 8,
  HIPMAC_MET_ACTIVITY = 1u << 9,
  HIPMAC_MET_POWER = 1u << 10,
  HIPMAC_MET_POWER_LIMIT = 1u << 11,
  HIPMAC_MET_EDGE = 1u << 12,
  HIPMAC_MET_HOTSPOT = 1u << 13,
  HIPMAC_MET_HBM = 1u << 14,
  HIPMAC_MET_VRSOC = 1u << 15,
  HIPMAC_MET_VRMEM = 1u << 16,
  HIPMAC_MET_FAN_RPM = 1u << 17,
  HIPMAC_MET_FAN_PCT = 1u << 18,
  HIPMAC_MET_VDDGFX = 1u << 19,
  HIPMAC_MET_FEATURES = 1u << 20,
  HIPMAC_MET_UPTIME = 1u << 21,
};

// hipmac_page_ext.valid
enum {
  HIPMAC_EXT_MEM = 1u << 0,
  HIPMAC_EXT_LINK = 1u << 1,
  HIPMAC_EXT_BUSY = 1u << 2,
  HIPMAC_EXT_OWNER = 1u << 3,
  HIPMAC_EXT_PROCS = 1u << 4,
};

// hipmac_page_ext.source
enum { HIPMAC_EXT_SRC_CLIENT = 0, HIPMAC_EXT_SRC_BROKER = 1 };

struct hipmac_dev_info {
  char name[64];      // "AMD Radeon RX Vega"
  char arch_name[64]; // "gfx900:sramecc-:xnack-"
  uint32_t pci_vendor, pci_device, pci_revision;
  uint32_t gfx_version;
  uint32_t num_cu;
  uint32_t num_se, num_sh_per_se;
  uint32_t simd_per_cu;
  uint32_t wave_size;
  uint32_t max_waves_per_simd;
  uint32_t max_threads_per_cu;
  uint32_t max_workgroup_size;
  uint32_t lds_size;
  uint32_t vgpr_per_simd;
  uint32_t sgpr_per_wave_max;
  uint32_t max_clock_khz; // highest shader clock
  uint32_t mem_clock_khz; // HBM clock
  uint32_t mem_bus_width;
  uint32_t l2_cache_size;
  uint32_t timestamp_freq_hz;
  uint64_t vram_total; // bytes
  uint64_t vram_usable;
  uint64_t bar0_size;
  uint64_t va_base, va_size;
  uint64_t shared_aperture_base;
  uint64_t private_aperture_base;
  uint32_t num_sdma_engines;
  uint32_t max_queues;
  uint32_t max_kernarg_size;
  uint32_t has_pcie_atomics;
  uint32_t dpm;
  uint32_t sclk_boot_khz, mclk_boot_khz;
};

// One sample of the card's SMU telemetry; a field whose bit in `valid` is clear is 0
struct hipmac_metrics {
  uint32_t valid; // HIPMAC_MET_*
  uint32_t sclk_mhz;
  uint32_t sclk_avg_mhz;
  uint32_t sclk_target_mhz;
  uint32_t gfx_level, gfx_levels;
  uint32_t mclk_mhz;
  uint32_t mclk_avg_mhz;
  uint32_t mem_level, mem_levels;
  uint32_t socclk_mhz;
  uint32_t soc_level, soc_levels;
  uint32_t activity_percent; // GFX activity
  uint32_t power_w;          // package power
  uint32_t power_limit_w;
  uint32_t power_limit_default_w, power_limit_min_w, power_limit_max_w;
  int32_t temp_edge_c, temp_hotspot_c, temp_hbm_c;
  int32_t temp_vrsoc_c, temp_vrmem_c;
  uint32_t fan_rpm;
  uint32_t fan_percent;
  uint32_t vddgfx_mv;
  uint32_t features;
  uint32_t throttle;
  uint32_t dpm_mode;
  uint32_t boot_age_s;
  uint32_t session_s;
  uint32_t sample_us;
  uint64_t timestamp_ns;
  uint32_t fclk_mhz, power_instant_w;
};

// Monitor statistics: not used by the nvtop backend, kept for the layout
struct hipmac_monitor_stats {
  uint32_t running;
  uint32_t samples, errors;
  uint32_t aborted;
  int32_t edge_max_c, hotspot_max_c, hbm_max_c;
  uint32_t power_max_w, fan_max_rpm;
  uint32_t sclk_min_mhz, sclk_max_mhz, mclk_min_mhz, mclk_max_mhz;
  uint64_t sclk_sum_mhz, mclk_sum_mhz;
  uint32_t sclk_n, mclk_n;
  uint32_t busy_samples;
  uint64_t busy_sclk_sum_mhz, busy_mclk_sum_mhz;
  uint32_t busy_sclk_n, busy_mclk_n;
  uint32_t throttled_samples;
  struct hipmac_metrics last;
};

struct hipmac_page_base {
  uint64_t magic;
  uint32_t version, size;
  volatile uint32_t seq; // seqlock: odd while the writer updates
  uint32_t pid;          // the writer (0: it closed the device)
  uint32_t interval_ms, running;
  uint64_t updated_ns; // CLOCK_REALTIME of the last sample
  uint64_t samples;
  struct hipmac_dev_info info;
  struct hipmac_monitor_stats stats;
  struct hipmac_metrics m;
};

struct hipmac_page_proc {
  uint32_t pid; // 0: free slot
  uint32_t vmid;
  uint64_t vram_bytes; // VRAM this client holds
  uint64_t host_bytes; // system memory mapped for it
  uint64_t h2d_bytes, d2h_bytes;
  uint64_t h2d_bps, d2h_bps; // bytes/s over the last interval
  uint32_t compute_busy_pm, sdma_busy_pm; // per mille of the probes with outstanding work
  char name[HIPMAC_PAGE_NAME_BYTES];
};

struct hipmac_page_ext {
  uint32_t magic;
  uint32_t size;
  uint32_t valid;  // HIPMAC_EXT_*
  uint32_t source; // HIPMAC_EXT_SRC_*
  uint64_t vram_total;
  uint64_t vram_reserved; // not allocatable by clients
  uint64_t vram_used;     // allocated by all clients together
  uint64_t host_mapped;
  uint64_t host_window;
  uint64_t h2d_bytes, d2h_bytes, d2d_bytes, bar_bytes;
  uint64_t h2d_bps, d2h_bps, d2d_bps;
  uint32_t compute_busy_pm, sdma_busy_pm; // 0..1000, the busiest client
  uint32_t busy_window_ms;
  uint32_t owner_pid;
  char owner_name[HIPMAC_PAGE_NAME_BYTES];
  uint32_t nprocs, pad;
  struct hipmac_page_proc procs[HIPMAC_PAGE_MAX_PROCS];
};

struct hipmac_page {
  struct hipmac_page_base base;
  struct hipmac_page_ext ext; // zeroed for a version 1 page
};

_Static_assert(sizeof(struct hipmac_dev_info) == 296, "hipmac page: dev_info layout");
_Static_assert(sizeof(struct hipmac_metrics) == 152, "hipmac page: metrics layout");
_Static_assert(sizeof(struct hipmac_monitor_stats) == 272, "hipmac page: monitor stats layout");
_Static_assert(sizeof(struct hipmac_page_base) == HIPMAC_PAGE_V1_SIZE, "hipmac page: base layout");
_Static_assert(sizeof(struct hipmac_page_proc) == 96, "hipmac page: proc layout");
_Static_assert(sizeof(struct hipmac_page_ext) == 936, "hipmac page: extension layout");
_Static_assert(sizeof(struct hipmac_page) == 1704, "hipmac page: page2 layout");

#endif // NVTOP_HIPMAC_PAGE_H_
