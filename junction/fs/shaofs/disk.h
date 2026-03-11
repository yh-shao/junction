#pragma once
#include "fs.h"
#include <cstdint>

DECLARE_BITMAP(imap, INODENUM);
extern int imap_size;

DECLARE_BITMAP(gmap, BLOCK_SIZE * 8);


extern "C" {
	#include "runtime/storage.h"
    // void readObj(void* obj, size_t siz, uint64_t lba_start, uint32_t lba_count);
    // void writeObj(void* obj, size_t siz, uint64_t lba_start, uint32_t lba_count);
}

void read_bm(unsigned long* bm, u_int64_t nbits, BlockID blockstart, u_int64_t blockcount);
void write_bm(unsigned long* bm, u_int64_t nbits, BlockID blockstart, u_int64_t blockcount);

void read_meta();

uint64_t extent_size(const iExtent* ext);
void read_extent(const iExtent *ext, uint64_t offset, char* buf, size_t size);
void write_extent(const iExtent *ext, uint64_t offset, const void *data, size_t size);

void test_write_disk();
void test_read_disk();