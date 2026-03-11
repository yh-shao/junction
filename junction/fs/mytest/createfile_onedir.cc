#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm> 
#include <numeric>   
#include <iomanip>   
#include <functional> 

const std::string TEST_DIR = "FSHAO:";

struct ThreadResult {
    std::vector<long long> latencies_ns;   // 存储该线程每一次操作的耗时（纳秒），用于后续统计 P99
};

void worker_task(int thread_id, int file_num, ThreadResult& result) 
{
    result.latencies_ns.reserve(file_num);  // 预分配内存，避免 vector 扩容本身造成的延迟干扰测试

    char filename[256];

    for (int i = 0; i < file_num; ++i) 
    {
        std::snprintf(filename, sizeof(filename), "%s/file_%d_%d", TEST_DIR.c_str(), thread_id, i);

        auto t1 = std::chrono::high_resolution_clock::now(); // --- 计时开始 ---
        int fd = open(filename, O_CREAT, 0644);
        auto t2 = std::chrono::high_resolution_clock::now(); // --- 计时结束 ---

        if (fd >= 0) 
        {
            close(fd);
            // 只有成功才记录延迟
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();  
            result.latencies_ns.push_back(dur);
        } 
        else 
        {
            std::cerr << "Thread " << thread_id << " failed to create file: " << filename << std::endl;
        }
    }
}

int main(int argc, char* argv[]) 
{
    if (argc != 3) 
    {
        std::cerr << "Usage: " << argv[0] << " <thread_count> <files_per_thread>" << std::endl;
        return 1;
    }

    int thread_count = std::atoi(argv[1]);
    int file_num_per_thread = std::atoi(argv[2]);
    long long total_expected = (long long)thread_count * file_num_per_thread;

    // mkdir(TEST_DIR.c_str(), 0755);  // 确保测试目录存在

    std::cout << "[Config] Threads: " << thread_count << ", Files/Thread: " << file_num_per_thread << std::endl;
    std::cout << "[Running] Collecting latency samples..." << std::endl;

    std::vector<std::thread> threads;
    std::vector<ThreadResult> results(thread_count);

    auto global_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < thread_count; ++i) 
        threads.emplace_back(worker_task, i, file_num_per_thread, std::ref(results[i]));

    for (auto& t : threads) 
        t.join();

    auto global_end = std::chrono::high_resolution_clock::now();
    double wall_time = std::chrono::duration<double>(global_end - global_start).count();

    // --- 数据汇总与分析 ---
    std::cout << "\n[Analyzing] Merging and sorting data for P99 calculation..." << std::endl;

    std::vector<long long> all_latencies;
    all_latencies.reserve(total_expected);

    // 将所有线程的数据合并到一个大 vector 中
    for (const auto& res : results) 
        all_latencies.insert(all_latencies.end(), res.latencies_ns.begin(), res.latencies_ns.end());
    if (all_latencies.empty()) 
    {
        std::cerr << "No files created?" << std::endl;
        return 1;
    }

    // 排序以计算百分位
    std::sort(all_latencies.begin(), all_latencies.end());

    long long total_ops = all_latencies.size();
    double iops = total_ops / wall_time;

    // 辅助 lambda：获取百分位数值 (单位转为微秒 us)
    auto get_percentile = [&](double p) -> double {
        size_t idx = (size_t)(p * total_ops);
        if (idx >= total_ops) idx = total_ops - 1;
        return all_latencies[idx] / 1000.0; // ns -> us
    };

    double avg_lat = (std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0) / total_ops) / 1000.0;

    // 输出报告
    std::cout << "\n=== Benchmark Results ===" << std::endl;
    std::cout << "Total Files   : " << total_ops << std::endl;
    std::cout << "Wall Time     : " << wall_time << " s" << std::endl;
    std::cout << "Throughput    : " << std::fixed << std::setprecision(2) << iops << " ops/s" << std::endl;
    std::cout << "-------------------------" << std::endl;
    std::cout << "Latency (microseconds):" << std::endl;
    std::cout << "  Min         : " << (all_latencies.front() / 1000.0) << " us" << std::endl;
    std::cout << "  Ave         : " << avg_lat << " us" << std::endl;
    std::cout << "  P50 (Median): " << get_percentile(0.50) << " us" << std::endl;
    std::cout << "  P90         : " << get_percentile(0.90) << " us" << std::endl;
    std::cout << "  P95         : " << get_percentile(0.95) << " us" << std::endl;
    std::cout << "  P99         : " << get_percentile(0.99) << " us  <-- Key Metric" << std::endl;
    std::cout << "  P99.9       : " << get_percentile(0.999) << " us" << std::endl;
    std::cout << "  Max         : " << (all_latencies.back() / 1000.0) << " us" << std::endl;
    std::cout << "=========================" << std::endl;

    for(auto v : all_latencies) 
        std::cout << v << ' '; // 记录每个点的纳秒数

    return 0;
}