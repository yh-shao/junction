#include "fs.h"
#include "dir.h"
#include "inodeCache.h"
#include "file.h"
#include "dentryCache.h"

class DirGuard 
{
    mutex_t* mtx = nullptr;

public:
    explicit DirGuard(const InodeHandle& ih) 
    {
        if (ih) 
        {
            auto acc = ih.read_access();
            mtx = &acc->dir_mtx; 
        }
        if (mtx) mutex_lock(mtx);
    }

    ~DirGuard() { if (mtx) mutex_unlock(mtx); }

    // 禁用拷贝
    DirGuard(const DirGuard&) = delete;
    DirGuard& operator=(const DirGuard&) = delete;
};

template <typename Func>
int dir_foreach(int dir_inum, Func callback) 
{
    uint64_t offset = 0;
    Dirent entry;
    char block_buf[BLOCK_SIZE];
    while (true) 
    {
        ssize_t read_bytes = file_read(dir_inum, block_buf, offset, BLOCK_SIZE);   // 一次读取一整个块（4KB），包含 8 个 Dirent
        
        if (read_bytes < 0) 
        {
            log_err("[dir_foreach] Failed to read entry at offset %lu", offset);
            return -1; 
        }
        
        int n_entries = read_bytes / sizeof(Dirent);
        Dirent* entries = reinterpret_cast<Dirent*>(block_buf);
        for (int i = 0; i < n_entries; i++)
            if (callback(entries[i], offset + i * sizeof(Dirent))) return 0;  // 执行上层注入的业务逻辑，若返回 true 则立刻终止遍历

        if (read_bytes < BLOCK_SIZE) break; // 到达文件末尾
        offset += read_bytes;
    }

    return 0;   
}

int dir_lookup(int dir_inum, const char* name, file_type_t* type)
{
    int found_inum = -1;

    dir_foreach(dir_inum, [&](Dirent& entry, uint64_t offset) {
        if (entry.inum != 0 && strncmp(entry.name, name, NAMESIZ) == 0) 
        {
            if (type) *type = entry.filetype;
            found_inum = entry.inum;
            return true; // 命中，停止遍历
        }
        return false;
    });

    return found_inum;
} 

bool dir_is_empty(int dir_inum)
{
    bool empty = true;
    
    dir_foreach(dir_inum, [&](Dirent& entry, uint64_t offset) {
        if (entry.inum != 0 && strncmp(entry.name, ".", NAMESIZ) != 0 && strncmp(entry.name, "..", NAMESIZ) != 0) 
        {
            empty = false; 
            return true; // 发现实体文件，立刻停止遍历
        }
        return false; 
    });
    
    return empty;
}

int dir_add_entry(int dir_inum, const char* name, int inum, file_type_t type)
{
    {
        InodeHandle dir_ih = ic_get_inode(dir_inum);
        if (!dir_ih) return -1;

        DirGuard vfs_guard(dir_ih);   // 获取 VFS 专属互斥锁，串行化当前目录的增删操作

        uint64_t target_offset = (uint64_t)-1;
        bool conflict = false;

        dir_foreach(dir_inum, [&](Dirent& entry, uint64_t offset) {
            if (entry.inum == 0) 
            {
                if (target_offset == (uint64_t)-1) target_offset = offset;  // 记录第一个空槽位
                return false;                                               // 继续检查重名
            }
            if (strncmp(entry.name, name, NAMESIZ) == 0) 
            {
                conflict = true;                                            
                return true;                                                // 发现冲突，立刻停止
            }
            return false;                                                   
        });

        if (conflict) 
        {
            log_err("[dir_add_entry] Entry '%s' already exists.", name);
            return -EEXIST;
        }

        if (target_offset == (uint64_t)-1)        
        {
            // 无空槽位，需追加。持有 VFS 锁时，file_size 是安全的
            auto read_acc = dir_ih.read_access();
            target_offset = read_acc->file_size; 
        }

        Dirent new_entry;
        memset(&new_entry, 0, sizeof(Dirent));
        new_entry.inum = inum;
        new_entry.filetype = type;
        strncpy(new_entry.name, name, NAMESIZ);

        ssize_t written = file_write(dir_inum, (const char*)&new_entry, target_offset, sizeof(Dirent));
        if (written != sizeof(Dirent)) 
        {
            log_err("[dir_add_entry] Failed to write directory entry.");
            return -1;
        }

        // 更新脏标记，以便将来同步 mtime
        dir_ih.write_access().mark_dirty();    
        get_dentry_cache().put(DentryKey(dir_inum, name), {inum, type});  // 加入 dentryCache
    }

    return 0;
}

int dir_delete_entry(int dir_inum, const char* name)
{
    bool deleted = false;

    {
        InodeHandle dir_ih = ic_get_inode(dir_inum);
        if (!dir_ih) return -1;

        // 获取 VFS 专属互斥锁
        DirGuard vfs_guard(dir_ih);

        dir_foreach(dir_inum, [&](Dirent& entry, uint64_t offset) {
            if (entry.inum != 0 && strncmp(entry.name, name, NAMESIZ) == 0) 
            {
                entry.inum = 0;                      
                memset(entry.name, 0, NAMESIZ);      
            
                // 数据擦除后写回原位置
                file_write(dir_inum, (const char*)&entry, offset, sizeof(Dirent));   
                deleted = true;
                return true; // 删完即走
            }
            return false; 
        });

        if (deleted) 
        {
            dir_ih.write_access().mark_dirty();
            get_dentry_cache().invalidate(DentryKey(dir_inum, name));      // 磁盘上文件已经被删了，必须把缓存里的条目也抹杀掉！
            return 0;
        }
    }
    
    return -1;   
}