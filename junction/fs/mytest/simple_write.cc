#include <iostream>
#include <fcntl.h>    
#include <unistd.h>   
#include <cstring>    
using namespace std;

int main() 
{
    const char* filename = "FSHAO:/test_write.dat";
    const size_t block_size = 4096; // 经典的 4KB 块大小
    char buffer[block_size];
    memset(buffer, 'M', block_size);

    int fd = open(filename, O_CREAT | O_WRONLY, 0666);
    if (fd < 0) 
    {
        cerr << "错误：文件打开失败！" << endl;
        return 1;
    }

    ssize_t bytes_written = write(fd, buffer, block_size);
    if (bytes_written == block_size) 
    {
        cout << "写入成功！" << bytes_written << " 字节已顺利送达 BlockCache" << endl;
    } 
    else 
    {
        cerr << "错误：写入异常，实际写入 " << bytes_written << " 字节。" << endl;
    }

    close(fd);
    return 0;
}