#pragma once
#include "inodeCache.h"
#include <functional>

using BlockVisitor = std::function<bool(char* data, size_t valid_len)>;  // 对每个 block 进行什么操作； 返回 true 表示继续遍历，返回 false 表示停止（找到目标了）
int foreach_file_block(MInode* inode, BlockVisitor visitor);
void write_file(MInode* ino, uint64_t oft, char* buf, uint64_t size);
void read_file(MInode* inode, uint64_t oft, void* buf, uint64_t size);
void read_full_file(MInode* inode, void* buf);
MInode* create_file(MInode* ino, const char* filename, file_type_t filetype);
void append_content(MInode* inode, const void *data, size_t siz);
void truncate_inode_data_locked(MInode*& inode, uint64_t start_offset);


void final_flush();