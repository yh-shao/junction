#include "dentry.h"
#include "inode.h"
#include "inodeCache.h"
#include "file.h"
#include "dentryCache.h"
#include <vector>
#include <cstring>
#include <string>

void split_path(const char *pathname, std::vector<std::string>& parts)
{
    if (!pathname) return;

    const char* start = pathname;
    const char* end   = pathname;
    
    while (1)
    {
        while (*start == '/') start++;
        if (*start == '\0') break;
        end = start;
        while (*end && *end != '/') end++;
        parts.emplace_back(start, end - start);
        if (*end == '\0') break;
        start = end;
    }
}

// int dir_lookup_locked(MInode* dir_inode, const char *name)    // 在 dir_inode 中查找 name，若存在则返回对应的 inum，若不存在则返回 -1
// {
//     if (dir_inode->disk_inode.type != DIRECTORY) return -1;

//     int found_inum = -1;
//     foreach_file_block(dir_inode, [&](char* data, size_t valid_len)->bool {
//         Dirent* entries = reinterpret_cast<Dirent*>(data);
//         int entry_count = valid_len / sizeof(Dirent);
//         for (int i = 0; i < entry_count; i++) 
//         {
//             if (entries[i].inum != 0 && strcmp(entries[i].name, name) == 0) 
//             {
//                 found_inum = entries[i].inum;
//                 return false;
//             }
//         }
//         return true;  // 返回 true 以继续遍历下一个块
//     });

//     return found_inum;
// }

int dir_lookup_locked(MInode* dir_inode, const char *name)    // 在 dir_inode 中查找 name，若存在则返回对应的 inum，若不存在则返回 -1
{
    // log_info("[dir_lookup_locked(%d, %s)]: lookup \"%s\" in dirinode %d", dir_inode->inum, name, name, dir_inode->inum);
    // log_info("current fsbase: 0x%lx, runtime fsbase: 0x%lx", _readfsbase_u64(), perthread_read(runtime_fsbase));

    if (dir_inode->disk_inode.type != DIRECTORY)
    {
        log_info("[dir_lookup(%d, %s)] Error: not a DIRECTORY", dir_inode->inum, name);
        return -1;
    }

    // thread_t *th = thread_self();
    // uint64_t before_dirlookup = thread_get_total_cycles(th) / cycles_per_us;

    // char* raw_buffer = new char[dir_inode->disk_inode.file_size];
    // Dirent* entries = reinterpret_cast<Dirent*>(raw_buffer);
    // if (!entries)
    // {
    //     log_info("[dir_lookup(%d, %s)->new(%lu)] ERROR: fail to new", dir_inode->inum, name, dir_inode->disk_inode.file_size);
    //     return -1;
    // }

    // read_full_file(dir_inode, entries);      // 好像多了一步从 cache 复制到 buffer 的操作

    // int entry_count = dir_inode->disk_inode.file_size / sizeof(Dirent), res = -1;
    // for (int i = 0; i < entry_count; i++)
    //     if (strcmp(entries[i].name, name) == 0)
    //     {
    //         res = entries[i].inum;
    //         break;
    //     }

    // delete[] raw_buffer;

    // // uint64_t after_dirlookup = thread_get_total_cycles(th) / cycles_per_us;
    // // log_info("dirlookup actual time: %lu us", after_dirlookup - before_dirlookup);
    
    // return res;

    int found_inum = -1;

    // Search block-by-block instead of reading full file
    foreach_file_block(dir_inode, [&](char* data, size_t valid_len) -> bool {
        size_t count = valid_len / sizeof(Dirent);
        Dirent* entries = (Dirent*)data;
        for (size_t i = 0; i < count; i++) 
            if (strcmp(entries[i].name, name) == 0) 
            {
                found_inum = entries[i].inum;
                return false; // Stop iteration
            }

        return true; // Continue
    });
    
    return found_inum;
}
int dir_lookup(MInode* dir_inode, const char *name)
{
    SpinGuard g(&dir_inode->lock);
    return dir_lookup_locked(dir_inode, name);
}

void add_dentry_locked(MInode* dir_inode, const char* name, int inum, file_type_t filetype)  // inum 的 hardlink 应当另行 +1
{
    Dirent new_ent = {.inum=inum, .filetype=filetype};
    strncpy(new_ent.name, name, NAMESIZ - 1);
    new_ent.name[NAMESIZ - 1] = '\0';
    append_content(dir_inode, &new_ent, sizeof(Dirent));
    dir_inode->dirty = true;
}
void add_dentry(MInode* dir_inode, const char* name, int inum, file_type_t filetype)   
{
    SpinGuard g(&dir_inode->lock);
    add_dentry_locked(dir_inode, name, inum, filetype);
}

void lookup(const char *pathname, IEntry& res)
{    
    auto& dentrycache = DentryCacheManager::instance();

    std::vector<std::string> parts;
    split_path(pathname, parts);

    int prefix_hit_index;  // 表示命中到哪一层（parts.size() 表示完整路径，0 表示 "/"）
    int base_inode;
    for (int i = parts.size(); i >= 0; --i)    // 从完整路径开始递减搜索
    {
        std::string probe_path = join_path(parts, i);
        if (dentrycache.get(probe_path.c_str(), base_inode))  // cache hit
        {
            prefix_hit_index = i;
            break;
        }
    } // 至少会命中到 “/”，此时 base_inode=0，prefix_hit_index=0

    // 基于 base_inode 搜索完整路径对应的 Inode
    MInode* current_inode = get_inode(base_inode), *last_inode = nullptr;

    // log_info("[lookup(%s)] start examining from inode %d", pathname, current_inode->inum);

    for (int i = prefix_hit_index; i < parts.size(); i++)    // 在 dentries 中搜索 parts[i]
    {
        // log_info("[lookup(%s)] looking for part '%s'", pathname, parts[i].c_str());
        // log_info("current fsbase: 0x%lx, runtime fsbase: 0x%lx", _readfsbase_u64(), perthread_read(runtime_fsbase));

        int target_inum = dir_lookup(current_inode, parts[i].c_str());
        if (target_inum == -1)  // 该目录下不存在 parts[i]
        {
            // log_info("not found in dir %d", current_inode->inum);

            if (i == parts.size() - 1)  // 仅是最后一个token不匹配（可能是新建文件）
            {
                release_inode(last_inode);
                res.code = 1;
                res.parent_ino = current_inode;
                res.ino = nullptr;
                strncpy(res.last_name, parts[i].c_str(), MAX_PATH_LEN);
            }
            else                        // 不合法路径
            {
                release_inode(last_inode);
                release_inode(current_inode);
                res.code = -1;
                res.parent_ino = nullptr;
                res.ino = nullptr; 
            }
            return;
        }

        // 该目录下存在 parts[i]，向下一级
        release_inode(last_inode);
        last_inode = current_inode;
        // uint64_t before_get_inode = rdtsc();
        current_inode = get_inode(target_inum);

        // uint64_t after_get_inode = rdtsc();
        // log_info("[get_inode] duration: %lu us", (after_get_inode - before_get_inode) / cycles_per_us);

        std::string full_path = join_path(parts, i + 1);  // 构造完整路径
        dentrycache.put(full_path.c_str(), target_inum);  // 存入 cache 中
    }
    // uint64_t after_examine = rdtsc();
    // log_info("[examine] duration: %lu us", (after_examine - before_examine) / cycles_per_us);

    // 完全匹配
    res.code = 0;
    res.parent_ino = last_inode;
    res.ino = current_inode;
    strncpy(res.last_name, parts.back().c_str(), MAX_PATH_LEN);
}

void delete_dentry(MInode*& dir_inode, char* name)
{
    SpinGuard g(&dir_inode->lock);

    // log_info("delete_dentry(%d, %s) START", dir_inode->inum, name);

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        log_info("[ERROR] Cannot delete special entries '.' or '..'");
        return;
    }

    if (dir_inode->disk_inode.type != DIRECTORY) 
    {
        log_info("[ERROR] delete_dentry(%d, %s): inode[%d] is not a dir", dir_inode->inum, name, dir_inode->inum);
        return;
    }

    /*
    uint64_t filesize = dir_inode->disk_inode.file_size;

    // Dirent* entries = (Dirent*)smalloc(filesize);
    char* raw_buffer = new char[filesize];
    Dirent* entries = reinterpret_cast<Dirent*>(raw_buffer);
    read_full_file(dir_inode, entries);
    int entry_count = filesize / sizeof(Dirent);
    bool found = false;
    int idx;
    for (idx = 0; idx < entry_count; idx++) 
        if (strcmp(entries[idx].name, name) == 0) 
        {
            found = true;
            break;
        }
    
    if (found == false)
    {
        log_info("dir [%d] doesn't include name \"%s\"", dir_inode->inum, name);
    }
    else
    {
        memmove(entries + idx, entries + idx + 1, (entry_count - idx - 1) * sizeof(Dirent));
        entry_count--;
        uint64_t newsize = entry_count * sizeof(Dirent);
        write_file(dir_inode, 0, (char*)entries, newsize);  // 从文件偏移 0 处开始写数据（可能会覆盖）
        dir_inode->dirty = true;
        dir_inode->disk_inode.file_size = newsize;
    }

    // sfree(entries);
    delete[] raw_buffer;

    // log_info("delete_dentry(%d, %s) OVER", dir_inode->inum, name);
    */


    // Optimized delete: Find entry, Swap with Last, Truncate
    uint64_t found_offset = 0;
    uint64_t scan_offset = 0;
    bool found = false;

    foreach_file_block(dir_inode, [&](char* data, size_t valid_len) -> bool {
        size_t count = valid_len / sizeof(Dirent);
        Dirent* entries = (Dirent*)data;
        for (size_t i = 0; i < count; i++) 
            if (strcmp(entries[i].name, name) == 0) 
            {
                found_offset = scan_offset + i * sizeof(Dirent);
                found = true;
                return false;
            }

        scan_offset += BLOCK_SIZE;
        return true;
    });
    
    if (!found)
    {
        log_info("dir [%d] doesn't include name \"%s\"", dir_inode->inum, name);
        return;
    }

    uint64_t file_size = dir_inode->disk_inode.file_size;
    uint64_t last_offset = file_size - sizeof(Dirent);

    if (found_offset != last_offset)
    {
        // Swap last entry to found position
        Dirent last_entry;
        read_file(dir_inode, last_offset, &last_entry, sizeof(Dirent));
        write_file(dir_inode, found_offset, (char*)&last_entry, sizeof(Dirent));
    }

    // Truncate the file to remove the last entry
    truncate_inode_data_locked(dir_inode, last_offset);
    dir_inode->dirty = true;
}

// Helper to check if directory is empty (except . and ..)
bool is_dir_empty(MInode* dir_inode) 
{
    if (dir_inode->disk_inode.file_size == 0) return true;
    
    char* raw_buffer = new char[dir_inode->disk_inode.file_size];
    Dirent* entries = reinterpret_cast<Dirent*>(raw_buffer);
    read_full_file(dir_inode, entries);
    
    int entry_count = dir_inode->disk_inode.file_size / sizeof(Dirent);
    bool empty = true;
    for (int i = 0; i < entry_count; i++) 
        if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0) 
        {
            empty = false;
            break;
        }

    delete[] raw_buffer;
    return empty;
}