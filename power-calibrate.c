/*
 * Copyright (C) 2014-2016 Canonical
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
 * Author: Colin Ian King <colin.king@canonical.com>
 *
 * Much of this code was derived from other GPL-2+ projects by the author
 * such as powerstat, fnotifystat and stress-ng.
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <sched.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include "perf.h"

#define MIN_RUN_DURATION	(10)	/* Minimum run duration */
#define DEFAULT_RUN_DURATION	(120)	/* Default duration */
#define SAMPLE_DELAY		(1)	/* Delay between samples in seconds */
#define START_DELAY_BATTERY 	(20)	/* Delay to wait before sampling */
#define START_DELAY_RAPL    	(0)	/* Delay to wait before sampling */
#define	RATE_ZERO_LIMIT		(0.001)	/* Less than this is a 0 power rate */
#define MAX_CPU_LOAD		(100)	/* Maximum CPU load */
#define DEFAULT_TIMEOUT		(10)	/* Zero load sleep duration */
#define CPU_ANY			(-1)

#define DETECT_DISCHARGING	(1)

#define OPT_DELAY		(0x00000001)
#define OPT_PROGRESS		(0x00000002)
#define OPT_CALIBRATE_EACH_CPU	(0x00000004)
#define OPT_RAPL		(0x00000008)

#define MAX_POWER_DOMAINS	(16)
#define MAX_POWER_VALUES	(MAX_POWER_DOMAINS + 1)

#define CPU_USER		(0)
#define CPU_NICE		(1)
#define CPU_SYS			(2)
#define CPU_IDLE		(3)
#define CPU_IOWAIT		(4)
#define CPU_IRQ			(5)
#define CPU_SOFTIRQ		(6)
#define CPU_INTR		(7)
#define CPU_CTXT		(8)
#define CPU_PROCS_RUN		(9)
#define CPU_PROCS_BLK		(10)
#define VOLTAGE_NOW		(11)
#define CURRENT_NOW 		(12)
#define PROC_FORK		(13)
#define PROC_EXEC		(14)
#define PROC_EXIT		(15)
#define BOGO_OPS		(16)
#define CPU_CYCLES		(17)
#define CPU_INSTRUCTIONS	(18)
#define POWER_NOW		(19)
#define POWER_DOMAIN_0          (20)
#define MAX_VALUES		(POWER_DOMAIN_0 + MAX_POWER_VALUES)

#define MWC_SEED_Z		(362436069UL)
#define MWC_SEED_W		(521288629UL)

#define SYS_CLASS_POWER_SUPPLY	"/sys/class/power_supply"
#define PROC_ACPI_BATTERY	"/proc/acpi/battery"

#define SYS_FIELD_VOLTAGE_NOW		"POWER_SUPPLY_VOLTAGE_NOW="
#define SYS_FIELD_POWER_NOW		"POWER_SUPPLY_POWER_NOW="
#define SYS_FIELD_ENERGY_NOW		"POWER_SUPPLY_ENERGY_NOW="
#define SYS_FIELD_CURRENT_NOW		"POWER_SUPPLY_CURRENT_NOW="
#define SYS_FIELD_CHARGE_NOW		"POWER_SUPPLY_CHARGE_NOW="
#define SYS_FIELD_STATUS_DISCHARGING  	"POWER_SUPPLY_STATUS=Discharging"

#if defined(__x86_64__) || defined(__x86_64) || \
    defined(__i386__) || defined(__i386)
#define RAPL_X86
#endif

/* Statistics entry */
typedef struct {
	double	value[MAX_VALUES];
	bool	inaccurate[MAX_VALUES];
} stats_t;

/* x,y data pair, for trend analysis */
typedef struct {
	double	x;
	double	y;

	double  voltage;
	int	cpu_id;
	int	cpus_used;
} value_t;

/* Bogo operation stats */
typedef struct {
	double	ops;
	uint8_t	padding[64];	/* Make ops not align on cache boundary */
} bogo_ops_t;

/* CPUs to use from -n option */
typedef struct cpu_info {
	struct cpu_info *next;
	int		cpu_id;	/* CPU number, 0 = first CPU */
	pid_t		pid;
#if defined(PERF_ENABLED)
	perf_t		perf;
#endif
} cpu_info_t;

/* CPU list */
typedef struct cpu_list {
	cpu_info_t	*head;
	cpu_info_t	*tail;
	uint32_t	count;
} cpu_list_t;

/* RAPL domain info */
typedef struct rapl_info {
	struct rapl_info *next;
	char 		*name;
	char 		*domain_name;
	double 		max_energy_uj;
	double 		last_energy_uj;
	double 		t_last;
	bool 		is_package;
} rapl_info_t;

typedef void (*func)(
	uint64_t param, const int instance, bogo_ops_t *bogo_ops);

static volatile bool stop_flag;			/* sighandler stop flag */
static int32_t opt_flags;			/* command options */
static char *app_name = "power-calibrate";	/* application name */

/*
 *  Attempt to catch a range of signals so
 *  we can clean
 */
static const int signals[] = {
	/* POSIX.1-1990 */
#ifdef SIGHUP
	SIGHUP,
#endif
#ifdef SIGINT
	SIGINT,
#endif
#ifdef SIGQUIT
	SIGQUIT,
#endif
#ifdef SIGFPE
	SIGFPE,
#endif
#ifdef SIGTERM
	SIGTERM,
#endif
#ifdef SIGUSR1
	SIGUSR1,
#endif
#ifdef SIGUSR2
	SIGUSR2,
	/* POSIX.1-2001 */
#endif
#ifdef SIGXCPU
	SIGXCPU,
#endif
#ifdef SIGXFSZ
	SIGXFSZ,
#endif
	/* Linux various */
#ifdef SIGIOT
	SIGIOT,
#endif
#ifdef SIGSTKFLT
	SIGSTKFLT,
#endif
#ifdef SIGPWR
	SIGPWR,
#endif
#ifdef SIGINFO
	SIGINFO,
#endif
#ifdef SIGVTALRM
	SIGVTALRM,
#endif
	-1,
};

/*
 *  units_to_str()
 *	units to strings
 */
static char *units_to_str(
	const double val,
	char *units,
	char *const buf,
	const size_t buflen)
{
	double v = (double)val;
	size_t i;
	static const char *scales[] = { "", "m", "µ", "n", "p", "f", "a", NULL };

	for (i = 0; i < 6; i++, v *= 1000) {
		if (v > 0.5)
			break;
	}
	snprintf(buf, buflen, "%.2f %s%s", v, scales[i], units);
	return buf;
}
/*
 *  value_to_str()
 *	values to strings
 */
static char *value_to_str(
	const double val,
	const bool inaccurate,
	char *const buf,
	const size_t buflen)
{
	if (inaccurate) {
		snprintf(buf, buflen, "-N/A-");
	} else {
		double v = (double)val;
		size_t i;
		static const char scales[] = " KMBTPE";

		for (i = 0; i < sizeof(scales) - 1; i++, v /= 1000) {
			if (v <= 500)
				break;
		}
		snprintf(buf, buflen, "%5.1f%c", v, scales[i]);
	}
	return buf;
}

/*
 *  timeval_to_double
 *	timeval to a double (in seconds)
 */
static inline double timeval_to_double(const struct timeval *const tv)
{
	return (double)tv->tv_sec + ((double)tv->tv_usec / 1000000.0);
}

/*
 *  double_to_timeval
 *	seconds in double to timeval
 */
static inline struct timeval double_to_timeval(const double val)
{
	struct timeval tv;

	tv.tv_sec = val;
	tv.tv_usec = (val - (time_t)val) * 1000000.0;

	return tv;
}

/*
 *  gettime_to_double()
 *	get time as a double
 */
static double gettime_to_double(void)
{
	struct timeval tv;

        if (gettimeofday(&tv, NULL) < 0) {
                fprintf(stderr, "gettimeofday failed: errno=%d (%s).\n",
                        errno, strerror(errno));
		return -1.0;
        }
        return timeval_to_double(&tv);
}

/*
 *  mwc()
 *      fast pseudo random number generator, see
 *      http://www.cse.yorku.ca/~oz/marsaglia-rng.html
 */
static inline uint32_t mwc(void)
{
	static uint32_t w = MWC_SEED_W, z = MWC_SEED_Z;

	z = 36969 * (z & 65535) + (z >> 16);
	w = 18000 * (w & 65535) + (w >> 16);
	return (z << 16) + w;
}

/*
 *  handle_sig()
 *	catch signal and flag a stop
 */
static void handle_sig(int dummy)
{
	(void)dummy;
	stop_flag = true;
}

/*
 *  set_affinity()
 *	set cpu affinity
 */
static int set_affinity(const int cpu)
{
	cpu_set_t mask;
	int ret;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	ret = sched_setaffinity(0, sizeof(mask), &mask);
	if (ret < 0) {
		fprintf(stderr, "sched_setffinity failed: errno=%d (%s).\n",
			errno, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 *  stress_cpu()
 *	stress CPU
 */
static void stress_cpu(
	const uint64_t cpu_load,
	const int instance,
	bogo_ops_t *bogo_ops)
{
	/*
	 * Normal use case, 100% load, simple spinning on CPU
	 */
	if (cpu_load == 100) {
		uint64_t i;
		for (;;) {
			for (i = 0; i < 1000000; i++) {
#if __GNUC__
				/* Stop optimising out */
				__asm__ __volatile__("");
#endif
				mwc();
				if (stop_flag) {
					bogo_ops[instance].ops += i;
					exit(EXIT_SUCCESS);
				}
			}
			bogo_ops[instance].ops += i;
		}
		exit(EXIT_SUCCESS);
	}

	if (cpu_load == 0) {
		for (;;) {
			sleep(DEFAULT_TIMEOUT);
			if (stop_flag)
				exit(EXIT_SUCCESS);
		}
		exit(EXIT_SUCCESS);
	}

	/*
	 * More complex percentage CPU utilisation.  This is
	 * not intended to be 100% accurate timing, it is good
	 * enough for most purposes.
	 */
	for (;;) {
		uint64_t i;
		double time_start, delay;
		struct timeval tv;

		time_start = gettime_to_double();
		for (i = 0; i < 1000000; i++) {
#if __GNUC__
			/* Stop optimising out */
			__asm__ __volatile__("");
#endif
			mwc();
			if (stop_flag) {
				bogo_ops[instance].ops += i;
				exit(EXIT_SUCCESS);
			}
		}
		bogo_ops[instance].ops += i;
		delay = gettime_to_double() - time_start;
		/* Must not calculate this with zero % load */
		delay *= (((100.0 / (double) cpu_load)) - 1.0);
		tv = double_to_timeval(delay);
		select(0, NULL, NULL, NULL, &tv);
	}
	exit(EXIT_SUCCESS);
}

/*
 *  stop_load()
 *	kill load child processes
 */
static void stop_load(cpu_list_t *cpu_list, const int total_procs)
{
	int i;
	cpu_info_t *c;

	/* Kill.. */
	for (c = cpu_list->head, i = 0; c && i < total_procs; c = c->next, i++) {
		if (c->pid > -1)
			kill(c->pid, SIGKILL);
	}
	/* And ensure we don't get zombies */
	for (c = cpu_list->head, i = 0; c && i < total_procs; c = c->next, i++) {
		if (c->pid > -1) {
			int status;

			waitpid(c->pid, &status, 0);
		}
		c->pid = -1;
	}
}

/*
 *  start_load()
 *	load system with some stress processes
 */
static void start_load(
	cpu_list_t *cpu_list,
	const int total_procs,
	const func load_func,
	const uint64_t param,
	bogo_ops_t *bogo_ops)
{
	int i;
	struct sigaction new_action;
	cpu_info_t *c;

	memset(&new_action, 0, sizeof(new_action));
	for (i = 0; signals[i] != -1; i++) {
		new_action.sa_handler = handle_sig;
		sigemptyset(&new_action.sa_mask);
		new_action.sa_flags = 0;

		(void)sigaction(signals[i], &new_action, NULL);
		(void)siginterrupt(signals[i], 1);
	}

	for (c = cpu_list->head, i = 0;
		c && i < total_procs; c = c->next, i++) {
		c->pid = fork();

		switch (c->pid) {
		case -1:
			fprintf(stderr, "Cannot fork, errno=%d (%s)\n",
				errno, strerror(errno));
			stop_load(cpu_list, i);
			exit(EXIT_FAILURE);
		case 0:
			/* Child */
			if (set_affinity(c->cpu_id) < 0)
				exit(0);
			load_func(param, i, bogo_ops);
			exit(0);
		default:
			break;
		}
	}
}

/*
 *  file_get()
 *	read a line from a /sys file
 */
static char *file_get(const char *const file)
{
	FILE *fp;
	char buffer[4096];

	if ((fp = fopen(file, "r")) == NULL)
		return NULL;

	if (fgets(buffer, sizeof(buffer), fp) == NULL) {
		(void)fclose(fp);
		return NULL;
	}
	(void)fclose(fp);

	return strdup(buffer);
}

/*
 *  get_time()
 *	Gather current time in buffer
 */
static void get_time(char *const buffer, const size_t buflen)
{
	struct tm tm;
	time_t now;

	now = time(NULL);
	if (now == ((time_t) -1)) {
		snprintf(buffer, buflen, "--:--:-- ");
	} else {
		(void)localtime_r(&now, &tm);
		snprintf(buffer, buflen, "%2.2d:%2.2d:%2.2d ",
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	}
}

/*
 *  stats_clear()
 *	clear stats
 */
static void stats_clear(stats_t *const stats)
{
	int i;

	for (i = 0; i < MAX_VALUES; i++) {
		stats->value[i] = 0.0;
		stats->inaccurate[i] = false;
	}
}

/*
 *  stats_clear_all()
 *	zero stats data
 */
static void stats_clear_all(stats_t *const stats, const int n)
{
	int i;

	for (i = 0; i < n; i++)
		stats_clear(&stats[i]);
}


/*
 *  stats_read()
 *	gather pertinent /proc/stat data
 */
static int stats_read(
	const int32_t num_cpus,
	stats_t *const stats,
	bogo_ops_t *bogo_ops)
{
	FILE *fp;
	char buf[4096];
	int i, j;

	static int indices[] = {
		CPU_USER, CPU_NICE, CPU_SYS, CPU_IDLE,
		CPU_IOWAIT, CPU_IRQ, CPU_SOFTIRQ, CPU_CTXT,
		CPU_INTR, CPU_PROCS_RUN, CPU_PROCS_BLK, -1
	};

	for (i = 0; (j = indices[i]) != -1; i++) {
		stats->value[j] = 0.0;
		stats->inaccurate[j] = true;
	}

	if ((fp = fopen("/proc/stat", "r")) == NULL) {
		fprintf(stderr, "Cannot read /proc/stat, errno=%d (%s).\n",
			errno, strerror(errno));
		return -1;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strncmp(buf, "cpu ", 4) == 0)
			if (sscanf(buf, "%*s %15lf %15lf %15lf %15lf %15lf %15lf %15lf",
			    &(stats->value[CPU_USER]),
			    &(stats->value[CPU_NICE]),
			    &(stats->value[CPU_SYS]),
			    &(stats->value[CPU_IDLE]),
			    &(stats->value[CPU_IOWAIT]),
			    &(stats->value[CPU_IRQ]),
			    &(stats->value[CPU_SOFTIRQ])) == 7) {
				stats->inaccurate[CPU_USER] = false;
				stats->inaccurate[CPU_NICE] = false;
				stats->inaccurate[CPU_SYS] = false;
				stats->inaccurate[CPU_IDLE] = false;
				stats->inaccurate[CPU_IOWAIT] = false;
				stats->inaccurate[CPU_IRQ] = false;
				stats->inaccurate[CPU_SOFTIRQ] = false;
			}
		if (strncmp(buf, "ctxt ", 5) == 0)
			if (sscanf(buf, "%*s %15lf", &(stats->value[CPU_CTXT])) == 1)
				stats->inaccurate[CPU_CTXT] = false;
		if (strncmp(buf, "intr ", 5) == 0)
			if (sscanf(buf, "%*s %15lf", &(stats->value[CPU_INTR])) == 1)
				stats->inaccurate[CPU_INTR] = false;
		if (strncmp(buf, "procs_running ", 14) == 0)
			if (sscanf(buf, "%*s %15lf",
				   &(stats->value[CPU_PROCS_RUN])) == 1)
				stats->inaccurate[CPU_PROCS_RUN] = false;
		if (strncmp(buf, "procs_blocked ", 14) == 0)
			if (sscanf(buf, "%*s %15lf",
				   &(stats->value[CPU_PROCS_BLK])) == 1)
				stats->inaccurate[CPU_PROCS_BLK] = false;
	}
	(void)fclose(fp);

	stats->value[BOGO_OPS] = 0;
	stats->inaccurate[BOGO_OPS] = false;
	for (i = 0; i < num_cpus; i++) {
		stats->value[BOGO_OPS] += bogo_ops[i].ops;
	}

	return 0;
}

/*
 *  stats_sane()
 *	check if stats are accurate and calculate a
 *	sane -ve delta
 */
static double stats_sane(
	const stats_t *const s1,
	const stats_t *const s2,
	const int index)
{
	double ret;

	/* Discard inaccurate or empty stats */
	if (s1->inaccurate[index] || s2->inaccurate[index])
		return 0.0;

	/*
	 *  On Nexus 4 we occasionally get idle time going backwards so
	 *  work around this by ensuring we don't get -ve deltas.
	 */
	ret = s2->value[index] - s1->value[index];
	return ret < 0.0 ? 0.0 : ret;
}

#define INACCURATE(s1, s2, index)       \
	(s1->inaccurate[index] | s2->inaccurate[index])

/*
 *  stats_gather()
 *	gather up delta between last stats and current to get
 * 	some form of per sample accounting calculated.
 */
static bool stats_gather(
	cpu_list_t *cpu_list,
	const uint32_t sample_delay,
	const stats_t *const s1,
	const stats_t *const s2,
	stats_t *const res)
{
	double total;
	int i, j;
	bool inaccurate = false;

	static int indices[] = {
		CPU_USER, CPU_NICE, CPU_SYS, CPU_IDLE,
		CPU_IOWAIT, -1
	};

	res->value[CPU_USER]    = stats_sane(s1, s2, CPU_USER);
	res->value[CPU_NICE]    = stats_sane(s1, s2, CPU_NICE);
	res->value[CPU_SYS]     = stats_sane(s1, s2, CPU_SYS);
	res->value[CPU_IDLE]    = stats_sane(s1, s2, CPU_IDLE);
	res->value[CPU_IOWAIT]  = stats_sane(s1, s2, CPU_IOWAIT);
	res->value[CPU_IRQ]     = stats_sane(s1, s2, CPU_IRQ);
	res->value[CPU_SOFTIRQ] = stats_sane(s1, s2, CPU_SOFTIRQ);
	res->value[CPU_CTXT]	= stats_sane(s1, s2, CPU_CTXT);
	res->value[CPU_INTR]	= stats_sane(s1, s2, CPU_INTR);
	res->value[BOGO_OPS]	= stats_sane(s1, s2, BOGO_OPS);

	res->value[CPU_CYCLES]  = 0.0;
	res->inaccurate[CPU_CYCLES] = false;
	res->value[CPU_INSTRUCTIONS]  = 0.0;
	res->inaccurate[CPU_INSTRUCTIONS] = false;

#if defined(PERF_ENABLED)
	{
		cpu_info_t *c;

		for (c = cpu_list->head; c; c = c->next) {
			double value;

			perf_counter(&c->perf, PERF_HW_CPU_CYCLES, &value);
			if (value > 0.0)
				res->value[CPU_CYCLES] += value;
			perf_counter(&c->perf, PERF_HW_INSTRUCTIONS, &value);
			if (value > 0.0)
				res->value[CPU_INSTRUCTIONS] += value;
		}
	}
#else
	(void)cpu_list;
#endif

	for (i = 0; (j = indices[i]) != -1; i++)
		inaccurate |= (s1->inaccurate[j] | s2->inaccurate[j]);

	total = res->value[CPU_USER] + res->value[CPU_NICE] +
		res->value[CPU_SYS] + res->value[CPU_IDLE] +
		res->value[CPU_IOWAIT];

	/*
	 * This should not happen, but we need to avoid division
	 * by zero or weird results if the data is deemed valid
	 */
	if (!inaccurate && total <= 0.0)
		return false;

	for (i = 0; (j = indices[i]) != -1; i++) {
		res->value[j] = (INACCURATE(s1, s2, j) || (total <= 0.0)) ?
		NAN : (100.0 * res->value[j]) / total;
	}
	res->value[CPU_CTXT] = (INACCURATE(s1, s2, CPU_CTXT) ||
				(sample_delay <= 0.0)) ?
		NAN : res->value[CPU_CTXT] / sample_delay;
	res->value[CPU_INTR] = (INACCURATE(s1, s2, CPU_INTR) ||
				(sample_delay <= 0.0)) ?
		NAN : res->value[CPU_INTR] / sample_delay;
	res->value[BOGO_OPS] = (INACCURATE(s1, s2, BOGO_OPS) ||
				(sample_delay <= 0.0)) ?
		NAN : res->value[BOGO_OPS] / sample_delay;
	res->value[CPU_PROCS_RUN] = s2->inaccurate[CPU_PROCS_RUN] ?
		NAN : s2->value[CPU_PROCS_RUN];
	res->value[CPU_PROCS_BLK] = s2->inaccurate[CPU_PROCS_BLK] ?
		NAN : s2->value[CPU_PROCS_BLK];

	return true;
}

/*
 *  stats_headings()
 *	dump heading columns
 */
static void stats_headings(const char *test)
{
	printf("%10.10s  User   Sys  Idle  Run  Ctxt/s  IRQ/s  Ops/s "
#if defined(PERF_ENABLED)
		"Cycl/s Inst/s "
#endif
		" Watts\n", test);
}

/*
 *  stats_print()
 *	print out statistics with accuracy depending if it's a summary or not
 */
static void stats_print(
	const char *const prefix,
	const bool summary,
	const stats_t *const s)
{
	char buf[10], bogo_ops[10];
#if defined(PERF_ENABLED)
	char cpu_cycles[10], cpu_instr[10];
#endif
	char *fmt;

	if (summary) {
		if (s->inaccurate[POWER_NOW])
			snprintf(buf, sizeof(buf), "-N/A-");
		else
			snprintf(buf, sizeof(buf), "%6.3f", s->value[POWER_NOW]);
	} else {
		snprintf(buf, sizeof(buf), "%6.3f%s", s->value[POWER_NOW],
			s->inaccurate[POWER_NOW] ? "E" : "");
	}

	value_to_str(s->value[BOGO_OPS], s->inaccurate[BOGO_OPS],
		bogo_ops, sizeof(bogo_ops));
#if defined(PERF_ENABLED)
	value_to_str(s->value[CPU_CYCLES], s->inaccurate[CPU_CYCLES],
		cpu_cycles, sizeof(cpu_cycles));
	value_to_str(s->value[CPU_INSTRUCTIONS], s->inaccurate[CPU_INSTRUCTIONS],
		cpu_instr, sizeof(cpu_instr));
#endif

	fmt = summary ?
#if defined(PERF_ENABLED)
		"%10.10s %5.1f %5.1f %5.1f %4.1f %7.1f %6.1f %6s %6s %6s %s\n" :
		"%10.10s %5.1f %5.1f %5.1f %4.0f %7.0f %6.0f %6s %6s %6s %s\n";
#else
		"%10.10s %5.1f %5.1f %5.1f %4.1f %7.1f %6.1f %6s %s\n" :
		"%10.10s %5.1f %5.1f %5.1f %4.0f %7.0f %6.0f %6s %s\n";
#endif
	printf(fmt,
		prefix,
		s->value[CPU_USER], s->value[CPU_SYS], s->value[CPU_IDLE],
		s->value[CPU_PROCS_RUN], s->value[CPU_CTXT],
		s->value[CPU_INTR],
		bogo_ops,
#if defined(PERF_ENABLED)
		cpu_cycles, cpu_instr,
#endif
		buf);
}

/*
 *  stats_average_stddev_min_max()
 *	calculate average, std deviation, min and max
 */
static void stats_average_stddev_min_max(
	const stats_t *const stats,
	const int num,
	stats_t *const average,
	stats_t *const stddev)
{
	int i, j, valid;

	for (j = 0; j < MAX_VALUES; j++) {
		double total = 0.0;

		for (valid = 0, i = 0; i < num; i++) {
			if (!stats[i].inaccurate[j]) {
				total += stats[i].value[j];
				valid++;
			}
		}

		if (valid) {
			average->value[j] = total / (double)valid;
			total = 0.0;
			for (i = 0; i < num; i++) {
				if (!stats[i].inaccurate[j]) {
					double diff = (double)stats[i].value[j]
						- average->value[j];
					diff = diff * diff;
					total += diff;
				}
			}
			stddev->value[j] = total / (double)num;
			stddev->value[j] = sqrt(stddev->value[j]);
		} else {
			average->inaccurate[j] = true;
			stddev->inaccurate[j] = true;

			average->value[j] = 0.0;
			stddev->value[j] = 0.0;
		}
	}
}

/*
 *  power_get_sys_fs()
 *	get power discharge rate from battery via /sys interface
 */
static int power_get_sys_fs(
	stats_t *stats,
	bool *const discharging,
	bool *const inaccurate)
{
	DIR *dir;
	struct dirent *dirent;
	double total_watts = 0.0;
	double average_voltage = 0.0;
	int n = 0;

	stats->value[POWER_NOW] = 0.0;
	stats->value[VOLTAGE_NOW] = 0.0;
	stats->value[CURRENT_NOW] = 0.0;
	*discharging = false;
	*inaccurate = true;

	if ((dir = opendir(SYS_CLASS_POWER_SUPPLY)) == NULL) {
		fprintf(stderr, "Machine does not have %s, cannot run the test.\n",
			SYS_CLASS_POWER_SUPPLY);
		return -1;
	}

	do {
		dirent = readdir(dir);
		if (dirent && strlen(dirent->d_name) > 2) {
			char path[PATH_MAX];
			char *data;
			int  val;
			FILE *fp;

			/* Check that type field matches the expected type */
			snprintf(path, sizeof(path), "%s/%s/type",
				SYS_CLASS_POWER_SUPPLY, dirent->d_name);
			if ((data = file_get(path)) != NULL) {
				bool mismatch = (strstr(data, "Battery") == NULL);
				free(data);
				if (mismatch)
					continue;	/* type don't match, skip this entry */
			} else
				continue;		/* can't check type, skip this entry */

			snprintf(path, sizeof(path), "%s/%s/uevent",
				SYS_CLASS_POWER_SUPPLY, dirent->d_name);
			if ((fp = fopen(path, "r")) == NULL) {
				fprintf(stderr, "Battery %s present but under supported - "
					"no state present.", dirent->d_name);
				(void)closedir(dir);
				return -1;
			} else {
				char buffer[4096];
				double voltage = 0.0;
				double amps_rate = 0.0;
				double watts_rate = 0.0;

				while (fgets(buffer, sizeof(buffer)-1, fp) != NULL) {
					if (strstr(buffer, SYS_FIELD_STATUS_DISCHARGING))
						*discharging = true;

					if (strstr(buffer, SYS_FIELD_CURRENT_NOW) &&
					    strlen(buffer) > sizeof(SYS_FIELD_CURRENT_NOW) - 1) {
						sscanf(buffer + sizeof(SYS_FIELD_CURRENT_NOW) - 1, "%12d", &val);
						amps_rate = (double)val / 1000000.0;
					}

					if (strstr(buffer, SYS_FIELD_POWER_NOW) &&
					    strlen(buffer) > sizeof(SYS_FIELD_POWER_NOW) - 1) {
						sscanf(buffer + sizeof(SYS_FIELD_POWER_NOW) - 1, "%12d", &val);
						watts_rate = (double)val / 1000000.0;
					}

					if (strstr(buffer, SYS_FIELD_VOLTAGE_NOW) &&
					    strlen(buffer) > sizeof(SYS_FIELD_VOLTAGE_NOW) - 1) {
						sscanf(buffer + sizeof(SYS_FIELD_VOLTAGE_NOW) - 1, "%12d", &val);
						voltage = (double)val / 1000000.0;
					}
				}
				average_voltage += voltage;
				total_watts     += watts_rate + voltage * amps_rate;
				n++;
				(void)fclose(fp);
			}
		}
	} while (dirent);

	(void)closedir(dir);

#if DETECT_DISCHARGING
	if (! *discharging) {
		printf("Machine is not discharging, cannot measure power usage.\n");
#if defined(RAPL_X86)
		printf("Alternatively, use the RAPL power measuring option '-R'.\n");
#endif
		return -1;
	}
#else
	*discharging = true;
#endif

	/*
 	 *  If the battery is helpful it supplies the rate already, in which case
	 *  we know the results from the battery are as good as we can and we don't
	 *  have to figure out anything from capacity change over time.
	 */
	if (total_watts > RATE_ZERO_LIMIT) {
		stats->value[POWER_NOW] = total_watts;
		stats->value[VOLTAGE_NOW] = average_voltage / (double)n;
		stats->value[CURRENT_NOW] = total_watts / average_voltage;
		*inaccurate = (total_watts < 0.0);
		return 0;
	}

	/*
	 *  Rate not known, battery is less than useful.  We could
	 *  calculate it from delta in charge, but that is not accurate
	 *  for this kind of use case, so error out instead.
	 */
	fprintf(stderr, "The battery just provided charge data which is not accurate enough.\n");
	return -1;
}

/*
 *  power_get_proc_acpi()
 *	get power discharge rate from battery via /proc/acpi interface
 */
static int power_get_proc_acpi(
	stats_t *stats,
	bool *const discharging,
	bool *const inaccurate)
{
	DIR *dir;
	FILE *file;
	struct dirent *dirent;
	char filename[PATH_MAX];
	double total_watts = 0.0;
	double average_voltage = 0.0;
	int n = 0;

	stats->value[POWER_NOW] = 0.0;
	stats->value[VOLTAGE_NOW] = 0.0;
	stats->value[CURRENT_NOW] = 0.0;
	*discharging = false;
	*inaccurate = true;

	if ((dir = opendir(PROC_ACPI_BATTERY)) == NULL) {
		fprintf(stderr, "Machine does not have %s, cannot run the test.\n",
			PROC_ACPI_BATTERY);
		return -1;
	}

	while ((dirent = readdir(dir))) {
		double voltage = 0.0;
		double amps_rate = 0.0;
		double watts_rate = 0.0;
		char buffer[4096];
		char *ptr;

		if (strlen(dirent->d_name) < 3)
			continue;

		sprintf(filename, "/proc/acpi/battery/%s/state", dirent->d_name);
		if ((file = fopen(filename, "r")) == NULL)
			continue;

		memset(buffer, 0, sizeof(buffer));
		while (fgets(buffer, sizeof(buffer), file) != NULL) {
			if (strstr(buffer, "present:") &&
			    strstr(buffer, "no"))
				break;

			if (strstr(buffer, "charging state:") &&
			    (strstr(buffer, "discharging") || strstr(buffer, "critical")))
				*discharging = true;

			ptr = strchr(buffer, ':');
			if (ptr) {
				ptr++;
				if (strstr(buffer, "present voltage"))
					voltage = strtoull(ptr, NULL, 10) / 1000.0;

				if (strstr(buffer, "present rate")) {
					if (strstr(ptr, "mW"))
						watts_rate = strtoull(ptr, NULL, 10) / 1000.0 ;
					if (strstr(ptr, "mA"))
						amps_rate = strtoull(ptr, NULL, 10) / 1000.0;
				}
			}
		}
		(void)fclose(file);

		/*
		 * Some HP firmware is broken and has an undefined
		 * 'present voltage' field and instead returns this in
		 * the design_voltage field, so work around this.
		 */
		if (voltage == 0.0) {
			sprintf(filename, "/proc/acpi/battery/%s/info",
				dirent->d_name);
			if ((file = fopen(filename, "r")) != NULL) {
				while (fgets(buffer, sizeof(buffer), file) != NULL) {
					ptr = strchr(buffer, ':');
					if (ptr) {
						ptr++;
						if (strstr(buffer, "design voltage:")) {
							voltage = strtoull(ptr, NULL, 10) / 1000.0;
							break;
						}
					}
				}
				(void)fclose(file);
			}
		}

		average_voltage += voltage;
		total_watts     += watts_rate + voltage * amps_rate;
		n++;
	}
	(void)closedir(dir);

#if DETECT_DISCHARGING
	if (! *discharging) {
		printf("Machine is indicating it is not discharging and hence "
		       "we cannot measure power usage.\n");
		return -1;
	}
#else
	*discharging = true;
#endif

	/*
 	 * If the battery is helpful it supplies the rate already, in which
	 * case we know the results from the battery are as good as we can
	 * and we don't have to figure out anything from capacity change over
	 * time.
	 */
	if (total_watts > RATE_ZERO_LIMIT) {
		stats->value[POWER_NOW] = total_watts;
		stats->value[VOLTAGE_NOW] = average_voltage / (double)n;
		stats->value[CURRENT_NOW] = total_watts / average_voltage;
		*inaccurate = (total_watts < 0.0);
		return 0;
	}

	/*
	 *  Rate not known, battery is less than useful.  We could
	 *  calculate it from delta in charge, but that is not accurate
	 *  for this kind of use case, so error out instead.
	 */
	fprintf(stderr, "The battery just provided charge data which is not accurate enough.\n");
	return -1;
}

#if defined(RAPL_X86)

/*
 *  rapl_free_list()
 *	free RAPL list
 */
static void rapl_free_list(rapl_info_t *rapl_list)
{
	rapl_info_t *rapl = rapl_list;

	while (rapl) {
		rapl_info_t *next = rapl->next;

		free(rapl->name);
		free(rapl->domain_name);
		free(rapl);
		rapl = next;
	}
}

/*
 *  rapl_get_domains()
 */
static int rapl_get_domains(rapl_info_t **rapl_list)
{
	DIR *dir;
        struct dirent *entry;
	int n = 0;

	dir = opendir("/sys/class/powercap");
	if (dir == NULL) {
		printf("Device does not have RAPL, cannot measure power usage.\n");
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		FILE *fp;
		rapl_info_t *rapl;

		/* Ignore non Intel RAPL interfaces */
		if (strncmp(entry->d_name, "intel-rapl", 10))
			continue;

		if ((rapl = calloc(1, sizeof(*rapl))) == NULL) {
			fprintf(stderr, "Cannot allocate RAPL information.\n");
			closedir(dir);
			return -1;
		}
		if ((rapl->name = strdup(entry->d_name)) == NULL) {
			fprintf(stderr, "Cannot allocate RAPL name information.\n");
			closedir(dir);
			free(rapl);
			return -1;
		}
		snprintf(path, sizeof(path),
			"/sys/class/powercap/%s/max_energy_range_uj",
			entry->d_name);

		rapl->max_energy_uj = 0.0;
		if ((fp = fopen(path, "r")) != NULL) {
			if (fscanf(fp, "%lf\n", &rapl->max_energy_uj) != 1)
				rapl->max_energy_uj = 0.0;
			(void)fclose(fp);
		}
		snprintf(path, sizeof(path),
			"/sys/class/powercap/%s/name",
			entry->d_name);

		rapl->domain_name = NULL;
		if ((fp = fopen(path, "r")) != NULL) {
			char domain_name[128];

			if (fgets(domain_name, sizeof(domain_name), fp) != NULL) {
				domain_name[strcspn(domain_name, "\n")] = '\0';
				rapl->domain_name = strdup(domain_name);
			}
			(void)fclose(fp);
		}
		if (rapl->domain_name == NULL) {
			free(rapl->name);
			free(rapl);
			continue;
		}

		rapl->is_package = (strncmp(rapl->domain_name, "package-", 8) == 0);
		rapl->next = *rapl_list;
		*rapl_list = rapl;
		n++;
	}
	(void)closedir(dir);

	if (!n)
		printf("Device does not have any RAPL domains, cannot power measure power usage.\n");
	return n;
}

/*
 *  power_get_rapl()
 *	get power discharge rate from battery via the RAPL interface
 */
static int power_get_rapl(
	rapl_info_t *rapl_list,
	stats_t *stats,
	bool *const discharging)
{
	double t_now;
	static bool first = true;
	rapl_info_t *rapl;
	int n = 0;

	/* Assume OK until found otherwise */
	stats->inaccurate[POWER_NOW] = false;
	stats->value[POWER_NOW] = 0.0;
	*discharging = false;

	t_now = gettime_to_double();

	for (rapl = rapl_list; rapl; rapl = rapl->next) {
		char path[PATH_MAX];
		FILE *fp;
		double ujoules;

		snprintf(path, sizeof(path),
			"/sys/class/powercap/%s/energy_uj",
			rapl->name);

		if ((fp = fopen(path, "r")) == NULL)
			continue;

		if (fscanf(fp, "%lf\n", &ujoules) == 1) {
			double t_delta = t_now - rapl->t_last;
			double last_energy_uj = rapl->last_energy_uj;

			rapl->t_last = t_now;

			/* Wrapped around since last time? */
			if (ujoules - rapl->last_energy_uj < 0.0) {
				rapl->last_energy_uj = ujoules;
				ujoules += rapl->max_energy_uj;
			} else {
				rapl->last_energy_uj = ujoules;
			}

			if (first || (t_delta <= 0.0)) {
				stats->value[POWER_DOMAIN_0 + n] = 0.0;
				stats->inaccurate[POWER_NOW] = true;
			} else {
				stats->value[POWER_DOMAIN_0 + n] =
					(ujoules - last_energy_uj) /
					(t_delta * 1000000.0);
			}
			if (rapl->is_package)
				stats->value[POWER_NOW] += stats->value[POWER_DOMAIN_0 + n];
			n++;
			*discharging = true;
		}
		fclose(fp);
	}

	if (first) {
		stats->inaccurate[POWER_NOW] = true;
		first = false;
	}

	if (!n) {
		printf("Device does not have any RAPL domains, cannot power measure power usage.\n");
		return -1;
	}
	return 0;
}
#endif

/*
 *  power_get()
 *	get consumption rate
 */
static int power_get(
	rapl_info_t *rapl_list,
	stats_t *stats,
	bool *const discharging,
	bool *const inaccurate)
{
	struct stat buf;
	int i;

	for (i = POWER_NOW; i < MAX_VALUES; i++) {
		stats->value[i] = i;
		stats->inaccurate[i] = 0.0;
	}
#if defined(RAPL_X86)
	if (opt_flags & OPT_RAPL)
		return power_get_rapl(rapl_list, stats, discharging);
#else
	(void)rapl_list;
#endif

	if ((stat(SYS_CLASS_POWER_SUPPLY, &buf) != -1) &&
	    S_ISDIR(buf.st_mode))
		return power_get_sys_fs(stats, discharging, inaccurate);

	if ((stat(PROC_ACPI_BATTERY, &buf) != -1) &&
	    S_ISDIR(buf.st_mode))
		return power_get_proc_acpi(stats, discharging, inaccurate);

	fprintf(stderr, "Machine does not seem to have a battery, cannot measure power.\n");
	return -1;
}

/*
 *  not_discharging()
 *	returns true if battery is not discharging
 */
static inline bool not_discharging(rapl_info_t *rapl_list)
{
	stats_t dummy;
	bool discharging, inaccurate;

	return power_get(rapl_list, &dummy, &discharging, &inaccurate) < 0;
}

/*
 *   monitor()
 *	monitor system activity and power consumption
 */
static inline int monitor(
	const int num_cpus,
	cpu_list_t *cpu_list,
	rapl_info_t *rapl_list,
	const int start_delay,
	const int sample_delay,
	const int max_readings,
	const char *test,
	const double percent_each,
	const double percent,
	bogo_ops_t *bogo_ops,
	double *busy,
	double *power,
	double *voltage,
	double *ops,
	double *cpu_cycles,
	double *cpu_instr)
{
	int readings = 0, i;
	int64_t t = 1;
	stats_t *stats, s1, s2, average, stddev;
	bool discharging, dummy_inaccurate;
	double time_start;

	if (start_delay > 0) {
		stats_t dummy;
		/* Gather up initial data */
		for (i = 0; i < start_delay; i++) {
			if (opt_flags & OPT_PROGRESS) {
				fprintf(stdout, "%10.10s: test warming up %5.1f%%..\r",
					test, 100.0 * i / start_delay);
				fflush(stdout);
			}
			if (power_get(rapl_list, &dummy, &discharging, &dummy_inaccurate) < 0)
				return -1;
			if (sleep(1) || stop_flag)
				return -1;
			if (!discharging)
				return -1;
		}
	}

	if (not_discharging(rapl_list))
		return -1;

	if ((stats = calloc(max_readings, sizeof(stats_t))) == NULL) {
		fprintf(stderr, "Cannot allocate statistics table.\n");
		return -1;
	}

	stats_clear_all(stats, max_readings);
	stats_clear(&average);
	stats_clear(&stddev);

	if ((time_start = gettime_to_double()) < 0.0) {
		free(stats);
		return -1;
	}

	if (stats_read(num_cpus, &s1, bogo_ops) < 0) {
		free(stats);
		return -1;
	}

	while (!stop_flag && (readings < max_readings)) {
		int ret = 0;
		double secs, time_now;
		struct timeval tv;
#if defined(PERF_ENABLED)
		cpu_info_t *c;
#endif

		if ((time_now = gettime_to_double()) < 0.0) {
			free(stats);
			return -1;
		}
#if defined(PERF_ENABLED)
		for (c = cpu_list->head; c; c = c->next)
			perf_start(&c->perf, c->pid);
#endif

		if (opt_flags & OPT_PROGRESS) {
			double progress = readings * 100.0 / max_readings;
			fprintf(stdout, "%10.10s: test progress %5.1f%% (total progress %6.2f%%)\r",
				test, progress,
				(progress * percent_each / 100.0) + percent);
			fflush(stdout);
		}

		secs = time_start + ((double)t * sample_delay) - time_now;
		if (secs < 0.0)
			goto sample_now;
		tv = double_to_timeval(secs);
		ret = select(0, NULL, NULL, NULL, &tv);
		if (ret < 0) {
#if defined(PERF_ENABLED)
			for (c = cpu_list->head; c; c = c->next)
				perf_stop(&c->perf);
#endif
			if (errno == EINTR)
				break;
			fprintf(stderr,"select failed: errno=%d (%s).\n",
				errno, strerror(errno));
			free(stats);
			return -1;
		}
sample_now:
		if (stop_flag)
			break;
		/* Time out, so measure some more samples */
		if (ret == 0) {
			char tmbuffer[10];
			bool discharging;

#if defined(PERF_ENABLED)
			for (c = cpu_list->head; c; c = c->next)
				perf_stop(&c->perf);
#endif

			get_time(tmbuffer, sizeof(tmbuffer));
			if (stats_read(num_cpus, &s2, bogo_ops) < 0)
				goto tidy_exit;

			/*
			 *  Total ticks was zero, something is broken,
			 *  so re-sample
			 */
			if (!stats_gather(cpu_list, sample_delay, &s1, &s2, &stats[readings])) {
				stats_clear(&stats[readings]);
				if (stats_read(num_cpus, &s1, bogo_ops) < 0)
					goto tidy_exit;
				continue;
			}

			if (power_get(rapl_list, &stats[readings], &discharging,
				      &stats[readings].inaccurate[POWER_NOW]) < 0)
				goto tidy_exit;

			if (!discharging)
				goto tidy_exit; /* No longer discharging! */

			readings++;
			s1 = s2;
			t++;
		}
	}

	/*
	 * Stats now gathered, calculate averages, stddev, min
	 * and max and display
	 */
	stats_average_stddev_min_max(stats, readings, &average, &stddev);
	if (readings > 0) {
		stats_print(test, true, &average);
	}
	*busy = 100.0 - average.value[CPU_IDLE];
	*power = average.value[POWER_NOW];
	*voltage = average.value[VOLTAGE_NOW];
	*ops = average.value[BOGO_OPS];
	*cpu_cycles = average.value[CPU_CYCLES];
	*cpu_instr = average.value[CPU_INSTRUCTIONS];

	free(stats);
	return 0;

tidy_exit:
	free(stats);
	return -1;

}

/*
 *  calc_trend()
 *	calculate linear trendline - compute gradient, y intercept and
 *	coefficient of determination.
 */
static int calc_trend(
	const char *heading,
	const int cpus_used,
	const value_t *values,
	const int num_values,
	double *gradient,
	double *intercept,
	double *r2)
{
	int i, n = 0;
	double a = 0.0, b, c = 0.0, d, e, f;
	double sum_x = 0.0, sum_y = 0.0;
	double sum_x2 = 0.0, sum_y2 = 0.0;
	double sum_xy = 0.0, r;
	double n1, n2;

	for (i = 0; i < num_values; i++) {
		if (cpus_used == CPU_ANY || cpus_used >= values[i].cpus_used) {
			a += values[i].x * values[i].y;
			sum_x += values[i].x;
			sum_y += values[i].y;
			sum_xy += (values[i].x * values[i].y);
				sum_x2 += (values[i].x * values[i].x);
			sum_y2 += (values[i].y * values[i].y);
			n++;
		}
	}

	if (!n) {
		printf("%s: Cannot perform trend analysis, zero samples.\n", heading);
		return -1;
	}

	/*
	 * Coefficient of determination R^2,
	 * http://mathbits.com/MathBits/TISection/Statistics2/correlation.htm
	 */
	n1 = sqrt(((double)n * sum_x2) - (sum_x * sum_x));
	n2 = sqrt(((double)n * sum_y2) - (sum_y * sum_y));
	d = n1 * n2;
	if (d <= 0.0) {
		printf("%s: Cannot perform trend analysis\n"
			"(the coefficient of determination is not invalid).\n", heading);
		return -1;
	}
	r  = (((double)n * sum_xy) - (sum_x * sum_y)) / d;
	*r2 = r * r;

	/*
	 *  Regression Equation(y) = a + bx
         *  Slope = (NΣXY - (ΣX)(ΣY)) / (NΣX2 - (ΣX)2)
 	 *  Intercept = (ΣY - b(ΣX)) / N
	 */
	a *= (double)n;
	b = sum_x * sum_y;
	c = sum_x2 * (double)n;
	d = sum_x * sum_x;
	e = sum_y;
	*gradient = (a - b) / (c - d);
	f = (*gradient) * sum_x;
	*intercept = (e - f) / (double)n;

	return 0;
}


/*
 *  show_help()
 *	simple help
 */
static void show_help(char *const argv[])
{
	printf("%s, version %s\n\n", app_name, VERSION);
	printf("usage: %s [options]\n", argv[0]);
	printf(" -d secs  specify delay before starting\n");
	printf(" -h show  help\n");
	printf(" -n cpus  specify number of CPUs to exercise\n");
	printf(" -o file  output results into YAML formatted file\n");
	printf(" -p       show progress\n");
	printf(" -r secs  specify run duration in seconds of each test cycle\n");
#if defined(RAPL_X86)
	printf(" -R       use Intel RAPL per CPU package data to measure Watts\n");
#endif
	printf(" -s num   number of samples (tests) per CPU for CPU calibration\n");
	printf("\nExample: power-calibrate  -R -r 20 -d 5 -s 21 -n 0 -p\n");
}

/*
 *  coefficient_r2()
 *	give some idea of r2
 */
static const char *coefficient_r2(const double r2)
{
	if (r2 < 0.4)
		return "very weak";
	if (r2 < 0.75)
		return "weak";
	if (r2 < 0.80)
		return "fair";
	if (r2 < 0.90)
		return "good";
	if (r2 < 0.95)
		return "strong";
	if (r2 < 1.0)
		return "very strong";
	return "perfect";
}

/*
 *  dump_yaml_values()
 *	output yaml formatted data
 */
static void dump_yaml_values(
	FILE *yaml,
	const char *heading,
	const char *field,
	const double value,
	const double r2)
{
	if (!yaml)
		return;

	fprintf(yaml, "  %s:\n", heading);
	fprintf(yaml, "    %s: %e\n", field, value);
	fprintf(yaml, "    r-squared: %f\n", r2);
}

/*
 *  dump_yaml_misc()
 *	output json misc test data
 */
static void dump_yaml_misc(FILE *yaml)
{
	time_t now;
	struct tm tm;
	struct utsname buf;

	now = time(NULL);
	if (now == ((time_t) -1)) {
		memset(&tm, 0, sizeof(tm));
	} else {
		localtime_r(&now, &tm);
	}

	memset(&buf, 0, sizeof(struct utsname));
	uname(&buf);

	fprintf(yaml, "  test-run:\n");
	fprintf(yaml, "    date: %2.2d/%2.2d/%-2.2d\n",
		tm.tm_mday, tm.tm_mon + 1, (tm.tm_year+1900) % 100);
	fprintf(yaml, "    time: %-2.2d:%-2.2d:%-2.2d\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	fprintf(yaml, "    sysname: %s\n", buf.sysname);
	fprintf(yaml, "    nodename: %s\n", buf.nodename);
	fprintf(yaml, "    release: %s\n", buf.release);
	fprintf(yaml, "    machine: %s\n", buf.machine);
}

/*
 *  calc_average_voltage()
 *	calculatate average voltage on CPU ids
 */
static double calc_average_voltage(
	const int cpus_used,
	value_t *values,
	const int num_values)
{
	int i, n = 0;
	double average = 0.0;

	for (i = 0; i < num_values; i++) {
		if (cpus_used == CPU_ANY || cpus_used >= values[i].cpus_used) {
			average += values[i].voltage;
			n++;
		}
	}
	average = (n == 0) ? 0.0 : average / n;

	if (average == 0.0)
		return -1;

	return average;
}

/*
 *  show_trend()
 *	Show power trend
 */
static void show_trend(
	FILE *yaml,
	const int cpus_used,
	value_t *values,
	const int num_values,
	const char *unit,
	const char *each,
	const char *heading,
	const char *field,
	bool power)
{
	char watts[16];
	double gradient, intercept, r2;
	double average_voltage =
		calc_average_voltage(cpus_used, values, num_values);

	if (calc_trend(heading, cpus_used, values, num_values, &gradient, &intercept, &r2) < 0)
		return;

	units_to_str(gradient, "W", watts, sizeof(watts));

	printf("  %s (%s) = (%s * %e) + %f\n",
		power ? "Power" : "Energy",
		power ? "Watts" : "Watt-seconds",
		unit, gradient, intercept);
	if (average_voltage > 0) {
		char amps[16], volts[16];
		units_to_str(average_voltage, "V", volts, sizeof(volts));
		units_to_str(gradient / average_voltage, "A", amps, sizeof(amps));
		printf("  %s is about %s (about %s @ %s)\n",
			each, watts, amps, volts);
	} else {
		printf("  %s is about %s%s\n", each, watts,
			power ? "" : "s");
	}
	printf("  Coefficient of determination R^2 = %f (%s)\n",
		r2, coefficient_r2(r2));

	dump_yaml_values(yaml, heading, field, gradient, r2);
}

/*
 *  monitor_cpu_load()
 *	load CPU(s) and gather stats
 */
static int monitor_cpu_load(
	FILE *fp,
	const int32_t num_cpus,
	const int32_t max_cpus,
	const int32_t samples_cpu,
	const int32_t sample_delay,
	cpu_list_t *cpu_list,
	rapl_info_t *rapl_list,
	const int start_delay,
	const int max_readings,
	bogo_ops_t *bogo_ops)
{
	uint32_t i, n = 0;
	value_t values_load[num_cpus * samples_cpu], *value_load = values_load;
	value_t values_ops[num_cpus * samples_cpu], *value_ops = values_ops;
	value_t values_cpu_cycles[num_cpus * samples_cpu], *value_cpu_cycles = values_cpu_cycles;
	value_t values_cpu_instr[num_cpus * samples_cpu], *value_cpu_instr = values_cpu_instr;
	double scale = (double)MAX_CPU_LOAD / (samples_cpu - 1);

	stats_headings("CPU load");
	for (i = 0; i < (uint32_t)samples_cpu; i++) {
		cpu_info_t *c;
		uint32_t n_cpus;

		for (n_cpus = 1, c = cpu_list->head; c; n_cpus++, c = c->next) {
			char buffer[1024];
			int cpu_load = scale * i;
			int ret;
			double percent_each = 100.0 / (samples_cpu * num_cpus);
			double percent = n * percent_each;

			snprintf(buffer, sizeof(buffer), "%d%% x %d",
				cpu_load, n_cpus);
			start_load(cpu_list, n_cpus, stress_cpu,
				(uint64_t)cpu_load, bogo_ops);

			ret = monitor(num_cpus, cpu_list, rapl_list,
				start_delay, sample_delay,
				max_readings, buffer,
				percent_each, percent, bogo_ops,
				&value_load->x, &value_load->y,
				&value_load->voltage,
				&value_ops->x,
				&value_cpu_cycles->x,
				&value_cpu_instr->x);
			value_ops->y = value_load->y;
			value_ops->voltage = value_load->voltage;
			value_ops->cpu_id = value_load->cpu_id = c->cpu_id;
			value_ops->cpus_used = value_load->cpus_used = n_cpus;

			value_cpu_cycles->y = value_load->y;
			value_cpu_cycles->voltage = value_load->voltage;
			value_cpu_cycles->cpu_id = value_load->cpu_id;
			value_cpu_cycles->cpus_used = value_load->cpus_used;

			value_cpu_instr->y = value_load->y;
			value_cpu_instr->voltage = value_load->voltage;
			value_cpu_instr->cpu_id = value_load->cpu_id;
			value_cpu_instr->cpus_used = value_load->cpus_used;

			stop_load(cpu_list, n_cpus);
			if (stop_flag || (ret < 0))
				return -1;
			value_load++;
			value_ops++;
			value_cpu_cycles++;
			value_cpu_instr++;
			n++;
		}
	}
	/* Keep static analysis happy */
	if (n <= 0) {
		printf("\nZero samples, cannot compute statistics.\n");
		return -1;
	}

	if (opt_flags & OPT_CALIBRATE_EACH_CPU) {
		cpu_info_t *c;
		int cpus_used = 0;

		for (c = cpu_list->head, cpus_used = 1; c; c = c->next, cpus_used++) {
			printf("\nFor %d CPU%s (of a %d CPU system):\n",
				cpus_used, cpus_used > 1 ? "s" : "", max_cpus);
			show_trend(NULL, cpus_used, values_load, n,
				"% CPU load", "1% CPU load",
				"cpu-load", "one-percent-cpu-load-watts", true);
			printf("\n");
			show_trend(NULL, cpus_used, values_ops, n,
				"bogo op", "1 bogo op",
				"bogo-op", "one-bogo-op-power-watt-seconds", false);

#if defined(PERF_ENABLED)
			printf("\n");
			show_trend(NULL, cpus_used, values_cpu_cycles, n,
				"CPU cycle", "1 CPU cycle",
				"cpu-cycle", "one-cpu-cycle-watt-seconds", false);
			printf("\n");
			show_trend(NULL, cpus_used, values_cpu_instr, n,
				"CPU instruction", "1 CPU instruction",
				"cpu-instruction", "on-cpu-instruction-watt-seconds", false);
#endif
		}
	} else {
		printf("\nFor %d CPU%s (of a %d CPU system):\n",
			cpu_list->count, cpu_list->count > 1 ? "s" : "", max_cpus);
		show_trend(fp, CPU_ANY, values_load, n,
			"% CPU load", "1% CPU load",
			"cpu-load", "one-percent-cpu-load-watts", true);
		printf("\n");
		show_trend(fp, CPU_ANY, values_ops, n,
			"bogo op", "1 bogo op",
			"bogo-op", "one-bogo-op-watts-seconds", false);
#if defined(PERF_ENABLED)
		printf("\n");
		show_trend(fp, CPU_ANY, values_cpu_cycles, n,
			"CPU cycle", "1 CPU cycle",
			"cpu-cycle", "one-cpu-cycle-watt-seconds", false);
		printf("\n");
		show_trend(fp, CPU_ANY, values_cpu_instr, n,
			"CPU instruction", "1 CPU instruction",
			"cpu-instruction", "on-cpu-instruction-watt-seconds", false);
#endif
	}
	return 0;
}

/*
 *  add_cpu_info()
 *	add cpu # to cpu_info list
 */
static int add_cpu_info(cpu_list_t *cpu_list, const int cpu)
{
	cpu_info_t *c;

	c = calloc(1, sizeof(cpu_info_t));
	if (!c) {
		fprintf(stderr, "Out of memory allocating CPU  info.\n");
		return -1;
	}
	if (cpu_list->head)
		cpu_list->tail->next = c;
	else
		cpu_list->head = c;

	c->cpu_id = cpu;
	cpu_list->tail = c;
	cpu_list->count++;

	return 0;
}


/*
 *  parse_cpu_info()
 *	parse cpu info
 */
static int parse_cpu_info(
	int32_t *num_cpus,
	const int32_t max_cpus,
	cpu_list_t *cpu_list,
	char *arg)
{
	char *str, *token;
	int n = 0;

	for (str = arg; (token = strtok(str, ",")) != NULL; str = NULL) {
		int cpu;
		char *endptr;

		errno = 0;
		cpu = strtol(token, &endptr, 10);
		if (errno || endptr == token) {
			fprintf(stderr, "Invalid CPU specified.\n");
			return -1;
		}
		if (cpu < 0 || cpu > max_cpus - 1) {
			fprintf(stderr, "CPU number out of range.\n");
			return -1;
		}
		if (add_cpu_info(cpu_list, cpu) < 0)
			return -1;
		n++;
	}
	if (!cpu_list->head) {
		fprintf(stderr, "No valid CPU numbers given.\n");
		return -1;
	}

	*num_cpus = n;

	return 0;
}

/*
 *  populate_cpu_info()
 *	if user has not supplied cpu info then
 *	we need to populate the list
 */
static inline int populate_cpu_info(const int32_t num_cpus, cpu_list_t *cpu_list)
{
	int cpu;

	if (cpu_list->head)
		return 0;

	for (cpu = 0; cpu < num_cpus; cpu++)
		if (add_cpu_info(cpu_list, cpu) < 0)
			return -1;

	return 0;
}

/*
 *  free_cpu_info()
 *	free CPU info list
 */
static void free_cpu_info(cpu_list_t *cpu_list)
{
	cpu_info_t *c = cpu_list->head;

	while (c) {
		cpu_info_t *next = c->next;
		free(c);
		c = next;
	}
	cpu_list->head = NULL;
	cpu_list->tail = NULL;
	cpu_list->count = 0;
}

int main(int argc, char * const argv[])
{
	int max_readings, run_duration, start_delay = START_DELAY_BATTERY;
	int opt_run_duration = DEFAULT_RUN_DURATION;
	char *filename = NULL;
	FILE *yaml = NULL;
	int ret = EXIT_FAILURE, i;
	struct sigaction new_action;
	bogo_ops_t *bogo_ops = NULL;
	rapl_info_t *rapl_list = NULL;		/* RAPL domain info list */
	cpu_list_t cpu_list;
	int32_t samples_cpu = 11.0;		/* samples per run */
	int32_t sample_delay = SAMPLE_DELAY;	/* time between each sampl */
	int32_t num_cpus;			/* number of CPUs */
	int32_t max_cpus;			/* number of CPUs in system */

	memset(&cpu_list, 0, sizeof(cpu_list));

	max_cpus = num_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (num_cpus < 1) {
		/* Zero CPUs makes no sense, -ve is an error */
		fprintf(stderr, "Cannot determine number of CPUs, errno=%d (%s).\n",
			errno, strerror(errno));
		goto out;
	}

	for (;;) {
		int c = getopt(argc, argv, "d:ehn:o:ps:r:R");
		if (c == -1)
			break;
		switch (c) {
		case 'd':
			opt_flags |= OPT_DELAY;
			start_delay = atoi(optarg);
			if (start_delay < 0) {
				fprintf(stderr, "Start delay must be 0 or more seconds.\n");
				goto out;
			}
			break;
		case 'e':
			opt_flags |= OPT_CALIBRATE_EACH_CPU;
			break;
		case 'h':
			show_help(argv);
			goto out;
		case 'n':
			if (parse_cpu_info(&num_cpus, max_cpus, &cpu_list, optarg) < 0)
				goto out;
			break;
		case 'o':
			filename = optarg;
			break;
		case 'p':
			opt_flags |= OPT_PROGRESS;
			break;
		case 'r':
			opt_run_duration = atoi(optarg);
			if (opt_run_duration < MIN_RUN_DURATION) {
				fprintf(stderr, "Minimum run duration must be %d seconds or more\n",
					MIN_RUN_DURATION);
				goto out;
			}
			break;
#if defined(RAPL_X86)
		case 'R':
			opt_flags |= OPT_RAPL;
			break;
#endif
		case 's':
			samples_cpu = atoi(optarg);
			if ((samples_cpu < 3.0) ||
			    (samples_cpu > MAX_CPU_LOAD)) {
				fprintf(stderr, "Samples for CPU measurements out of range.\n");
				goto out;
			}
			break;
		default:
			show_help(argv);
			goto out;
		}
	}

	if ((opt_flags & (OPT_RAPL | OPT_DELAY)) == OPT_RAPL)
		start_delay = START_DELAY_RAPL;

	populate_cpu_info(num_cpus, &cpu_list);

#if defined(RAPL_X86)
	if ((opt_flags & OPT_RAPL) && (rapl_get_domains(&rapl_list) < 1))
		exit(EXIT_FAILURE);
#endif

	if (optind < argc) {
		sample_delay = atoi(argv[optind++]);
		if (sample_delay < 1) {
			fprintf(stderr, "Sample delay must be >= 1.\n");
			goto out;
		}
	}

	if (filename) {
		if ((yaml = fopen(filename, "w")) == NULL) {
			fprintf(stderr, "Cannot open json output file '%s', "
				"errno=%d (%s).\n",
				filename, errno, strerror(errno));
			goto out;
		}
		fprintf(yaml, "---\n%s:\n", app_name);
	}

	memset(&new_action, 0, sizeof(new_action));
	for (i = 0; signals[i] != -1; i++) {
		new_action.sa_handler = handle_sig;
		sigemptyset(&new_action.sa_mask);
		new_action.sa_flags = 0;

		if (sigaction(signals[i], &new_action, NULL) < 0) {
			fprintf(stderr, "sigaction failed: errno=%d (%s).\n",
				errno, strerror(errno));
			goto out;
		}
		(void)siginterrupt(signals[i], 1);
	}

	run_duration = opt_run_duration;
	max_readings = run_duration / sample_delay;

	bogo_ops = mmap(NULL, sizeof(bogo_ops_t) * num_cpus,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (bogo_ops == MAP_FAILED) {
		fprintf(stderr, "mmap failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto out;
	}

	if (not_discharging(rapl_list))
		goto out;

	if (monitor_cpu_load(yaml, num_cpus, max_cpus, samples_cpu, sample_delay,
		&cpu_list, rapl_list, start_delay, max_readings, bogo_ops) < 0)
		goto out;

	ret = EXIT_SUCCESS;
out:
	if (bogo_ops)
		(void)munmap(bogo_ops, sizeof(bogo_ops_t) * num_cpus);
	if (yaml) {
		dump_yaml_misc(yaml);

		fprintf(yaml, "...\n");
		(void)fclose(yaml);
		if (ret != EXIT_SUCCESS)
			unlink(filename);
	}
	if (cpu_list.head)
		free_cpu_info(&cpu_list);

#if defined(RAPL_X86)
	rapl_free_list(rapl_list);
#endif

	exit(ret);
}
