
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
# define	HAS_KQUEUE
#endif

#if defined(HAS_KQUEUE)
#include <sys/event.h>
#endif

#include "lib_acl.h"
#include "fiber/libfiber.h"

static int g_failed = 0;

#if defined(HAS_KQUEUE)

static void fail(const char *msg)
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

static int expect_filter(struct kevent *events, int n, short filter)
{
	int i;
	for (i = 0; i < n; i++) {
		if (events[i].filter == filter) {
			return 1;
		}
	}
	return 0;
}

static int with_socketpair(void (*fn)(int, int))
{
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		fail("socketpair failed");
		return -1;
	}
	fn(fds[0], fds[1]);
	close(fds[0]);
	close(fds[1]);
	return 0;
}

static void drain_read(int fd)
{
	char buf[64];
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n > 0) {
			continue;
		}
		if (n == 0) {
			break;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		}
		break;
	}
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
		fail("kqueue() failed");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ADD | EV_ENABLE,
		0, 0, (void *) 0x11);
	EV_SET(&changes[1], fd_read, EVFILT_WRITE, EV_ADD | EV_ENABLE,
		0, 0, (void *) 0x22);

	if (kevent(kqfd, changes, 2, NULL, 0, NULL) < 0) {
		fail("kevent add failed");
		return;
	}

	if (write(fd_write, "x", 1) != 1) {
		fail("write failed");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n < 0) {
		fail("kevent wait failed");
		return;
	}

	if (n < 2 || !expect_filters(events, n, EVFILT_READ, EVFILT_WRITE)) {
		fail("expected both READ and WRITE events");
	}

	n = read(fd_read, buf, sizeof(buf) - 1);
	if (n <= 0) {
		fail("read failed");
	}

	buf[n] = '\0';
	printf("Read: %s\r\n", buf);

	close(kqfd);
}

static void test_oneshot(int fd_read, int fd_write)
{
#ifdef EV_ONESHOT
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	char buf[8];

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (oneshot)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ,
		EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add oneshot failed");
		return;
	}

	if (write(fd_write, "o", 1) != 1) {
		fail("write failed (oneshot)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || !expect_filter(events, n, EVFILT_READ)) {
		fail("oneshot: first read event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));

	/* Should be removed after oneshot. */
	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){0, 0});
	if (n != 0) {
		fail("oneshot: event should be removed");
		return;
	}

	close(kqfd);
#else
	(void) fd_read;
	(void) fd_write;
	printf("EV_ONESHOT not supported, skip oneshot test\n");
#endif
}

static void test_disable_enable(int fd_read, int fd_write)
{
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	char buf[8];

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (disable/enable)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ADD | EV_DISABLE,
		0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add disable failed");
		return;
	}

	if (write(fd_write, "d", 1) != 1) {
		fail("write failed (disable)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){0, 0});
	if (n != 0) {
		fail("disable: event should be disabled");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("enable failed");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || !expect_filter(events, n, EVFILT_READ)) {
		fail("enable: read event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));
	close(kqfd);
}

static void test_delete(int fd_read, int fd_write)
{
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (delete)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add delete failed");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_DELETE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent delete failed");
		return;
	}

	if (write(fd_write, "x", 1) != 1) {
		fail("write failed (delete)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){0, 0});
	if (n != 0) {
		fail("delete: event should be removed");
		return;
	}

	if (kevent(kqfd, changes, 1, NULL, 0, NULL) >= 0) {
		fail("delete: expected error on deleting non-existent filter");
		return;
	}
	if (acl_fiber_last_error() != ENOENT) {
		fail("delete: expected ENOENT");
	}

	close(kqfd);
}

static void test_timeout(int fd_read, int fd_write)
{
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	struct timespec ts;

	(void) fd_write;

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (timeout)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add timeout failed");
		return;
	}

	/* timeout = 0 (return immediately) */
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	n = kevent(kqfd, NULL, 0, events, 2, &ts);
	if (n != 0) {
		fail("timeout: zero should return 0");
		return;
	}

	/* invalid tv_nsec */
	ts.tv_sec = 0;
	ts.tv_nsec = 2000000000;
	n = kevent(kqfd, NULL, 0, events, 2, &ts);
	if (n >= 0) {
		fail("timeout: invalid tv_nsec should fail");
		return;
	}
	if (acl_fiber_last_error() != FIBER_EINVAL) {
		fail("timeout: expected EINVAL");
		return;
	}

	close(kqfd);
}

static void test_error_eof(void)
{
#ifdef EV_EOF
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
		fail("socketpair failed (eof)");
		return;
	}

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (eof)");
		return;
	}

	EV_SET(&changes[0], fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add eof failed");
		return;
	}

	close(fds[1]);

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n < 1 || !expect_filter(events, n, EVFILT_READ)) {
		fail("eof: read event missing");
		return;
	}
	{
		char buf[8];
		int r = (int) read(fds[0], buf, sizeof(buf));
		if (r != 0) {
			fail("eof: read should return 0");
			return;
		}
	}

	close(kqfd);
#ifdef _WIN32
	(void) fds[0];
#else
	close(fds[0]);
#endif
#else
	printf("EV_EOF not supported, skip eof test\n");
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
		fail("kqueue() failed (dispatch)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ,
		EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, (void *) 0x33);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add dispatch failed");
		return;
	}

	if (write(fd_write, "y", 1) != 1) {
		fail("write failed (dispatch)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || events[0].filter != EVFILT_READ) {
		fail("dispatch: first read event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));

	/* The event should be disabled after the first delivery. */
	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){0, 0});
	if (n != 0) {
		fail("dispatch: event should be disabled");
		return;
	}

	/* Re-enable and verify it fires again. */
	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("dispatch: enable failed");
		return;
	}

	if (write(fd_write, "z", 1) != 1) {
		fail("write failed (dispatch re-enable)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || events[0].filter != EVFILT_READ) {
		fail("dispatch: re-enabled event missing");
		return;
	}

	(void) read(fd_read, buf, sizeof(buf));

	close(kqfd);
#else
	(void) fd_read;
	(void) fd_write;
	printf("EV_DISPATCH not supported, skip dispatch test\n");
#endif
}
#endif

static void test_ev_clear(int fd_read, int fd_write)
{
#ifdef EV_CLEAR
	int kqfd;
	struct kevent changes[1];
	struct kevent events[2];
	int n;
	struct timespec ts;

	kqfd = kqueue();
	if (kqfd < 0) {
		fail("kqueue() failed (ev_clear)");
		return;
	}

	EV_SET(&changes[0], fd_read, EVFILT_READ,
		EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("kevent add ev_clear failed");
		return;
	}

	if (write(fd_write, "c", 1) != 1) {
		fail("write failed (ev_clear)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || !expect_filter(events, n, EVFILT_READ)) {
		fail("ev_clear: first read event missing");
		return;
	}

	/* No read yet, should remain disabled. */
	ts.tv_sec = 0;
	ts.tv_nsec = 0;
	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &ts);
	if (n != 0) {
		fail("ev_clear: event should be disabled before read");
		return;
	}

	drain_read(fd_read);

	/* Re-enable the filter, then verify it fires again. */
	EV_SET(&changes[0], fd_read, EVFILT_READ, EV_ENABLE, 0, 0, NULL);
	if (kevent(kqfd, changes, 1, NULL, 0, NULL) < 0) {
		fail("ev_clear: enable failed");
		return;
	}

	if (write(fd_write, "d", 1) != 1) {
		fail("write failed (ev_clear rearm)");
		return;
	}

	memset(events, 0, sizeof(events));
	n = kevent(kqfd, NULL, 0, events, 2, &(struct timespec){1, 0});
	if (n != 1 || !expect_filter(events, n, EVFILT_READ)) {
		fail("ev_clear: rearmed event missing");
		return;
	}

	drain_read(fd_read);
	close(kqfd);
#else
	(void) fd_read;
	(void) fd_write;
	printf("EV_CLEAR not supported, skip ev_clear test\n");
#endif
}

static void fiber_main(ACL_FIBER *fiber, void *ctx acl_unused)
{
#if !defined(HAS_KQUEUE)
	(void) fiber;
	printf("HAS_KQUEUE not defined, skip kqueue hook tests\n");
	acl_fiber_schedule_stop();
	return;
#else
	printf("[TEST] dual_read_write\r\n");
	with_socketpair(test_dual_read_write);
	printf("[TEST] oneshot\r\n");
	with_socketpair(test_oneshot);
	printf("[TEST] disable_enable\r\n");
	with_socketpair(test_disable_enable);
	printf("[TEST] delete\r\n");
	with_socketpair(test_delete);
	printf("[TEST] timeout\r\n");
	with_socketpair(test_timeout);
	printf("[TEST] error_eof\r\n");
	test_error_eof();
	printf("[TEST] ev_clear\r\n");
	with_socketpair(test_ev_clear);
	printf("[TEST] dispatch\r\n");
	with_socketpair(test_dispatch);

	acl_fiber_schedule_stop();
	(void) fiber;
#endif
}

int main(int argc acl_unused, char **argv acl_unused)
{
	acl_fiber_msg_stdout_enable(1);

	acl_fiber_create(fiber_main, NULL, 128000);
	acl_fiber_schedule_with(FIBER_EVENT_KERNEL);

	return g_failed ? 1 : 0;
}
