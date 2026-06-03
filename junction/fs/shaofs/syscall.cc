#include "utili.h"
#include "syscall.h"
#include "dentryCache.h"
#include "file.h"
#include "namei.h"
#include "dir.h"
#include "inodeCache.h"
#include "blockCache.h"
#include "extent.h"
#include "junction/fs/file.h"

struct InodeLifecycle {
    spinlock_t lock;
    uint32_t open_count;
    bool unlink_pending;
    bool free_in_progress;
};

static InodeLifecycle inode_lifecycle[INODENUM];
static volatile int inode_lifecycle_initialized;
static spinlock_t inode_lifecycle_init_lock = SPINLOCK_INITIALIZER;

static void init_inode_lifecycle_table()
{
    if (likely(atomic_read(&inode_lifecycle_initialized))) return;

    SpinGuardNP g(&inode_lifecycle_init_lock);
    if (atomic_read(&inode_lifecycle_initialized)) return;

    for (int i = 0; i < INODENUM; i++)
    {
        spin_lock_init(&inode_lifecycle[i].lock);
        inode_lifecycle[i].open_count = 0;
        inode_lifecycle[i].unlink_pending = false;
        inode_lifecycle[i].free_in_progress = false;
    }
    atomic_write(&inode_lifecycle_initialized, 1);
}

static InodeLifecycle* get_inode_lifecycle(int inum)
{
    if (unlikely(inum < 0 || inum >= INODENUM)) return nullptr;
    init_inode_lifecycle_table();
    return &inode_lifecycle[inum];
}

void shaofs_reset_inode_lifecycle(int inum)
{
    InodeLifecycle* state = get_inode_lifecycle(inum);
    if (unlikely(!state)) return;

    SpinGuardNP g(&state->lock);
    state->open_count = 0;
    state->unlink_pending = false;
    state->free_in_progress = false;
}

void shaofs_pin_open_inode(int inum)
{
    InodeLifecycle* state = get_inode_lifecycle(inum);
    if (unlikely(!state)) return;

    SpinGuardNP g(&state->lock);
    state->open_count++;
}

void my_close(int inum)
{
    RuntimeFSBaseGuard g;

    InodeLifecycle* state = get_inode_lifecycle(inum);
    if (unlikely(!state)) return;

    bool should_free = false;
    {
        SpinGuardNP guard(&state->lock);
        if (likely(state->open_count > 0)) state->open_count--;
        if (state->open_count == 0 && state->unlink_pending && !state->free_in_progress)
        {
            state->free_in_progress = true;
            should_free = true;
        }
    }

    if (!should_free) return;

    bool freed = ic_free_inode(inum);
    {
        SpinGuardNP guard(&state->lock);
        if (freed) state->unlink_pending = false;
        state->free_in_progress = false;
    }
}

static int shaofs_unlink_inode(int inum)
{
    InodeLifecycle* state = get_inode_lifecycle(inum);
    if (unlikely(!state)) return -EINVAL;

    bool should_free = false;
    {
        SpinGuardNP guard(&state->lock);
        if (state->open_count == 0 && !state->free_in_progress)
        {
            state->free_in_progress = true;
            should_free = true;
        }
        else state->unlink_pending = true;
    }

    if (!should_free) return 0;

    bool freed = ic_free_inode(inum);
    {
        SpinGuardNP guard(&state->lock);
        if (freed) state->unlink_pending = false;
        state->free_in_progress = false;
    }
    return freed ? 0 : -EIO;
}

struct OpenResult {
    int inum;
    file_type_t type;
};

static int open_existing_or_root(const char* pathname, OpenResult* out)
{
    if (strcmp(pathname, "/") == 0)
    {
        shaofs_pin_open_inode(ROOT_INO);
        *out = {ROOT_INO, DIRECTORY};
        return 0;
    }

    char name[NAMESIZ];
    int parent_inum = nameiparent(pathname, name);
    if (parent_inum == -1) return -ENOENT;

    file_type_t type;
    int inum = dir_lookup_pin(parent_inum, name, &type);
    if (inum == -1) return -ENOENT;

    *out = {inum, type};
    return 0;
}

static int create_regular_file(const char* pathname, int flags, OpenResult* out)
{
    char name[NAMESIZ];
    int parent_inum = nameiparent(pathname, name);
    if (parent_inum == -1) return -ENOENT;

    while (true)
    {
        file_type_t type;
        int inum = dir_lookup_pin(parent_inum, name, &type);
        if (inum != -1)
        {
            if (flags & junction::kFlagExclusive)
            {
                my_close(inum);
                log_err("[fs_open] File already exists (O_EXCL triggered): %s", pathname);
                return -EEXIST;
            }

            if (type == DIRECTORY)
            {
                my_close(inum);
                log_err("[fs_open] Cannot open a directory with O_CREAT: %s", pathname);
                return -EISDIR;
            }

            *out = {inum, type};
            return 0;
        }

        InodeHandle new_ih = ic_alloc_inode(REGULAR);
        if (!new_ih)
        {
            log_err("[fs_open] Inode space exhausted for %s", pathname);
            return -ENOSPC;
        }

        int new_inum = new_ih.read_access()->idx;
        shaofs_pin_open_inode(new_inum);

        int ret = dir_add_entry(parent_inum, name, new_inum, REGULAR);
        if (ret == 0)
        {
            *out = {new_inum, REGULAR};
            return 0;
        }

        my_close(new_inum);
        ic_free_inode(new_inum);

        if (ret == -EEXIST)
        {
            if (flags & junction::kFlagExclusive)
                return -EEXIST;
            continue;
        }

        log_err("Fail to add dentry");
        return ret;
    }
}

static int validate_open_type(int inum, file_type_t type, int flags)
{
    if ((flags & junction::kFlagDirectory) && type != DIRECTORY)
    {
        my_close(inum);
        return -ENOTDIR;
    }

    if (type == DIRECTORY && ((flags & junction::kAccessModeMask) != O_RDONLY || (flags & (junction::kFlagTruncate | junction::kFlagDirect))))
    {
        my_close(inum);
        return -EISDIR;
    }

    return 0;
}

int my_open(const char* pathname, int flags, mode_t mode, file_type_t* type_out)
{
    RuntimeFSBaseGuard g;
    (void)mode;

    OpenResult opened = {-1, REGULAR};
    int ret = (flags & junction::kFlagCreate) ? create_regular_file(pathname, flags, &opened)
                                              : open_existing_or_root(pathname, &opened);
    if (ret != 0) return ret;

    ret = validate_open_type(opened.inum, opened.type, flags);
    if (ret != 0) return ret;

    if (flags & junction::kFlagTruncate)
    {
        truncate_inode(opened.inum);
    }

    if (type_out) *type_out = opened.type;
    return opened.inum;
}

ssize_t my_read(int inum, void *buf, off_t* off, size_t len, bool direct)
{
    RuntimeFSBaseGuard g;

    ssize_t ret = direct ? file_read_direct(inum, (char*)buf, *off, len)
                         : file_read(inum, (char*)buf, *off, len);
    if (ret >= 0) *off += ret;
    return ret;
}

ssize_t my_write(int inum, const void *buf, off_t* off, size_t len, bool direct, bool append)
{
    RuntimeFSBaseGuard g;

    ssize_t ret;
    if (append && direct)
        ret = file_write_direct_append(inum, (const char*)buf, len, off);
    else if (append)
        ret = file_write_append(inum, (const char*)buf, len, off);
    else
        ret = direct ? file_write_direct(inum, (const char*)buf, *off, len)
                     : file_write(inum, (const char*)buf, *off, len);

    if (ret >= 0 && !append) *off += ret;
    return ret;
}

static void init_directory_entries(Dirent* entries, int new_inum, int parent_inum)
{
    memset(entries, 0, sizeof(Dirent) * 2);
    entries[0].inum = new_inum;
    entries[0].filetype = DIRECTORY;
    strncpy(entries[0].name, ".", NAMESIZ);
    entries[1].inum = parent_inum;
    entries[1].filetype = DIRECTORY;
    strncpy(entries[1].name, "..", NAMESIZ);
}

static void link_new_directory(const InodeHandle& new_ih, int parent_inum)
{
    {
        auto write_acc = new_ih.write_access();
        write_acc->nlink++;
        mark_inode_metadata_dirty(&*write_acc);
        write_acc.mark_dirty();
    }

    InodeHandle parent_ih = ic_get_inode(parent_inum);
    if (parent_ih)
    {
        auto write_acc = parent_ih.write_access();
        write_acc->nlink++;
        mark_inode_metadata_dirty(&*write_acc);
        write_acc.mark_dirty();
    }
}

int my_mkdir(const char *pathname, mode_t mode)
{
    // log_info("mkdir(%s)", pathname);

    RuntimeFSBaseGuard g;
    (void)mode;

    char name[NAMESIZ];
    int parent_inum = nameiparent(pathname, name);
    if (parent_inum == -1) return -ENOENT;

    file_type_t type;
    int existing_inum = dir_lookup(parent_inum, name, &type);
    if (existing_inum != -1)
    {
        InodeHandle existing_ih = ic_get_inode(existing_inum);
        if (!existing_ih)
        {
            log_err("[my_mkdir] Failed to verify existing entry: %s", pathname);
            return -EIO;
        }

        bool stale;
        {
            auto acc = existing_ih.read_access();
            stale = !acc->used;
        }
        if (!stale)
        {
            log_err("[my_mkdir] Directory/File already exists: %s", pathname);
            return -EEXIST;
        }
        if (dir_delete_stale_entry(parent_inum, name) != 0) return -EEXIST;
    }

    // 分配新的目录 Inode
    InodeHandle new_ih = ic_alloc_inode(DIRECTORY);
    if (!new_ih) 
    {
        log_err("[my_mkdir] Inode space exhausted for %s", pathname);
        return -ENOSPC;
    }
    int new_inum = new_ih.read_access()->idx;

    Dirent entries[2];
    init_directory_entries(entries, new_inum, parent_inum);

    ssize_t written = file_write(new_inum, (const char*)entries, 0, sizeof(entries));
    if (written != sizeof(entries)) 
    {
        log_err("[my_mkdir] Failed to init . and .. for %s", pathname);
        ic_free_inode(new_inum);
        return -EIO;
    }

    // 将新目录注册到父目录中
    int ret = dir_add_entry(parent_inum, name, new_inum, DIRECTORY);
    if (ret != 0)
    {
        ic_free_inode(new_inum);
        return ret;
    }

    link_new_directory(new_ih, parent_inum);
    return 0;
}

int my_unlink(const char *pathname)
{
    RuntimeFSBaseGuard g;

    char name[NAMESIZ];
    int parent_inum = nameiparent(pathname, name);
    if (parent_inum == -1) return -ENOENT;

    int inum;
    file_type_t type;
    int ret = dir_delete_file_entry(parent_inum, name, &inum, &type);
    if (ret != 0) return ret;
    return shaofs_unlink_inode(inum);
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

static void fill_stat_from_inode(const MInode* inode, int inum, struct stat *st)
{
    memset(st, 0, sizeof(struct stat));

    st->st_ino   = inum;
    st->st_nlink = inode->nlink;
    st->st_size  = inode->file_size;

    // 文件类型映射
    switch (inode->type) 
    {
      case REGULAR:   st->st_mode = S_IFREG | 0644; break;
      case DIRECTORY: st->st_mode = S_IFDIR | 0755; break;
      case SYMLINK:   st->st_mode = S_IFLNK | 0777; break;
      default:        st->st_mode = S_IFREG | 0644; break;
    }

    st->st_blksize = BLOCK_SIZE;

    // 统计已分配的物理块数（st_blocks 单位为 512B 扇区）
    uint64_t allocated_blocks = 0;
    auto count_extent = [](const iExtent& ext, void* arg) -> bool {
        *static_cast<uint64_t*>(arg) += ext.block_count;
        return true;
    };
    if (!inode_for_each_extent(inode, true, count_extent, &allocated_blocks)) log_err("[fill_stat_from_inode] failed to walk extents for inode %d", inum);

    st->st_blocks = allocated_blocks * (BLOCK_SIZE / 512);

    st->st_dev   = 0;
    st->st_atime = inode->atime;
    st->st_mtime = inode->mtime;
    st->st_ctime = inode->ctime;
}

int my_fstat(int inum, struct stat *statbuf)
{
    RuntimeFSBaseGuard g;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -ENOENT;

    auto read_acc = ih.read_access();
    if (!read_acc->used) return -ENOENT;

    fill_stat_from_inode(&(*read_acc), inum, statbuf);
    return 0;
}

int my_newfstatat(const char *pathname, struct stat *statbuf)
{
    RuntimeFSBaseGuard g;

    int inum = namei(pathname);
    if (inum == -1) return -ENOENT;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -ENOENT;

    auto read_acc = ih.read_access();
    if (!read_acc->used) return -ENOENT;

    fill_stat_from_inode(&(*read_acc), inum, statbuf);
    return 0;
}

struct FsyncSnapshot {
    uint64_t dirty_start;
    uint64_t dirty_end;
    uint64_t dirty_seq;
    uint64_t inode_dirty_seq;
    uint64_t inode_fsync_seq;
    bool has_dirty_data;
    bool need_inode_flush;
};

static int snapshot_fsync_state_locked(MInode* inode, uint64_t file_size, FsyncSnapshot* snapshot)
{
    snapshot->dirty_start = 0;
    snapshot->dirty_end = 0;
    snapshot->dirty_seq = 0;
    snapshot->inode_dirty_seq = inode->inode_dirty_seq;
    snapshot->inode_fsync_seq = inode->inode_fsync_seq;
    snapshot->need_inode_flush = snapshot->inode_dirty_seq != snapshot->inode_fsync_seq;
    snapshot->has_dirty_data = false;

    {
        SpinGuardNP dirty_g(&inode->dirty_lock);
        snapshot->has_dirty_data = atomic_read(&inode->has_dirty_data_cache);
        if (snapshot->has_dirty_data)
        {
            snapshot->dirty_start = inode->dirty_data_start;
            snapshot->dirty_end = MIN(inode->dirty_data_end, file_size);
            snapshot->dirty_seq = inode->dirty_data_seq;
        }
    }

    return 0;
}

static int flush_dirty_data_runs(MInode* inode, const FsyncSnapshot& snapshot)
{
    if (!snapshot.has_dirty_data || snapshot.dirty_start >= snapshot.dirty_end) return 0;

    BlockID first_logical = snapshot.dirty_start / BLOCK_SIZE;
    BlockID last_logical = (snapshot.dirty_end - 1) / BLOCK_SIZE;
    BlockID run_start = INVALID_BLOCK_ID;
    uint32_t run_count = 0;

    auto flush_run = [&]() -> bool {
        if (run_count == 0) return true;
        bool ok = bc_flush_blocks_contiguous(run_start, run_count);
        run_start = INVALID_BLOCK_ID;
        run_count = 0;
        return ok;
    };

    for (BlockID logical = first_logical; logical <= last_logical; logical++)
    {
        BlockID phys_blk = inode_bmap_locked(inode, logical, false, nullptr);
        if (phys_blk == INVALID_BLOCK_ID)
        {
            if (!flush_run()) return -EIO;
            continue;
        }

        if (run_count > 0 && phys_blk == run_start + run_count && run_count < 1024)
        {
            run_count++;
            continue;
        }

        if (!flush_run()) return -EIO;
        run_start = phys_blk;
        run_count = 1;
    }

    return flush_run() ? 0 : -EIO;
}

static int clear_dirty_if_unchanged(const InodeHandle& ih, uint64_t dirty_seq)
{
    auto write_acc = ih.write_access();
    if (!write_acc->used) return -ENOENT;

    SpinGuardNP dirty_g(&write_acc->dirty_lock);
    if (write_acc->dirty_data_seq == dirty_seq)
    {
        write_acc->clear_dirty_data_unlocked();
        write_acc->dirty_data_seq++;
    }
    return 0;
}

static int update_inode_fsync_seq_if_unchanged(const InodeHandle& ih, const FsyncSnapshot& snapshot)
{
    auto write_acc = ih.write_access();
    if (!write_acc->used) return -ENOENT;
    if (write_acc->inode_dirty_seq == snapshot.inode_dirty_seq && write_acc->inode_fsync_seq == snapshot.inode_fsync_seq)
        write_acc->inode_fsync_seq = snapshot.inode_dirty_seq;
    return 0;
}

int my_fsync(int inum)
{
    RuntimeFSBaseGuard g;

    InodeHandle ih = ic_get_inode(inum);
    if (unlikely(!ih)) return -ENOENT;

    FsyncSnapshot snapshot;

    {
        auto read_acc = ih.read_access();
        if (!read_acc->used) return -ENOENT;
        MInode* inode = const_cast<MInode*>(&(*read_acc));

        int ret = snapshot_fsync_state_locked(inode, read_acc->file_size, &snapshot);
        if (ret != 0) return ret;

        ret = flush_dirty_data_runs(inode, snapshot);
        if (ret != 0) return ret;

        if (snapshot.need_inode_flush && !inode_flush_extent_metadata(&(*read_acc)))
        {
            return -EIO;
        }
    }

    if (!snapshot.has_dirty_data && !snapshot.need_inode_flush) return 0;

    if (snapshot.has_dirty_data)
    {
        int ret = clear_dirty_if_unchanged(ih, snapshot.dirty_seq);
        if (ret != 0) return ret;
    }

    if (snapshot.need_inode_flush)
    {
        if (!ic_flush_inode(inum)) return -EIO;

        int ret = update_inode_fsync_seq_if_unchanged(ih, snapshot);
        if (ret != 0) return ret;
    }

    return 0;
}
