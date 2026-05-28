#pragma once

#include "fs.h"

extern "C" {
#include "runtime/storage.h"
}

#if CRASH_CONSISTENCY

void journal_init();
bool journal_recover();
void journal_mark_dirty();
void journal_mark_clean();
void journal_build_metadata_map();
bool journal_commit_blocks(const BlockID* blocks, const void* const* images, uint32_t count);
bool journal_commit_single(BlockID block, const void* image);
bool journal_commit_single_batched(BlockID block, const void* image);
bool journal_write_metadata(const void* data, size_t size, BlockID lba_start, off_t offset);
bool journal_is_metadata_block(BlockID block);
bool journal_commit_returns_after_checkpoint(BlockID block);
void journal_register_metadata_block(BlockID block);
void journal_register_metadata_extent(BlockID start, uint64_t count);

#else

static inline void journal_init() {}
static inline bool journal_recover() { return true; }
static inline void journal_mark_dirty() {}
static inline void journal_mark_clean() {}
static inline void journal_build_metadata_map() {}
static inline bool journal_commit_blocks(const BlockID*, const void* const*, uint32_t) { return true; }
static inline bool journal_commit_single(BlockID, const void*) { return true; }
static inline bool journal_commit_single_batched(BlockID, const void*) { return true; }
static inline bool journal_write_metadata(const void* data, size_t size, BlockID lba_start, off_t offset) { return storage_write_obj(data, size, lba_start, offset) == 0; }
static inline bool journal_is_metadata_block(BlockID) { return false; }
static inline bool journal_commit_returns_after_checkpoint(BlockID) { return true; }
static inline void journal_register_metadata_block(BlockID) {}
static inline void journal_register_metadata_extent(BlockID, uint64_t) {}

#endif
