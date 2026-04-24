#include "namei.h"
#include "dir.h"
#include "inodeCache.h"
#include <cstring>
#include "dentryCache.h"

// 辅助函数：从路径字符串中跳过连续的 '/'，提取出下一个目录/文件名 (Token)；比如：path="///a//b/c"，调用后 name="a"，返回指向 "//b/c" 的指针
static const char* skipelem(const char* path, char* name)
{
    while (*path == '/') path++;        // 跳过开头的连续斜杠
    if (*path == '\0') return nullptr;  // 如果字符串结束了，说明没有下一个元素了

    const char* s = path;                          // 记录单词的起始位置
    while (*path != '/' && *path != '\0') path++;  // 寻找下一个斜杠或字符串末尾
    
    // 计算单词长度，并防止缓冲区溢出
    int len = path - s;
    if (len >= NAMESIZ) 
    {
        log_warn("[skipelem] File name too long!");
        len = NAMESIZ - 1;
    }
    memcpy(name, s, len);  // 拷贝单词到 name 缓冲区
    name[len] = '\0';
    
    while (*path == '/') path++;  // 再次跳过多余的斜杠，使得返回的指针干净利落
    
    return path;
}


static int namex(const char* path, bool nameiparent, char* name)  // nameiparent 参数决定是返回目标本身 (false)，还是返回其父目录 (true)
{
    if (path == nullptr || *path == '\0') return -1;  // 如果路径为空，直接报错

    int curr_inum;
    if (path[0] == '/')  // 绝对路径，从根目录开始
    {
        curr_inum = ROOT_INO; 
    }
    else  // 相对路径，从当前工作目录开始
    {
        log_err("[namei] Relative paths are not supported yet.");
        return -1;
    }

    char name_buf[NAMESIZ];   // 临时存放中间解析出来的每一级名字
    if (name == nullptr) name = name_buf;

    while ((path = skipelem(path, name)) != nullptr) 
    {
        if (nameiparent && *path == '\0') return curr_inum;   // 如果是要寻找父目录，并且当前剥出来的已经是最后一级了（path 走到头了），那就直接停下，此时 curr_inum 正好是父目录的 Inode，而 name 里正好是最后一级的文件名

        // 在当前目录 curr_inum 中，查找名为 name 的下一级

        auto handle = get_dentry_cache().getHandle(DentryKey(curr_inum, name));   // 先查 dentryCache
        if (!handle) 
        {
            log_err("[namei] Path component '%s' not found.", name);
            return -1; // 某一层断了，直接宣告解析失败
        }

        int next_inum;
        file_type_t type;

        {
            auto read_acc = handle.read_access();
            next_inum = read_acc->inum;
            type = read_acc->type;
        }
        if (next_inum == -1) 
        {
            log_err("[namei] Path component '%s' not found.", name);
            return -1;
        }

        // 安全检查：如果路径还没解析完，但当前节点已经不是目录了，则是非法的；比如想解析 "/a/b/c"，但 "b" 只是个普通文本文件，不能再往下钻了
        if (*path != '\0' && type != DIRECTORY) 
        {
            log_err("[namei] Path component '%s' is not a directory.", name);
            return -1;
        }

        // 准备进入下一级
        curr_inum = next_inum;
    }

    if (nameiparent) return -1;  // 如果我们要找父目录，但路径只是个 "/"，skipelem 会直接返回 nullptr 进不去循环。根目录没有（常规意义上的路径字符串体现的）父目录，此时应当报错。

    return curr_inum;
}

int namei(const char* path)
{
    char name[NAMESIZ];
    return namex(path, false, name);
}
int nameiparent(const char* path, char* name)
{
    return namex(path, true, name);
}