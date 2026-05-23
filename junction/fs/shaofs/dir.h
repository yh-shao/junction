#pragma once

#include "fs.h"

struct DirIndex;
void dir_index_destroy(DirIndex* index);

typedef struct {
    int         inum;
    file_type_t filetype;   // 4B
    char        name[NAMESIZ];
    char        pad[249];
} Dirent;
static_assert(sizeof(Dirent) == 512, "Dirent size must be 512B");

static inline bool dirent_is_empty(const Dirent* d)   // 判断一个目录项是否为空 slot（inum=0 且 name 为空字符串）
{
    return d->inum == 0 && d->name[0] == '\0';
}

int dir_lookup(int dir_inum, const char* name, file_type_t* type);
int dir_lookup_pin(int dir_inum, const char* name, file_type_t* type);
bool dir_is_empty(int dir_inum);
int dir_add_entry(int dir_inum, const char* name, int inum, file_type_t type);
int dir_delete_entry(int dir_inum, const char* name);
int dir_delete_stale_entry(int dir_inum, const char* name);
int dir_delete_file_entry(int dir_inum, const char* name, int* inum, file_type_t* type);