#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define _GNU_SOURCE
#define __USE_GNU 1

int main(int argc, char *argv[])
{
    int fd;
    char buff[1024] = {0};
    char *str = "hello linux!";
    int len = strlen(str);
    int i = 0;

    fd = open(argv[1], O_RDWR | O_CREAT);
    if(fd < -1) {
    	perror("open failed!");
    	return -1;
    }
    write(fd, str, len);
    close(fd);

    fd = open(argv[1], O_RDWR);
    read(fd, buff, len);
    printf("buff=%s\n", buff);
    for(i=0; i<len; ++i) {
    	printf("0x%x ", buff[i]);
    }
    close(fd);
    return 0;
}
