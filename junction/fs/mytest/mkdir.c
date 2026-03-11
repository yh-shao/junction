#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>

int main() 
{
    const char *dir_name = "FSHAO:/temp_dir";

    if (mkdir(dir_name, 0755) == 0)  // 创建目录（实际权限还与 umask 有关）
    {
    	printf("目录 '%s' 创建成功。\n", dir_name);
    } 
    else 
    {
        perror("创建失败");
        return 1;
    }

    return 0;
}