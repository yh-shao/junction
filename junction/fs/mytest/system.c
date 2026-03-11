#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

int main() 
{    
    int status = system("mkdir -p /FSHAO/test_dir2");
    printf("status code: %d\n", status);
    
    return 0;
}