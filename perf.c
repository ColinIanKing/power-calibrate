/*
 * Copyright (C) 2014-2016 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#define _GNU_SOURCE

#include "perf.h"

#if defined(PERF_ENABLED)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

#define THOUSAND	(1000.0)
#define MILLION		(THOUSAND * THOUSAND)
#define BILLION		(THOUSAND * MILLION)
#define TRILLION	(THOUSAND * BILLION)
#define QUADRILLION	(THOUSAND * TRILLION)
#define QUINTILLION	(THOUSAND * QUADRILLION)

#define PERF_INVALID     (~0ULL)

#define PERF_INFO(type, config)	\
	{ PERF_ ## config, PERF_TYPE_ ## type, PERF_COUNT_ ## config }

/* perf counters to be read */
static const perf_info_t perf_info[PERF_MAX] = {
	PERF_INFO(HARDWARE, HW_CPU_CYCLES),
	PERF_INFO(HARDWARE, HW_INSTRUCTIONS),
};

int perf_start(perf_t *p, const pid_t pid)
{
	int i;

	memset(p, 0, sizeof(perf_t));
	p->perf_opened = 0;

	for (i = 0; i < PERF_MAX; i++) {
		p->perf_stat[i].fd = -1;
		p->perf_stat[i].counter = 0;
	}

	if (pid <= 0)
		return 0;

	for (i = 0; i < PERF_MAX; i++) {
		struct perf_event_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.type = perf_info[i].type;
		attr.config = perf_info[i].config;
		attr.disabled = 1;
		attr.inherit = 1;
		attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
				   PERF_FORMAT_TOTAL_TIME_RUNNING;
		attr.size = sizeof(attr);
		p->perf_stat[i].fd = syscall(__NR_perf_event_open, &attr, pid, -1, -1, 0);
		if (p->perf_stat[i].fd > -1)
			p->perf_opened++;
		else {
			fprintf(stderr, "perf fail: %d %s\n", errno, strerror(errno));
		}
	}
	if (!p->perf_opened) {
		munmap(p, sizeof(perf_t));
		return -1;
	}

	for (i = 0; i < PERF_MAX; i++) {
		int fd = p->perf_stat[i].fd;

		if (fd > -1) {
			if (ioctl(fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0) {
				(void)close(fd);
				p->perf_stat[i].fd = -1;
				continue;
			}
			if (ioctl(fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
				(void)close(fd);
				p->perf_stat[i].fd = -1;
			}
		}
	}
	return 0;
}

/*
 *  perf_stop()
 *	stop and read counters
 */
int perf_stop(perf_t *p)
{
	/* perf data */
	typedef struct {
		uint64_t counter;		/* perf counter */
		uint64_t time_enabled;		/* perf time enabled */
		uint64_t time_running;		/* perf time running */
	} perf_data_t;

	size_t i = 0;
	perf_data_t data;
	ssize_t ret;
	int rc = -1;
	double scale;

	if (!p)
		return -1;
	if (!p->perf_opened)
		goto out_ok;

	for (i = 0; i < PERF_MAX; i++) {
		int fd = p->perf_stat[i].fd;

		if (fd < 0) {
			p->perf_stat[i].counter = PERF_INVALID;
			continue;
		}
		if (ioctl(fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0) {
			p->perf_stat[i].counter = 0;
		} else {
			memset(&data, 0, sizeof(data));
			ret = read(fd, &data, sizeof(data));
			if (ret != sizeof(data))
				p->perf_stat[i].counter = PERF_INVALID;
			else {
				/* Ensure we don't get division by zero */
				if (data.time_running == 0) {
					scale = (data.time_enabled == 0) ? 1.0 : 0.0;
				} else {
					scale = (double)data.time_enabled / (double)data.time_running;
				}
				p->perf_stat[i].counter = (uint64_t)((double)data.counter * scale);
			}
		}
		(void)close(fd);
		p->perf_stat[i].fd = -1;
	}
out_ok:
	rc = 0;
	for (; i < PERF_MAX; i++)
		p->perf_stat[i].counter = PERF_INVALID;

	return rc;
}

/*
 *  perf_counter
 *	fetch counter and index via perf ID
 */
void perf_counter(
	const perf_t *p,
	const int id,
	double *counter)
{
	int i;

	for (i = 0; i < PERF_MAX; i++) {
		if (perf_info[i].id == id) {
			if (p->perf_stat[i].counter == PERF_INVALID) {
				*counter = 0;
			} else {
				*counter = (double)p->perf_stat[i].counter;
			}
		}
	}
}
#endif
