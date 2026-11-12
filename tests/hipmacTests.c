/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Tests of the hipmac backend against fake metrics pages: no card, no driver. The backend source is included (as the NVIDIA
 * tests do); the tests drive it through the same entry points nvtop uses (gpuinfo_init_info_extraction ...
 * gpuinfo_refresh_processes). */

#include "../src/extract_gpuinfo_hipmac.c"
#include "hipmac_fake_page.h"
#include "nvtop/extract_gpuinfo.h"

#include <stdlib.h>
#include <sys/stat.h>

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    if (!(condition)) {                                                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                                  \
      return false;                                                                                                    \
    }                                                                                                                  \
  } while (0)

static char page_path[256];
static LIST_HEAD(devices);

static void use_page(void) {
  snprintf(page_path, sizeof(page_path), "/tmp/nvtop-hipmac-test-%d.page", (int)getpid());
  setenv("UT4G_METRICS_PAGE", page_path, 1);
  unsetenv("NVTOP_HIPMAC_ALWAYS");
  unlink(page_path);
}

static bool start(unsigned *count) {
  CHECK(gpuinfo_init_info_extraction(count, &devices));
  return true;
}

static void stop(void) { gpuinfo_shutdown_info_extraction(&devices); }

static struct gpu_info *only_device(void) { return list_first_entry(&devices, struct gpu_info, list); }

static bool refresh_all(void) {
  CHECK(gpuinfo_populate_static_infos(&devices));
  CHECK(gpuinfo_refresh_dynamic_info(&devices));
  CHECK(gpuinfo_refresh_processes(&devices));
  CHECK(gpuinfo_fix_dynamic_info_from_process_info(&devices));
  return true;
}

static bool dynamic_all_invalid(const struct gpuinfo_dynamic_info *d) {
  for (size_t i = 0; i < sizeof(d->valid); ++i)
    if (d->valid[i] != 0)
      return false;
  return true;
}

/* a version 2 page written by ut4gd with two clients: every group of the page shows up */
static bool broker_page(void) {
  use_page();
  struct hipmac_page page;
  fake_page_fill(&page, 3, 2, 0);
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  unsigned count = 0;
  CHECK(start(&count));
  CHECK(count == 1);
  CHECK(refresh_all());
  struct gpu_info *dev = only_device();
  CHECK(strcmp(dev->vendor->name, "hipmac") == 0);

  const struct gpuinfo_static_info *s = &dev->static_info;
  CHECK(GPUINFO_STATIC_FIELD_VALID(s, device_name) && strcmp(s->device_name, "AMD Radeon RX Vega") == 0);
  CHECK(GPUINFO_STATIC_FIELD_VALID(s, device_architecture) && strcmp(s->device_architecture, "gfx900:sramecc-:xnack-") == 0);
  CHECK(GPUINFO_STATIC_FIELD_VALID(s, n_shared_cores) && s->n_shared_cores == 56);
  CHECK(GPUINFO_STATIC_FIELD_VALID(s, l2cache_size) && s->l2cache_size == (4u << 20));
  CHECK(GPUINFO_STATIC_FIELD_VALID(s, memory_type) && strcmp(s->memory_type, "HBM2") == 0);
  CHECK(!s->integrated_graphics);

  const struct gpuinfo_dynamic_info *d = &dev->dynamic_info;
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, gpu_clock_speed) && d->gpu_clock_speed == 1000 + (3 * 53) % 590);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, gpu_clock_speed_max) && d->gpu_clock_speed_max == 1590);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, mem_clock_speed) && d->mem_clock_speed == 945);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, mem_clock_speed_max) && d->mem_clock_speed_max == 945);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, extra_clocks) && d->extra_clock_count == 1);
  CHECK(strcmp(d->extra_clocks[0].name, "SoC") == 0 && d->extra_clocks[0].speed == 1107);
  // the generic code takes the larger of the SMU's GFX activity and the sum of the clients' busy shares (85 + 12)
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, gpu_util_rate) && d->gpu_util_rate == 97);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, total_memory) && d->total_memory == 8176 * FAKE_MIB);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, used_memory) && d->used_memory == (2048 + 2560) * FAKE_MIB);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, free_memory) && d->free_memory == d->total_memory - d->used_memory);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, mem_util_rate) && d->mem_util_rate == 56);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_rx) && d->pcie_rx == (1253 * UINT64_C(1000000)) / 1024);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_tx) && d->pcie_tx == 8500000 / 1024);
  CHECK(!GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_link_gen) && !GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_link_width));
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, fan_speed) && d->fan_speed == 35);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, fan_rpm) && d->fan_rpm == 1800);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, gpu_temp) && d->gpu_temp == 55 + 3);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, power_draw) && d->power_draw == (90 + (3 * 5) % 90) * 1000);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, power_draw_max) && d->power_draw_max == 180000);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, effective_load_rate));

  CHECK(dev->processes_count == 2);
  const struct gpu_process *a = &dev->processes[0], *b = &dev->processes[1];
  CHECK(a->pid == getpid() && b->pid == getppid());
  CHECK(a->type == gpu_process_compute);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(a, gpu_memory_usage) && a->gpu_memory_usage == 2048 * FAKE_MIB);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(b, gpu_memory_usage) && b->gpu_memory_usage == 2560 * FAKE_MIB);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(a, gpu_usage) && a->gpu_usage == 85);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(b, gpu_usage) && b->gpu_usage == 12);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(a, gpu_memory_percentage) && a->gpu_memory_percentage == 25);
  CHECK(GPUINFO_PROCESS_FIELD_VALID(a, cmdline) && a->cmdline && a->cmdline[0]); // the generic code, or the page's name
  stop();
  unlink(page_path);
  return true;
}

/* the first version of the page: the base only. What it does not carry is unavailable, not zero */
static bool version1_page(void) {
  use_page();
  struct hipmac_page page;
  fake_page_fill(&page, 0, 0, 0);
  page.base.version = 1;
  page.base.size = HIPMAC_PAGE_V1_SIZE;
  memset(&page.ext, 0, sizeof(page.ext));
  CHECK(fake_page_write(page_path, &page, HIPMAC_PAGE_V1_SIZE));
  unsigned count = 0;
  CHECK(start(&count));
  CHECK(count == 1);
  CHECK(refresh_all());
  const struct gpuinfo_dynamic_info *d = &only_device()->dynamic_info;
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, gpu_util_rate) && GPUINFO_DYNAMIC_FIELD_VALID(d, power_draw));
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, total_memory) && d->total_memory == 8176 * FAKE_MIB); // from the device info
  CHECK(!GPUINFO_DYNAMIC_FIELD_VALID(d, used_memory) && !GPUINFO_DYNAMIC_FIELD_VALID(d, mem_util_rate));
  CHECK(!GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_rx) && !GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_tx));
  CHECK(only_device()->processes_count == 0);
  stop();
  unlink(page_path);
  return true;
}

/* a page whose writer stopped updating it: the device stays listed, nothing about it is claimed */
static bool stale_page(void) {
  use_page();
  struct hipmac_page page;
  fake_page_fill(&page, 1, 2, 0);
  page.base.updated_ns -= UINT64_C(10) * 1000000000;
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  unsigned count = 0;
  CHECK(start(&count));
  CHECK(count == 1);
  CHECK(refresh_all());
  CHECK(dynamic_all_invalid(&only_device()->dynamic_info));
  CHECK(only_device()->processes_count == 0);
  CHECK(strstr(only_device()->vendor->last_error_string(), "not being updated") != NULL);
  // the writer is back: the same device shows data again, without a restart
  fake_page_fill(&page, 2, 2, 0);
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  CHECK(gpuinfo_refresh_dynamic_info(&devices) && gpuinfo_refresh_processes(&devices));
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(&only_device()->dynamic_info, gpu_util_rate));
  CHECK(only_device()->processes_count == 2);
  // a closed device (pid 0, not running) is stale too
  fake_page_fill(&page, 3, 2, 0);
  page.base.running = 0;
  page.base.pid = 0;
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  CHECK(gpuinfo_refresh_dynamic_info(&devices) && gpuinfo_refresh_processes(&devices));
  CHECK(dynamic_all_invalid(&only_device()->dynamic_info) && only_device()->processes_count == 0);
  // a writer whose process is gone
  fake_page_fill(&page, 4, 2, 2000000);
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  CHECK(gpuinfo_refresh_dynamic_info(&devices));
  CHECK(dynamic_all_invalid(&only_device()->dynamic_info));
  stop();
  unlink(page_path);
  return true;
}

/* no page: no device, unless asked for; then it fills in when a page appears */
static bool missing_page(void) {
  use_page();
  unsigned count = 7;
  CHECK(start(&count));
  CHECK(count == 0);
  CHECK(strstr(gpuinfo_hipmac_last_error_string(), "no metrics page") != NULL);
  stop();

  setenv("NVTOP_HIPMAC_ALWAYS", "1", 1);
  CHECK(start(&count));
  CHECK(count == 1);
  CHECK(refresh_all());
  CHECK(dynamic_all_invalid(&only_device()->dynamic_info));
  CHECK(strstr(only_device()->static_info.device_name, "hipmac") != NULL); // the placeholder name
  struct hipmac_page page;
  fake_page_fill(&page, 5, 1, 0);
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  CHECK(gpuinfo_populate_static_infos(&devices) && gpuinfo_refresh_dynamic_info(&devices) &&
        gpuinfo_refresh_processes(&devices));
  CHECK(strcmp(only_device()->static_info.device_name, "AMD Radeon RX Vega") == 0);
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(&only_device()->dynamic_info, gpu_util_rate));
  CHECK(only_device()->processes_count == 1);
  stop();
  unsetenv("NVTOP_HIPMAC_ALWAYS");
  unlink(page_path);
  return true;
}

/* files that are not pages must never be taken for one, nor crash the reader */
static bool bad_pages(void) {
  use_page();
  struct hipmac_page good;
  fake_page_fill(&good, 1, HIPMAC_PAGE_MAX_PROCS, 0);
  struct hipmac_page bad;
  unsigned count;

  bad = good;
  bad.base.magic = 0x1234;
  CHECK(fake_page_write(page_path, &bad, sizeof(bad)));
  CHECK(start(&count) && count == 0);
  stop();

  bad = good;
  bad.base.version = 3;
  CHECK(fake_page_write(page_path, &bad, sizeof(bad)));
  CHECK(start(&count) && count == 0);
  stop();

  bad = good;
  bad.base.seq |= 1; // the writer died in the middle of an update
  CHECK(fake_page_write(page_path, &bad, sizeof(bad)));
  CHECK(start(&count) && count == 0);
  stop();

  bad = good;
  bad.ext.magic = 0; // version 2 without its extension header
  CHECK(fake_page_write(page_path, &bad, sizeof(bad)));
  CHECK(start(&count) && count == 0);
  stop();

  CHECK(fake_page_write(page_path, &good, sizeof(good) - 200)); // a truncated copy of a version 2 page
  CHECK(start(&count) && count == 0);
  stop();

  CHECK(fake_page_write(page_path, &good, HIPMAC_PAGE_V1_SIZE - 1)); // shorter than the base: no mapping beyond the file
  CHECK(start(&count) && count == 0);
  stop();

  CHECK(fake_page_write(page_path, &good, 0));
  CHECK(start(&count) && count == 0);
  stop();

  // a corrupt row count never reads past the table
  bad = good;
  bad.ext.nprocs = 1000;
  CHECK(fake_page_write(page_path, &bad, sizeof(bad)));
  CHECK(start(&count) && count == 1);
  CHECK(refresh_all());
  CHECK(only_device()->processes_count == HIPMAC_PAGE_MAX_PROCS);
  stop();
  unlink(page_path);
  return true;
}

/* a page written by a client directly (not by the daemon): one row, the owner */
static bool client_page(void) {
  use_page();
  struct hipmac_page page;
  fake_page_fill(&page, 6, 1, 0);
  page.ext.source = HIPMAC_EXT_SRC_CLIENT;
  page.ext.valid &= ~(uint32_t)HIPMAC_EXT_LINK; // a group that is not valid reads as unavailable, whatever the bytes say
  CHECK(fake_page_write(page_path, &page, sizeof(page)));
  unsigned count = 0;
  CHECK(start(&count) && count == 1);
  CHECK(refresh_all());
  const struct gpuinfo_dynamic_info *d = &only_device()->dynamic_info;
  CHECK(!GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_rx) && !GPUINFO_DYNAMIC_FIELD_VALID(d, pcie_tx));
  CHECK(GPUINFO_DYNAMIC_FIELD_VALID(d, used_memory) && d->used_memory == 2048 * FAKE_MIB);
  CHECK(only_device()->processes_count == 1 && only_device()->processes[0].pid == getpid());
  stop();
  unlink(page_path);
  return true;
}

int main(int argc, char **argv) {
  static const struct {
    const char *name;
    bool (*run)(void);
  } cases[] = {
      {"broker_page", broker_page}, {"version1_page", version1_page}, {"stale_page", stale_page},
      {"missing_page", missing_page}, {"bad_pages", bad_pages},       {"client_page", client_page},
  };
  if (argc != 2) {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return 2;
  }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (strcmp(argv[1], cases[i].name) == 0)
      return cases[i].run() ? 0 : 1;
  }
  fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
