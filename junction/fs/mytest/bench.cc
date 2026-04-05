#ifndef _GNU_SOURCE
#define _GNU_SOURCE // 必须定义此宏，才能使用 O_DIRECT
#endif

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <cstdlib>

using namespace std;

// 全局原子变量，用于多线程汇总统计数据
atomic<long long> total_bytes_written(0);
atomic<long long> total_io_ops(0);

/**
 * 核心工作线程函数：负责向独立的文件中疯狂写入数据
 * @param thread_id 线程编号
 * @param file_path 测试文件路径
 * @param block_size 每次写入的数据块大小 (例如 4096)
 * @param io_count 每个线程执行的 IO 次数
 */
void io_worker(int thread_id, const string& file_path, size_t block_size, int io_count) 
{
    // 内存对齐：O_DIRECT 强制要求内存地址必须是块大小(通常512或4096)的整数倍
    void* buffer = nullptr;
    if (posix_memalign(&buffer, 4096, block_size) != 0) {
        cerr << "线程 " << thread_id << " 内存对齐分配失败！" << endl;
        return;
    }
    
    memset(buffer, 'A' + (thread_id % 26), block_size);  // 用垃圾数据填充缓冲区

    int fd = open(file_path.c_str(), O_CREAT | O_WRONLY, 0666);
    if (fd < 0) 
    {
        cerr << "线程 " << thread_id << " 打开文件失败" << endl;
        free(buffer);
        return;
    }

    long long local_bytes = 0;
    long long local_ops = 0;

    for (int i = 0; i < io_count; ++i) 
    {
        ssize_t bytes_written = write(fd, buffer, block_size);
        
        if (bytes_written > 0) 
        {
            local_bytes += bytes_written;
            local_ops++;
        } 
        else 
        {
            cerr << "线程 " << thread_id << " 写入失败！" << endl;
            break;
        }
    }

    close(fd);
    free(buffer);

    // 将局部统计数据安全地累加到全局原子变量中
    total_bytes_written += local_bytes;
    total_io_ops += local_ops;
}

int main(int argc, char* argv[]) 
{
    // 参数配置
    size_t block_size = 4096;
    int num_threads = 1;             // 线程数        
    int io_count_per_thread = 10000; // 每个线程处理 10000 次请求

    cout << "=========================================" << endl;
    cout << "当前配置 -> 线程数: " << num_threads 
         << " | 块大小: " << block_size << "B" 
         << " | 每线程 IO 次数: " << io_count_per_thread << endl;

    vector<thread> workers;
    auto start_time = chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)   // 启动多线程并发测试
    {
        string file_name = "FSHAO:/test_data_thread_" + to_string(i) + ".dat";   // 每个线程写一个独立的文件，避免文件系统层面的 inode 锁竞争干扰纯粹的 I/O 测试
        workers.emplace_back(io_worker, i, file_name, block_size, io_count_per_thread);
    }

    for (auto& t : workers) t.join();

    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double> elapsed = end_time - start_time;
    double seconds = elapsed.count();

    // 4. 计算并输出核心指标
    double iops = total_io_ops.load() / seconds;
    double bandwidth_mb = (total_bytes_written.load() / (1024.0 * 1024.0)) / seconds;

    cout << "\n================ 测试结果 ================" << endl;
    cout << "总耗时: \t" << seconds << " 秒" << endl;
    cout << "总写入量: \t" << total_bytes_written.load() / (1024.0 * 1024.0) << " MB" << endl;
    cout << ">> 极限 IOPS: \t" << iops << " 次/秒" << endl;
    cout << ">> 吞吐带宽: \t" << bandwidth_mb << " MB/s" << endl;
    cout << "=========================================" << endl;

    // 清理测试生成的临时大文件 (可选)
    // for (int i = 0; i < num_threads; ++i) {
    //     string file_name = "./test_data_thread_" + to_string(i) + ".dat";
    //     unlink(file_name.c_str()); 
    // }

    return 0;
}