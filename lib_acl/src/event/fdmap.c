#include "StdAfx.h"
#ifndef ACL_PREPARE_COMPILE

#include "stdlib/acl_define.h"
#include <string.h>

#ifdef ACL_BCB_COMPILER
#pragma hdrstop
#endif

#include "stdlib/acl_msg.h"
#include "stdlib/acl_mymalloc.h"

#endif

#include "fdmap.h"

typedef struct FD_ENTRY {
	int   fd;
	void *ctx;
} FD_ENTRY;

struct ACL_FD_MAP {
	FD_ENTRY *table;
	int       size;
};

ACL_FD_MAP *acl_fdmap_create(int size)
{
	const char *myname = "acl_fdmap_create";
	ACL_FD_MAP *map;

	if (size < 0) {
		acl_msg_error("%s(%d): maxfd(%d) invalid",
			myname, __LINE__, size);
		size = 10240;
	}

	map = (ACL_FD_MAP *) acl_mycalloc(1, sizeof(ACL_FD_MAP));
	if (map == NULL) {
		acl_msg_error("%s(%d): calloc error(%s)",
			myname, __LINE__, acl_last_serror());
		return NULL;
	}

	map->size = size;
	map->table = (FD_ENTRY *) acl_mycalloc(map->size, sizeof(FD_ENTRY));
	if (map->table == NULL) {
		acl_msg_fatal("%s(%d): calloc error(%s)",
			myname, __LINE__, acl_last_serror());
	}

	return map;
}

void acl_fdmap_add(ACL_FD_MAP *map, int fd, void *ctx)
{
	const char *myname = "acl_fdmap_add";

	if (map == NULL) {
		acl_msg_error("%s(%d): map NULL", myname, __LINE__);
		return;
	}

	if (fd >= map->size) {
		acl_msg_warn("%s(%d): fd(%d) >= map's size(%d), extend it to %d",
			myname, __LINE__, fd, map->size, fd + 1024);
		map->size = fd + 1024;
		map->table = (FD_ENTRY *) acl_myrealloc(map->table,
				map->size * sizeof(FD_ENTRY));
	}
	map->table[fd].fd = fd;
	map->table[fd].ctx = ctx;
}

void acl_fdmap_del(ACL_FD_MAP *map, int fd)
{
	const char *myname = "acl_fdmap_del";

	if (map == NULL) {
		acl_msg_error("%s(%d): map NULL", myname, __LINE__);
		return;
	}

	if (fd >= map->size) {
		acl_msg_error("%s(%d): fd(%d) >= map's size(%d)",
			myname, __LINE__, fd, map->size);
	}
}

void *acl_fdmap_ctx(ACL_FD_MAP *map, int fd)
{
	const char *myname = "acl_fdmap_ctx";

	if (map == NULL) {
		acl_msg_error("%s(%d): map NULL", myname, __LINE__);
		return NULL;
	}

	if (fd >= map->size) {
		acl_msg_error("%s(%d): fd(%d) >= map's size(%d)",
			myname, __LINE__, fd, map->size);
		return NULL;
	}

	return map->table[fd].ctx;
}

void acl_fdmap_free(ACL_FD_MAP *map)
{
	if (map) {
		acl_myfree(map->table);
		acl_myfree(map);
	}
}
