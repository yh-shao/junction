#pragma once
#include "fs.h"

int namei(const char* path);                    // 解析路径，返回目标文件的 Inode 号，失败则返回 -1
int nameiparent(const char* path, char* name);  // 解析路径，返回目标文件的【父目录】的 Inode 号，并将最后一级的文件名提取出来，失败则返回 -1