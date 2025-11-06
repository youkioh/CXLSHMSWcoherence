CC = gcc
CFLAGS = -Wall -Wextra

all: replication_daemon flush_all page_coherence

replication_daemon: replication_daemon.c
	$(CC) $(CFLAGS) -o replication_daemon replication_daemon.c

flush_all: flush_all.c
	$(CC) $(CFLAGS) -o flush_all flush_all.c

page_coherence: page_coherence.c
	$(CC) $(CFLAGS) -o page_coherence page_coherence.c

clean:
	rm -f replication_daemon flush_all

.PHONY: clean