
#include <stdint.h>
#include <stdlib.h>
#include <bits/ensure.h>
#include <mlibc/elf/startup.h>

extern char **environ;

extern "C" void __mlibc_entry(int (*main_fn)(int argc, char *argv[], char *env[])) {
    int argc = *((int *)0x1000);
    char **argv = (char **)0x1010;

    __mlibc_run_constructors();

	auto result = main_fn(argc, argv, environ);
	exit(result);
}

