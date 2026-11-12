/* SPDX-License-Identifier: GPL-3.0-or-later */

/* Keeps a live fake hipmac metrics page going (no card needed) so that nvtop can be run against it:
 *
 *   hipmac_fake_page /tmp/fake.page [clients] &
 *   UT4G_METRICS_PAGE=/tmp/fake.page nvtop
 *
 * It rewrites the page every 500 ms with moving load, clocks, power and link throughput; kill it to see the page go stale. */

#include "hipmac_fake_page.h"

#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <page file> [clients (0-8, default 2)]\n", argv[0]);
    return 2;
  }
  unsigned clients = argc > 2 ? (unsigned)atoi(argv[2]) : 2;
  struct hipmac_page page;
  for (unsigned tick = 0;; ++tick) {
    fake_page_fill(&page, tick, clients, 0);
    if (!fake_page_write(argv[1], &page, sizeof(page))) {
      perror("hipmac_fake_page: cannot write the page");
      return 1;
    }
    usleep(500000);
  }
}
