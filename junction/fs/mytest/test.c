#include <fcntl.h>

int main()
{
    int fd = open("FSHAO:/file", O_RDONLY);
    
    return 0;
}