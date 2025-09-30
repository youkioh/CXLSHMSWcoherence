#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

int syscall_flush_replicas = 467;

int main (int argc, char *argv[])
{
    long res = syscall(syscall_flush_replicas);
    if (res == -1) {
        perror("syscall");
        exit(EXIT_FAILURE);
    }
    printf("syscall flush_replicas returned: %ld\n", res);
    return 0;
}