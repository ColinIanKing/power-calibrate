/*
 * Copyright (C) 2014 Canonical
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

#define MIN_RUN_DURATION	(30)		/* We recommend a run of 2 minute pers sample */
#define DEFAULT_RUN_DURATION	(120)
#define SAMPLE_DELAY		(1)		/* Delay between samples in seconds */
#define START_DELAY		(20)		/* Delay to wait before sampling */
#define	RATE_ZERO_LIMIT		(0.001)		/* Less than this we call the power rate zero */
#define	CTXT_SAMPLES		(20)
#define MAX_CPU_LOAD		(100)
#define DEFAULT_TIMEOUT		(10)
#define CTXT_STOP		'X'

#define OPT_CPU_LOAD		(0x00000001)
#define OPT_CTXT_LOAD		(0x00000002)
#define OPT_PROGRESS		(0x00000004)

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
#define POWER_NOW		(11)
#define VOLTAGE_NOW		(12)
#define CURRENT_NOW 		(13)
#define PROC_FORK		(14)
#define PROC_EXEC		(15)
#define PROC_EXIT		(16)
#define BOGO_OPS		(17)
#define MAX_VALUES		(18)

#define MWC_SEED_Z		(362436069ULL)
#define MWC_SEED_W		(521288629ULL)

#define SYS_CLASS_POWER_SUPPLY		"/sys/class/power_supply"
#define PROC_ACPI_BATTERY		"/proc/acpi/battery"

#define SYS_FIELD_VOLTAGE_NOW		"POWER_SUPPLY_VOLTAGE_NOW="
#define SYS_FIELD_POWER_NOW		"POWER_SUPPLY_POWER_NOW="
#define SYS_FIELD_ENERGY_NOW		"POWER_SUPPLY_ENERGY_NOW="
#define SYS_FIELD_CURRENT_NOW		"POWER_SUPPLY_CURRENT_NOW="
#define SYS_FIELD_CHARGE_NOW		"POWER_SUPPLY_CHARGE_NOW="
#define SYS_FIELD_STATUS_DISCHARGING  	"POWER_SUPPLY_STATUS=Discharging"

/* Measurement entry */
typedef struct {
	double	value;	/* Measurment value */
	time_t	when;	/* When it was measured */
} measurement_t;

/* Statistics entry */
typedef struct {
	double	value[MAX_VALUES];
	bool	inaccurate[MAX_VALUES];
} stats_t;

/* x,y data pair, for trend analysis */
typedef struct {
	double 	x;
	double	y;
} tuple_t;

typedef struct {
	double	ops;
	uint8_t	padding[64];
} bogo_ops_t;

typedef void (*func)(uint64_t param, const uint32_t instance, bogo_ops_t *bogo_ops);

static int32_t sample_delay   = SAMPLE_DELAY;	/* time between each sample in secs */
static volatile bool stop_flag;			/* sighandler stop flag */
static int32_t num_cpus;				/* number of CPUs */
static int32_t opt_flags;				/* command options */
static int32_t samples_cpu = 10.0;
static int32_t samples_ctxt = CTXT_SAMPLES;
static char *app_name = "power-calibrate";
static bogo_ops_t *bogo_ops;

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
#ifdef SIGILL
	SIGILL,
#endif
#ifdef SIGABRT
	SIGABRT,
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
	static const char *scales[] = {
		"",
		"m",
		"µ",
		"n",
		"p",
		"f",
		NULL
	};
	size_t i;

	for (i = 0; i < 6; i++, v *= 1000) {
		if (v > 0.5)
			break;
	}
	snprintf(buf, buflen, "%.2f %s%s", v, scales[i], units);
	return buf;
}
/*
 *  uint64_to_str()
 *	uint64_t values to strings
 */
static char *uint64_to_str(
	const uint64_t val,
	char *const buf,
	const size_t buflen)
{
	double v = (double)val;
	static const char scales[] = " KMB";
	size_t i;

	for (i = 0; i < sizeof(scales) - 1; i++, v /= 1000) {
		if (v <= 500)
			break;
	}
	snprintf(buf, buflen, "%5.1f%c", v, scales[i]);
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
static inline uint64_t mwc(void)
{
	static uint64_t w = MWC_SEED_W, z = MWC_SEED_Z;

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
void set_affinity(int cpu)
{
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu % num_cpus, &mask);
	(void)sched_setaffinity(0, sizeof(mask), &mask);
}

/*
 *  stress_cpu()
 *	stress CPU
 */
static void stress_cpu(
	const uint64_t cpu_load,
	const uint32_t instance,
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
 *  stress_ctxt
 *	generate context switches by periodic wakeups
 */
static void stress_ctxt(
	const uint64_t delay,
	const uint32_t instance,
	bogo_ops_t *bogo_ops)
{
	pid_t pid;
	int pipefds[2];

	if (pipe(pipefds) < 0) {
		fprintf(stderr, "stress_ctxt: pipe failed, errno=%d [%d]\n", errno, getpid());
		exit(EXIT_FAILURE);
	}

	pid = fork();
	if (pid < 0) {
		(void)close(pipefds[0]);
		(void)close(pipefds[1]);
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		/* Child, immediately exit */
		(void)close(pipefds[1]);

		for (;;) {
			char ch;

			for (;;) {
				if (read(pipefds[0], &ch, sizeof(ch)) <= 0)
					break;	/* happens on pipe breakage */
				bogo_ops[instance].ops++;
				if (stop_flag || (ch == CTXT_STOP))
					break;
			}
			(void)close(pipefds[0]);
			exit(EXIT_SUCCESS);
		}
	} else {
		char ch = '_';

		/* Parent */
		(void)close(pipefds[0]);

		while (!stop_flag) {
			if (write(pipefds[1],  &ch, sizeof(ch)) < 0) {
				fprintf(stderr, "stress_ctxt: write failed, errno=%d [%d]\n", errno, getpid());
				break;
			}
			struct timeval tv;
			tv.tv_sec = delay / 1000000;
			tv.tv_usec = delay % 1000000;
			select(0, NULL, NULL, NULL, &tv);
		}

		ch = CTXT_STOP;
		if (write(pipefds[1],  &ch, sizeof(ch)) <= 0)
			fprintf(stderr, "stress_ctxt: termination write failed, errno=%d [%d]\n", errno, getpid());
		kill(pid, SIGKILL);
	}
	exit(EXIT_SUCCESS);
}

/*
 *  stop_load()
 *	kill load child processes
 */
static void stop_load(const pid_t *pids, const int32_t total_procs)
{
	int32_t i;

	/* Kill.. */
	for (i = 0; i < total_procs; i++) {
		if (pids[i])
			kill(pids[i], SIGKILL);
	}
	/* And ensure we don't get zombies */
	for (i = 0; i < total_procs; i++) {
		if (pids[i]) {
			int status;
			wait(&status);
		}
	}
}

/*
 *  start_load()
 *	load system with some stress processes
 */
void start_load(
	pid_t *pids,
	const uint32_t total_procs,
	const func load_func,
	const uint64_t param,
	bogo_ops_t *bogo_ops)
{
	uint32_t n_procs;
	int i;
	struct sigaction new_action;

	memset(&new_action, 0, sizeof(new_action));
	for (i = 0; signals[i] != -1; i++) {
		new_action.sa_handler = handle_sig;
		sigemptyset(&new_action.sa_mask);
		new_action.sa_flags = 0;

		(void)sigaction(signals[i], &new_action, NULL);
		(void)siginterrupt(signals[i], 1);
	}

	memset(pids, 0, sizeof(pid_t) * total_procs);

	for (n_procs = 0; n_procs < total_procs; n_procs++) {
		int fd;
		int pid = fork();

		switch (pid) {
		case -1:
			fprintf(stderr, "Cannot fork\n");
			stop_load(pids, n_procs);
			exit(EXIT_FAILURE);
		case 0:
			set_affinity(n_procs);

			for (fd = (getdtablesize() - 1); fd > 2; fd--)
				(void)close(fd);
			/* Child */
			load_func(param, n_procs, bogo_ops);
			exit(0);
		default:
			pids[n_procs] = pid;
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
static int stats_read(stats_t *const stats, bogo_ops_t *bogo_ops)
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
			if (sscanf(buf, "%*s %15lf", &(stats->value[CPU_PROCS_RUN])) == 1)
				stats->inaccurate[CPU_PROCS_RUN] = false;
		if (strncmp(buf, "procs_blocked ", 14) == 0)
			if (sscanf(buf, "%*s %15lf", &(stats->value[CPU_PROCS_BLK])) == 1)
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
	res->value[CPU_CTXT] = (INACCURATE(s1, s2, CPU_CTXT) || (sample_delay <= 0.0)) ?
			NAN : res->value[CPU_CTXT] / sample_delay;
	res->value[CPU_INTR] = (INACCURATE(s1, s2, CPU_INTR) || (sample_delay <= 0.0)) ?
			NAN : res->value[CPU_INTR] / sample_delay;
	res->value[BOGO_OPS] = (INACCURATE(s1, s2, BOGO_OPS) || (sample_delay <= 0.0)) ?
			NAN : res->value[BOGO_OPS] / sample_delay;
	res->value[CPU_PROCS_RUN] = s2->inaccurate[CPU_PROCS_RUN] ? NAN : s2->value[CPU_PROCS_RUN];
	res->value[CPU_PROCS_BLK] = s2->inaccurate[CPU_PROCS_BLK] ? NAN : s2->value[CPU_PROCS_BLK];

	return true;
}

/*
 *  stats_headings()
 *	dump heading columns
 */
static void stats_headings(const char *test)
{
	printf("%8.8s  User   Sys  Idle  Run Ctxt/s  IRQ/s  Ops/s  Watts  Volts   Amps\n", test);
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
	char buf[10], str[16];
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

	uint64_to_str(s->value[BOGO_OPS], str, sizeof(str));

	fmt = summary ?
		"%8.8s %5.1f %5.1f %5.1f %4.1f %6.1f %6.1f %6s %s %6.3f %6.3f\n" :
		"%8.8s %5.1f %5.1f %5.1f %4.0f %6.0f %6.0f %6s %s %6.3f %6.3f\n";
	printf(fmt,
		prefix,
		s->value[CPU_USER], s->value[CPU_SYS], s->value[CPU_IDLE],
		s->value[CPU_PROCS_RUN], s->value[CPU_CTXT], s->value[CPU_INTR],
		str, buf,
		s->value[VOLTAGE_NOW], s->value[CURRENT_NOW]);
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
					double diff = (double)stats[i].value[j] - average->value[j];
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
 *  power_rate_get_sys_fs()
 *	get power discharge rate from battery via /sys interface
 */
static int power_rate_get_sys_fs(
	double *const power_now,
	double *const voltage_now,
	double *const current_now,
	bool *const discharging,
	bool *const inaccurate)
{
	DIR *dir;
	struct dirent *dirent;
	double total_watts = 0.0;
	double average_voltage = 0.0;
	int n = 0;

	*power_now = 0.0;
	*voltage_now = 0.0;
	*current_now = 0.0;
	*discharging = false;
	*inaccurate = true;

	if ((dir = opendir(SYS_CLASS_POWER_SUPPLY)) == NULL) {
		fprintf(stderr, "Machine does not have %s, cannot run the test.\n", SYS_CLASS_POWER_SUPPLY);
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
			snprintf(path, sizeof(path), "%s/%s/type", SYS_CLASS_POWER_SUPPLY, dirent->d_name);
			if ((data = file_get(path)) != NULL) {
				bool mismatch = (strstr(data, "Battery") == NULL);
				free(data);
				if (mismatch)
					continue;	/* type don't match, skip this entry */
			} else
				continue;		/* can't check type, skip this entry */

			snprintf(path, sizeof(path), "%s/%s/uevent", SYS_CLASS_POWER_SUPPLY, dirent->d_name);
			if ((fp = fopen(path, "r")) == NULL) {
				fprintf(stderr, "Battery %s present but under supported - no state present.", dirent->d_name);
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

	if (! *discharging) {
		printf("Machine is not discharging, cannot measure power usage.\n");
		return -1;
	}

	/*
 	 *  If the battery is helpful it supplies the rate already, in which case
	 *  we know the results from the battery are as good as we can and we don't
	 *  have to figure out anything from capacity change over time.
	 */
	if (total_watts > RATE_ZERO_LIMIT) {
		*power_now = total_watts;
		*voltage_now = average_voltage / (double)n;
		*current_now = total_watts / average_voltage;
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
 *  power_rate_get_proc_acpi()
 *	get power discharge rate from battery via /proc/acpi interface
 */
static int power_rate_get_proc_acpi(
	double *const power_now,
	double *const voltage_now,
	double *const current_now,
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

	*power_now = 0.0;
	*voltage_now = 0.0;
	*current_now = 0.0;
	*discharging = false;
	*inaccurate = true;

	if ((dir = opendir(PROC_ACPI_BATTERY)) == NULL) {
		fprintf(stderr, "Machine does not have %s, cannot run the test.\n", PROC_ACPI_BATTERY);
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
			sprintf(filename, "/proc/acpi/battery/%s/info", dirent->d_name);
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

	if (! *discharging) {
		printf("Machine is indicating it is not discharging and hence "
		       "we cannot measure power usage.\n");
		return -1;
	}

	/*
 	 * If the battery is helpful it supplies the rate already, in which
	 * case we know the results from the battery are as good as we can
	 * and we don't have to figure out anything from capacity change over
	 * time.
	 */
	if (total_watts > RATE_ZERO_LIMIT) {
		*power_now = total_watts;
		*voltage_now = average_voltage / (double)n;
		*current_now = total_watts / average_voltage;
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
 *  power_rate_get()
 *	get consumption rate
 */
static int power_rate_get(
	double *const power_now,
	double *const voltage_now,
	double *const current_now,
	bool *const discharging,
	bool *const inaccurate)
{
	struct stat buf;

	if ((stat(SYS_CLASS_POWER_SUPPLY, &buf) != -1) &&
	    S_ISDIR(buf.st_mode))
		return power_rate_get_sys_fs(power_now, voltage_now, current_now, discharging, inaccurate);

	if ((stat(PROC_ACPI_BATTERY, &buf) != -1) &&
	    S_ISDIR(buf.st_mode))
		return power_rate_get_proc_acpi(power_now, voltage_now, current_now, discharging, inaccurate);

	fprintf(stderr, "Machine does not seem to have a battery, cannot measure power.\n");
	return -1;
}

/*
 *  not_discharging()
 *	returns true if battery is not discharging
 */
static inline bool not_discharging(void)
{
	double power, voltage, current;
	bool discharging, inaccurate;

	return power_rate_get(&power, &voltage, &current, &discharging, &inaccurate) < 0;
}

/*
 *   monitor()
 *	monitor system activity and power consumption
 */
static int monitor(
	const int start_delay,
	const int max_readings,
	const char *test,
	double percent_each,
	double percent,
	bogo_ops_t *bogo_ops,
	double *busy,
	double *ctxt,
	double *power,
	double *voltage,
	double *ops)
{
	int readings = 0, i;
	int64_t t = 1;
	stats_t *stats, s1, s2, average, stddev;
	bool discharging, dummy_inaccurate;
	double dummy_power, dummy_voltage, dummy_current;
	double time_start;

	if (start_delay > 0) {
		/* Gather up initial data */
		for (i = 0; i < start_delay; i++) {
			if (opt_flags & OPT_PROGRESS) {
				fprintf(stdout, "%8.8s: test calibrating %5.1f%%..\r",
					test, 100.0 * i / start_delay);
				fflush(stdout);
			}
			if (power_rate_get(&dummy_power, &dummy_voltage, &dummy_current,
					   &discharging, &dummy_inaccurate) < 0)
				return -1;
			if (sleep(1) || stop_flag)
				return -1;
			if (!discharging)
				return -1;
		}
	}

	if (not_discharging())
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
	if (stats_read(&s1, bogo_ops) < 0) {
		free(stats);
		return -1;
	}

	while (!stop_flag && (readings < max_readings)) {
		int ret = 0;
		double secs, time_now;
		struct timeval tv;

		if ((time_now = gettime_to_double()) < 0.0) {
			free(stats);
			return -1;
		}

		if (opt_flags & OPT_PROGRESS) {
			double progress = readings * 100.0 / max_readings;
			fprintf(stdout, "%8.8s: test progress %5.1f%% (total progress %6.2f%%)\r",
				test, progress, (progress * percent_each / 100.0) + percent);
			fflush(stdout);
		}

		secs = time_start + ((double)t * sample_delay) - time_now;
		if (secs < 0.0)
			goto sample_now;
		tv = double_to_timeval(secs);

		ret = select(0, NULL, NULL, NULL, &tv);
		if (ret < 0) {
			if (errno == EINTR)
				break;
			fprintf(stderr,"select: %s\n", strerror(errno));
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

			get_time(tmbuffer, sizeof(tmbuffer));
			if (stats_read(&s2, bogo_ops) < 0) {
				free(stats);
				return -1;
			}

			/*
			 *  Total ticks was zero, something is broken, so re-sample
			 */
			if (!stats_gather(&s1, &s2, &stats[readings])) {
				stats_clear(&stats[readings]);
				if (stats_read(&s1, bogo_ops) < 0) {
					free(stats);
					return -1;
				}
				continue;
			}

			if (power_rate_get(&stats[readings].value[POWER_NOW],
					   &stats[readings].value[VOLTAGE_NOW],
					   &stats[readings].value[CURRENT_NOW],
					   &discharging,
					   &stats[readings].inaccurate[POWER_NOW]) < 0) {
				free(stats);
				return -1; 	/* Failure to read */
			}

			if (!discharging) {
				free(stats);
				return -1;	/* No longer discharging! */
			}

			readings++;
			s1 = s2;
			t++;
		}
	}

	/* Stats now gathered, calculate averages, stddev, min and max and display */
	stats_average_stddev_min_max(stats, readings, &average, &stddev);
	if (readings > 0) {
		stats_print(test, true, &average);
	}
	*busy = 100.0 - average.value[CPU_IDLE];
	*ctxt = average.value[CPU_CTXT];
	*power = average.value[POWER_NOW];
	*voltage = average.value[VOLTAGE_NOW];
	*ops = average.value[BOGO_OPS];

	free(stats);
	return 0;
}

/*
 *  calc_trend()
 *	calculate linear trendline - compute gradient, y intercept and
 *	coefficient of determination.
 */
static void calc_trend(
	const tuple_t *tuples,
	const int n,
	double *gradient,
	double *intercept,
	double *r2)
{
	int i;
	double a = 0.0, b, c = 0.0, d, e, f;
	double sum_x = 0.0, sum_y = 0.0;
	double sum_x2 = 0.0, sum_y2 = 0.0;
	double sum_xy = 0.0, r;

	for (i = 0; i < n; i++) {
		a += tuples[i].x * tuples[i].y;
		sum_x += tuples[i].x;
		sum_y += tuples[i].y;
		sum_xy += (tuples[i].x * tuples[i].y);
		sum_x2 += (tuples[i].x * tuples[i].x);
		sum_y2 += (tuples[i].y * tuples[i].y);
	}

	/*
	 * Coefficient of determination R^2,
	 * http://mathbits.com/MathBits/TISection/Statistics2/correlation.htm
	 */
	r  = (((double)n * sum_xy) - (sum_x * sum_y)) /
	      (sqrt(((double)n * sum_x2) - (sum_x * sum_x)) *
	       sqrt(((double)n * sum_y2) - (sum_y * sum_y)));
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
}


/*
 *  show_help()
 *	simple help
 */
static void show_help(char *const argv[])
{
	printf("%s, version %s\n\n", app_name, VERSION);
	printf("usage: %s [options]\n", argv[0]);
	printf(" -c       calibrate CPU power usage\n");
	printf(" -C       calibrate context switch power usage\n");
	printf(" -d secs  specify delay before starting, default is %d seconds\n", START_DELAY);
	printf(" -h show  help\n");
	printf(" -n cpus  specify number of CPUs\n");
	printf(" -o file  output results into json formatted file\n");
	printf(" -p       show progress\n");
	printf(" -r secs  specify run duration in seconds of each test cycle\n");
	printf(" -s num   number of samples (tests) per CPU for CPU calibration\n");
	printf(" -S num   number of samples (tests) per CPU for context switch calibration\n");
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
 *  dump_json_values()
 *	output json formatted data
 */
static void dump_json_values(
	FILE *fp,
	const char *heading,
	const char *field,
	const double value,
	const double r2)
{
	if (!fp)
		return;

	fprintf(fp, "    \"%s\":{\n", heading);
	fprintf(fp, "      \"%s\":%f,\n", field, value);
	fprintf(fp, "      \"r-squared\":%f\n", r2);
	fprintf(fp, "    },\n");
}

/*
 *  dump_json_misc()
 *	output json misc test data
 */
static void dump_json_misc(FILE *fp)
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

	fprintf(fp, "    \"test-run\":{\n");
	fprintf(fp, "      \"date\":\"%2.2d/%2.2d/%-2.2d\",\n",
		tm.tm_mday, tm.tm_mon + 1, (tm.tm_year+1900) % 100);
	fprintf(fp, "      \"time\":\"%2.2d:%2.2d:%-2.2d\",\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec);
	fprintf(fp, "      \"sysname\":\"%s\",\n", buf.sysname);
	fprintf(fp, "      \"nodename\":\"%s\",\n", buf.nodename);
	fprintf(fp, "      \"release\":\"%s\",\n", buf.release);
	fprintf(fp, "      \"machine\":\"%s\"\n", buf.machine);
	fprintf(fp, "    }\n");
}


/*
 *  monitor_cpu_load()
 *	load CPU(s) and gather stats
 */
static int monitor_cpu_load(
	FILE *fp,
	const int start_delay,
	const int max_readings,
	bogo_ops_t *bogo_ops)
{
	uint32_t cpus, i, n = 0;
	tuple_t	 tuples_load[num_cpus * samples_cpu], *tuple_load = tuples_load;
	tuple_t	 tuples_ops[num_cpus * samples_cpu], *tuple_ops = tuples_ops;
	double gradient, intercept, r2;
	double average_voltage = 0.0;
	double scale = MAX_CPU_LOAD / (samples_cpu - 1);
	char watts[16], amps[16], volts[16];

	stats_headings("CPU load");
	for (i = 0; i < (uint32_t)samples_cpu; i++) {
		pid_t pids[num_cpus];

		for (cpus = 1; cpus <= (uint32_t)num_cpus; cpus++) {
			char buffer[1024];
			double dummy, voltage;
			int cpu_load = scale * i;
			int ret;
			double percent_each = 100.0 / (samples_cpu * num_cpus);
			double percent = n * percent_each;

			snprintf(buffer, sizeof(buffer), "%d%% x %d", cpu_load, cpus);
			start_load(pids, cpus, stress_cpu, (uint64_t)cpu_load, bogo_ops);

			ret = monitor(start_delay, max_readings, buffer, percent_each, percent, bogo_ops,
				&tuple_load->x, &dummy, &tuple_load->y, &voltage, &tuple_ops->x);
			tuple_ops->y = tuple_load->y;
			stop_load(pids, cpus);
			if (stop_flag || (ret < 0))
				return -1;
			tuple_load++;
			tuple_ops++;
			n++;
			average_voltage += voltage;
		}
	}
	average_voltage /= n;

	calc_trend(tuples_load, num_cpus * samples_cpu, &gradient, &intercept, &r2);

	units_to_str(gradient, "W", watts, sizeof(watts));
	units_to_str(average_voltage, "V", volts, sizeof(volts));
	units_to_str(gradient / average_voltage, "A", amps, sizeof(amps));

	printf("\n");
	printf("Power (Watts) = (%% CPU load * %f) + %f\n", gradient, intercept);
	printf("Each 1%% CPU load is about %s (about %s @ %s)\n", watts, amps, volts);
	printf("Coefficient of determination R^2 = %f (%s)\n", r2, coefficient_r2(r2));
	printf("\n");

	calc_trend(tuples_ops, num_cpus * samples_cpu, &gradient, &intercept, &r2);

	units_to_str(gradient, "W", watts, sizeof(watts));
	units_to_str(average_voltage, "V", volts, sizeof(volts));
	units_to_str(gradient / average_voltage, "A", amps, sizeof(amps));

	printf("Power (Watts) = (bogo op * %e) + %f\n", gradient, intercept);
	printf("1 bogo ops is about %s (about %s @ %s)\n", watts, amps, volts);
	printf("Coefficient of determination R^2 = %f (%s)\n", r2, coefficient_r2(r2));
	printf("\n");

	dump_json_values(fp, "cpu-load", "one-percent-cpu-load", gradient, r2);

	return 0;
}

/*
 *  monitor_ctxt_load()
 *	load CPU(s) with context switches and gather stats
 */
static int monitor_ctxt_load(
	FILE *fp,
	const int start_delay,
	const int max_readings,
	bogo_ops_t *bogo_ops)
{
	int i, n = 0;
	tuple_t	 tuples_ctxt[samples_ctxt], *tuple_ctxt = tuples_ctxt;
	tuple_t	 tuples_ops[num_cpus * samples_cpu], *tuple_ops = tuples_ops;
	double gradient, intercept, r2, x;
	double average_voltage = 0.0;
	double scale = 1000.0 / (samples_ctxt - 1);
	char watts[16], amps[16], volts[16];

	stats_headings("Wakeups");
	for (i = 0; i < samples_ctxt; i++) {
		pid_t pids[num_cpus];
		char buffer[1024];
		int ret;
		double dummy, voltage;
		uint64_t delay = (i > 0) ? 1000000 / (scale * i) : 1000000 * 60;
		double percent_each = 100.0 / samples_ctxt;
		double percent = i * percent_each;

		snprintf(buffer, sizeof(buffer), "%.1f", scale * i);

		memset(bogo_ops, 0, sizeof(bogo_ops_t) * num_cpus);
		start_load(pids, 1, stress_ctxt, delay, bogo_ops);
		ret = monitor(start_delay, max_readings, buffer, percent_each, percent,
			bogo_ops, &dummy, &x, &tuple_ctxt->y, &voltage, &tuple_ops->y);
		tuple_ctxt->x = tuple_ops->x = x;
		stop_load(pids, 1);
		if (stop_flag || (ret < 0))
			return -1;
		tuple_ctxt++;
		tuple_ops++;
		n++;
		average_voltage += voltage;
	}
	average_voltage /= n;

	calc_trend(tuples_ctxt, samples_ctxt, &gradient, &intercept, &r2);

	units_to_str(gradient, "W", watts, sizeof(watts));
	units_to_str(average_voltage, "V", volts, sizeof(volts));
	units_to_str(gradient / average_voltage, "A", amps, sizeof(amps));

	printf("\n");
	printf("Power (Watts) = (context switches * %f) + %f\n", gradient, intercept);
	printf("1 context switch is about %s (about %s @ %s)\n", watts, amps, volts);
	printf("Coefficient of determination R^2 = %f (%s)\n", r2, coefficient_r2(r2));
	printf("\n");

	dump_json_values(fp, "context-switches", "one-context-switch", gradient, r2);

	return 0;
}

int main(int argc, char * const argv[])
{
	int max_readings, run_duration, start_delay = START_DELAY;
	int opt_run_duration = DEFAULT_RUN_DURATION;
	char *filename = NULL;
	FILE *fp = NULL;
	int ret = EXIT_FAILURE, i;
	struct sigaction new_action;

	num_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (num_cpus < 0) {
		fprintf(stderr, "Cannot determine number of CPUs, errno=%d (%s).\n",
			errno, strerror(errno));
		goto out;
	}

	for (;;) {
		int c = getopt(argc, argv, "cCd:hn:o:ps:S:r:");
		if (c == -1)
			break;
		switch (c) {
		case 'c':
			opt_flags |= OPT_CPU_LOAD;
			break;
		case 'C':
			opt_flags |= OPT_CTXT_LOAD;
			break;
		case 'd':
			start_delay = atoi(optarg);
			if (start_delay < 0) {
				fprintf(stderr, "Start delay must be 0 or more seconds.\n");
				goto out;
			}
			break;
		case 'h':
			show_help(argv);
			goto out;
		case 'n':
			num_cpus = atoi(optarg);
			if (num_cpus < 1) {
				fprintf(stderr, "Number of CPUs must be 1 or more.\n");
				goto out;
			}
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
				fprintf(stderr, "Minimum run duration must be %d seconds or more\n", MIN_RUN_DURATION);
				goto out;
			}
			break;
		case 's':
			samples_cpu = atoi(optarg);
			if ((samples_cpu < 3.0) || (samples_cpu > MAX_CPU_LOAD)) {
				fprintf(stderr, "Samples for CPU measurements out of range.\n");
				goto out;
			}
			break;
		case 'S':
			samples_ctxt = atoi(optarg);
			if ((samples_ctxt < 3.0) || (samples_ctxt > CTXT_SAMPLES)) {
				fprintf(stderr, "Samples for context switches out of range.\n");
				goto out;
			}
			break;
		default:
			show_help(argv);
			goto out;
		}
	}

	if (!(opt_flags & (OPT_CPU_LOAD | OPT_CTXT_LOAD))) {
		fprintf(stderr, "Requires -c or -C option(s).\n");
		goto out;
	}

	if (optind < argc) {
		sample_delay = atoi(argv[optind++]);
		if (sample_delay < 1) {
			fprintf(stderr, "Sample delay must be >= 1\n");
			goto out;
		}
	}

	if (filename) {
		if ((fp = fopen(filename, "w")) == NULL) {
			fprintf(stderr, "Cannot open json output file '%s'.\n", filename);
			goto out;
		}
		fprintf(fp, "{\n  \"%s\":{\n", app_name);
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

	//run_duration = opt_run_duration + START_DELAY - start_delay;
	run_duration = opt_run_duration;
	max_readings = run_duration / sample_delay;

	bogo_ops = mmap(NULL, sizeof(bogo_ops_t) * num_cpus,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (bogo_ops == MAP_FAILED) {
		fprintf(stderr, "mmap failed: errno=%d (%s).\n",
			errno, strerror(errno));
		goto out;
	}

	if (not_discharging())
		goto out;

	if (opt_flags & OPT_CPU_LOAD)
		if (monitor_cpu_load(fp, start_delay, max_readings, bogo_ops) < 0)
			goto out;
	if (opt_flags & OPT_CTXT_LOAD)
		if (monitor_ctxt_load(fp, start_delay, max_readings, bogo_ops) < 0)
			goto out;

	ret = EXIT_SUCCESS;
out:
	if (bogo_ops)
		(void)munmap(bogo_ops, sizeof(bogo_ops_t) * num_cpus);
	if (fp) {
		dump_json_misc(fp);

		fprintf(fp, "  }\n}\n");
		(void)fclose(fp);
		if (ret != EXIT_SUCCESS)
			unlink(filename);
	}
	exit(ret);
}
