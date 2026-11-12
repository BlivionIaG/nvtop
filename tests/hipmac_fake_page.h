/* SPDX-License-Identifier: GPL-3.0-or-later */

/* A fake metrics page of the hipmac stack: what a card owner publishes, without a card.
 * Shared by the backend tests (hipmacTests.c) and by hipmac_fake_page, the tool that keeps a live page going so that
 * nvtop can be run against it. */

#ifndef HIPMAC_FAKE_PAGE_H_
#define HIPMAC_FAKE_PAGE_H_

#include "nvtop/hipmac_page.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FAKE_MIB (UINT64_C(1) << 20)

static uint64_t fake_page_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* A version 2 page written by `writer_pid` (0: this process) with `nclients` process rows (at most 8; the rows are this
 * process, its parent, then pids that do not exist). `tick` moves the load and the clocks so that a live page changes. */
static void fake_page_fill(struct hipmac_page *p, unsigned tick, unsigned nclients, pid_t writer_pid) {
  memset(p, 0, sizeof(*p));
  struct hipmac_page_base *b = &p->base;
  b->magic = HIPMAC_PAGE_MAGIC;
  b->version = HIPMAC_PAGE_VERSION;
  b->size = sizeof(*p);
  b->seq = 2 * (tick + 1);
  b->pid = (uint32_t)(writer_pid ? writer_pid : getpid());
  b->interval_ms = 1000;
  b->running = 1;
  b->updated_ns = fake_page_now_ns();
  b->samples = tick + 1;

  struct hipmac_dev_info *i = &b->info;
  snprintf(i->name, sizeof(i->name), "AMD Radeon RX Vega");
  snprintf(i->arch_name, sizeof(i->arch_name), "gfx900:sramecc-:xnack-");
  i->pci_vendor = 0x1002;
  i->pci_device = 0x687f;
  i->num_cu = 56;
  i->num_se = 4;
  i->max_clock_khz = 1590000;
  i->mem_clock_khz = 945000;
  i->mem_bus_width = 2048;
  i->l2_cache_size = 4u << 20;
  i->vram_total = 8176 * FAKE_MIB;
  i->vram_usable = 8000 * FAKE_MIB;

  struct hipmac_metrics *m = &b->m;
  m->valid = HIPMAC_MET_SCLK | HIPMAC_MET_MCLK | HIPMAC_MET_SOCCLK | HIPMAC_MET_ACTIVITY | HIPMAC_MET_POWER |
             HIPMAC_MET_POWER_LIMIT | HIPMAC_MET_EDGE | HIPMAC_MET_HOTSPOT | HIPMAC_MET_FAN_RPM | HIPMAC_MET_FAN_PCT;
  m->activity_percent = 40 + (tick * 7) % 55;
  m->sclk_mhz = 1000 + (tick * 53) % 590;
  m->mclk_mhz = 945;
  m->socclk_mhz = 1107;
  m->power_w = 90 + (tick * 5) % 90;
  m->power_limit_w = 180;
  m->temp_edge_c = 55 + (int)(tick % 20);
  m->temp_hotspot_c = m->temp_edge_c + 9;
  m->fan_rpm = 1800;
  m->fan_percent = 35;
  m->timestamp_ns = b->updated_ns;

  struct hipmac_page_ext *e = &p->ext;
  e->magic = HIPMAC_PAGE_EXT_MAGIC;
  e->size = sizeof(*e);
  e->valid = HIPMAC_EXT_MEM | HIPMAC_EXT_LINK | HIPMAC_EXT_BUSY | HIPMAC_EXT_OWNER | HIPMAC_EXT_PROCS;
  e->source = HIPMAC_EXT_SRC_BROKER;
  e->vram_total = 8176 * FAKE_MIB;
  e->vram_reserved = 176 * FAKE_MIB;
  e->host_mapped = 96 * FAKE_MIB;
  e->host_window = 1024 * FAKE_MIB;
  e->h2d_bps = 1250 * UINT64_C(1000000) + tick * UINT64_C(1000000);
  e->d2h_bps = 8500000;
  e->busy_window_ms = 1000;
  e->owner_pid = b->pid;
  snprintf(e->owner_name, sizeof(e->owner_name), "ut4gd");
  if (nclients > HIPMAC_PAGE_MAX_PROCS)
    nclients = HIPMAC_PAGE_MAX_PROCS;
  e->nprocs = nclients;
  for (unsigned c = 0; c < nclients; ++c) {
    struct hipmac_page_proc *r = &e->procs[c];
    r->pid = c == 0 ? (uint32_t)getpid() : c == 1 ? (uint32_t)getppid() : 2000000u + c; // beyond pid_max: gone
    r->vmid = 8 + c;
    r->vram_bytes = (2048 + 512 * (uint64_t)c) * FAKE_MIB;
    r->host_bytes = 32 * FAKE_MIB;
    r->h2d_bps = e->h2d_bps / (c + 1);
    r->d2h_bps = e->d2h_bps / (c + 1);
    r->compute_busy_pm = c == 0 ? 850 : 120;
    r->sdma_busy_pm = 100;
    snprintf(r->name, sizeof(r->name), c == 0 ? "tf-serve" : c == 1 ? "hipcc-bench" : "client%u", c);
    e->vram_used += r->vram_bytes;
    e->h2d_bytes += 1000 * FAKE_MIB * (c + 1);
    e->compute_busy_pm = e->compute_busy_pm > r->compute_busy_pm ? e->compute_busy_pm : r->compute_busy_pm;
  }
}

/* Writes len bytes of the page (sizeof(struct hipmac_page) for version 2, HIPMAC_PAGE_V1_SIZE for a version 1 file)
 * into path, replacing the file in one step. */
static bool fake_page_write(const char *path, const struct hipmac_page *p, size_t len) {
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return false;
  bool ok = write(fd, p, len) == (ssize_t)len;
  ok = close(fd) == 0 && ok;
  return ok && rename(tmp, path) == 0;
}

#endif // HIPMAC_FAKE_PAGE_H_
