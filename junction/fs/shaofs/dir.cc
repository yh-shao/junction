#include "fs.h"
#include "dir.h"
#include "inodeCache.h"
#include "file.h"
#include "dentryCache.h"

// 获取目录 inode 的 dir_mtx 指针（通过短暂的 inode cache 读锁获取）
static rwmutex_t* get_dir_mtx(const InodeHandle& ih)
{
    auto acc = ih.read_access();
    return &acc->dir_mtx;
}

// RAII 目录读锁：允许多个 lookup / is_empty 并发执行
class DirReadGuard
{
    rwmutex_t* mtx;
public:
    explicit DirReadGuard(const InodeHandle& ih) : mtx(get_dir_mtx(ih)) { rwmutex_rdlock(mtx); }
    ~DirReadGuard() { rwmutex_unlock(mtx); }
    DirReadGuard(const DirReadGuard&) = delete;
    DirReadGuard& operator=(const DirReadGuard&) = delete;
};

// RAII 目录写锁：add_entry / delete_entry 需要排他访问
class DirWriteGuard
{
    rwmutex_t* mtx;
public:
    explicit DirWriteGuard(const InodeHandle& ih) : mtx(get_dir_mtx(ih)) { rwmutex_wrlock(mtx); }
    ~DirWriteGuard() { rwmutex_unlock(mtx); }
    DirWriteGuard(const DirWriteGuard&) = delete;
    DirWriteGuard& operator=(const DirWriteGuard&) = delete;
};

// 遍历目录中的所有 Dirent。调用前 caller 应已持有 dir_mtx（读锁或写锁）。
template <typename Func>
static int dir_foreach_locked(int dir_inum, Func callback)
{
    char block_buf[BLOCK_SIZE];
    uint64_t offset = 0;

    while (true)
    {
        ssize_t read_bytes = file_read(dir_inum, block_buf, offset, BLOCK_SIZE);
        if (read_bytes < 0) return -1;

        int n_entries = read_bytes / sizeof(Dirent);
        Dirent* entries = reinterpret_cast<Dirent*>(block_buf);
        for (int i = 0; i < n_entries; i++)
            if (callback(entries[i], offset + i * sizeof(Dirent))) return 0;

        if (read_bytes < BLOCK_SIZE) break;
        offset += read_bytes;
    }

    return 0;
}

int dir_lookup(int dir_inum, const char* name, file_type_t* type)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return -1;

    DirReadGuard rguard(dir_ih);   // 目录读锁，允许并发 lookup

    int found_inum = -1;
    dir_foreach_locked(dir_inum, [&](const Dirent& entry, uint64_t) {
        if (!dirent_is_empty(&entry) && strncmp(entry.name, name, NAMESIZ) == 0)
        {
            if (type) *type = entry.filetype;
            found_inum = entry.inum;
            return true;
        }
        return false;
    });

    return found_inum;
}

bool dir_is_empty(int dir_inum)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return true;

    DirReadGuard rguard(dir_ih);

    bool empty = true;
    dir_foreach_locked(dir_inum, [&](const Dirent& entry, uint64_t) {
        if (!dirent_is_empty(&entry) &&
            strncmp(entry.name, ".", NAMESIZ) != 0 &&
            strncmp(entry.name, "..", NAMESIZ) != 0)
        {
            empty = false;
            return true;
        }
        return false;
    });

    return empty;
}

int dir_add_entry(int dir_inum, const char* name, int inum, file_type_t type)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return -1;

    DirWriteGuard wguard(dir_ih);   // 目录写锁，串行化增删操作

    uint64_t target_offset = (uint64_t)-1;
    bool conflict = false;

    dir_foreach_locked(dir_inum, [&](const Dirent& entry, uint64_t offset) {
        if (dirent_is_empty(&entry))
        {
            if (target_offset == (uint64_t)-1) target_offset = offset;
            return false;   // 继续检查重名
        }
        if (strncmp(entry.name, name, NAMESIZ) == 0)
        {
            conflict = true;
            return true;
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

    dir_ih.write_access().mark_dirty();
    get_dentry_cache().put(DentryKey(dir_inum, name), {inum, type});

    return 0;
}

int dir_delete_entry(int dir_inum, const char* name)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return -1;

    DirWriteGuard wguard(dir_ih);

    bool deleted = false;
    dir_foreach_locked(dir_inum, [&](Dirent& entry, uint64_t offset) {
        if (!dirent_is_empty(&entry) && strncmp(entry.name, name, NAMESIZ) == 0)
        {
            // 清空该 slot：inum=0 且 name 清零，满足 dirent_is_empty() 判定
            entry.inum = 0;
            memset(entry.name, 0, NAMESIZ);

            file_write(dir_inum, (const char*)&entry, offset, sizeof(Dirent));
            deleted = true;
            return true;
        }
        return false;
    });

    if (deleted)
    {
        dir_ih.write_access().mark_dirty();
        get_dentry_cache().invalidate(DentryKey(dir_inum, name));
        return 0;
    }

    return -1;
}
