#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

int syscall_enable_coherence = 470;
int syscall_disable_coherence = 471;


int main (int argc, char *argv[])
{
    if (argv[1] == NULL) {
        fprintf(stderr, "Usage: %s <enable|disable> \n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (strcmp(argv[1], "enable") == 0) {
        long res = syscall(syscall_enable_coherence);
        if (res == -1) {
            perror("syscall enable_coherence");
            exit(EXIT_FAILURE);
        }
    } else if (strcmp(argv[1], "disable") == 0) {
        long res = syscall(syscall_disable_coherence);
        if (res == -1) {
            perror("syscall disable_coherence");
            exit(EXIT_FAILURE);
        }
    } else {
        fprintf(stderr, "Invalid command. Usage: %s <enable|disable>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    return 0;
}