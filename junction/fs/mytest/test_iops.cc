#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

// 测试参数配置
const int THREAD_COUNT = 8;             // 测试线程数
const int BLOCK_SIZE = 4096;            // 每次写入 4KB (必须是扇区大小的倍数以支持 O_DIRECT)
const int TEST_DURATION_SEC = 10;       // 测试持续时间
const size_t FILE_SIZE = 1ULL * 1024 * 1024 * 1024; // 目标测试文件大小 (1GB)

std::atomic<bool> keep_running(true);
std::atomic<uint64_t> total_io_operations(0);

void iops_worker(int fd) {
    // O_DIRECT 要求内存地址必须是按块对齐的！
    // 不能直接使用 new 或 malloc，必须使用 posix_memalign 或 aligned_alloc
    void* buffer = nullptr;
    if (posix_memalign(&buffer, BLOCK_SIZE, BLOCK_SIZE) != 0) {
        perror("Memory alignment failed");
        return;
    }
    memset(buffer, 'A', BLOCK_SIZE); // 填充一些测试数据

    // 设置随机数生成器，用于生成随机的 offset
    std::mt19937_64 rng(std::random_device{}());
    // 保证随机 offset 是 BLOCK_SIZE 的整数倍，且不超过文件大小
    size_t max_blocks = FILE_SIZE / BLOCK_SIZE;
    std::uniform_int_distribution<size_t> dist(0, max_blocks - 1);

    uint64_t local_ops = 0;

    // 高频压测循环
    while (keep_running.load(std::memory_order_relaxed)) {
        size_t random_offset = dist(rng) * BLOCK_SIZE;

        // 核心：使用 pwrite 进行随机位置写入，不影响其他线程
        ssize_t ret = pwrite(fd, buffer, BLOCK_SIZE, random_offset);
        
        if (ret == BLOCK_SIZE) {
            local_ops++;
        } else if (ret < 0) {
            // 生产环境中应有更完善的错误处理
            perror("pwrite error");
            break;
        }
    }

    total_io_operations.fetch_add(local_ops, std::memory_order_relaxed);
    free(buffer);
}

int main() 
{
    const char* test_file = "FSHAO:/test_iops_file.dat";

    // 准备测试文件 (如果不存在则创建，预分配空间)
    std::cout << "Preparing test file (1GB)..." << std::endl;
    int fd = open(test_file, O_CREAT | O_RDWR, 0666);
    if (fd < 0) 
    {
        perror("Failed to open file. Is your filesystem supporting O_DIRECT?");
        return 1;
    }
    // 预先分配磁盘空间（避免压测时触发文件系统元数据分配的开销）
    fallocate(fd, 0, 0, FILE_SIZE); 

    // 启动压测线程
    std::cout << "Starting IOPS test with " << THREAD_COUNT << " threads..." << std::endl;
    std::vector<std::thread> threads;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < THREAD_COUNT; ++i) 
        threads.emplace_back(iops_worker, fd);

    std::this_thread::sleep_for(std::chrono::seconds(TEST_DURATION_SEC));
    keep_running.store(false);

    for (auto& t : threads) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    // 计算并输出 IOPS
    uint64_t ops = total_io_operations.load();
    double iops = ops / elapsed.count();

    std::cout << "-----------------------------------" << std::endl;
    std::cout << "Test completed in " << elapsed.count() << " seconds." << std::endl;
    std::cout << "Total writes (4KB): " << ops << std::endl;
    std::cout << "IOPS: " << iops << std::endl;
    std::cout << "Throughput: " << (iops * 4096) / (1024 * 1024) << " MB/s" << std::endl;
    std::cout << "-----------------------------------" << std::endl;

    close(fd);
    unlink(test_file); // 清理测试文件
    return 0;
}