
#ifndef	__TEST_FIBER_INCLUDE_H__
#define	__TEST_FIBER_INCLUDE_H__

#ifdef	__cplusplus
extern "C" {
#endif

void test_fiber_register(void);

int test_kqueue_hook(AUT_LINE *test_line, void *arg);

#ifdef	__cplusplus
}
#endif

#endif
