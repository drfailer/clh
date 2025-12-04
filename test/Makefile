CC=gcc
CFLAGS=\
	   -Wall -Wextra -Wuninitialized -Wno-sign-compare -Wno-override-init \
	   -g \
	   -fdiagnostics-color=auto \
	   -I.. \
	   -I$(HOME)/Programming/usr/include \
	   -I$(HOME)/Programming/projects/tracer-c/tracer \
	   -I/usr/lib/x86_64-linux-gnu/pmix2/include
LDFLAGS=-L$(HOME)/Programming/usr/lib -lucp -lucs \
		-L/usr/lib/x86_64-linux-gnu/pmix2/lib -lpmix -lpthread \
		-L$(HOME)/Programming/projects/tracer-c/build/lib -l:libtracer.a

prog: main.c ../clh/*.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

memcheck: prog
	mpirun -n 2 \
		valgrind \
		--suppressions=/usr/lib/x86_64-linux-gnu/pmix2/share/pmix/pmix-valgrind.supp \
		--leak-check=full \
		--track-origins=yes \
		--show-leak-kinds=all \
		prog

clean:
	rm -rf build
