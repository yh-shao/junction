#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <numeric>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <iomanip>
#include <random>
#include <unistd.h>

class Timer {
public:
    void start() 
    { 
        start_time = std::chrono::high_resolution_clock::now(); 
    }
    double stop() 
    { 
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> ms = end_time - start_time;
        return ms.count();
    }
private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
};

void print_stats(const std::string& name, double total_ms, int ops) 
{
    double avg_latency = total_ms / ops;
    double iops = (ops / total_ms) * 1000.0;
    std::cout << std::left << std::setw(30) << name 
              << " | Time: " << std::setw(8) << total_ms << " ms"
              << " | Avg Latency: " << std::setw(8) << avg_latency << " ms"
              << " | IOPS: " << iops << std::endl;
}

void test_latency_sequential_write() 
{
    std::cout << "\n--- [Single-Threaded] Sequential Write/Read Latency ---" << std::endl;
    std::string filename = "FSHAO:/latency_test_file";
    int fd = open(filename.c_str(), O_CREAT, 0644);
    if (fd < 0) { std::cerr << "Open failed" << std::endl; return; }

    const int num_blocks = 1000; // Write 1000 * 4KB = 4MB
    char buffer[4096];
    memset(buffer, 'A', 4096);

    Timer t;
    t.start();
    for (int i = 0; i < num_blocks; ++i) write(fd, buffer, 4096);

    double write_ms = t.stop();
    print_stats("Seq Write (4KB blocks)", write_ms, num_blocks);
    std::cout << "Write Throughput: " << (num_blocks * 4.0 / 1024.0) / (write_ms / 1000.0) << " MB/s" << std::endl;

    close(fd);

    // Read back
    fd = open(filename.c_str(), 0, 0);
    t.start();
    for (int i = 0; i < num_blocks; ++i) read(fd, buffer, 4096);

    double read_ms = t.stop();
    print_stats("Seq Read (4KB blocks)", read_ms, num_blocks);
    std::cout << "Read Throughput: " << (num_blocks * 4.0 / 1024.0) / (read_ms / 1000.0) << " MB/s" << std::endl;

    close(fd);
}

void test_metadata_latency() 
{
    std::cout << "\n--- [Single-Threaded] Metadata Latency (Create/Close) ---" << std::endl;
    const int num_files = 100;
    Timer t;

    t.start();
    for (int i = 0; i < num_files; ++i) 
    {
        std::string name = "FSHAO:/file_" + std::to_string(i);
        int fd = open(name.c_str(), O_CREAT, 0644);
        close(fd);
    }
    double total_ms = t.stop();
    print_stats("Create+Close Empty File", total_ms, num_files);
}

void workload_thread(int id, int ops_per_thread) 
{
    char buffer[4096];
    memset(buffer, 'T', 4096);
    
    // Each thread writes to its own file to reduce inode contention, focusing on FS locking mechanisms (block allocation, cache, global structures).
    std::string filename = "FSHAO:/thread_file_" + std::to_string(id);

    int fd = open(filename.c_str(), O_CREAT, 0644);
    if (fd < 0) return;

    for (int i = 0; i < ops_per_thread; ++i) write(fd, buffer, 4096);

    close(fd);
}

void test_multithread_throughput(int num_threads) 
{
    std::cout << "\n--- [Multi-Threaded] Write Throughput (" << num_threads << " threads) ---" << std::endl;
    
    const int ops_per_thread = 200; // 200 blocks * 4KB = 800KB per thread
    std::vector<std::thread> threads;
    
    Timer t;
    t.start();
    
    for (int i = 0; i < num_threads; ++i) 
        threads.emplace_back(workload_thread, i + 100, ops_per_thread); // ID offset to avoid name collision with previous tests
    
    for (auto& th : threads) th.join();
    
    double total_ms = t.stop();
    int total_ops = num_threads * ops_per_thread;
    double total_mb = total_ops * 4.0 / 1024.0;
    
    print_stats("Concurrent Write (4KB)", total_ms, total_ops);
    std::cout << "Aggregate Throughput: " << total_mb / (total_ms / 1000.0) << " MB/s" << std::endl;
}

int main() 
{   
    test_metadata_latency();
    test_latency_sequential_write();
    
    // Multi-threading tests    
    test_multithread_throughput(1);
    test_multithread_throughput(2);
    test_multithread_throughput(4);
    test_multithread_throughput(8);
        
    return 0;
}
