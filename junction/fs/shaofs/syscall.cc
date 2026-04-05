#include "utili.h"
#include "syscall.h"
#include "dentryCache.h"
#include "file.h"
#include "namei.h"
#include "dir.h"
#include "inodeCache.h"
#include "blockCache.h"
#include "junction/fs/file.h"


int my_open2(const char* pathname, int flags, mode_t mode)
{
    // log_info("BlockSize: %lu", storage_block_size());
    // log_info("BlockNum: %lu", storage_num_blocks());

    RuntimeFSBaseGuard g;
    // storage_read_obj(&sb, sizeof(SuperBlock), SUPERBLOCK_LOCATION, SUPERBLOCK_NUM);
    // log_info("magic num: %x", sb.magic_number);
    
    // char buffer[BLOCK_SIZE];
    // storage_read(buffer, 0, 1);

    // bc_read(SUPERBLOCK_LOCATION, buffer);
    // SuperBlock* sb = (SuperBlock*)buffer;
    // log_info("magic num: %x", sb->magic_number);

    return 1;
}

int my_open(const char* pathname, int flags, mode_t mode)
{
    // log_info("open(%s)", pathname);

    RuntimeFSBaseGuard g;

    int inum = -1;

    if (flags & junction::kFlagCreate)
    {
        char name[NAMESIZ];
        int parent_inum = nameiparent(pathname, name);  // 查找父目录 Inode，并提取最后一级的文件名
        if (parent_inum == -1) 
        {
            log_err("[fs_open] Invalid path or parent directory missing: %s", pathname);
            return -1;
        }

        while (true)   // retry 循环
        {
          file_type_t type;
          inum = dir_lookup(parent_inum, name, &type);
          if (inum != -1) // 文件已经存在于该目录中
          {
            if (flags & junction::kFlagExclusive) 
            {
                log_err("[fs_open] File already exists (O_EXCL triggered): %s", pathname);
                return -1;
            }

            if (type == DIRECTORY) 
            {
                log_err("[fs_open] Cannot open a directory with O_CREAT: %s", pathname);
                return -1;
            }
            
            break;
          }
          else   // 文件不存在，执行真正的创建逻辑
          {
            InodeHandle new_ih = ic_alloc_inode(REGULAR);  // 分配一个新的 Inode
            if (!new_ih) 
            {
                log_err("[fs_open] Inode space exhausted for %s", pathname);
                return -1;
            }
            int new_inum = new_ih.read_access()->idx;

            int ret = dir_add_entry(parent_inum, name, new_inum, REGULAR);  // 将新文件注册到父目录中
            if (ret != 0)   
            {
                ic_free_inode(new_inum); // 回滚刚分配的 Inode

                if (ret == -EEXIST)
                {
                  if (flags & junction::kFlagExclusive) 
                  {
                    log_err("[fs_open] Concurrent creation conflict (O_EXCL): %s", pathname);
                    return -1;
                  }
                  continue;
                }

                log_err("Fail to add dentry");
                return -1;
            }

            inum = new_inum; // 创建成功！
            log_info("created a new file: %s", pathname);
            break;
          }
        }
    }
    else   // 普通打开，不带创建语义
    {
        inum = namei(pathname);
        if (inum == -1) 
        {
            log_err("[fs_open] File not found: %s", pathname);
            return -1;
        }
    }

    // 处理文件截断
    // if (inum != -1 && (flags & junction::kFlagTruncate)) 
    // {
    //     truncate_inode(inum);
    // }

    return inum;
}

ssize_t my_read(int inum, void *buf, off_t* off, size_t len, bool direct) 
{
    // log_info("read(inum=%d, len=%d)", inum, len);
    RuntimeFSBaseGuard g;

    ssize_t ret = file_read(inum, (char*)buf, *off, len);
    if (ret >= 0) *off += ret;
    return ret;
}

ssize_t my_write(int inum, const void *buf, off_t* off, size_t len, bool direct) 
{
    // uint64_t start_us = microtime(), end_us;
    // log_info("write(inum=%d, len=%d)", inum, len);
    RuntimeFSBaseGuard g;

    size_t ret = file_write(inum, (const char*)buf, *off, len);
    if (ret >= 0) *off += ret;
    // end_us = microtime();
    // log_info("write: %lu us", end_us - start_us);
    return ret;
}

int my_mkdir(const char *pathname, mode_t mode)
{
    // log_info("mkdir(%s)", pathname);

    RuntimeFSBaseGuard g;

    char name[NAMESIZ];
    int parent_inum = nameiparent(pathname, name);  // 解析路径，获取父目录 Inode 和目标目录名
    if (parent_inum == -1) 
    {
        log_err("[my_mkdir] Invalid path or parent directory missing: %s", pathname);
        return -1; // ENOENT
    }

    // 乐观的快速查重（无锁并发）
    file_type_t type;
    if (dir_lookup(parent_inum, name, &type) != -1) 
    {
        log_err("[my_mkdir] Directory/File already exists: %s", pathname);
        return -EEXIST;
    }

    // 分配新的目录 Inode
    InodeHandle new_ih = ic_alloc_inode(DIRECTORY);
    if (!new_ih) 
    {
        log_err("[my_mkdir] Inode space exhausted for %s", pathname);
        return -1; // ENOSPC
    }
    int new_inum = new_ih.read_access()->idx; // 短暂获取只读指针以读取 idx

    // 初始化新目录的 "." 和 ".." 目录项
    Dirent entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].inum = new_inum;
    entries[0].filetype = DIRECTORY;
    strncpy(entries[0].name, ".", NAMESIZ);
    entries[1].inum = parent_inum;
    entries[1].filetype = DIRECTORY;
    strncpy(entries[1].name, "..", NAMESIZ);

    ssize_t written = file_write(new_inum, (const char*)entries, 0, sizeof(entries));
    if (written != sizeof(entries)) 
    {
        log_err("[my_mkdir] Failed to init . and .. for %s", pathname);
        ic_free_inode(new_inum); // I/O 失败，回滚并销毁刚分配的 Inode
        return -1; // EIO
    }

    // 将新目录注册到父目录中
    int ret = dir_add_entry(parent_inum, name, new_inum, DIRECTORY);
    if (ret != 0) // 如果这里返回冲突 (-EEXIST)，说明刚才短暂的间隙有其他线程抢先创建了同名文件
    {
        ic_free_inode(new_inum); // 完美回滚：销毁新建的 Inode，回收占用的物理块
        return ret;
    }

    // 成功注册后，更新两者的硬链接计数 (nlink)
    {
        auto write_acc = new_ih.write_access();
        write_acc->nlink++;  // 新目录的 nlink 为 2，ic_alloc_inode 默认将 nlink 设为了 1，所以这里我们只需要加 1
        write_acc.mark_dirty();
    }

    {
        // 父目录的 nlink 增加 1 (因为新子目录内部多了一个指向它的 "..")
        InodeHandle parent_ih = ic_get_inode(parent_inum);
        if (parent_ih) 
        {
            auto write_acc = parent_ih.write_access();
            write_acc->nlink++;
            write_acc.mark_dirty();
        }
    }

    // 如果你有 Dentry Cache，可以在此处将其加入内存缓存
    // auto& dentrycache = DentryCacheManager::instance();
    // dentrycache.put(pathname, new_inum);

    log_info("created a new directory: %s", pathname);
    return 0;
}

off_t my_lseek(int inum, off_t offset, int whence, off_t old_off)
{
    RuntimeFSBaseGuard g;

    off_t new_off = -1;
    switch (whence) 
    {
      case SEEK_SET: new_off = offset;              break;
      case SEEK_CUR: new_off = old_off + offset;    break;
      case SEEK_END: 
      {
        InodeHandle ih = ic_get_inode(inum);
        if (unlikely(!ih)) 
        {
            log_err("[file_read] Failed to get inode %d from cache", inum);
            return -1;
        }
        new_off = ih.read_access()->file_size + offset; 
        break;
      }
      default: break;
    }
    if (new_off < 0) return -EINVAL;
    return new_off;
}