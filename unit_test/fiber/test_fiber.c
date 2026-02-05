#include "lib_acl.h"
#include "test_fiber.h"
#include "test_fibertab.h"

#include "fiber/libfiber.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(HAS_KQUEUE)
#include <sys/event.h>
#endif

static int g_failed = 0;

#if defined(HAS_KQUEUE)
static void test_fail(const char *msg)
{
	printf("FAIL: %s\n", msg);
	g_failed = 1;
}

static int expect_filters(struct kevent *events, int n,
	short filter1, short filter2)
{
	int has1 = 0, has2 = 0, i;

	for (i = 0; i < n; i++) {
		if (events[i].filter == filter1) {
			has1 = 1;
		} else if (events[i].filter == filter2) {
			has2 = 1;
		}
	}

	return has1 && has2;
}

static void test_dual_read_write(int fd_read, int fd_write)
{
	int kqfd;
	struct kevent changes[2];
	struct kevent events[2];
	int n;
	char buf[8];

	kqfd = kqueue();
	if (kqfd < 0) {
		test_fail("kqueue() failed");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ADD | EV_ENABLE,
		0, 0, (void *) 0x11);
	EV_SET(&changes[1], fd_read, EVFILT_WRITE, EV_ADD | EV_ENABLE,
		0, 0, (void *) 0x22);

	if (kevent(kqfd, changes, 2, NULL, 0, NULL) < 0) {
		test_fail("kevent add failed");
		return;
	}

	if (write(fd_write, "x", 1) != 1) {
		test_fail("write failed");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, NULL);
	if (n < 0) {
		test_fail("kevent wait failed");
		return;
	}

	if (n < 2 || !expect_filters(events, n, EVFILT_READ, EVFILT_WRITE)) {
		test_fail("expected both READ and WRITE events");
	}

	(void) read(fd_read, buf, sizeof(buf));
}

static void test_ev_clear(int fd_read, int fd_write)
{
#ifdef EV_CLEAR
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	struct timespec ts;
	char buf[8];

	kqfd = kqueue();
	if (kqfd < 0) {
		test_fail("kqueue() failed (ev_clear)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ,
		EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, (void *) 0x44);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		test_fail("kevent add ev_clear failed");
		return;
	}

	if (write(fd_write, "c", 1) != 1) {
		test_fail("write failed (ev_clear)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, NULL);
	if (n != 1 || events[0].filter != EVFILT_READ) {
		test_fail("ev_clear: first read event missing");
		return;
	}

	/* No read yet, should remain disabled. */
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &ts);
	if (n != 0) {
		test_fail("ev_clear: event should be disabled before read");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));

	/* After read, rearm and verify again. */
	if (write(fd_write, "d", 1) != 1) {
		test_fail("write failed (ev_clear rearm)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, NULL);
	if (n != 1 || events[0].filter != EVFILT_READ) {
		test_fail("ev_clear: rearmed event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));
#else
	(void) fd_read;
	(void) fd_write;
	printf("EV_CLEAR not supported, skip ev_clear test\n");
#endif
}

static void test_dispatch(int fd_read, int fd_write)
{
#ifdef EV_DISPATCH
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	char buf[8];

	kqfd = kqueue();
	if (kqfd < 0) {
		test_fail("kqueue() failed (dispatch)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ,
		EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, (void *) 0x33);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		test_fail("kevent add dispatch failed");
		return;
	}

	if (write(fd_write, "y", 1) != 1) {
		test_fail("write failed (dispatch)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, NULL);
	if (n != 1 || events[0].filter != EVFILT_READ) {
		test_fail("dispatch: first read event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));

	/* The event should be disabled after the first delivery. */
	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){0, 0});
	if (n != 0) {
		test_fail("dispatch: event should be disabled");
		return;
	}

	/* Re-enable and verify it fires again. */
	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		test_fail("dispatch: enable failed");
		return;
	}

	if (write(fd_write, "z", 1) != 1) {
		test_fail("write failed (dispatch re-enable)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, NULL);
	if (n != 1 || events[0].filter != EVFILT_READ) {
		test_fail("dispatch: re-enabled event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));
#else
	(void) fd_read;
	(void) fd_write;
	printf("EV_DISPATCH not supported, skip dispatch test\n");
#endif
}
#endif

static void fiber_main(ACL_FIBER *fiber, void *ctx acl_unused)
{
#if !defined(HAS_KQUEUE)
	printf("HAS_KQUEUE not defined, skip kqueue hook tests\n");
	acl_fiber_schedule_stop();
	(void) fiber;
	return;
#else
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		test_fail("socketpair failed");
		acl_fiber_schedule_stop();
		return;
	}

	test_dual_read_write(fds[0], fds[1]);
	test_ev_clear(fds[0], fds[1]);
	test_dispatch(fds[0], fds[1]);

	close(fds[0]);
	close(fds[1]);

	acl_fiber_schedule_stop();
	(void) fiber;
#endif
}

int test_kqueue_hook(AUT_LINE *test_line, void *arg acl_unused)
{
	(void) test_line;

	g_failed = 0;

	acl_fiber_msg_stdout_enable(1);
	acl_fiber_create(fiber_main, NULL, 128000);
	acl_fiber_schedule_with(FIBER_EVENT_KERNEL);

	return g_failed ? -1 : 0;
}

void test_fiber_register(void)
{
	aut_register(__test_fn_tab);
}
