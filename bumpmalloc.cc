// g++ -O3 -std=c++11 -g -fPIC -shared -o bumpmalloc.so bumpmalloc.cc
// Then LD_PRELOAD=./bumpmalloc.so.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>

#include <pthread.h>

#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

namespace {

// Maps 2*sz virtual address space, the upper half as a sentinel value.
void* mmap_(size_t sz) {
	void *res = mmap(nullptr, 2 * sz, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (res == MAP_FAILED) return nullptr;
	mprotect((char *)res + sz, sz, PROT_NONE);
	return res;
}

const int SIG = 0xdead;
const size_t ALIGN = 32;
const size_t KB = 1024, MB = KB * KB, TB = MB * MB;
const size_t MAX_SIZE = 1 * TB;
char* buf_start;
std::atomic<char*> bump;

template <typename T> constexpr T aligned(T in, size_t align = ALIGN) {
	return (T)(((uintptr_t)in + align - 1) & -align);
}

template <typename T> bool safe_mul(T& dst, T src) {
	dst *= src; // La di dah.
	return true;
}
}

typedef struct {
  size_t real_size;
  int sig;
} header;

// calculate aligned header size
static_assert( sizeof(header) <= ALIGN, "Header too long" );
const size_t HEADER_SIZE = aligned(sizeof(header));

void* malloc(size_t sz) {
  size_t real_size = ((sz > 2048) ? sz*3 : sz) + HEADER_SIZE; // expect realloc ahead of time!!!

  sz = aligned(real_size);

  // Assume that if this returns null we're being initialized and this can
  // only happen in one thread. (i.e. we assume that malloc must be called
  // before the first thread is created. using clone directly it might be
  // possible to avoid malloc until then, but let's assume you don't.)
  char *res = bump.fetch_add(sz);
  if (unlikely(res == nullptr)) {
    size_t max_size = MAX_SIZE;
    while (!(buf_start = (char *)mmap_(max_size)))
      if (!(max_size >>= 1)) abort();
    bump.store(buf_start + sz);
    res = buf_start;
  }

  ((header *)res)->real_size = sz - HEADER_SIZE;
  ((header *)res)->sig = SIG;
  res = res + HEADER_SIZE;

  return (char *)res;
}

void* calloc(size_t n, size_t sz) {
  if (safe_mul(sz, n)) {
    void *p = malloc(sz);
    memset(p, 0, sz);		// zeroing is necessary as free will move bump pointer back!!!
    return p;
  }
  return nullptr;
}

int posix_memalign(void **pp, size_t al, size_t sz) {
  // valid when al <= ALIGN
  *pp = malloc(al + sz);
  if (aligned(*pp, al) != *pp ) {
    abort();
  }
  return 0;
}

/*
void *memalign(size_t al, size_t sz) {
  //TODO
}

void *aligned_alloc(size_t al, size_t sz) {
  //TODO
}

void *valloc(size_t sz) {
  //TODO
  // memalign(sysconf(_SC_PAGESIZE),size)
}
*/

inline header *block2header(void *p) {
  return (header *)((char *)p - HEADER_SIZE);
}

void* realloc(void* p, size_t sz) {
  if (unlikely(sz <= 0)) {
    if (p != nullptr) free(p);
    return nullptr;
  }
  else if (unlikely(p == nullptr))
    return malloc(sz);
  else {
    header *h = block2header(p);
    if (unlikely(h->sig != SIG)) {
      abort();
    }
    if ( h->real_size >= sz )
      return p;

    // try to expand last block by just bumping pointer
    size_t new_sz = aligned(sz + HEADER_SIZE) - HEADER_SIZE; // apply coefficient as well???
    char *np = (char *)p + h->real_size;
    char *next = (char *)p + new_sz;
    if (bump.compare_exchange_strong(np, next)) {
      // correct real_size
      h->real_size = new_sz;
      return p;
    }

    // If sz is expanding the buffer, we may read outside it. No matter though,
    // that's going to be mapped (to zero pages if nothing else) unless sz is
    // large enough to reach the end of memory.
    return memcpy(malloc(sz), p, sz);
  }
}

void free(void* p) {
  if (unlikely(p == nullptr))
    return;

  header *h = block2header(p);
  if (unlikely(h->sig != SIG)) {
    abort();
  }
  h->sig = 0;
  char *np = (char *)p + h->real_size;

  //try freeing last block => improves memory usage and cache locality
  bump.compare_exchange_strong(np, (char *)h);
}
