CC=gcc
CFLAGS=-Wall -Wextra -Wuninitialized -Wno-sign-compare -fdiagnostics-color=auto \
	   -I$(HOME)/Programming/usr/include -I/usr/lib/x86_64-linux-gnu/pmix2/include \
	   -fPIC -g -O3
LDFLAGS=-L$(HOME)/Programming/usr/lib -lucp -lucs -L/usr/lib/x86_64-linux-gnu/pmix2/lib -lpmix -lpthread
SRC=$(wildcard clh/*.c)
OBJ=$(addprefix build/,$(SRC:clh/%.c=%.o))
DEP=$(addprefix build/,$(SRC:clh/%.c=%.d))

.PHONY all: build/lib/libclh.so build/lib/libclh.a

build/lib/libclh.so: $(OBJ)
	@mkdir -p build/lib
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -shared

build/lib/libclh.a: $(OBJ)
	@mkdir -p build/lib
	ar rcs $@ $^

build/%.o: clh/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -o $@ -c $<

clean:
	rm -rf build

-include $(DEP)
