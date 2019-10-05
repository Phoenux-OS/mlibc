
#include <bits/ensure.h>
#include <mlibc/debug.hpp>
#include <mlibc/sysdeps.hpp>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>

#define STUB_ONLY { __ensure(!"STUB_ONLY function was called"); __builtin_unreachable(); }

namespace mlibc {
    // The basics
    void sys_libc_log(const char *message) {
        asm volatile (
            "int $0x80"
            :
            : "a"(4), "c"(message)
            : "edx"
        );
    }
    void sys_libc_panic() {
        sys_libc_log("\nmlibc: panic.\n");
        for (;;);
    }

    // Memory Related Functions
    int sys_vm_map(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) STUB_ONLY
    int sys_vm_unmap(void *pointer, size_t size) STUB_ONLY

    int sys_anon_free(void *pointer, size_t size) STUB_ONLY
    int sys_anon_allocate(size_t size, void **pointer) {
        size_t heap_base;
        size_t heap_size;
        asm volatile (
            "mov $0x10, %%eax;"
            "int $0x80;"
            "mov %%eax, %%ebx;"
            "mov $0x11, %%eax;"
            "int $0x80;"
            : "=b" (heap_base), "=a" (heap_size)
            :
            : "edx"
        );
        size_t ptr = heap_base + heap_size;
        asm volatile (
            "int $0x80"
            :
            : "a"(0x12), "c"(heap_size + size)
            : "edx"
        );
    	*pointer = (void*)ptr;
        return 0;
    }

    // Misc functions
    #ifndef MLIBC_BUILDING_RTDL
    int sys_clock_get(int clock, time_t *secs, long *nanos) STUB_ONLY

    // TODO: Actually implement this
    int sys_futex_wait(int *pointer, int expected) STUB_ONLY
    int sys_futex_wake(int *pointer) STUB_ONLY

    // Task functions
    void sys_exit(int status) STUB_ONLY
    int sys_tcb_set(void *pointer) STUB_ONLY
    #endif

    // File functions
    int sys_open(const char *path, int flags, int *fd) STUB_ONLY
    int sys_close(int fd) STUB_ONLY

    int sys_seek(int fd, off_t offset, int whence, off_t *new_offset) {
        off_t ret;

        switch (whence) {
            case SEEK_SET:
                whence = 0;
            case SEEK_END:
                whence = 1;
            case SEEK_CUR:
                whence = 2;
        }

        asm volatile ("int $0x80"
                : "=a"(ret)
                : "a"(0x2e), "c"(fd), "d"(offset), "D"(whence)
        );

        *new_offset = ret;

        return 0;
    }

    int sys_read(int fd, void *buf, size_t count, ssize_t *bytes_read) STUB_ONLY

    #ifndef MLIBC_BUILDING_RTDL
    int sys_write(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
        asm volatile ("int $0x80"
                :
                : "a"(0x2d), "c"(fd), "d"(buf), "D"(count)
        );

        *bytes_written = count;
        return 0;
    }
    int sys_isatty(int fd) {
        return 0;
    }
    #endif

} // namespace mlibc

