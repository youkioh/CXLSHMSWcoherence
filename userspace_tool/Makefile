CC = gcc
CFLAGS = -Wall -Wextra

all: replication_daemon flush_all

replication_daemon: replication_daemon.c
	$(CC) $(CFLAGS) -o replication_daemon replication_daemon.c

flush_all: flush_all.c
	$(CC) $(CFLAGS) -o flush_all flush_all.c

clean:
	rm -f replication_daemon flush_all

.PHONY: clean