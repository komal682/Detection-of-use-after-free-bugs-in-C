CC = gcc
CFLAGS = -Og -g -fPIC
LDFLAGS = -L. -Wl,-rpath,. -lmemory -lpthread

# Add rbtest to the default target so it builds automatically
default: libmemory.so rbtest

libmemory.so: memory.c
	$(CC) -shared $(CFLAGS) -o libmemory.so memory.c -lpthread

rbtest: rbtree_test.c libmemory.so
	$(CC) $(CFLAGS) -o rbtest rbtree_test.c $(LDFLAGS)

run_rb: rbtest
	/usr/bin/time -v ./rbtest 50000

clean:
	rm -f libmemory.so rbtest