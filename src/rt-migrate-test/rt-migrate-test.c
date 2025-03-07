// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt-migrate-test.c
 * Copyright (C) 2007-2009 Steven Rostedt <srostedt@redhat.com>
 */
#include <stdio.h>
#ifndef __USE_XOPEN2K
# define __USE_XOPEN2K
#endif
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdarg.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sched.h>
#include <pthread.h>
#include <inttypes.h>

#include <linux/unistd.h>

#include "rt-utils.h"

int nr_tasks;
int lfd;

static int mark_fd = -1;
static __thread char buff[BUFSIZ+1];

static void setup_ftrace_marker(void)
{
	struct stat st;
	char *files[] = {
		"/sys/kernel/debug/tracing/trace_marker",
		"/debug/tracing/trace_marker",
		"/debugfs/tracing/trace_marker",
	};
	int ret;
	unsigned int i;

	for (i = 0; i < (sizeof(files) / sizeof(char *)); i++) {
		ret = stat(files[i], &st);
		if (ret >= 0)
			goto found;
	}
	/* todo, check mounts system */
	return;
found:
	mark_fd = open(files[i], O_WRONLY);
}

static void ftrace_write(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (mark_fd < 0)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buff, BUFSIZ, fmt, ap);
	va_end(ap);

	write(mark_fd, buff, n);
}

#define nano2sec(nan) (nan / 1000000000ULL)
#define nano2ms(nan) (nan / 1000000ULL)
#define nano2usec(nan) (nan / 1000ULL)
#define usec2nano(sec) (sec * 1000ULL)
#define ms2nano(ms) (ms * 1000000ULL)
#define sec2nano(sec) (sec * 1000000000ULL)
#define INTERVAL ms2nano(100ULL)
#define RUN_INTERVAL ms2nano(20ULL)
#define NR_RUNS 50
#define PRIO_START 2
/* 1 millisec off */
#define MAX_ERR  usec2nano(1000)

#define PROGRESS_CHARS 70

static unsigned long long interval = INTERVAL;
static unsigned long long run_interval = RUN_INTERVAL;
static unsigned long long max_err = MAX_ERR;
static int nr_runs = NR_RUNS;
static int prio_start = PRIO_START;
static int check;
static int stop;
static int equal;

static unsigned long long now;

static int done;
static int loop;
static int duration;
static int quiet;
static char jsonfile[MAX_PATH];

static pthread_barrier_t start_barrier;
static pthread_barrier_t end_barrier;
static unsigned long long **intervals;
static unsigned long long **intervals_length;
static unsigned long **intervals_loops;
static long *thread_pids;

static char buffer[BUFSIZ];

static void perr(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buffer, BUFSIZ, fmt, ap);
	va_end(ap);

	perror(buffer);
	fflush(stderr);
	exit(-1);
}

static void print_progress_bar(int percent)
{
	int i;
	int p;

	if (percent > 100)
		percent = 100;

	/* Use stderr, so we don't capture it */
	putc('\r', stderr);
	putc('|', stderr);
	for (i=0; i < PROGRESS_CHARS; i++)
		putc(' ', stderr);
	putc('|', stderr);
	putc('\r', stderr);
	putc('|', stderr);

	p = PROGRESS_CHARS * percent / 100;

	for (i=0; i < p; i++)
		putc('-', stderr);

	fflush(stderr);
}

static void usage(int error)
{
	printf("rt-migrate-test %1.2f\n", VERSION);
	printf("Usage:\n"
	       "rt-migrate-test <options> [NR_TASKS]\n\n"
	       "-c       --check           Stop if lower prio task is quicker than higher (off)\n"
	       "-D TIME  --duration=TIME   Specify a length for the test run.\n"
	       "                           Append 'm', 'h', or 'd' to specify minutes, hours or\n"
	       "                           days.\n"
	       "-e       --equal           Use equal prio for #CPU-1 tasks (requires > 2 CPUS)\n"
	       "-h       --help            Print this help message\n"
	       "         --json=FILENAME   write final results into FILENAME, JSON formatted\n"
	       "-l LOOPS --loops=LOOPS     Number of iterations to run (50)\n"
	       "-m TIME  --maxerr=TIME     Max allowed error (microsecs)\n"
	       "-p PRIO  --prio=PRIO       base priority to start RT tasks with (2)\n"
	       "-q       --quiet           print a summary only on exit\n"
	       "-r TIME  --run-time=TIME   Run time (ms) to busy loop the threads (20)\n"
	       "-s TIME  --sleep-time=TIME Sleep time (ms) between intervals (100)\n\n"
	       "  () above are defaults \n"
	       );
	exit(error);
}

enum option_value {
	OPT_CHECK=1, OPT_DURATION, OPT_EQUAL, OPT_HELP, OPT_JSON,
	OPT_LOOPS, OPT_MAXERR, OPT_PRIO, OPT_QUIET, OPT_RUN_TIME,
	OPT_SLEEP_TIME
};

static void parse_options(int argc, char *argv[])
{
	for (;;) {
		int option_index = 0;
		/** Options for getopt */
		static struct option long_options[] = {
			{"check",	no_argument,		NULL, OPT_CHECK},
			{"duration",	required_argument,	NULL, OPT_DURATION},
			{"equal",	no_argument,		NULL, OPT_EQUAL},
			{"help",	no_argument,		NULL, OPT_HELP},
			{"json",	required_argument,      NULL, OPT_JSON},
			{"loops",	required_argument,	NULL, OPT_LOOPS},
			{"maxerr",	required_argument,	NULL, OPT_MAXERR},
			{"prio",	required_argument,	NULL, OPT_PRIO},
			{"quiet",	no_argument,		NULL, OPT_QUIET},
			{"run-time",	required_argument,	NULL, OPT_RUN_TIME},
			{"sleep-time",	required_argument,	NULL, OPT_SLEEP_TIME},
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "cD:ehl:m:p:qr:s:",
			long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case OPT_CHECK:
		case 'c':
			check = 1;
			break;
		case OPT_DURATION:
		case 'D':
			duration = parse_time_string(optarg);
			break;
		case OPT_EQUAL:
		case 'e':
			equal = 1;
			break;
		case OPT_HELP:
		case '?':
		case 'h':
			usage(0);
			break;
		case OPT_JSON:
			strncpy(jsonfile, optarg, strnlen(optarg, MAX_PATH-1));
			break;
		case OPT_LOOPS:
		case 'l':
			nr_runs = atoi(optarg);
			break;
		case OPT_MAXERR:
		case 'm':
			max_err = usec2nano(atoi(optarg));
			break;
		case OPT_PRIO:
		case 'p':
			prio_start = atoi(optarg);
			break;
		case OPT_QUIET:
		case 'q':
			quiet = 1;
			break;
		case OPT_RUN_TIME:
		case 'r':
			run_interval = atoi(optarg);
			break;
		case OPT_SLEEP_TIME:
		case 's':
			interval = atoi(optarg);
			break;
		default:
			usage(1);
		}
	}
}

static unsigned long long get_time(void)
{
	struct timeval tv;
	unsigned long long time;

	gettimeofday(&tv, NULL);

	time = sec2nano(tv.tv_sec);
	time += usec2nano(tv.tv_usec);

	return time;
}

static void record_time(int id, unsigned long long time, unsigned long l)
{
	unsigned long long ltime;

	if (loop >= nr_runs)
		return;
	time -= now;
	ltime = get_time();
	ltime -= now;
	intervals[loop][id] = time;
	intervals_length[loop][id] = ltime;
	intervals_loops[loop][id] = l;
}

static int calc_prio(int id)
{
	int prio = equal && id && (id < nr_tasks - 1) ? 1 : id;
	return prio + prio_start;
}

static void print_results(void)
{
	int i;
	int t;
	unsigned long long tasks_max[nr_tasks];
	unsigned long long tasks_min[nr_tasks];
	unsigned long long tasks_avg[nr_tasks];

	memset(tasks_max, 0, sizeof(tasks_max[0])*nr_tasks);
	memset(tasks_min, 0xff, sizeof(tasks_min[0])*nr_tasks);
	memset(tasks_avg, 0, sizeof(tasks_avg[0])*nr_tasks);

	printf("Iter: ");
	for (t=0; t < nr_tasks; t++)
		printf("%6d  ", t);
	printf("\n");

	for (i=0; i < nr_runs; i++) {
		printf("%4d:   ", i);
		for (t=0; t < nr_tasks; t++) {
			unsigned long long itv = intervals[i][t];

			if (tasks_max[t] < itv)
				tasks_max[t] = itv;
			if (tasks_min[t] > itv)
				tasks_min[t] = itv;
			tasks_avg[t] += itv;
			printf("%6lld  ", nano2usec(itv));
		}
		printf("\n");
		printf(" len:   ");
		for (t=0; t < nr_tasks; t++) {
			unsigned long long len = intervals_length[i][t];

			printf("%6lld  ", nano2usec(len));
		}
		printf("\n");
		printf(" loops: ");
		for (t=0; t < nr_tasks; t++) {
			unsigned long loops = intervals_loops[i][t];

			printf("%6ld  ", loops);
		}
		printf("\n");
		printf("\n");
	}

	printf("Parent pid: %d\n", getpid());

	for (t=0; t < nr_tasks; t++) {
		printf(" Task %d (prio %d) (pid %ld):\n", t, calc_prio(t),
			thread_pids[t]);
		printf("   Max: %lld us\n", nano2usec(tasks_max[t]));
		printf("   Min: %lld us\n", nano2usec(tasks_min[t]));
		printf("   Tot: %lld us\n", nano2usec(tasks_avg[t]));
		printf("   Avg: %lld us\n", nano2usec(tasks_avg[t] / nr_runs));
		printf("\n");
	}

	if (check) {
		if (check < 0)
			printf(" Failed!\n");
		else
			printf(" Passed!\n");
	}
}

static void write_stats(FILE *f, void *data)
{
	int i;
	int t;
	unsigned long long tasks_max[nr_tasks];
	unsigned long long tasks_min[nr_tasks];
	unsigned long long tasks_avg[nr_tasks];

	memset(tasks_max, 0, sizeof(tasks_max[0])*nr_tasks);
	memset(tasks_min, 0xff, sizeof(tasks_min[0])*nr_tasks);
	memset(tasks_avg, 0, sizeof(tasks_avg[0])*nr_tasks);

	for (i=0; i < nr_runs; i++) {
		for (t=0; t < nr_tasks; t++) {
			unsigned long long itv = intervals[i][t];

			if (tasks_max[t] < itv)
				tasks_max[t] = itv;
			if (tasks_min[t] > itv)
				tasks_min[t] = itv;
			tasks_avg[t] += itv;
		}
	}

	fprintf(f, "  \"num_threads\": %d,\n", nr_tasks);
	fprintf(f, "  \"thread\": {\n");
	for (i = 0; i < nr_tasks; i++) {
		fprintf(f, "    \"%u\": {\n", i);
		fprintf(f, "      \"prio\": %d,\n", calc_prio(i));
		fprintf(f, "      \"min\": %lld,\n", nano2usec(tasks_min[i]));
		fprintf(f, "      \"avg\": %lld,\n", nano2usec(tasks_avg[i]) / nr_runs);
		fprintf(f, "      \"max\": %lld,\n", nano2usec(tasks_max[i]));
		fprintf(f, "      \"total\": %lld\n", nano2usec(tasks_avg[i]));
		fprintf(f, "    }%s\n", i == nr_tasks - 1 ? "" : ",");
	}
	fprintf(f, "  }\n");
}

static unsigned long busy_loop(unsigned long long start_time)
{
	unsigned long long time;
	unsigned long l = 0;

	do {
		l++;
		time = get_time();
	} while ((time - start_time) < RUN_INTERVAL);

	return l;
}

void *start_task(void *data)
{
	long id = (long)data;
	unsigned long long start_time;
	int prio = calc_prio(id);
	struct sched_param param = {
		.sched_priority = prio,
	};
	int ret;
	int high = 0;
	cpu_set_t cpumask;
	cpu_set_t save_cpumask;
	int cpu = 0;
	unsigned long l;
	long pid;

	ret = sched_getaffinity(0, sizeof(save_cpumask), &save_cpumask);
	if (ret < 0)
		perr("getting affinity");

	pid = gettid();
	thread_pids[id] = pid;

	/* Check if we are the highest prio task */
	if (id == nr_tasks-1)
		high = 1;

	ret = sched_setscheduler(0, SCHED_FIFO, &param);
	if (ret < 0 && !id)
		fprintf(stderr, "Warning, can't set priorities\n");

	while (!done) {
		if (high) {
			/* rotate around the CPUS */
			if (!CPU_ISSET(cpu, &save_cpumask))
				cpu = 0;
			CPU_ZERO(&cpumask);
			CPU_SET(cpu, &cpumask); cpu++;
			sched_setaffinity(0, sizeof(cpumask), &cpumask);
		}
		pthread_barrier_wait(&start_barrier);
		start_time = get_time();
		ftrace_write("Thread %d: started %lld diff %lld\n",
			     pid, start_time, start_time - now);
		l = busy_loop(start_time);
		record_time(id, start_time, l);
		pthread_barrier_wait(&end_barrier);
	}

	return (void*)pid;
}

static int check_times(int l)
{
	int i;
	unsigned long long last;
	unsigned long long last_loops;
	unsigned long long last_length;

	for (i=0; i < nr_tasks; i++) {
		if (i && last < intervals[l][i] &&
		    ((intervals[l][i] - last) > max_err)) {
			/*
			 * May be a false positive.
			 * Make sure that we did more loops
			 * our start is before the end
			 * and the end should be tested.
			 */
			if (intervals_loops[l][i] < last_loops ||
			    intervals[l][i] > last_length ||
			    (intervals_length[l][i] > last_length &&
			     intervals_length[l][i] - last_length > max_err)) {
				ftrace_write("Task %d FAILED\n", thread_pids[i]);
				check = -1;
				return 1;
			}
		}
		last = intervals[l][i];
		last_loops = intervals_loops[l][i];
		last_length = intervals_length[l][i];
	}
	return 0;
}

static void stop_log(int sig)
{
	stop = 1;
}

static int count_cpus(void)
{
	FILE *fp;
	char buf[2048];
	int cpus = 0;
	char *pbuf;
	size_t *pn;
	size_t n;
	int r;

	n = 2048;
	pn = &n;
	pbuf = buf;

	fp = fopen("/proc/cpuinfo", "r");
	if (!fp)
		perr("Can not read cpuinfo");

	while ((r = getline(&pbuf, pn, fp)) >= 0) {
		char *p;

		if (strncmp(buf, "processor", 9) != 0)
			continue;
		for (p = buf+9; isspace(*p); p++)
			;
		if (*p == ':')
			cpus++;
	}
	fclose(fp);

	return cpus;
}

int main (int argc, char **argv)
{
	pthread_t *threads;
	long i;
	int ret;
	struct timespec intv;
	struct sched_param param;

	rt_init(argc, argv);
	parse_options(argc, argv);

	signal(SIGINT, stop_log);
	signal(SIGALRM, stop_log);

	if (duration)
		alarm(duration);

	if (argc >= (optind + 1))
		nr_tasks = atoi(argv[optind]);
	else
		nr_tasks = count_cpus() + 1;

	threads = malloc(sizeof(*threads) * nr_tasks);
	if (!threads)
		perr("malloc");
	memset(threads, 0, sizeof(*threads) * nr_tasks);

	ret = pthread_barrier_init(&start_barrier, NULL, nr_tasks + 1);
	ret = pthread_barrier_init(&end_barrier, NULL, nr_tasks + 1);
	if (ret < 0)
		perr("pthread_barrier_init");

	intervals = malloc(sizeof(void*) * nr_runs);
	if (!intervals)
		perr("malloc intervals array");

	intervals_length = malloc(sizeof(void*) * nr_runs);
	if (!intervals_length)
		perr("malloc intervals length array");

	intervals_loops = malloc(sizeof(void*) * nr_runs);
	if (!intervals_loops)
		perr("malloc intervals loops array");

	thread_pids = malloc(sizeof(long) * nr_tasks);
	if (!thread_pids)
		perr("malloc thread_pids");

	for (i=0; i < nr_runs; i++) {
		intervals[i] = malloc(sizeof(unsigned long long)*nr_tasks);
		if (!intervals[i])
			perr("malloc intervals");
		memset(intervals[i], 0, sizeof(unsigned long long)*nr_tasks);

		intervals_length[i] = malloc(sizeof(unsigned long long)*nr_tasks);
		if (!intervals_length[i])
			perr("malloc length intervals");
		memset(intervals_length[i], 0, sizeof(unsigned long long)*nr_tasks);

		intervals_loops[i] = malloc(sizeof(unsigned long)*nr_tasks);
		if (!intervals_loops[i])
			perr("malloc loops intervals");
		memset(intervals_loops[i], 0, sizeof(unsigned long)*nr_tasks);
	}

	for (i=0; i < nr_tasks; i++) {
		if (pthread_create(&threads[i], NULL, start_task, (void *)i))
			perr("pthread_create");

	}

	/*
	 * Progress bar uses stderr to let users see it when
	 * redirecting output. So we convert stderr to use line
	 * buffering so the progress bar doesn't flicker.
	 */
	setlinebuf(stderr);

	/* up our prio above all tasks */
	memset(&param, 0, sizeof(param));
	param.sched_priority = nr_tasks + prio_start;
	if (sched_setscheduler(0, SCHED_FIFO, &param))
		fprintf(stderr, "Warning, can't set priority of main thread!\n");



	intv.tv_sec = nano2sec(INTERVAL);
	intv.tv_nsec = INTERVAL % sec2nano(1);

	if (!quiet)
		print_progress_bar(0);

	setup_ftrace_marker();

	for (loop=0; loop < nr_runs; loop++) {
		unsigned long long end;

		/* Release the CPU so all can get to the next barrier */
		nanosleep(&intv, NULL);

		now = get_time();

		ftrace_write("Loop %d now=%lld\n", loop, now);

		pthread_barrier_wait(&start_barrier);

		ftrace_write("All running!!!\n");

		nanosleep(&intv, NULL);

		if (!quiet)
			print_progress_bar((loop * 100)/nr_runs);

		end = get_time();
		ftrace_write("Loop %d end now=%lld diff=%lld\n", loop, end, end - now);

		pthread_barrier_wait(&end_barrier);

		if (stop || (check && check_times(loop))) {
			loop++;
			nr_runs = loop;
			break;
		}
	}
	putc('\n', stderr);

	pthread_barrier_wait(&start_barrier);
	done = 1;
	pthread_barrier_wait(&end_barrier);

	for (i=0; i < nr_tasks; i++)
		pthread_join(threads[i], (void*)&thread_pids[i]);

	print_results();

	if (strlen(jsonfile) != 0)
		rt_write_json(jsonfile, check < 0, write_stats, NULL);

	if (stop) {
		/*
		 * We use this test in bash while loops
		 * So if we hit Ctrl-C then let the while
		 * loop know to break.
		 */
		if (check < 0)
			exit(-1);
		else
			exit(1);
	}
	if (check < 0)
		exit(-1);
	else
		exit(0);

	return 0;
}
