CC = gcc
CFLAGS = -Wall -Wextra

TARGET = flush_all
SRCS = flush_all.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: all clean