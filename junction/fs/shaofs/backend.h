#pragma once
#include "fs.h"

/**
 * @brief Interface for the Storage Backend, e.g. Disk drive, File, DRAM, Database, ... 
 */
template <typename Key, typename Value>
class Backend {
public:
    virtual bool read(const Key& key, Value& value)        = 0;
    virtual bool write(const Key& key, const Value& value) = 0;

    virtual ~Backend() = default;
};


class NVMeSSD : public Backend<BlockID, BlockData> {
public:
    bool read(const BlockID& key, BlockData& value) override;
    bool write(const BlockID& key, BlockData const& value) override;

    static NVMeSSD& getInstance()   // singleton，全局只有 1 个 NVMeSSD 实例
    {
        static NVMeSSD instance;
        return instance;
    }
    NVMeSSD(const NVMeSSD&) = delete;
    void operator=(const NVMeSSD&) = delete;
private:
    NVMeSSD() = default;
};

// class InodeBackend : public Backend<int, DInode> {
// public:
//     bool read(const int& inum, DInode& value) override;
//     bool write(const int& inum, const DInode& value) override;

//     static InodeBackend& getInstance() 
//     {
//         static InodeBackend instance;
//         return instance;
//     }

//     InodeBackend(const InodeBackend&) = delete;
//     void operator=(const InodeBackend&) = delete;
// private:
//     InodeBackend() = default;
// };