#pragma once
#include "sysdeps.h"

typedef struct fsdb_file_info {
	int type;
	uint32_t mode;
	int days;
	int mins;
	int ticks;
	char *comment;

} fsdb_file_info;

void fsdb_init_file_info(fsdb_file_info *info);
int fsdb_set_file_info(const char *nname, fsdb_file_info *info);

extern int g_fsdb_debug;
extern int my_errno;