#pragma once

#include "fs.h"

typedef struct {
    int         inum;
    file_type_t filetype;   // 4B
    char        name[NAMESIZ];
    char        pad[249];
} Dirent;
static_assert(sizeof(Dirent) == 512, "Dirent size must be 512B");


int dir_lookup_locked(DInode* dir_inode, int dir_inum, const char* name, file_type_t* type);
int dir_lookup(int dir_inum, const char* name, file_type_t* type);

bool dir_is_empty_locked(DInode* dir_inode, int dir_inum);
bool dir_is_empty(int dir_inum);

int dir_add_entry_locked(DInode* dir_inode, int dir_inum, const char* name, int inum, file_type_t type);
int dir_add_entry(int dir_inum, const char* name, int inum, file_type_t type);

int dir_delete_entry_locked(DInode* dir_inode, int dir_inum, const char* name);
int dir_delete_entry(int dir_inum, const char* name);