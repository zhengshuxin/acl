#ifndef	__TEST_FIBERTAB_INCLUDE_H__
#define	__TEST_FIBERTAB_INCLUDE_H__

#include "lib_acl.h"
#ifdef	__cplusplus
extern "C" {
#endif

#include "test_fiber.h"

static AUT_FN_ITEM __test_fn_tab[] = {
	{ "test_kqueue_hook", "test_kqueue_hook", test_kqueue_hook, NULL, 0 },
	{ NULL, NULL, NULL, NULL, 0},
};

#ifdef	__cplusplus
}
#endif

#endif
