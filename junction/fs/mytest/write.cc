#ifndef _GNU_SOURCE
#define _GNU_SOURCE // 启用 O_DIRECT 宏
#endif

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <cstdlib>
using namespace std;

void measure_single_write(const string& filename, bool use_direct_io)   // use_direct_io 是否开启物理直写 (绕过缓存并强制落盘)
{
    size_t block_size = 4096;
    void* buffer = nullptr;
    if (posix_memalign(&buffer, 4096, block_size) != 0)   // 为了兼容 O_DIRECT，必须进行 4K 内存对齐
    {
        cerr << "内存对齐失败！" << endl;
        return;
    }
    memset(buffer, 'W', block_size); // 填入测试数据

    // 配置文件打开标志
    int flags = O_CREAT | O_WRONLY;
    if (use_direct_io) flags |= O_DIRECT | O_SYNC; 

    int fd = open(filename.c_str(), flags, 0666);
    if (fd < 0) 
    {
        cerr << "文件打开失败！若是 O_DIRECT 报错，说明该文件系统不支持直写(例如 tmpfs)。" << endl;
        free(buffer);
        return;
    }

    auto start_time = chrono::high_resolution_clock::now();
    ssize_t bytes_written = write(fd, buffer, block_size);
    auto end_time = chrono::high_resolution_clock::now();

    if (bytes_written != block_size) 
    {
        cerr << "写入异常，预期写入 " << block_size << " 字节，实际写入 " << bytes_written << " 字节" << endl;
    } 
    else 
    {
        auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
        double duration_us = duration_ns / 1000.0;
        cout << (use_direct_io ? "[物理直写 O_DIRECT+O_SYNC]" : "[缓存写入 Buffered I/O]  ") << " -> ";
        cout << "耗时: " << duration_us << " 微秒 (" << duration_ns << " 纳秒)" << endl;
    }

    close(fd);
    free(buffer);
    // unlink(filename.c_str()); // 删除生成的临时文件
}

int main() 
{
    measure_single_write("FSHAO:/test_buffered.dat", false);   // 测试一：普通操作系统的默认写入（数据其实只到了内存）
    // measure_single_write("FSHAO:/test_direct.dat", true);      // 测试二：极其苛刻的底层物理直写（逼问硬件的真实反应速度）
    return 0;
}