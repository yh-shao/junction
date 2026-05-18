#include "fs.h"
#include "dir.h"
#include "inodeCache.h"
#include "file.h"
#include "dentryCache.h"

struct DirIndexNode
{
    DirIndexNode* hash_next;
    DirIndexNode* free_next;
    uint64_t hash;
    uint64_t offset;
    int inum;
    file_type_t type;
    char name[NAMESIZ];
};

static constexpr uint32_t kDirIndexNodeChunkEntries = 512;
static constexpr uint32_t kDirIndexMinBuckets = 16;
static constexpr uint32_t kDirIndexMaxBuckets = 32768;

struct DirIndexChunk
{
    DirIndexChunk* next;
    DirIndexNode nodes[kDirIndexNodeChunkEntries];
};

struct DirIndex
{
    DirIndexNode** buckets;
    DirIndexChunk* chunks;
    DirIndexNode* spare_nodes;
    DirIndexNode* free_slots;
    uint32_t bucket_count;
    uint32_t entry_count;
    uint32_t live_children = 0;
};

void dir_index_destroy(DirIndex* index)
{
    if (!index) return;

    DirIndexChunk* chunk = index->chunks;
    while (chunk)
    {
        DirIndexChunk* next = chunk->next;
        sfree(chunk);
        chunk = next;
    }
    if (index->buckets) sfree(index->buckets);
    sfree(index);
}

static inline bool dir_name_is_dot_or_dotdot(const char* name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static inline uint64_t dir_name_hash(const char* name)
{
    uint64_t h = 5381;
    for (int i = 0; i < NAMESIZ && name[i] != '\0'; i++)
        h = ((h << 5) + h) + (unsigned char)name[i];
    return h ? h : 1;
}

static inline uint32_t dir_index_bucket(uint64_t hash, uint32_t bucket_count)
{
    return (uint32_t)(hash & (bucket_count - 1));
}

static uint32_t dir_index_bucket_count_for(size_t slots)
{
    uint32_t buckets = kDirIndexMinBuckets;
    size_t want = slots < 8 ? kDirIndexMinBuckets : slots * 2;
    while (buckets < want && buckets < kDirIndexMaxBuckets) buckets <<= 1;
    return buckets;
}

static bool dir_index_alloc_buckets(DirIndex* index, uint32_t bucket_count)
{
    size_t bytes = bucket_count * sizeof(DirIndexNode*);
    DirIndexNode** buckets = static_cast<DirIndexNode**>(szalloc(bytes));
    if (unlikely(!buckets)) return false;

    index->buckets = buckets;
    index->bucket_count = bucket_count;
    return true;
}

static bool dir_index_add_chunk(DirIndex* index)
{
    DirIndexChunk* chunk = static_cast<DirIndexChunk*>(szalloc(sizeof(DirIndexChunk)));
    if (unlikely(!chunk)) return false;

    chunk->next = index->chunks;
    index->chunks = chunk;

    for (uint32_t i = 0; i < kDirIndexNodeChunkEntries; i++)
    {
        chunk->nodes[i].free_next = index->spare_nodes;
        index->spare_nodes = &chunk->nodes[i];
    }

    return true;
}

static DirIndexNode* dir_index_alloc_node(DirIndex* index)
{
    if (unlikely(!index->spare_nodes) && !dir_index_add_chunk(index)) return nullptr;

    DirIndexNode* node = index->spare_nodes;
    index->spare_nodes = node->free_next;
    memset(node, 0, sizeof(*node));
    return node;
}

static void dir_index_release_node(DirIndex* index, DirIndexNode* node)
{
    node->free_next = index->spare_nodes;
    index->spare_nodes = node;
}

static DirIndexNode* dir_index_find(DirIndex* index, const char* name, uint64_t hash)
{
    if (!index || !index->buckets) return nullptr;

    uint32_t bucket = dir_index_bucket(hash, index->bucket_count);
    for (DirIndexNode* node = index->buckets[bucket]; node; node = node->hash_next)
        if (node->hash == hash && strncmp(node->name, name, NAMESIZ) == 0) return node;
    return nullptr;
}

static bool dir_index_rehash(DirIndex* index, uint32_t new_bucket_count)
{
    if (new_bucket_count <= index->bucket_count) return true;

    size_t bytes = new_bucket_count * sizeof(DirIndexNode*);
    DirIndexNode** new_buckets = static_cast<DirIndexNode**>(szalloc(bytes));
    if (unlikely(!new_buckets)) return false;

    for (uint32_t i = 0; i < index->bucket_count; i++)
    {
        DirIndexNode* node = index->buckets[i];
        while (node)
        {
            DirIndexNode* next = node->hash_next;
            uint32_t bucket = dir_index_bucket(node->hash, new_bucket_count);
            node->hash_next = new_buckets[bucket];
            new_buckets[bucket] = node;
            node = next;
        }
    }

    sfree(index->buckets);
    index->buckets = new_buckets;
    index->bucket_count = new_bucket_count;
    return true;
}

static bool dir_index_maybe_grow(DirIndex* index)
{
    if (index->bucket_count >= kDirIndexMaxBuckets) return true;
    if ((index->entry_count + 1) * 4 < index->bucket_count * 3) return true;
    return dir_index_rehash(index, index->bucket_count << 1);
}

static void dir_index_insert_live(DirIndex* index, DirIndexNode* node)
{
    uint32_t bucket = dir_index_bucket(node->hash, index->bucket_count);
    node->hash_next = index->buckets[bucket];
    index->buckets[bucket] = node;
    index->entry_count++;
    if (!dir_name_is_dot_or_dotdot(node->name)) index->live_children++;
}

static void dir_index_remove_live(DirIndex* index, DirIndexNode* node)
{
    uint32_t bucket = dir_index_bucket(node->hash, index->bucket_count);
    DirIndexNode** link = &index->buckets[bucket];
    while (*link && *link != node) link = &(*link)->hash_next;
    if (*link == node) *link = node->hash_next;

    if (index->entry_count > 0) index->entry_count--;
    if (!dir_name_is_dot_or_dotdot(node->name) && index->live_children > 0) index->live_children--;
    node->hash_next = nullptr;
}

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

static DirIndex* dir_get_index_locked(const InodeHandle& dir_ih)
{
    auto acc = dir_ih.read_access();
    return acc->dir_index;
}

static int dir_build_index_locked(int dir_inum, const InodeHandle& dir_ih, DirIndex* index)
{
    size_t slots;
    {
        auto acc = dir_ih.read_access();
        slots = acc->file_size / sizeof(Dirent);
    }

    if (!dir_index_alloc_buckets(index, dir_index_bucket_count_for(slots))) return -1;
    uint32_t chunks = (uint32_t)((slots + kDirIndexNodeChunkEntries - 1) / kDirIndexNodeChunkEntries);
    if (chunks == 0) chunks = 1;
    for (uint32_t i = 0; i < chunks; i++)
        if (!dir_index_add_chunk(index)) return -1;

    bool failed = false;
    int ret = dir_foreach_locked(dir_inum, [&](const Dirent& entry, uint64_t offset) {
        DirIndexNode* node = dir_index_alloc_node(index);
        if (unlikely(!node))
        {
            failed = true;
            return true;
        }

        node->offset = offset;
        if (dirent_is_empty(&entry))
        {
            node->free_next = index->free_slots;
            index->free_slots = node;
            return false;
        }

        node->hash = dir_name_hash(entry.name);
        node->inum = entry.inum;
        node->type = entry.filetype;
        strncpy(node->name, entry.name, NAMESIZ);
        node->name[NAMESIZ - 1] = '\0';
        dir_index_insert_live(index, node);
        return false;
    });
    return failed ? -1 : ret;
}

static DirIndex* dir_ensure_index_locked(int dir_inum, const InodeHandle& dir_ih)
{
    if (DirIndex* index = dir_get_index_locked(dir_ih)) return index;

    DirIndex* new_index = static_cast<DirIndex*>(szalloc(sizeof(DirIndex)));
    if (unlikely(!new_index)) return nullptr;

    if (dir_build_index_locked(dir_inum, dir_ih, new_index) != 0)
    {
        dir_index_destroy(new_index);
        return nullptr;
    }

    auto acc = dir_ih.write_access();
    if (!acc->dir_index)
    {
        acc->dir_index = new_index;
        return new_index;
    }

    DirIndex* existing = acc->dir_index;
    dir_index_destroy(new_index);
    return existing;
}

int dir_lookup(int dir_inum, const char* name, file_type_t* type)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return -1;

    DirReadGuard rguard(dir_ih);   // 目录读锁，允许并发 lookup

    if (DirIndex* index = dir_get_index_locked(dir_ih))
    {
        DirIndexNode* node = dir_index_find(index, name, dir_name_hash(name));
        if (!node) return -1;
        if (type) *type = node->type;
        return node->inum;
    }

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

    if (DirIndex* index = dir_get_index_locked(dir_ih)) return index->live_children == 0;

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

    DirIndex* index = dir_ensure_index_locked(dir_inum, dir_ih);
    if (unlikely(!index)) return -1;

    uint64_t hash = dir_name_hash(name);
    if (dir_index_find(index, name, hash))
    {
        log_err("[dir_add_entry] Entry '%s' already exists.", name);
        return -EEXIST;
    }

    uint64_t target_offset;
    bool reused_free_slot = false;
    DirIndexNode* node = nullptr;

    if (index->free_slots)
    {
        node = index->free_slots;
        index->free_slots = node->free_next;
        target_offset = node->offset;
        reused_free_slot = true;
    }
    else
    {
        if (!dir_index_maybe_grow(index)) return -1;
        node = dir_index_alloc_node(index);
        if (unlikely(!node)) return -1;

        auto read_acc = dir_ih.read_access();
        target_offset = read_acc->file_size;
        node->offset = target_offset;
    }

    Dirent new_entry;
    memset(&new_entry, 0, sizeof(Dirent));
    new_entry.inum = inum;
    new_entry.filetype = type;
    strncpy(new_entry.name, name, NAMESIZ);
    new_entry.name[NAMESIZ - 1] = '\0';

    ssize_t written = file_write(dir_inum, (const char*)&new_entry, target_offset, sizeof(Dirent));
    if (written != sizeof(Dirent))
    {
        if (reused_free_slot)
        {
            node->free_next = index->free_slots;
            index->free_slots = node;
        }
        else dir_index_release_node(index, node);
        log_err("[dir_add_entry] Failed to write directory entry.");
        return -1;
    }

    node->hash = hash;
    node->inum = inum;
    node->type = type;
    strncpy(node->name, name, NAMESIZ);
    node->name[NAMESIZ - 1] = '\0';
    dir_index_insert_live(index, node);

    dir_ih.write_access().mark_dirty();
    get_dentry_cache().put(DentryKey(dir_inum, name), {inum, type});

    return 0;
}

int dir_delete_entry(int dir_inum, const char* name)
{
    InodeHandle dir_ih = ic_get_inode(dir_inum);
    if (!dir_ih) return -1;

    DirWriteGuard wguard(dir_ih);

    DirIndex* index = dir_ensure_index_locked(dir_inum, dir_ih);
    if (unlikely(!index)) return -1;

    DirIndexNode* node = dir_index_find(index, name, dir_name_hash(name));
    if (!node) return -1;

    Dirent empty_entry;
    memset(&empty_entry, 0, sizeof(empty_entry));

    uint64_t offset = node->offset;
    ssize_t written = file_write(dir_inum, (const char*)&empty_entry, offset, sizeof(Dirent));
    if (written != sizeof(Dirent)) return -1;

    dir_index_remove_live(index, node);
    memset(node->name, 0, NAMESIZ);
    node->inum = 0;
    node->type = UNKNOWN;
    node->hash = 0;
    node->offset = offset;
    node->free_next = index->free_slots;
    index->free_slots = node;

    dir_ih.write_access().mark_dirty();
    get_dentry_cache().invalidate(DentryKey(dir_inum, name));
    return 0;
}
