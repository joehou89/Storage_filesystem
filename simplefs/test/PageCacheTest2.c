#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main(int argc, char *argv[])
{
    int fd;
    fd = open(argv[1], O_RDWR);
    if(fd < 0) {
    	perror("open failed!");
    	return -1;
    }
    fsync(fd);
    close(fd);

    return 0;
}

