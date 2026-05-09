#include "journal.h"

#if CRASH_CONSISTENCY

#include "dir.h"
#include "utili.h"
#include <cstring>
#include <cstdlib>
#include <vector>

struct JournalHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t state;
    uint32_t entry_count;
    uint64_t seq;
    uint64_t checksum;
};
static_assert(sizeof(JournalHeader) <= BLOCK_SIZE, "JournalHeader must fit in one block");

struct JournalEntry {
    BlockID home_block;
    BlockID image_block;
    uint64_t checksum;
};
static_assert(sizeof(JournalEntry) <= BLOCK_SIZE, "JournalEntry must fit in one block");

static constexpr uint32_t kJournalMagic = 0x4a53484f;  // "JSHO"
static constexpr uint32_t kJournalVersion = 1;
static constexpr uint32_t kJournalEmpty = 0;
static constexpr uint32_t kJournalCommitted = 1;
static constexpr uint32_t kJournalDirty = 2;
static constexpr uint32_t kMaxJournalEntries = 64;
static_assert(kMaxJournalEntries * sizeof(JournalEntry) <= BLOCK_SIZE, "journal entry table must fit in one block");

static spinlock_t journal_lock;
static spinlock_t metadata_lock;
static uint64_t journal_seq;

struct BlockRange {
    BlockID start;
    uint64_t count;
};
static std::vector<BlockRange>* metadata_ranges;

static inline uint64_t fnv1a64(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) 
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static inline bool journal_layout_valid()
{
    return sb.journal_blocknum >= 4 && sb.journal_blockstart > sb.group_blockstart && sb.journal_blockstart + sb.journal_blocknum <= sb.total_blocknum;
}

static inline BlockID mount_state_lba()
{
    return sb.journal_blockstart + sb.journal_blocknum - 1;
}

static inline uint64_t header_checksum(const JournalHeader& hdr, const JournalEntry* entries)
{
    JournalHeader tmp = hdr;
    tmp.checksum = 0;

    uint64_t h = fnv1a64(&tmp, sizeof(tmp));
    h ^= fnv1a64(entries, sizeof(JournalEntry) * hdr.entry_count);
    h *= 1099511628211ULL;
    return h;
}

static bool write_zero_block(BlockID blk)
{
    alignas(BLOCK_SIZE) char zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    return storage_write(zero, blk, 1) == 0;
}

static bool read_block_header(BlockID blk, JournalHeader* hdr)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    if (storage_read(block, blk, 1) != 0) return false;
    memcpy(hdr, block, sizeof(*hdr));
    return true;
}

static bool write_block_header(BlockID blk, const JournalHeader& hdr)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, &hdr, sizeof(hdr));
    return storage_write(block, blk, 1) == 0;
}

static bool read_full_block(BlockID blk, void* out)
{
    return storage_read(out, blk, 1) == 0;
}

static bool write_full_block(BlockID blk, const void* image)
{
    if (journal_is_metadata_block(blk)) return journal_commit_single(blk, image);
    return storage_write(image, blk, 1) == 0;
}

static bool read_header(JournalHeader* hdr, JournalEntry* entries)
{
    alignas(BLOCK_SIZE) char header_block[BLOCK_SIZE];
    alignas(BLOCK_SIZE) char entry_block[BLOCK_SIZE];

    if (storage_read(header_block, sb.journal_blockstart, 1) != 0) return false;
    memcpy(hdr, header_block, sizeof(*hdr));

    if (storage_read(entry_block, sb.journal_blockstart + 1, 1) != 0) return false;
    memcpy(entries, entry_block, sizeof(JournalEntry) * kMaxJournalEntries);
    return true;
}

static bool write_header(const JournalHeader& hdr, const JournalEntry* entries)
{
    alignas(BLOCK_SIZE) char header_block[BLOCK_SIZE];
    alignas(BLOCK_SIZE) char entry_block[BLOCK_SIZE];

    memset(header_block, 0, sizeof(header_block));
    memset(entry_block, 0, sizeof(entry_block));
    memcpy(header_block, &hdr, sizeof(hdr));
    memcpy(entry_block, entries, sizeof(JournalEntry) * hdr.entry_count);

    if (storage_write(entry_block, sb.journal_blockstart + 1, 1) != 0) return false;
    if (storage_write(header_block, sb.journal_blockstart, 1) != 0) return false;
    return true;
}

static bool replay_committed(const JournalHeader& hdr, const JournalEntry* entries)
{
    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];

    for (uint32_t i = 0; i < hdr.entry_count; ++i) 
    {
        const JournalEntry& e = entries[i];
        if (e.home_block >= sb.total_blocknum || (e.home_block >= sb.journal_blockstart && e.home_block < sb.journal_blockstart + sb.journal_blocknum)) 
        {
            log_err("[journal] invalid replay target block %lu", e.home_block);
            return false;
        }
        if (e.image_block < sb.journal_blockstart || e.image_block >= mount_state_lba()) 
        {
            log_err("[journal] invalid image block %lu", e.image_block);
            return false;
        }

        if (storage_read(block, e.image_block, 1) != 0) return false;
        if (fnv1a64(block, BLOCK_SIZE) != e.checksum) 
        {
            log_err("[journal] checksum mismatch for home block %lu", e.home_block);
            return false;
        }
        if (storage_write(block, e.home_block, 1) != 0) return false;
    }

    return write_zero_block(sb.journal_blockstart);
}

static inline void bitmap_set_local(unsigned long* bmap, uint32_t bit)
{
    bmap[bit / (sizeof(unsigned long) * 8)] |= 1UL << (bit % (sizeof(unsigned long) * 8));
}

static inline bool bitmap_test_local(const unsigned long* bmap, uint32_t bit)
{
    return (bmap[bit / (sizeof(unsigned long) * 8)] >> (bit % (sizeof(unsigned long) * 8))) & 1UL;
}

static inline void bitmap_clear_local(unsigned long* bmap, uint32_t bit)
{
    bmap[bit / (sizeof(unsigned long) * 8)] &= ~(1UL << (bit % (sizeof(unsigned long) * 8)));
}

static bool group_for_data_block(BlockID block, uint32_t* gid, uint32_t* bit)
{
    if (block < sb.group_blockstart || block >= sb.journal_blockstart) return false;
    uint64_t off = block - sb.group_blockstart;
    uint64_t group = off / TOTALBLOCKS_PERGROUP;
    uint64_t in_group = off % TOTALBLOCKS_PERGROUP;
    if (group >= sb.group_num || in_group < BMAPNUM_PERGROUP) return false;
    *gid = group;
    *bit = in_group - BMAPNUM_PERGROUP;
    return *bit < DATABLOCKS_PERGROUP;
}

static bool mark_extent_allocated(unsigned long* group_bitmaps, uint32_t* used_counts, const iExtent& ext)
{
    for (uint64_t i = 0; i < ext.block_count; ++i) 
    {
        uint32_t gid, bit;
        if (!group_for_data_block(ext.physical_start + i, &gid, &bit)) return false;
        unsigned long* bmap = group_bitmaps + (uint64_t)gid * (BLOCK_SIZE / sizeof(unsigned long));
        if (!bitmap_test_local(bmap, bit)) 
        {
            bitmap_set_local(bmap, bit);
            used_counts[gid]++;
        }
    }
    return true;
}

static bool inode_extent_valid(const DInode& din)
{
    if (din.valid_extent_count > DIRECT_EXTENT_NUM + EXTENTS_PER_BLOCK) return false;
    uint32_t direct_count = MIN(din.valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
    for (uint32_t i = 0; i < direct_count; ++i) 
    {
        const iExtent& ext = din.direct_extents[i];
        if (ext.block_count == 0) return false;
        uint32_t gid, bit;
        if (!group_for_data_block(ext.physical_start, &gid, &bit)) return false;
        if (!group_for_data_block(ext.physical_start + ext.block_count - 1, &gid, &bit)) return false;
    }
    return true;
}

static bool inode_is_live(const DInode* inodes, uint32_t inum)
{
    if (inum >= sb.inode_num) return false;
    const DInode& din = inodes[inum];
    if (!din.used) return false;
    if (din.type == UNKNOWN) return false;
    return inode_extent_valid(din);
}

static bool scrub_directory_block(BlockID block, const DInode* inodes)
{
    alignas(BLOCK_SIZE) char buf[BLOCK_SIZE];
    if (storage_read(buf, block, 1) != 0) return false;

    bool changed = false;
    Dirent* ents = reinterpret_cast<Dirent*>(buf);
    constexpr uint32_t ents_per_block = BLOCK_SIZE / sizeof(Dirent);
    for (uint32_t i = 0; i < ents_per_block; ++i) 
    {
        Dirent& d = ents[i];
        if (d.inum == 0 && d.name[0] == '\0') continue;
        if (strncmp(d.name, ".", NAMESIZ) == 0 || strncmp(d.name, "..", NAMESIZ) == 0) continue;
        if (!inode_is_live(inodes, d.inum) || inodes[d.inum].type != d.filetype) 
        {
            memset(&d, 0, sizeof(d));
            changed = true;
        }
    }

    return !changed || journal_commit_single(block, buf);
}

static bool repair_filesystem_state()
{
    DInode* inodes = static_cast<DInode*>(aligned_alloc(BLOCK_SIZE, sb.itable_blocknum * BLOCK_SIZE));
    if (inodes == nullptr) return false;
    if (storage_read(inodes, sb.itable_blockstart, sb.itable_blocknum) != 0) 
    {
        free(inodes);
        return false;
    }

    size_t imap_bytes = sb.imap_blocknum * BLOCK_SIZE;
    unsigned long* new_imap = static_cast<unsigned long*>(aligned_alloc(BLOCK_SIZE, imap_bytes));
    if (new_imap == nullptr) 
    {
        free(inodes);
        return false;
    }
    memset(new_imap, 0, imap_bytes);

    size_t gmap_longs_per_group = BLOCK_SIZE / sizeof(unsigned long);
    unsigned long* group_bitmaps = static_cast<unsigned long*>(aligned_alloc(BLOCK_SIZE, (uint64_t)sb.group_num * BLOCK_SIZE));
    uint32_t* used_counts = static_cast<uint32_t*>(calloc(sb.group_num, sizeof(uint32_t)));
    GroupDescriptor* gdt = static_cast<GroupDescriptor*>(aligned_alloc(BLOCK_SIZE, sb.gdt_blocknum * BLOCK_SIZE));
    if (group_bitmaps == nullptr || used_counts == nullptr || gdt == nullptr) 
    {
        free(gdt);
        free(used_counts);
        free(group_bitmaps);
        free(new_imap);
        free(inodes);
        return false;
    }
    memset(group_bitmaps, 0, (uint64_t)sb.group_num * BLOCK_SIZE);
    memset(gdt, 0, sb.gdt_blocknum * BLOCK_SIZE);

    for (uint32_t inum = 0; inum < sb.inode_num; ++inum) 
    {
        DInode& din = inodes[inum];
        if (!din.used) continue;
        if (din.type == UNKNOWN || !inode_extent_valid(din)) 
        {
            memset(&din, 0, sizeof(din));
            din.idx = inum;
            din.indirect_extent_block = sb.indirect_block_start + inum;
            continue;
        }

        bitmap_set_local(new_imap, inum);

        uint32_t direct_count = MIN(din.valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
        for (uint32_t i = 0; i < direct_count; ++i)
            mark_extent_allocated(group_bitmaps, used_counts, din.direct_extents[i]);

        if (din.valid_extent_count > DIRECT_EXTENT_NUM && din.indirect_extent_block != 0) 
        {
            iExtent* ind = static_cast<iExtent*>(aligned_alloc(BLOCK_SIZE, BLOCK_SIZE));
            if (ind && storage_read(ind, din.indirect_extent_block, 1) == 0) 
            {
                uint32_t indirect_count = din.valid_extent_count - direct_count;
                for (uint32_t i = 0; i < indirect_count && i < EXTENTS_PER_BLOCK; ++i)
                    mark_extent_allocated(group_bitmaps, used_counts, ind[i]);
            }
            free(ind);
        }
    }

    for (uint32_t inum = 0; inum < sb.inode_num; ++inum) 
    {
        const DInode& din = inodes[inum];
        if (!din.used || din.type != DIRECTORY) continue;

        uint32_t direct_count = MIN(din.valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
        for (uint32_t i = 0; i < direct_count; ++i)
            journal_register_metadata_extent(din.direct_extents[i].physical_start, din.direct_extents[i].block_count);

        for (uint32_t i = 0; i < direct_count; ++i) 
        {
            const iExtent& ext = din.direct_extents[i];
            for (uint64_t b = 0; b < ext.block_count; ++b)
                scrub_directory_block(ext.physical_start + b, inodes);
        }
    }

    if (!journal_write_metadata(new_imap, imap_bytes, sb.imap_blockstart, 0)) goto fail;

    for (uint32_t gid = 0; gid < sb.group_num; ++gid) 
    {
        BlockID bitmap_lba = sb.group_blockstart + (uint64_t)gid * TOTALBLOCKS_PERGROUP;
        unsigned long* bmap = group_bitmaps + (uint64_t)gid * gmap_longs_per_group;
        if (!journal_commit_single(bitmap_lba, bmap)) goto fail;

        gdt[gid].free_blocks_count = DATABLOCKS_PERGROUP - used_counts[gid];
        gdt[gid].next_free_hint = 0;
        while (gdt[gid].next_free_hint < DATABLOCKS_PERGROUP && bitmap_test_local(bmap, gdt[gid].next_free_hint)) gdt[gid].next_free_hint++;
        if (gdt[gid].next_free_hint >= DATABLOCKS_PERGROUP) gdt[gid].next_free_hint = 0;
    }
    if (!journal_write_metadata(gdt, sb.group_num * sizeof(GroupDescriptor), sb.gdt_blockstart, 0)) goto fail;
    if (!journal_write_metadata(inodes, sb.itable_blocknum * BLOCK_SIZE, sb.itable_blockstart, 0)) goto fail;

    free(gdt);
    free(used_counts);
    free(group_bitmaps);
    free(new_imap);
    free(inodes);
    return true;

fail:
    free(gdt);
    free(used_counts);
    free(group_bitmaps);
    free(new_imap);
    free(inodes);
    return false;
}

void journal_init()
{
    spin_lock_init(&journal_lock);
    spin_lock_init(&metadata_lock);
    journal_seq = 1;
    if (metadata_ranges == nullptr) 
    {
        metadata_ranges = new std::vector<BlockRange>();
        metadata_ranges->reserve(4096);
    }
}

bool journal_recover()
{
    if (!journal_layout_valid()) 
    {
        log_warn("[journal] invalid or missing journal area, skip recovery");
        return true;
    }

    JournalHeader hdr;
    JournalEntry entries[kMaxJournalEntries];
    if (!read_header(&hdr, entries)) return false;

    JournalHeader mount_hdr;
    if (!read_block_header(mount_state_lba(), &mount_hdr)) return false;
    bool needs_repair = mount_hdr.magic == kJournalMagic && mount_hdr.version == kJournalVersion && mount_hdr.state == kJournalDirty;

    if (hdr.magic != 0 && hdr.state != kJournalEmpty) 
    {
        if (hdr.magic != kJournalMagic || hdr.version != kJournalVersion) 
        {
            log_warn("[journal] unknown journal header magic=0x%x version=%u, clearing", hdr.magic, hdr.version);
            if (!write_zero_block(sb.journal_blockstart)) return false;
            needs_repair = true;
        } 
        else if (hdr.entry_count == 0 || hdr.entry_count > kMaxJournalEntries || hdr.entry_count + 2 >= sb.journal_blocknum) 
        {
            log_warn("[journal] invalid entry_count=%u, clearing header", hdr.entry_count);
            if (!write_zero_block(sb.journal_blockstart)) return false;
            needs_repair = true;
        } 
        else if (hdr.checksum != header_checksum(hdr, entries)) 
        {
            log_warn("[journal] incomplete or torn transaction, clearing header");
            if (!write_zero_block(sb.journal_blockstart)) return false;
            needs_repair = true;
        } 
        else if (hdr.state == kJournalCommitted) 
        {
            log_info("[journal] replaying %u metadata blocks", hdr.entry_count);
            if (!replay_committed(hdr, entries)) return false;
            if (hdr.seq >= journal_seq) journal_seq = hdr.seq + 1;
            needs_repair = true;
        } 
        else 
        {
            log_warn("[journal] uncommitted transaction state=%u, clearing", hdr.state);
            if (!write_zero_block(sb.journal_blockstart)) return false;
            needs_repair = true;
        }
    }

    if (needs_repair) 
    {
        log_info("[journal] previous mount was dirty, repairing metadata state");
        if (!repair_filesystem_state()) return false;
        return write_zero_block(mount_state_lba());
    }
    return true;
}

void journal_mark_dirty()
{
    if (!journal_layout_valid()) return;

    JournalHeader hdr;
    JournalEntry entries[kMaxJournalEntries];
    memset(entries, 0, sizeof(entries));
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kJournalMagic;
    hdr.version = kJournalVersion;
    hdr.state = kJournalDirty;
    hdr.entry_count = 0;
    hdr.seq = journal_seq++;
    hdr.checksum = header_checksum(hdr, entries);

    SpinGuard guard(&journal_lock);
    write_block_header(mount_state_lba(), hdr);
}

void journal_mark_clean()
{
    if (!journal_layout_valid()) return;

    SpinGuard guard(&journal_lock);
    write_zero_block(mount_state_lba());
}

bool journal_is_metadata_block(BlockID block)
{
    if (block >= sb.total_blocknum) return false;
    if (block < sb.group_blockstart) return true;
    if (block >= sb.journal_blockstart && block < sb.journal_blockstart + sb.journal_blocknum) return true;
    if (block >= sb.journal_blockstart) return false;

    uint64_t off = block - sb.group_blockstart;
    if ((off % TOTALBLOCKS_PERGROUP) < BMAPNUM_PERGROUP) return true;

    bool is_meta = false;
    SpinGuard guard(&metadata_lock);
    if (metadata_ranges) 
    {
        for (const BlockRange& r : *metadata_ranges) 
        {
            if (block >= r.start && block < r.start + r.count) 
            {
                is_meta = true;
                break;
            }
        }
    }
    return is_meta;
}

void journal_register_metadata_block(BlockID block)
{
    journal_register_metadata_extent(block, 1);
}

void journal_register_metadata_extent(BlockID start, uint64_t count)
{
    if (count == 0 || start == 0 || start >= sb.total_blocknum) return;
    if (start >= sb.journal_blockstart) return;

    SpinGuard guard(&metadata_lock);
    if (metadata_ranges == nullptr) return;

    for (BlockRange& r : *metadata_ranges) 
    {
        if (start >= r.start && start + count <= r.start + r.count) return;
        if (r.start + r.count == start) 
        {
            r.count += count;
            return;
        }
        if (start + count == r.start) 
        {
            r.start = start;
            r.count += count;
            return;
        }
    }
    metadata_ranges->push_back({start, count});
}

bool journal_commit_blocks(const BlockID* blocks, const void* const* images, uint32_t count)
{
    if (count == 0) return true;
    if (!journal_layout_valid()) return false;
    if (count > kMaxJournalEntries || count + 2 >= sb.journal_blocknum) 
    {
        log_err("[journal] transaction too large: %u blocks", count);
        return false;
    }

    SpinGuard guard(&journal_lock);

    JournalEntry entries[kMaxJournalEntries];
    memset(entries, 0, sizeof(entries));

    for (uint32_t i = 0; i < count; ++i) 
    {
        if (!journal_is_metadata_block(blocks[i])) 
        {
            log_err("[journal] refusing to journal non-metadata block %lu", blocks[i]);
            return false;
        }
        entries[i].home_block = blocks[i];
        entries[i].image_block = sb.journal_blockstart + 2 + i;
        entries[i].checksum = fnv1a64(images[i], BLOCK_SIZE);
        if (storage_write(images[i], entries[i].image_block, 1) != 0) return false;
    }

    JournalHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = kJournalMagic;
    hdr.version = kJournalVersion;
    hdr.state = kJournalCommitted;
    hdr.entry_count = count;
    hdr.seq = journal_seq++;
    hdr.checksum = header_checksum(hdr, entries);

    if (!write_header(hdr, entries)) return false;

    for (uint32_t i = 0; i < count; ++i)
        if (storage_write(images[i], blocks[i], 1) != 0) return false;

    return write_zero_block(sb.journal_blockstart);
}

bool journal_commit_single(BlockID block, const void* image)
{
    const BlockID blocks[1] = {block};
    const void* images[1] = {image};
    return journal_commit_blocks(blocks, images, 1);
}

bool journal_write_metadata(const void* data, size_t size, BlockID lba_start, off_t offset)
{
    if (size == 0) return true;

    const char* src = static_cast<const char*>(data);
    uint64_t start_block = lba_start + offset / BLOCK_SIZE;
    size_t inner = offset % BLOCK_SIZE;
    size_t remaining = size;

    alignas(BLOCK_SIZE) char block[BLOCK_SIZE];
    while (remaining > 0) 
    {
        size_t n = MIN((size_t)BLOCK_SIZE - inner, remaining);
        if (n == BLOCK_SIZE) 
        {
            if (!write_full_block(start_block, src)) return false;
        } 
        else 
        {
            if (!read_full_block(start_block, block)) return false;
            memcpy(block + inner, src, n);
            if (!write_full_block(start_block, block)) return false;
        }

        src += n;
        remaining -= n;
        start_block++;
        inner = 0;
    }

    return true;
}

void journal_build_metadata_map()
{
    if (metadata_ranges == nullptr) return;

    DInode* inode_block = static_cast<DInode*>(aligned_alloc(BLOCK_SIZE, BLOCK_SIZE));
    if (inode_block == nullptr) 
    {
        log_warn("[journal] failed to allocate inode scan buffer");
        return;
    }

    uint64_t scanned = 0;
    for (uint64_t b = 0; b < sb.itable_blocknum; ++b) 
    {
        if (storage_read(inode_block, sb.itable_blockstart + b, 1) != 0) break;

        for (uint32_t i = 0; i < INODENUM_PER_BLOCK && scanned < sb.inode_num; ++i, ++scanned) 
        {
            const DInode& din = inode_block[i];
            if (!din.used || din.type != DIRECTORY) continue;

            uint32_t direct_count = MIN(din.valid_extent_count, (uint32_t)DIRECT_EXTENT_NUM);
            for (uint32_t e = 0; e < direct_count; ++e)
                journal_register_metadata_extent(din.direct_extents[e].physical_start, din.direct_extents[e].block_count);

            if (din.valid_extent_count > DIRECT_EXTENT_NUM && din.indirect_extent_block != 0) 
            {
                iExtent* ind = static_cast<iExtent*>(aligned_alloc(BLOCK_SIZE, BLOCK_SIZE));
                if (ind == nullptr) continue;
                if (storage_read(ind, din.indirect_extent_block, 1) == 0) 
                {
                    uint32_t indirect_count = din.valid_extent_count - direct_count;
                    for (uint32_t e = 0; e < indirect_count && e < EXTENTS_PER_BLOCK; ++e)
                        journal_register_metadata_extent(ind[e].physical_start, ind[e].block_count);
                }
                free(ind);
            }
        }
    }

    free(inode_block);
}

#endif
