#include "lib_acl.h"
#include <stdio.h>
#include <stdlib.h>
#include "fiber/lib_fiber.h"

static int __fibers_count = 10;

static void fiber_main(ACL_FIBER *fiber acl_unused, void *ctx)
{
	ACL_FIBER_SEM *sem = (ACL_FIBER_SEM *) ctx;
	int left;

	printf("fiber-%d begin to sem_wait\r\n", acl_fiber_self());
	left = acl_fiber_sem_wait(sem);
	printf("fiber-%d sem_wait ok, left: %d\r\n", acl_fiber_self(), left);

	printf("fiber-%d begin sleep\r\n", acl_fiber_self());
	acl_fiber_sleep(1);
	printf("fiber-%d wakeup\r\n", acl_fiber_self());

	left = acl_fiber_sem_post(sem);
	printf("fiber-%d sem_post ok, left: %d\r\n", acl_fiber_self(), left);
}

static void fiber_waiter(ACL_FIBER *fiber, void *ctx)
{
	ACL_FIBER_SEM *sem = (ACL_FIBER_SEM *) ctx;
	int ret;

	printf("fiber waiter: %d\r\n", acl_fiber_self());
	sleep(1);
	ret = acl_fiber_sem_wait(sem);
	printf("sem: %d, killed: %s\r\n",
		ret, acl_fiber_killed(fiber) ? "yes" : "no");
}

struct WAITERS {
	ACL_FIBER_SEM *sem;
	ACL_FIBER **waiters;
	int n;
};

static void fiber_killer(ACL_FIBER *fiber acl_unused, void *ctx)
{
	struct WAITERS *waiters = (struct WAITERS *) ctx;
	int i;

	acl_fiber_sem_wait(waiters->sem);
	for (i = 0; i < 5; i++) {
		sleep(1);
		printf("killer wakeup, i=%d\r\n", i);
	}

	for (i = 0; i < waiters->n; i++) {
		acl_fiber_kill(waiters->waiters[i]);
	}
}

static void fiber_timed_wait(ACL_FIBER *fiber acl_unused, void *ctx)
{
	ACL_FIBER_SEM *sem = (ACL_FIBER_SEM *) ctx;
	int ret, timeo = 10000;
	time_t begin, end;

	begin = time(NULL);
	ret = acl_fiber_sem_timed_wait(sem, timeo);
	end = time(NULL);

	if (ret >= 0) {
		printf("%s: wait ok, tc: %ld\r\n", __FUNCTION__, end - begin);
	} else if (errno == EAGAIN) {
		printf("%s: wait timed out, error=%s, tc: %ld\r\n",
			__FUNCTION__, strerror(errno), end - begin);
	} else {
		printf("%s: wait error, error=%s, tc: %ld\r\n",
			__FUNCTION__, strerror(errno), end - begin);
	}
}

static void usage(const char *procname)
{
	printf("usage: %s -h [help]\r\n"
		" -n sem_max\r\n"
		" -c fibers_count\r\n"
		" -A [if wakeup the waiter in async mode, default: false]\r\n",
		procname);
}

int main(int argc, char *argv[])
{
	int  ch, nfibers = __fibers_count, i, sem_max = 2, async_mode = 0;
	ACL_FIBER_SEM *sem;
	struct WAITERS waiters;

	while ((ch = getopt(argc, argv, "hn:c:A")) > 0) {
		switch (ch) {
		case 'h':
			usage(argv[0]);
			return 0;
		case 'n':
			sem_max = atoi(optarg);
			break;
		case 'c':
			nfibers = atoi(optarg);
			break;
		case 'A':
			async_mode = 1;
			break;
		default:
			break;
		}
	}

	__fibers_count = nfibers;

	sem = acl_fiber_sem_create2(sem_max, async_mode ?
			ACL_FIBER_SEM_F_ASYNC : 0);

	for (i = 0; i < nfibers; i++) {
		acl_fiber_create(fiber_main, sem, 128000);
	}

	waiters.sem = acl_fiber_sem_create(1);
	waiters.waiters = (ACL_FIBER **) calloc(nfibers, sizeof(ACL_FIBER*));
	waiters.n = nfibers;

	for (i = 0; i < nfibers; i++) {
		waiters.waiters[i] = acl_fiber_create(fiber_waiter,
			waiters.sem, 128000);
	}

	acl_fiber_create(fiber_killer, &waiters, 128000);

	acl_fiber_create(fiber_timed_wait, sem, 128000);

	acl_fiber_schedule();

	acl_fiber_sem_free(sem);
	acl_fiber_sem_free(waiters.sem);

	printf("--- All fibers Over ----\r\n");
	return 0;
}
