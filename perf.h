/*
 * Copyright (C) 2013-2015 Canonical, Ltd.
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
#ifndef __PERF_POWER_CALIBRATE_H__
#define __PERF_POWER_CALIBRATE_H__

#define _GNU_SOURCE

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/syscall.h>

#if defined(__linux__) && defined(__NR_perf_event_open)
#define PERF_ENABLED
#endif

enum {
	PERF_HW_CPU_CYCLES = 0,
	PERF_HW_INSTRUCTIONS,
        PERF_MAX
};

/* per perf counter info */
typedef struct {
	uint64_t counter;               /* perf counter */
	int      fd;                    /* perf per counter fd */
} perf_stat_t;

typedef struct {
	perf_stat_t	perf_stat[PERF_MAX]; /* perf counters */
	int		perf_opened;	/* count of opened counters */
} perf_t;

/* used for table of perf events to gather */
typedef struct {
	int id;				/* stress-ng perf ID */
	unsigned long type;		/* perf types */
	unsigned long config;		/* perf type specific config */
} perf_info_t;

extern int perf_start(perf_t *p, const pid_t pid);
extern int perf_stop(perf_t *p);
extern void perf_counter(const perf_t *p, const int id, double *counter);

#endif
