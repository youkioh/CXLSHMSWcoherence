#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>

int syscall_replication_start = 468;
int syscall_replication_stop = 469;


int main (int argc, char *argv[])
{
    int sampling_interval;
    int hot_page_percentage;

    if (argv[1] == NULL) {
        fprintf(stderr, "Usage: %s <start|stop> [sampling_interval] [hot_page_percentage]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (strcmp(argv[1], "start") == 0) {
        if (argv[2] == NULL || argv[3] == NULL) {
            fprintf(stderr, "Usage: %s start <sampling_interval> <hot_page_percentage>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        sampling_interval = atoi(argv[2]);
        hot_page_percentage = atoi(argv[3]);
        long res = syscall(syscall_replication_start, sampling_interval, hot_page_percentage);
        if (res == -1) {
            perror("syscall replication_start");
            exit(EXIT_FAILURE);
        }
        printf("Replication daemon started with sampling interval: %d\n", sampling_interval);
    } else if (strcmp(argv[1], "stop") == 0) {
        long res = syscall(syscall_replication_stop);
        if (res == -1) {
            perror("syscall replication_stop");
            exit(EXIT_FAILURE);
        }
        printf("Replication daemon stopped successfully\n");
    } else {
        fprintf(stderr, "Invalid command. Usage: %s <start|stop> [sampling_interval]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    return 0;
}