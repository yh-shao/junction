#pragma once

/**
 * @brief Interface for the Storage Backend, e.g. Disk drive, File, DRAM, Database, ... 
 */
template <typename Key, typename Value>
class Backend {
public:
    virtual bool read(const Key& key, Value& value)        = 0;
    virtual bool write(const Key& key, const Value& value) = 0;
    virtual bool write_cleans_entry(const Key&) const { return true; }

    virtual ~Backend() = default;
};
