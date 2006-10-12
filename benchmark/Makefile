CFLAGS=-Wall

all: dm io-latency-test

io-latency-test: io-latency-test.c
	$(CC) -pthread -o $@ $^

install:

clean: 
	rm -f dm

distclean: clean
