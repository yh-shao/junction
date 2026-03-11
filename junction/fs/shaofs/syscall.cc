#include "utili.h"
#include "syscall.h"
#include "dentry.h"
#include "dentryCache.h"
#include "file.h"
#include "junction/fs/file.h"

int my_open(const char *pathname, int flags, mode_t mode)
{
    RuntimeFSBaseGuard g;

    IEntry* ent = new IEntry;
    if (!ent)
    {
      log_info("[openat()] ERROR: fail to new() IEntry");
      return -1;
    }

    if (pathname[0] == '/')    // 绝对路径
    {
      lookup(pathname, *ent);  // 解析该路径

      int inum;
      if (ent->code == 1)           // 部分匹配（可能是新建文件）
      {
        if (flags & junction::kFlagCreate)   // 新建文件
        {
          // log_info("creating new file: %s", ent->last_name);
          
          MInode* dir_inode = ent->parent_ino;
          if (!dir_inode) 
          {
              log_info("[usys_openat] ERROR: parent dir not found");
              delete ent;
              return -1;
          }

          MInode* newinode = create_file(dir_inode, ent->last_name, REGULAR);
          release_inode(dir_inode); // Release parent
          if (!newinode) 
          {
              log_info("[usys_openat] ERROR: create_file() failed for %s", ent->last_name);
              delete ent;
              return -1;
          }
          inum = newinode->inum;
          release_inode(newinode);  // 这个 ref 应当在 close() 中再释放？

          auto& dentrycache = DentryCacheManager::instance();
          dentrycache.put(pathname, inum);
        }
        else   // 路径错误 
        {
          log_info("[usys_openat] ERROR: illegal pathname %s (not found)", pathname);
          if (ent->parent_ino != nullptr) release_inode(ent->parent_ino);
          delete ent;
          return -1;
        }
      }
      else if (ent->code == -1)   // 路径错误
      {
        log_info("[usys_openat] ERROR: illegal pathname %s", pathname);
        delete ent;
        return -1;
      }
      else    // 完全匹配
      {
        // log_info("this file alreadly exists!");
        inum = ent->ino->inum;
        release_inode(ent->parent_ino);
        release_inode(ent->ino);    // 这个 ref 应当在 close() 中再释放？
        delete ent;
      }

      // log_info("opened file inum: %d", inum);
      return inum;
    }
    else
    {
      log_info("[openat(%s)] This is a relative path (not supported currently).", pathname);
      delete ent; 
      return -1;
    }
}

ssize_t my_read(int inum, void *buf, size_t len) 
{
    RuntimeFSBaseGuard g;

    MInode* inode_ptr = get_inode(inum);
    if (!inode_ptr) 
    {
      log_info("[ERROR] usys_read: inode %d not found", inum);
      return -1;
    }

    size_t n = MIN(len, inode_ptr->disk_inode.file_size);
    read_file(inode_ptr, 0, buf, n);
    release_inode(inode_ptr);
    return static_cast<ssize_t>(n);
}

ssize_t my_write(int inum, const void *buf, size_t len) 
{
    RuntimeFSBaseGuard g;

    MInode* inode_ptr = get_inode(inum);
    if (!inode_ptr) 
    { 
      log_info("[ERROR] usys_write: inode %d not found", inum);
      return -1;
    }

    {
      SpinGuard g(&inode_ptr->lock);
      append_content(inode_ptr, buf, len);
    }

    release_inode(inode_ptr);
    return len;
}

long my_mkdir(const char *pathname, mode_t mode)
{
    log_info("[my_mkdir(%s)]", pathname);

    RuntimeFSBaseGuard g;

    IEntry* ent = new IEntry;
    if (!ent) return -1;

    long ret = -1;

    if (pathname[0] == '/') 
    {
        lookup(pathname, *ent);
        if (ent->code == 1)   // Partial match - good
        {
            MInode* dir_inode = ent->parent_ino;
            if (dir_inode) 
            {
                MInode* newinode = create_file(dir_inode, ent->last_name, DIRECTORY); 
                if (newinode) 
                {
                    // Init . and ..
                    Dirent entries[2] = {
                        {.inum = newinode->inum, .filetype = DIRECTORY, .name = "."},
                        {.inum = dir_inode->inum, .filetype = DIRECTORY, .name = ".."},
                    };
                    
                    {
                        SpinGuard g(&newinode->lock);
                        append_content(newinode, entries, sizeof(entries));
                        newinode->disk_inode.nlink++; // 1 -> 2 (for .)
                        newinode->dirty = true;
                    }

                    {
                        SpinGuard g(&dir_inode->lock);
                        dir_inode->disk_inode.nlink++; // for ..
                        dir_inode->dirty = true;
                    }
                    
                    auto& dentrycache = DentryCacheManager::instance();
                    dentrycache.put(pathname, newinode->inum);
                    release_inode(newinode);
                    ret = 0;
                }
                release_inode(dir_inode);
            }
        } 
        else 
        {
            if (ent->code == 0)  // Already exists
            {
                release_inode(ent->parent_ino);
                release_inode(ent->ino);
                ret = -EEXIST;
            }
        }
    }
    else
    {
        log_info("[my_mkdirat(%s)] This is a relative path (not supported currently).", pathname);
    }

    delete ent;
    return ret;
}
