#include "fs.h"
#include "backend.h"
// #include "blockCache.h"
extern "C" {
#include "runtime/storage.h"
}

bool NVMeSSD::read(const BlockID& key, BlockData& value)
{
    int ret = storage_read(static_cast<void*>(value.data), key, 1);
    return (ret == 0);
}
bool NVMeSSD::write(const BlockID& key, BlockData const& value)
{
    int ret = storage_write(static_cast<const void*>(value.data), key, 1);
    return (ret == 0);
}

// bool InodeBackend::read(const int& inum, DInode& value) 
// {
//     BlockID blockidx = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
//     int offset = inum % INODENUM_PER_BLOCK;

//     BlockHandle handle = bc_get_handle(blockidx);
//     if (!handle) return false;

//     auto acc = handle.access();
//     DInode* inode_tbl = reinterpret_cast<DInode*>(acc->data);
//     value = inode_tbl[offset];
//     return true;
// }
// bool InodeBackend::write(const int& inum, const DInode& value) 
// {
//     BlockID blockidx = sb.itable_blockstart + inum / INODENUM_PER_BLOCK;
//     int offset = inum % INODENUM_PER_BLOCK;

//     BlockHandle handle = bc_get_handle(blockidx);
//     if (!handle) return false;

//     auto acc = handle.access();
//     DInode* inode_tbl = reinterpret_cast<DInode*>(acc->data);
//     inode_tbl[offset] = value;
//     acc.mark_dirty();  
    
//     return true;
// }