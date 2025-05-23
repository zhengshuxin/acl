#include "stdafx.h"
#include "common.h"

#include "fiber.h"

#ifdef SYS_UNIX

#if defined(__arm64__)
# ifndef USE_BOOST_JMP
#  define USE_BOOST_JMP
#  undef SHARE_STACK
# endif
#endif

#if defined(USE_BOOST_JMP)
# include "boost_jmp.h"
#elif defined(USE_JMP_DEF)
# define USE_JMP
# include "x86_jmp.h"
#elif defined(USE_JMP_EXP)
# define USE_JMP
# include "exp_jmp.h"
#elif defined(USE_JMP_SYS)
# define USE_JMP
# define SETJMP(ctx) sigsetjmp(ctx, 0)
# define LONGJMP(ctx) siglongjmp(ctx, 1)
#endif  // USE_BOOST_JMP

#ifdef USE_VALGRIND
# include <valgrind/valgrind.h>
#endif

typedef struct FIBER_UNIX {
	ACL_FIBER fiber;
#ifdef	USE_VALGRIND
	unsigned int vid;
#endif

#if	defined(USE_BOOST_JMP)
	fcontext_t fcontext;
	char      *stack;
	void (*fn)(ACL_FIBER *, void *);
	void *arg;
#else
# if	defined(USE_JMP_DEF)
	unsigned long long env[10];
# elif	defined(USE_JMP_SYS)
	sigjmp_buf env;
# elif	defined(USE_JMP_EXP)
	label_t env;
# endif
	ucontext_t *context;
#endif
	size_t size;
	char  *buff;
	size_t dlen;
} FIBER_UNIX;

#if defined(ANDROID)

void acl_fiber_stack_print(const char *tag)
{
	(void) tag;
}

#else

#include <execinfo.h>

void acl_fiber_stack_print(const char *tag)
{
#define	STACK_SIZE	100
	void *buffer[STACK_SIZE];
	int nptrs = backtrace(buffer, STACK_SIZE), j;
	char **frames = backtrace_symbols(buffer, nptrs);
	if (frames == NULL) {
		msg_error("%s(%d): backtrace_symbols null", __FUNCTION__, __LINE__);
		return;
	}

	for (j = 0; j < nptrs; j++) {
		msg_info("%s:  %s", tag ? tag : "none", frames[j]);
	}

	free(frames);
}

#endif // ANDROID

#ifdef	DEBUG_STACK
#include <libunwind.h>
ACL_FIBER_STACK *acl_fiber_stacktrace(const ACL_FIBER *fiber, size_t max)
{
	const FIBER_UNIX *fb = (const FIBER_UNIX*) fiber;
	unw_cursor_t cursor;
	unw_word_t off, pc;
	ACL_FIBER_STACK *stack;
	char name[128];
	int  ret;

	if (fb->context == NULL) {
		msg_error("%s(%d): fb's context null", __FUNCTION__, __LINE__);
		return NULL;
	}

	ret = unw_init_local(&cursor, (unw_context_t*) fb->context);
	if (ret != 0) {
		msg_error("%s(%d): unw_init_local error=%s, ret=%d",
			__FUNCTION__, __LINE__, strerror(errno), ret);
		return NULL;
	}

	stack = (ACL_FIBER_STACK*) mem_malloc(sizeof(ACL_FIBER_STACK));
	stack->frames = (ACL_FIBER_FRAME*) mem_malloc(max * sizeof(ACL_FIBER_FRAME));
	stack->count = 0;
	stack->size  = max;

	msg_info("%s(%d): stack's max size=%zd", __FUNCTION__, __LINE__, max);

	while (unw_step(&cursor) > 0 && stack->count < stack->size) {
		ret = unw_get_proc_name(&cursor, name, sizeof(name), &off);
		if (ret != 0) {
			msg_error("%s(%d): unw_get_proc_name error=%s, ret=%d",
				__FUNCTION__, __LINE__, strerror(errno), ret);
		} else {
			unw_get_reg(&cursor, UNW_REG_IP, &pc);
			stack->frames[stack->count].func = mem_strdup(name);
			stack->frames[stack->count].pc   = (long) pc;
			stack->frames[stack->count].off  = (long) off;
			stack->count++;
		}
	}

	msg_info("%s(%d): the fiber(%p)'s stack=%p", __FUNCTION__, __LINE__,
		fiber, stack);

	return stack;
}
#else
ACL_FIBER_STACK *acl_fiber_stacktrace(const ACL_FIBER *fiber, size_t max)
{
	msg_error("%s(%d): Not supported, fiber-%d, max=%zd",
		__FUNCTION__, __LINE__, acl_fiber_id(fiber), max);
	return NULL;
}
#endif

void acl_fiber_stackfree(ACL_FIBER_STACK *stack)
{
	size_t i;

	if (stack) {
		for (i = 0; i < stack->count; i++) {
			mem_free(stack->frames[i].func);
		}

		mem_free(stack->frames);
		mem_free(stack);
	}
}

#if	defined(USE_BOOST_JMP)
# if	defined(SHARE_STACK)
#  error "Not support shared stack when using boost jmp!"
# endif

typedef struct {
	FIBER_UNIX *from;
	FIBER_UNIX *to;
} JUMP_CTX;

static void swap_fcontext(FIBER_UNIX *from, FIBER_UNIX *to)
{
	JUMP_CTX jump, *jmp;
	transfer_t trans;
	
	jump.from  = from;
	jump.to    = to;
	trans      = jump_fcontext(to->fcontext, &jump);
	jmp        = (JUMP_CTX*) trans.data;
	jmp->from->fcontext = trans.fctx;
}

#endif

#if	defined(SHARE_STACK)

static void fiber_stack_save(FIBER_UNIX *curr, const char *stack_top)
{
	curr->dlen = fiber_share_stack_bottom() - stack_top;
	if (curr->dlen > curr->size) {
		stack_free(curr->buff);
		curr->buff = (char *) stack_alloc(curr->dlen);
		curr->size = curr->dlen;
	}
	memcpy(curr->buff, stack_top, curr->dlen);
}

static void fiber_stack_restore(FIBER_UNIX *curr)
{
	// After coming back, the current fiber's stack should be
	// restored and copied from its private memory to the shared
	// stack running memory.
	if ((curr->fiber.oflag & ACL_FIBER_ATTR_SHARE_STACK)
		&& curr->fiber.status != FIBER_STATUS_EXITING
		&& curr->dlen > 0) {

		char *bottom = fiber_share_stack_bottom();
		memcpy(bottom - curr->dlen, curr->buff, curr->dlen);
		fiber_share_stack_set_dlen(curr->dlen);
	}
	// else if from->dlen == 0, the fiber must be the origin fiber
	// that its fiber id should be 0.
}

#endif	// SHARE_STACK

//#define RESTORE_FIRST

void fiber_real_swap(ACL_FIBER *from, ACL_FIBER *to)
{
	// The shared stack mode isn't supported in USE_BOOST_JMP current,
	// which may be supported in future.
	// If the fiber isn't in exiting status, and not the origin fiber,
	// the fiber's running stack should be copied from the shared running
	// stack to the fiber's private memory.
#if	defined(SHARE_STACK)
	if ((from->oflag & ACL_FIBER_ATTR_SHARE_STACK)
		&& from->status != FIBER_STATUS_EXITING && from->tid > 0) {

		char stack_top = 0;
		fiber_stack_save((FIBER_UNIX*) from, &stack_top);
	}

# if	defined(RESTORE_FIRST)
	fiber_stack_restore((FIBER_UNIX*) to);
# endif
#endif

#if	defined(USE_BOOST_JMP)
	swap_fcontext((FIBER_UNIX*) from, (FIBER_UNIX*) to);
#elif	defined(USE_JMP)

	/* Use setcontext() for the initial jump, as it allows us to set up
	 * a stack, but continue with longjmp() as it's much faster.
	 */
# if	defined(USE_JMP_SYS)
	if (SETJMP(((FIBER_UNIX*) from)->env) == 0) {
# else
	if (SETJMP(&((FIBER_UNIX*) from)->env) == 0) {
# endif
		/* If the fiber was ready for the first time, just call the
		 * setcontext() for the fiber to start, else just jump to the
		 * fiber's running stack.
		 */
		if (to->flag & FIBER_F_STARTED) {
# if	defined(USE_JMP_SYS)
			LONGJMP(((FIBER_UNIX*) to)->env);
# else
			LONGJMP(&((FIBER_UNIX*) to)->env);
# endif
		} else {
			setcontext(((FIBER_UNIX*) to)->context);
		}
	}
#else	// Use the default context swap API
	if (swapcontext(((FIBER_UNIX*) from)->context,
			((FIBER_UNIX*) to)->context) < 0) {
		msg_fatal("%s(%d), %s: swapcontext error %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
#endif

#if	defined(SHARE_STACK) && !defined(RESTORE_FIRST)
	fiber_stack_restore((FIBER_UNIX*) acl_fiber_running());
#endif
}

#if	defined(USE_BOOST_JMP)

static void fiber_unix_start(transfer_t arg)
{
	JUMP_CTX *jmp = (JUMP_CTX*) arg.data;
	jmp->from->fcontext = arg.fctx;
	jmp->to->fiber.flag |= FIBER_F_STARTED;
	fiber_start(&jmp->to->fiber, jmp->to->fn, jmp->to->arg);
}

#else	// !USE_BOOST_JMP

typedef struct {
	FIBER_UNIX *fb;
	void (*fn)(ACL_FIBER *, void *);
	void *arg;
} FIBER_CTX;

union FIBER_ARG
{
	void *p;
	int   i[2];
};

static void fiber_unix_start(unsigned int x, unsigned int y)
{
	union FIBER_ARG carg;
	FIBER_UNIX *fb;
	void (*fn)(ACL_FIBER *, void *);
	void *arg;
	FIBER_CTX  *ctx;

	carg.i[0] = x;
	carg.i[1] = y;

	ctx = (FIBER_CTX*) carg.p;
	fb  = ctx->fb;
	fn  = ctx->fn;
	arg = ctx->arg;

	mem_free(ctx);

#if	defined(USE_JMP) && !defined(DEBUG_STACK)
	/* When using setjmp/longjmp, the context just be used only once,
	 * so we can free it here to save some memory.
	 */
	if (fb->context != NULL) {
		stack_free(fb->context);
		fb->context = NULL;
	}
#endif
	fb->fiber.flag |= FIBER_F_STARTED;
	fiber_start(&fb->fiber, fn, arg);

	if (fb->context != NULL) {
		stack_free(fb->context);
		fb->context = NULL;
	}
}

#endif // !USE_BOOST_JMP

void fiber_real_init(ACL_FIBER *fiber, size_t size,
	void (*fn)(ACL_FIBER *, void *), void *arg)
{
	FIBER_UNIX *fb = (FIBER_UNIX *) fiber;
#if	!defined(USE_BOOST_JMP)
	FIBER_UNIX *origin;
	union FIBER_ARG carg;
	FIBER_CTX *ctx;
	sigset_t zero;
#endif
	
	if (fb->size < size) {
		/* If using realloc, real memory will be used, when we first
		 * free and malloc again, then we'll just use virtual memory,
		 * because memcpy will be called in realloc.
		 */
		stack_free(fb->buff);
		fb->buff = (char *) stack_alloc(size);
		fb->size = size;
	}

#if	defined(USE_BOOST_JMP)
	fb->fn  = fn;
	fb->arg = arg;

# if	defined(SHARE_STACK)
	if (fb->fiber.oflag & ACL_FIBER_ATTR_SHARE_STACK) {
		fb->stack = fiber_share_stack_bottom();
		fb->fcontext = make_fcontext(fb->stack,
			fiber_share_stack_size(),
			(void(*)(transfer_t)) fiber_unix_start);
	} else {
		fb->stack = fb->buff + fb->size;
		fb->fcontext = make_fcontext(fb->stack, fb->size,
			(void(*)(transfer_t)) fiber_unix_start);
	}
# else
	fb->stack = fb->buff + fb->size;
	fb->fcontext = make_fcontext(fb->stack, fb->size,
		(void(*)(transfer_t)) fiber_unix_start);
# endif
#else	// !USE_BOOST_JMP
	if (fb->context == NULL) {
		fb->context = (ucontext_t *) stack_alloc(sizeof(ucontext_t));
	}

	memset(fb->context, 0, sizeof(ucontext_t));

	sigemptyset(&zero);
	sigaddset(&zero, SIGPIPE);
	sigaddset(&zero, SIGSYS);
	sigaddset(&zero, SIGALRM);
	sigaddset(&zero, SIGURG);
	sigaddset(&zero, SIGWINCH);
	sigprocmask(SIG_BLOCK, &zero, &fb->context->uc_sigmask);

	if (getcontext(fb->context) < 0) {
		msg_fatal("%s(%d), %s: getcontext error: %s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}

# if	defined(SHARE_STACK)
	if (fb->fiber.oflag & ACL_FIBER_ATTR_SHARE_STACK) {
		fb->context->uc_stack.ss_sp   = fiber_share_stack_addr();
		fb->context->uc_stack.ss_size = fiber_share_stack_size();
	} else {
		fb->context->uc_stack.ss_sp   = fb->buff;
		fb->context->uc_stack.ss_size = fb->size;
	}
# else
	fb->context->uc_stack.ss_sp   = fb->buff;
	fb->context->uc_stack.ss_size = fb->size;
# endif

	origin = (FIBER_UNIX*) fiber_origin();
	fb->context->uc_link = origin->context;

	ctx      = (FIBER_CTX*) mem_malloc(sizeof(FIBER_CTX));
	ctx->fb  = fb;
	ctx->fn  = fn;
	ctx->arg = arg;
	carg.p   = ctx;

	makecontext(fb->context, (void(*)(void)) fiber_unix_start,
		2, carg.i[0], carg.i[1]);
#endif

#ifdef USE_VALGRIND
	/* Avoid the valgrind warning */
# if	defined(USE_BOOST_JMP)
	fb->vid = VALGRIND_STACK_REGISTER(fb->buff, fb->stack);
# else
	fb->vid = VALGRIND_STACK_REGISTER(fb->context->uc_stack.ss_sp,
		(char*)fb->context->uc_stack.ss_sp
		+ fb->context->uc_stack.ss_size);
# endif
#endif
}

void fiber_real_free(ACL_FIBER *fiber)
{
	FIBER_UNIX *fb = (FIBER_UNIX *) fiber;

#ifdef USE_VALGRIND
	VALGRIND_STACK_DEREGISTER(fb->vid);
#endif

#if	!defined(USE_BOOST_JMP)
	if (fb->context) {
		stack_free(fb->context);
	}
#endif
	stack_free(fb->buff);
	mem_free(fb);
}

ACL_FIBER *fiber_real_alloc(const ACL_FIBER_ATTR *attr)
{
	FIBER_UNIX *fb = (FIBER_UNIX *) mem_calloc(1, sizeof(*fb));
	size_t size = attr ? attr->stack_size : 128000;

	/* No using calloc just avoiding using real memory */
	fb->buff           = (char *) stack_alloc(size);
	fb->size           = size;
	fb->fiber.oflag    = attr ? attr->oflag : 0;

	return (ACL_FIBER *) fb;
}

ACL_FIBER *fiber_real_origin(void)
{
	FIBER_UNIX *fb = (FIBER_UNIX *) mem_calloc(1, sizeof(*fb));

#ifdef	USE_BOOST_JMP
	fb->size           = 32 * 1024;
	fb->buff           = (char *) stack_alloc(fb->size);

#elif	defined(USE_JMP)
	/* Set context NULL when using setjmp that setcontext will not be
	 * called in fiber_swap.
	 */
	fb->context = NULL;
#else
	fb->context = (ucontext_t *) stack_alloc(sizeof(ucontext_t));
#endif

	/* The origin fiber must be set FIBER_F_STARTED indicating the origin
	 * fiber has been started, which will be used as checking condition
	 * when swaping from the other fiber to the origin fiber.
	 */ 
	fb->fiber.flag = FIBER_F_STARTED;

	return &fb->fiber;
}

#endif
