GCC = ../../install/bin/gcc

all:test.s
test.s : test.c
	$(GCC) -S -mdta $^ -o $@
