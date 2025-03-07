// SPDX-License-Identifier: GPL-2.0-or-later
/*
	Pip stress - Priority Inheritance with processes

    Copyright (C) 2009, John Kacur <jkacur@redhat.com>
*/

/*
 * This program demonstrates the technique of using priority inheritance (PI)
 * mutexes with processes instead of threads.
 * The way to do this is to obtain some shared memory - in this case with
 * mmap that backs a pthread_mutex_t since this will support PI.
 * Pay particular attention to how this is intialized to support processes.
 * Function init_shared_pthread_mutex() does this by setting the
 * pthread_mutexattr to PTHREAD_PROCESS_SHARED and the mutex protocol to
 * PTHREAD_PRIO_INHERIT.
 * In this program we purposely try to invoke a classic priority inversion.
 * A low priority process grabs the mutex and does some work.
 * A high priority process comes a long and is blocked since the mutex is taken.
 * A medium priority process that doesn't require the mutex then takes the
 * processor. Because the processes are restricted to one cpu, the low priority
 * processes never makes any progress because the medium priority process
 * runs in an infinite loop. This is a priority inversion because the
 * medium priority process is running at the expensive of the high priority
 * process. However, since we have used PRIO_INHERIT and are running on a
 * machine that supports preemption, the high priority process will lend it's
 * priority to the low priority process which will preempt the medium priority
 * process. The low priority process will then release the mutex which the
 * high priority process can obtain. When the high priority process gets to run
 * it kills the medium priority process.
 * The state structure keeps track of the progress. Although this program
 * is set up to likely trigger an inversion, there is no guarantee that
 * scheduling will make that happen. After the program completes it reports
 * whether a priority inversion occurred or not. In either case this program
 * demonstrates how to use priority inheritance mutexes with processes.
 * In fact, you would be better off to avoid scenarios in which a priority
 * inversion occurs if possible - this program tries to trigger them just
 * to show that it works. If you are having difficulty triggering an inversion,
 * merely increase the time that the low priority process sleeps while
 * holding the lock. (usleep);
 * Also note that you have to run as a user with permission to change
 * scheduling priorities.
 */

#include "pip_stress.h"

#include <unistd.h>
#include <getopt.h>

/* default time for low priority thread usleep */
useconds_t usleep_val = 500;

pthread_mutex_t *resource;

/* This records the state to determine whether a priority inversion occurred */
struct State {
	int low_owns_resource;
	int high_started;
	int high_owns_resource;
	int medium_started;
	int inversion;
	pthread_mutex_t *mutex;
};

struct State *statep;

const int policy = SCHED_FIFO;
static int prio_min;	/* Initialized for the minimum priority of policy */

static void usage(int error)
{
	printf("pip_stress V %1.2f\n", VERSION);
	printf("Usage:\n"
	       "pip_stress <options>\n"\
	       "-h       --help          Show this help menu.\n"\
	       "-u TIME  --usleep=TIME   Specify the sleep time in usec of the low priority process.\n"\
	       "                         Defaults to 500.\n"
	       );
	exit(error);
}

int main(int argc, char *argv[])
{
	void *mptr;	/* memory pointer */
	pid_t pid1, pid2;
	cpu_set_t set, *setp = &set;
	int res;
	int *minimum_priority = (int*)&prio_min;

	for (;;) {
		struct option long_options[] = {
			{ "help",	no_argument,		NULL,	'h' },
			{ "usleep",	required_argument,	NULL,	'u' },
			{ NULL,		0,			NULL,	0 },
		};

		int c = getopt_long(argc, argv, "hu:", long_options, NULL);
		if (c == -1)
			break;
		switch (c) {
		case 'h':
			usage(0);
			break;
		case 'u':
			usleep_val = strtoul(optarg, NULL, 10);
			break;
		default:
			usage(1);
			break;
		};
	}

	*minimum_priority = sched_get_priority_min(policy);

	if (check_privs())
		exit(1);

	mptr = mmap_page();	/* Get a page of shared memory */
	resource = (pthread_mutex_t*)mptr;	/* point our lock to it */
	mptr += sizeof(pthread_mutex_t);	/* advance the memory pointer */

	/* Initialize our mutex via the resource pointer */
	init_shared_pthread_mutex(resource, PTHREAD_PRIO_INHERIT);

	statep = (struct State*)mptr;
	mptr += sizeof(struct State);

	init_state();	/* Initialize the state structure */

	statep->mutex = (pthread_mutex_t*)mptr;	/* point the next lock to it */
	mptr += sizeof(pthread_mutex_t);	/* advance the memory pointer */

	/* Initialize our State mutex */
	init_shared_pthread_mutex(statep->mutex, PTHREAD_PRIO_NONE);

	set_rt_prio(0, prio_min, policy);

	/* We restrict this program to the first cpu, inorder to increase
	 * the likelihood of a priority inversion */
	CPU_ZERO(setp);
	CPU_SET(0, setp);
	res = sched_setaffinity(0, sizeof(set), setp);
	if (res == -1) {
		int err = errno;
		err_msg("sched_setaffinity: ");
		err_exit(err, NULL);
	}

	pid1 = fork();
	if (pid1 == -1) {
		perror("fork");
		exit(1);
	} else if (pid1 != 0) {		/* parent code */
		low(pid1);
	} else {			/* child code */
		pid2 = fork();		/* parent code */
		if (pid2 == -1) {
			perror("fork: ");
			exit(1);
		} else if (pid2 != 0) {		/* parent code */
			high(pid2);
		} else {			/* child code */
			medium();
		}
	}

	exit(0);
}

/* Initialize the structure that tracks when a priority inversion occurs */
void init_state(void)
{
	/* Init the State structure */
	statep->low_owns_resource = 0;
	statep->high_started = 0;
	statep->high_owns_resource = 0;
	statep->medium_started = 0;
	/* Assume an inversion will occur until proven false */
	statep->inversion = 1;
}

/* @pid = high priority process pid */
void low(pid_t pid)
{
	int status;
	Pthread_mutex_lock(resource);
		Pthread_mutex_lock(statep->mutex);
			statep->low_owns_resource = 1;
			if (statep->high_owns_resource ||
					statep->medium_started) {
				statep->inversion = 0;
			}
		Pthread_mutex_unlock(statep->mutex);
		usleep(usleep_val);
	Pthread_mutex_unlock(resource);
	waitpid(pid, &status, 0);
}

void medium(void)
{
	set_rt_prio(0, prio_min+1, policy);
	Pthread_mutex_lock(statep->mutex);
		statep->medium_started = 1;
		if (!statep->high_started)
			statep->inversion = 0;
	Pthread_mutex_unlock(statep->mutex);

	for(;;);	/* infinite loop */
}

/* @pid = medium priority process pid */
void high(pid_t pid)
{
	int status;
	set_rt_prio(0, prio_min+2, policy);

	/* Must come after raising the priority */
	Pthread_mutex_lock(statep->mutex);
		statep->high_started = 1;
	Pthread_mutex_unlock(statep->mutex);

	Pthread_mutex_lock(resource);
	Pthread_mutex_lock(statep->mutex);
		statep->high_owns_resource = 1;
		if (!statep->low_owns_resource || !statep->medium_started)
			statep->inversion = 0;
	Pthread_mutex_unlock(statep->mutex);
	Pthread_mutex_unlock(resource);
	kill(pid, SIGKILL);	/* kill the medium thread */
	waitpid(pid, &status, 0);

	Pthread_mutex_lock(statep->mutex);

	if (statep->inversion)
		printf("Successfully used priority inheritance to handle an inversion\n");
	else
		printf("No inversion incurred\n");
	Pthread_mutex_unlock(statep->mutex);
}

/* mmap a page of anonymous shared memory */
void *mmap_page(void)
{
	void *mptr;
	long pgsize = sysconf(_SC_PAGE_SIZE);

	mptr = mmap(NULL, pgsize, PROTRW, MMAP_FLAGS, 0, 0);
	if (mptr == MAP_FAILED) {
		perror("In function mmap_page - mmap");
		exit(1);
	}

	return mptr;
}

long process_shared_mutex_available(void)
{
	long res = -1;	/* undefined */
#ifdef _POSIX_THREAD_PROCESS_SHARED
	res = sysconf(_SC_THREAD_PROCESS_SHARED);
	if (res == -1) {
		int err = errno;	/* save the error number */
		err_msg("%s: sysconf(_SC_THREAD_PROCESS_SHARED): ", __func__);
		err_exit(err, NULL);
	}
#else
#error _POSIX_THREAD_PROCESS_SHARED is not defined
#endif
	return res;
}

void Pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	int err;
	err = pthread_mutexattr_init(attr);
	if (err) {
		err_msg("%s: pthread_mutexattr_init(): ", __func__);
		err_exit(err, NULL);
	}
}

void Pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
	int err;
	err = pthread_mutexattr_setpshared(attr, pshared);
	if (err) {
		err_msg("%s: pthread_mutexattr_setpshared(): ", __func__);
		err_exit(err, NULL);
	}
}

void Pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol)
{
	int err;
	err = pthread_mutexattr_setprotocol(attr, protocol);
	if (err) {
		err_msg("%s: pthread_mutexattr_setprotocol(): ", __func__);
		err_exit(err, NULL);
	}
}

void Pthread_mutex_init(pthread_mutex_t *mutex, pthread_mutexattr_t *attr)
{
	int err;
	err = pthread_mutex_init(mutex, attr);
	if (err) {
		err_msg("%s: pthread_mutex_init(): ", __func__);
		err_exit(err, NULL);
	}
}

void Pthread_mutex_lock(pthread_mutex_t *mutex)
{
	int err;
	err = pthread_mutex_lock(mutex);
	if (err) {
		err_msg("%s: pthread_mutex_lock(): ", __func__);
		err_exit(err, NULL);
	}
}

void Pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	int err;
	err = pthread_mutex_unlock(mutex);
	if (err) {
		err_msg("%s: pthread_mutex_unlock(): ", __func__);
		err_exit(err, NULL);
	}
}

void init_shared_pthread_mutex(pthread_mutex_t *mutex, int protocol)
{
	pthread_mutexattr_t attr;

	process_shared_mutex_available();

	Pthread_mutexattr_init(&attr);
	Pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	Pthread_mutexattr_setprotocol(&attr, protocol);

	Pthread_mutex_init(mutex, &attr);
}

/* Set the priority and policy of a process */
int set_rt_prio(pid_t pid, int prio, int policy)
{
	int err;
	struct sched_param param;
	struct sched_param *pparam = &param;
	pparam->sched_priority = prio;
	err = sched_setscheduler(pid, policy, pparam);
	if (err) {
		err = errno;	/* save the errno */
		err_msg_n(err, "%s: sched_setscheduler(): ", __func__);
		err_msg("%s: prio = %d\n", __func__, prio);
		err_msg("%s: pparam->sched_priority = %d\n", __func__, pparam->sched_priority);
		err_msg("%s: policy = %d\n", __func__, policy);
	}
	return err;	/* 0 on success */
}

int get_rt_prio(pid_t pid)
{
	int err;
	struct sched_param param;
	err = sched_getparam(pid, &param);
	if (err) {
		err = errno;	/* save the errno */
		err_msg_n(err, "%s: get_rt_prio(): ", __func__);
		return -1;
	}
	return param.sched_priority;
}
