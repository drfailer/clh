CC=gcc
CFLAGS=\
	   -Wall -Wextra -Wuninitialized -Wno-sign-compare \
	   -g \
	   -fdiagnostics-color=auto \
	   -I.. \
	   -I$(HOME)/Programming/usr/include \
	   -I/usr/lib/x86_64-linux-gnu/pmix2/include
LDFLAGS=-L$(HOME)/Programming/usr/lib -lucp -lucs -L/usr/lib/x86_64-linux-gnu/pmix2/lib -lpmix -lpthread

prog: main.c ../clh/clh.c ../clh/pmi.c ../clh/buffer.c ../clh/cache.c ../clh/ucx_utils.c ../clh/thread.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf build
