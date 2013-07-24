GCC = ../../install/bin/gcc

TESTS = test.s fibf.s
all:$(TESTS)
test.s : test.c
	$(GCC) -S -mdta $^ -o $@
fibf.s : fibf.c
	$(GCC) -S -mdta $^ -o $@

clean:
	rm -rf $(TESTS)
