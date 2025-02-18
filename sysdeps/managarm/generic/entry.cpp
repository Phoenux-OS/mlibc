
#include <pthread.h>
#include <stdlib.h>
#include <sys/auxv.h>

#include <bits/ensure.h>
#include <mlibc/allocator.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/posix-pipe.hpp>
#include <mlibc/sysdeps.hpp>

#include <frg/eternal.hpp>
#include <frigg/vector.hpp>
#include <frigg/string.hpp>
#include <frigg/protobuf.hpp>

// defined by the POSIX library
void __mlibc_initLocale();

extern "C" uintptr_t *__dlapi_entrystack();

// declared in posix-pipe.hpp
thread_local Queue globalQueue;

HelHandle __mlibc_posix_lane;
void *__mlibc_clk_tracker_page;

namespace {
	struct managarm_process_data {
		HelHandle posix_lane;
		HelHandle *file_table;
		void *clock_tracker_page;
	};

	thread_local HelHandle *cachedFileTable;

	// This construction is a bit weird: Even though the variables above
	// are thread_local we still protect their initialization with a pthread_once_t
	// (instead of using a C++ constructor).
	// We do this in order to able to clear the pthread_once_t after a fork.
	pthread_once_t hasCachedInfos = PTHREAD_ONCE_INIT;

	void actuallyCacheInfos() {
		managarm_process_data data;
		HEL_CHECK(helSyscall1(kHelCallSuper + 1, reinterpret_cast<HelWord>(&data)));

		__mlibc_posix_lane = data.posix_lane;
		cachedFileTable = data.file_table;
		__mlibc_clk_tracker_page = data.clock_tracker_page;
	}
}

SignalGuard::SignalGuard() {
	sigset_t all;
	sigfillset(&all);
	if(mlibc::sys_sigprocmask(SIG_BLOCK, &all, &_restoreMask))
		mlibc::panicLogger() << "SignalGuard() failed" << frg::endlog;
}

SignalGuard::~SignalGuard() {
	if(mlibc::sys_sigprocmask(SIG_SETMASK, &_restoreMask, nullptr))
		mlibc::panicLogger() << "~SignalGuard() failed" << frg::endlog;
}

MemoryAllocator &getSysdepsAllocator() {
	// use frg::eternal to prevent a call to __cxa_atexit().
	// this is necessary because __cxa_atexit() call this function.
	static frg::eternal<VirtualAllocator> virtualAllocator;
	static frg::eternal<MemoryAllocator> singleton{virtualAllocator.get()};
	return singleton.get();
}

HelHandle getPosixLane() {
	cacheFileTable();
	return __mlibc_posix_lane;
}

HelHandle *cacheFileTable() {
	// TODO: Make sure that this is signal-safe (it is called e.g. by sys_clock_get()).
	pthread_once(&hasCachedInfos, &actuallyCacheInfos);
	return cachedFileTable;
}

void clearCachedInfos() {
	hasCachedInfos = PTHREAD_ONCE_INIT;
}

struct LibraryGuard {
	LibraryGuard();
};

static LibraryGuard guard;

static int __mlibc_argc;
static char **__mlibc_argv;

LibraryGuard::LibraryGuard() {
	__mlibc_initLocale();

	// Parse the environment.
	// TODO: Copy the arguments instead of pointing to them?
	auto env = __dlapi_entrystack();
	__mlibc_argc = *env++;
	__mlibc_argv = reinterpret_cast<char **>(env);
	env += __mlibc_argc; // Skip all arguments.
	__ensure(!*env);
	env++;

	while(*env) {
		auto string = reinterpret_cast<char *>(*env);
		auto fail = putenv(string);
		__ensure(!fail);
		env++;
	}
}

// The environment was build by the LibraryGuard.
extern char **environ;

extern "C" void __mlibc_entry(int (*main_function)(int argc, char *argv[], char *env[])) {
	auto result = main_function(__mlibc_argc, __mlibc_argv, environ);
	exit(result);
}

