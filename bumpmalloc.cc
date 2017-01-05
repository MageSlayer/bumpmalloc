// g++ -O3 -std=c++11 -g -fPIC -shared -o bumpmalloc.so bumpmalloc.cc
// Then LD_PRELOAD=./bumpmalloc.so.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>

namespace {

// Maps 2*sz virtual address space, the upper half as a sentinel value.
void* mmap_(size_t sz) {
	void *res = mmap(nullptr, 2 * sz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (res == MAP_FAILED) return nullptr;
	mprotect((char *)res + sz, sz, PROT_NONE);
	return res;
}

const size_t ALIGN = 16;
const size_t KB = 1024, MB = KB * KB, TB = MB * MB;
const size_t MAX_SIZE = 1 * TB;
char* buf_start;
std::atomic<char*> bump;

template <typename T> T aligned(T in, size_t align = ALIGN) {
	return (T)(((uintptr_t)in + align - 1) & -align);
}

template <typename T> bool safe_mul(T& dst, T src) {
	dst *= src; // La di dah.
	return true;
}
}

void* malloc(size_t sz) {
	sz = aligned(sz);

	// Assume that if this returns null we're being initialized and this can
	// only happen in one thread. (i.e. we assume that malloc must be called
	// before the first thread is created. using clone directly it might be
	// possible to avoid malloc until then, but let's assume you don't.)
	if (char *res = bump.fetch_add(sz)) return res;

	size_t max_size = MAX_SIZE;
	while (!(buf_start = (char *)mmap_(max_size)))
		if (!(max_size >>= 1)) abort();
	bump.store(buf_start + sz);
	return buf_start;
}

void* calloc(size_t n, size_t sz) {
	if (safe_mul(sz, n)) {
		return malloc(sz);
	}
	return nullptr;
}

int posix_memalign(void **pp, size_t al, size_t sz) {
	*pp = aligned(malloc(al + sz), al);
	return 0;
}

void* realloc(void* p, size_t sz) {
	// If sz is expanding the buffer, we may read outside it. No matter though,
	// that's going to be mapped (to zero pages if nothing else) unless sz is
	// large enough to reach the end of memory.
	return p ? memcpy(malloc(sz), p, sz) : malloc(sz);
}

void free(void*) {}