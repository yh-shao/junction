#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

int main()
{
    int fd = open("FSHAO:/file", O_RDWR | O_CREAT, 0644), ret;
    printf("[OPEN] ret val = %d\n", fd);

    size_t n = 2000;
    char *buf = (char*)malloc(n);
    buf[0] = 0;  // touch 一下

    ret = read(fd, buf, n);
    printf("[READ1] ret val = %d\n", ret);


    char str[] = "This is file!";
    ret = write(fd, str, sizeof(str));
    printf("[WRITE] ret val = %d\n", ret);

    ret = read(fd, buf, n);
    printf("[READ2] ret val = %d\n", ret);
    
    for (int i = 0; i < ret; i++) printf("%c", buf[i]);
    printf("\n");


    close(fd);

    exit(0);
}