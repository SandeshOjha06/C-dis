CC     = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L -I./src
TARGET = kvstore

SRCS = src/main.c \
       src/server_epoll.c \
       src/commands.c \
       src/store.c \
       src/persist.c

OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test:
	$(CC) $(CFLAGS) -o tests/test_store \
		tests/test_store.c src/store.c
	./tests/test_store

benchmark:
	$(CC) $(CFLAGS) -o benchmark src/benchmark.c
	./benchmark

clean:
	rm -f $(OBJS) $(TARGET) tests/test_store benchmark

.PHONY: all test benchmark clean