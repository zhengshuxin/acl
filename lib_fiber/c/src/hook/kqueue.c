#include "stdafx.h"

#if defined(HAS_KQUEUE) && !defined(DISABLE_HOOK)

#include "common.h"
#include "fiber/libfiber.h"
#include "event.h"
#include "fiber.h"
#include "hook.h"
#include <sys/event.h>

/****************************************************************************/

/**
 * One socket fd has one KQUEUE_CTX in kqueue mode which was set in FILE_EVENT.
 */
struct KQUEUE_CTX {
	int  fd;		// The socket fd.
	int  mask;		// The event mask been set (EVENT_READ | EVENT_WRITE).
	int  rmask;		// The result event mask.
	FILE_EVENT *fe;		// Refer to the FILE_EVENT with the socket fd.
	KQUEUE *kq;		// Refer to the KQUEUE with the kqfd.
	uintptr_t ident;	// Same as kevent's ident (fd).
	short filter;		// EVFILT_READ or EVFILT_WRITE.
	void *udata;		// User data (same as kevent's udata).
};

struct KQUEUE_EVENT {
	RING        me;
	RING        me4kq;
	ACL_FIBER  *fiber;

	kqueue_proc *proc;
	long long   expire;

	struct kevent *events;
	int nevents;
	int nready;
};

/**
 * One kqueue fd with one KQUEUE with one KQUEUE_EVENT and many KQUEUE_CTX.
 *
 *          KQUEUE_EVENT
 *               ^
 *               |
 * kqueue fd -> KQUEUE -|-> socket -> KQUEUE_CTX
 *                      |-> socket -> KQUEUE_CTX
 *                      |-> socket -> KQUEUE_CTX
 *                      |-> ...
 */

struct KQUEUE {
	int          kqfd;	// Duplicate the current thread's kqueue fd.
	KQUEUE_CTX **fds;	// Hold KQUEUE_CTX objects of the kqfd.
	size_t       nfds;	// The fds array's size.
	RING         kes;	// Refer to kqueue fd's KQUEUE_EVENT.
};

// Hold the KQUEUE objects of one thread; And the KQUEUE object was created in
// kqueue/kqueue_try_register.
static __thread ARRAY *__kqfds = NULL;
static ARRAY     *__main_kqfds = NULL;

static KQUEUE *kqueue_alloc(int kqfd);
static void kqueue_free(KQUEUE *kq);

/****************************************************************************/

void acl_fiber_share_kqueue(int yes)
{
	(void) yes;
}

// Maybe called by the fcntl API being hooked.
int kqueue_try_register(int kqfd)
{
	int sys_kqfd;
	EVENT *ev;
	KQUEUE *kq;

	if (kqfd < 0) {
		return -1;
	}

	ev = fiber_io_event();
	if (ev == NULL) {
		return -1;
	}

	sys_kqfd = (int) event_handle(ev);
	assert(sys_kqfd >= 0);

	if (kqfd != sys_kqfd) {
		ITER iter;
		int found = 0;
		foreach(iter, __kqfds) {
			KQUEUE *tmp = (KQUEUE *) iter.data;
			if (tmp->kqfd == kqfd) {
				found = 1;
				break;
			}
		}
		if (!found) {
			return -1;
		}
	}

	kqfd = kqfd >= 0 ? dup(kqfd) : socket(AF_INET, SOCK_STREAM, 0);
	if (kqfd < 0) {
		msg_error("%s(%d): create fd error: %s",
			__FILE__, __LINE__, last_serror());
		return -1;
	}

	kq = kqueue_alloc(kqfd);
	return kq->kqfd;
}

/****************************************************************************/

static void kqueue_event_free(KQUEUE_EVENT *ke)
{
	mem_free(ke);
}

static KQUEUE_EVENT *kqueue_event_alloc(void)
{
	KQUEUE_EVENT *ke = (KQUEUE_EVENT*) mem_calloc(1, sizeof(KQUEUE_EVENT));

	ring_init(&ke->me);
	ring_init(&ke->me4kq);
	return ke;
}

static void fiber_on_exit(void *ctx)
{
	KQUEUE_EVENT *ke = (KQUEUE_EVENT*) ctx;
	kqueue_event_free(ke);
}

static __thread int __local_key;

static KQUEUE_EVENT *fiber_kqueue_event()
{
	KQUEUE_EVENT *ke = (KQUEUE_EVENT*) acl_fiber_get_specific(__local_key);
	if (ke) {
		return ke;
	}

	ke = kqueue_event_alloc();
	acl_fiber_set_specific(&__local_key, ke, fiber_on_exit);
	ke->fiber = acl_fiber_running();
	return ke;
}

/****************************************************************************/

static pthread_key_t  __once_key;
static pthread_once_t __once_control = PTHREAD_ONCE_INIT;

static void thread_free(void *ctx fiber_unused)
{
	ITER iter;

	if (__kqfds == NULL) {
		return;
	}

	if (__kqfds == __main_kqfds) {
		__main_kqfds = NULL;
	}

	foreach(iter, __kqfds) {
		KQUEUE *kq = (KQUEUE *) iter.data;

		if (kq->kqfd >= 0 && (*sys_close)(kq->kqfd) < 0) {
			fiber_save_errno(acl_fiber_last_error());
		}

		kqueue_free(kq);
	}

	array_free(__kqfds, NULL);
	__kqfds = NULL;
}

static void main_thread_free(void)
{
	if (__main_kqfds) {
		thread_free(__main_kqfds);
		__main_kqfds = NULL;
	}
}

static void thread_init(void)
{
	if (pthread_key_create(&__once_key, thread_free) != 0) {
		msg_fatal("%s(%d), %s: pthread_key_create error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
}

// Create one KQUEUE for the specified kqfd.
static KQUEUE *kqueue_alloc(int kqfd)
{ 
	KQUEUE *kq;
	int maxfd = open_limit(0), i;

	if (maxfd <= 0) {
		msg_fatal("%s(%d), %s: open_limit error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}

	/* Using thread local to store the kqueue handles for each thread. */
	if (__kqfds == NULL) {
		if (pthread_once(&__once_control, thread_init) != 0) {
			msg_error("%s(%d), %s: pthread_once error %s",
				__FILE__, __LINE__, __FUNCTION__, last_serror());
			return NULL;
		}

		__kqfds = array_create(5, ARRAY_F_UNORDER);
		if (thread_self() == main_thread_self()) {
			__main_kqfds = __kqfds;
			atexit(main_thread_free);
		} else if (pthread_setspecific(__once_key, __kqfds) != 0) {
			msg_error("pthread_setspecific error!");
			return NULL;
		}
	}

	kq = (KQUEUE*) mem_malloc(sizeof(KQUEUE));
	array_append(__kqfds, kq);

	kq->kqfd = kqfd;
	kq->nfds = maxfd;
	kq->fds  = (KQUEUE_CTX **) mem_malloc(maxfd * sizeof(KQUEUE_CTX *));

	for (i = 0; i < maxfd; i++) {
		kq->fds[i] = NULL;
	}

	ring_init(&kq->kes);
	return kq;
}

static void kqueue_free(KQUEUE *kq)
{
	size_t i;

	for (i = 0; i < kq->nfds; i++) {
		if (kq->fds[i] != NULL) {
			mem_free(kq->fds[i]);
		}
	}

	mem_free(kq->fds);
	mem_free(kq);
}

// Close and free one KQUEUE with the specified kqfd.
int kqueue_close(int kqfd)
{
	EVENT *ev;
	int sys_kqfd;
	KQUEUE *kq = NULL;
	int pos = -1;
	ITER iter;

	if (__kqfds == NULL || kqfd < 0) {
		return -1;
	}

	foreach(iter, __kqfds) {
		KQUEUE *tmp = (KQUEUE *) iter.data;
		if (tmp->kqfd == kqfd) {
			kq  = tmp;
			pos = iter.i;
			break;
		}
	}

	if (kq == NULL) {
		return -1;
	}

	ev = fiber_io_event();
	assert(ev);

	sys_kqfd = (int) event_handle(ev);
	assert(sys_kqfd >= 0);

	// We can't close the kqfd same as the internal fiber event's fd.
	// Because we've created a new fd as a duplication of internal kqfd
	// in kqueue_alloc by calling sys API dup(), the kqfd here shouldn't
	// be same as the internal sys_kqfd.

	if (kqfd == sys_kqfd) {
		msg_error("%s(%d): can't close the event sys_kqfd=%d",
			__FUNCTION__, __LINE__, kqfd);
		return -1;
	}

	kqueue_free(kq);
	array_delete(__kqfds, pos, NULL);

	// Because the kqfd was created by dup or by creating socket as kqueue
	// handle, so it should be closed by the system close API directly.
	return (*sys_close)(kqfd);
}

static KQUEUE *kqueue_find(int kqfd)
{
	ITER iter;

	if (__kqfds == NULL) {
		msg_error("%s(%d), %s: __kqfds NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return NULL;
	}

	// We should find the KQUEUE with the specified kqfd.
	foreach(iter, __kqfds) {
		KQUEUE *kq = (KQUEUE *) iter.data;
		if (kq->kqfd == kqfd) {
			return kq;
		}
	}

	msg_error("%s(%d, %s: not found kqfd=%d",
		__FILE__, __LINE__, __FUNCTION__, kqfd);
	return NULL;
}

/****************************************************************************/

// Hook the system API to create one kqueue fd.
int kqueue(void)
{
	EVENT *ev;
	KQUEUE *kq;
	int    sys_kqfd, kqfd;

	if (sys_kqueue == NULL) {
		hook_once();
	}

	if (!var_hook_sys_api) {
		return sys_kqueue ? (*sys_kqueue)() : -1;
	}

	ev = fiber_io_event();
	assert(ev);

	// Get the current thread's kqueue fd.
	sys_kqfd = (int) event_handle(ev);
	if (sys_kqfd < 0) {
		msg_info("%s(%d), %s: no event_handle %d",
			__FILE__, __LINE__, __FUNCTION__, sys_kqfd);
	}

	// Duplicate sys_kqfd and return a new fd to the caller.
	kqfd = sys_kqfd >= 0 ? dup(sys_kqfd) : socket(AF_INET, SOCK_STREAM, 0);
	if (kqfd < 0) {
		msg_error("%s(%d): create fd error: %s",
			__FILE__, __LINE__, last_serror());
		return -1;
	}

	kq = kqueue_alloc(kqfd);
	if (kq == NULL) {
		msg_error("%s(%d): can't alloc KQUEUE for kqfd=%d",
			__FUNCTION__, __LINE__, kqfd);
		return -1;
	}

	return kq->kqfd;
}

static void read_callback(EVENT *ev, FILE_EVENT *fe)
{
	KQUEUE_CTX   *kqx = fe->kqx;
	KQUEUE_EVENT *ke;
	KQUEUE *kq;
	RING *r;

	assert(kqx);
	assert(kqx->fd == fe->fd);
	assert(kqx->mask & EVENT_READ);

	kq = kqx->kq;
	assert(kq);

	r = ring_succ(&kq->kes);
	if (r == &kq->kes) {
		msg_error("%s(%d): kq->kes empty", __FUNCTION__, __LINE__);
		return;
	}

	ke = ring_to_appl(r, KQUEUE_EVENT, me4kq);

	// If the ready count exceeds the nevents been set which limits the
	// buffer space to hold the ready fds, we just return to let the left
	// ready fds kept in system buffer, and hope they'll be handled in the
	// next kevent().
	if (ke->nready >= ke->nevents) {
		return;
	}

	assert(kq->fds[kqx->fd] == kqx);

	// Check if this event already exists (e.g., both READ and WRITE)
	int found = 0;
	int i;
	for (i = 0; i < ke->nready; i++) {
		if (ke->events[i].ident == kqx->ident &&
		    ke->events[i].filter == EVFILT_READ) {
			found = 1;
			break;
		}
	}

	if (!found) {
		// Save the fd IO event's result to the result receiver been set in
		// kevent() as below.
		if ((ev->flag & EVENT_F_USE_ONCE) != 0 && !(fe->mask & EVENT_ONCE)) {
			EV_SET(&ke->events[ke->nready], kqx->ident, EVFILT_READ,
				EV_ADD | EV_ENABLE, 0, 0, kqx->udata);
		} else {
			EV_SET(&ke->events[ke->nready], kqx->ident, EVFILT_READ,
				EV_ADD | EV_ENABLE, 0, 0, kqx->udata);
		}

		// Remove the kqueue timer when the first event arriving.
		if (ke->nready == 0) {
			timer_cache_remove(ev->kqueue_timer, ke->expire, &ke->me);
			ring_prepend(&ev->kqueue_ready, &ke->me);
		}

		// Check if we need to increment nready (only if WRITE is not already set)
		int has_write = 0;
		for (i = 0; i < ke->nready; i++) {
			if (ke->events[i].ident == kqx->ident &&
			    ke->events[i].filter == EVFILT_WRITE) {
				has_write = 1;
				break;
			}
		}
		if (!has_write) {
			ke->nready++;
		}
	}

	SET_READABLE(fe);
}

static void write_callback(EVENT *ev fiber_unused, FILE_EVENT *fe)
{
	KQUEUE_CTX   *kqx = fe->kqx;
	KQUEUE_EVENT *ke;
	KQUEUE *kq;
	RING  *r;

	assert(kqx);
	assert(kqx->fd == fe->fd);
	assert(kqx->mask & EVENT_WRITE);

	kq = kqx->kq;
	assert(kq);

	r = ring_succ(&kq->kes);
	if (r == &kq->kes) {
		msg_error("%s(%d): kq->kes empty", __FUNCTION__, __LINE__);
		return;
	}

	ke = ring_to_appl(r, KQUEUE_EVENT, me4kq);

	if (ke->nready >= ke->nevents) {
		return;
	}

	assert(kq->fds[kqx->fd] == kqx);

	// Check if this event already exists (e.g., both READ and WRITE)
	int found = 0;
	int i;
	for (i = 0; i < ke->nready; i++) {
		if (ke->events[i].ident == kqx->ident &&
		    ke->events[i].filter == EVFILT_WRITE) {
			found = 1;
			break;
		}
	}

	if (!found) {
		// Save the fd IO event's result to the result receiver been set in
		// kevent() as below.
		if ((ev->flag & EVENT_F_USE_ONCE) != 0 && !(fe->mask & EVENT_ONCE)) {
			EV_SET(&ke->events[ke->nready], kqx->ident, EVFILT_WRITE,
				EV_ADD | EV_ENABLE, 0, 0, kqx->udata);
		} else {
			EV_SET(&ke->events[ke->nready], kqx->ident, EVFILT_WRITE,
				EV_ADD | EV_ENABLE, 0, 0, kqx->udata);
		}

		// Remove the kqueue timer when the first event arriving.
		if (ke->nready == 0) {
			timer_cache_remove(ev->kqueue_timer, ke->expire, &ke->me);
			ring_prepend(&ev->kqueue_ready, &ke->me);
		}

		// Check if we need to increment nready (only if READ is not already set)
		int has_read = 0;
		for (i = 0; i < ke->nready; i++) {
			if (ke->events[i].ident == kqx->ident &&
			    ke->events[i].filter == EVFILT_READ) {
				has_read = 1;
				break;
			}
		}
		if (!has_read) {
			ke->nready++;
		}
	}

	SET_WRITABLE(fe);
}

static void kqueue_ctl_add(EVENT *ev, KQUEUE *kq,
	const struct kevent *kev, int fd)
{
	KQUEUE_CTX *kqx = kq->fds[fd];

	if (kqx == NULL) {
		kqx = kq->fds[fd] = (KQUEUE_CTX *) mem_malloc(sizeof(KQUEUE_CTX));
	}

	kqx->fd     = fd;
	kqx->mask   = EVENT_NONE;
	kqx->rmask  = EVENT_NONE;
	kqx->kq     = kq;
	kqx->ident  = kev->ident;
	kqx->udata  = kev->udata;

	if (kev->filter == EVFILT_READ) {
		kqx->mask   |= EVENT_READ;
		kqx->filter  = EVFILT_READ;
		kqx->fe      = fiber_file_open(fd);
		kqx->fe->kqx = kqx;

		event_add_read(ev, kqx->fe, read_callback);
		SET_READWAIT(kqx->fe);
	}

	if (kev->filter == EVFILT_WRITE) {
		kqx->mask   |= EVENT_WRITE;
		kqx->filter  = EVFILT_WRITE;
		kqx->fe      = fiber_file_open(fd);
		kqx->fe->kqx = kqx;

		event_add_write(ev, kqx->fe, write_callback);
		SET_WRITEWAIT(kqx->fe);
	}
}

static void kqueue_ctl_del(EVENT *ev, KQUEUE *kq, int fd, short filter)
{
	KQUEUE_CTX *kqx = kq->fds[fd];
	if (kqx == NULL) {
		return;
	}

	if (filter == EVFILT_READ && (kqx->mask & EVENT_READ)) {
		assert(kqx->fe);
		event_del_read(ev, kqx->fe, 1);
		CLR_READWAIT(kqx->fe);
		kqx->mask &= ~EVENT_READ;
	}

	if (filter == EVFILT_WRITE && (kqx->mask & EVENT_WRITE)) {
		assert(kqx->fe);
		event_del_write(ev, kqx->fe, 1);
		CLR_WRITEWAIT(kqx->fe);
		kqx->mask &= ~EVENT_WRITE;
	}

	if (kqx->mask == EVENT_NONE) {
		kq->fds[fd] = NULL;
		kqx->fd      = -1;
		kqx->mask    = EVENT_NONE;
		kqx->rmask   = EVENT_NONE;
		if (kqx->fe) {
			kqx->fe->kqx = NULL;
		}
		kqx->fe      = NULL;
		kqx->kq      = NULL;
		memset(&kqx->ident, 0, sizeof(kqx->ident));
		memset(&kqx->udata, 0, sizeof(kqx->udata));
		mem_free(kqx);
	}
}

static void kqueue_callback(EVENT *ev fiber_unused, KQUEUE_EVENT *ke)
{
	if (ke->fiber->status != FIBER_STATUS_READY) {
		FIBER_READY(ke->fiber);
	}
}

int kevent(int kq, const struct kevent *changelist, int nchanges,
	struct kevent *eventlist, int nevents, const struct timespec *timeout)
{
	EVENT *ev;
	KQUEUE *kq_obj;
	int i;

	if (sys_kevent == NULL) {
		hook_once();
	}

	if (!var_hook_sys_api) {
		return sys_kevent ? (*sys_kevent)(kq, changelist, nchanges,
			eventlist, nevents, timeout) : -1;
	}

	ev = fiber_io_event();
	if (ev == NULL) {
		msg_error("%s(%d), %s: EVENT NULL",
			__FILE__, __LINE__, __FUNCTION__);
		return -1;
	}

	kq_obj = kqueue_find(kq);
	if (kq_obj == NULL) {
		msg_error("%s(%d), %s: not exist kq %d",
			__FILE__, __LINE__, __FUNCTION__, kq);
		return -1;
	}

	// Handle changelist (modify events) - similar to epoll_ctl
	if (nchanges > 0 && changelist != NULL) {
		for (i = 0; i < nchanges; i++) {
			const struct kevent *kev = &changelist[i];
			int fd = (int) kev->ident;

			if (fd < 0 || fd >= (int) kq_obj->nfds) {
				msg_error("%s(%d), %s: invalid fd=%d",
					__FILE__, __LINE__, __FUNCTION__, fd);
				continue;
			}

			if (kev->flags & EV_ADD) {
				kqueue_ctl_add(ev, kq_obj, kev, fd);
			} else if (kev->flags & EV_DELETE) {
				kqueue_ctl_del(ev, kq_obj, fd, kev->filter);
			} else if (kev->flags & EV_ENABLE) {
				// Re-enable the event
				if (kev->filter == EVFILT_READ) {
					KQUEUE_CTX *kqx = kq_obj->fds[fd];
					if (kqx && (kqx->mask & EVENT_READ)) {
						event_add_read(ev, kqx->fe, read_callback);
						SET_READWAIT(kqx->fe);
					}
				}
				if (kev->filter == EVFILT_WRITE) {
					KQUEUE_CTX *kqx = kq_obj->fds[fd];
					if (kqx && (kqx->mask & EVENT_WRITE)) {
						event_add_write(ev, kqx->fe, write_callback);
						SET_WRITEWAIT(kqx->fe);
					}
				}
			} else if (kev->flags & EV_DISABLE) {
				// Disable the event
				if (kev->filter == EVFILT_READ) {
					KQUEUE_CTX *kqx = kq_obj->fds[fd];
					if (kqx && (kqx->mask & EVENT_READ)) {
						event_del_read(ev, kqx->fe, 1);
						CLR_READWAIT(kqx->fe);
					}
				}
				if (kev->filter == EVFILT_WRITE) {
					KQUEUE_CTX *kqx = kq_obj->fds[fd];
					if (kqx && (kqx->mask & EVENT_WRITE)) {
						event_del_write(ev, kqx->fe, 1);
						CLR_WRITEWAIT(kqx->fe);
					}
				}
			}
		}
	}

	// Handle eventlist (wait for events) - similar to epoll_wait
	if (nevents > 0 && eventlist != NULL && timeout != NULL) {
		KQUEUE_EVENT *ke;
		long long now;
		int timeout_ms;

		ke = fiber_kqueue_event();
		ke->events    = eventlist;
		ke->nevents   = nevents;
		ke->fiber     = acl_fiber_running();
		ke->proc      = kqueue_callback;
		ke->nready    = 0;

		// Convert timespec to milliseconds
		if (timeout->tv_sec < 0) {
			timeout_ms = -1;
		} else {
			timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
		}

		if (timeout_ms >= 0) {
			ke->expire = event_get_stamp(ev) + timeout_ms;
			if (ev->timeout < 0 || timeout_ms < ev->timeout) {
				ev->timeout = timeout_ms;
			}
		} else {
			ke->expire = -1;
		}

		// Clear events array
		if (!(ev->flag & EVENT_F_USE_ONCE)) {
			for (i = 0; i < ke->nevents; i++) {
				memset(&ke->events[i], 0, sizeof(struct kevent));
			}
		}

		while (1) {
			timer_cache_add(ev->kqueue_timer, ke->expire, &ke->me);

			ring_prepend(&kq_obj->kes, &ke->me4kq);

			ke->fiber->wstatus |= FIBER_WAIT_KQUEUE;

			WAITER_INC(ev);
			acl_fiber_switch();
			WAITER_DEC(ev);

			ke->fiber->wstatus &= ~FIBER_WAIT_KQUEUE;
			ring_detach(&ke->me4kq);

			if (ke->nready == 0) {
				timer_cache_remove(ev->kqueue_timer, ke->expire, &ke->me);
			}

			if (acl_fiber_killed(ke->fiber)) {
				acl_fiber_set_error(ke->fiber->errnum);
				if (ke->nready == 0) {
					ke->nready = -1;
				}
				break;
			}

			if (ke->nready != 0 || timeout_ms == 0) {
				break;
			}

			now = event_get_stamp(ev);
			if (ke->expire > 0 && now >= ke->expire) {
				acl_fiber_set_error(FIBER_ETIME);
				break;
			}
		}

		return ke->nready;
	}

	// If only changelist (no wait), return 0
	return 0;
}

#define TO_APPL	ring_to_appl

void wakeup_kqueue_waiters(EVENT *ev)
{
	RING_ITER iter;
	RING *head;
	KQUEUE_EVENT *ke;
	long long now = event_get_stamp(ev);
	TIMER_CACHE_NODE *node = TIMER_FIRST(ev->kqueue_timer), *next;

	// Handle the KQUEUE process with no events happened when time expired.
	while (node && node->expire >= 0 && node->expire <= now) {
		next = TIMER_NEXT(ev->kqueue_timer, node);

		ring_foreach(iter, &node->ring) {
			ke = TO_APPL(iter.ptr, KQUEUE_EVENT, me);
			ke->proc(ev, ke);
		}

		node = next;
	}

	// Handle the KQUEUE process for the happened events.
	while ((head = ring_pop_head(&ev->kqueue_ready)) != NULL) {
		ke = TO_APPL(head, KQUEUE_EVENT, me);
		ke->proc(ev, ke);
	}

	ring_init(&ev->kqueue_ready);
}

#endif	// end HAS_KQUEUE
