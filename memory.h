#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stddef.h>

void *__wrap_malloc(size_t Size); // Called by user, defined in mem.s
void __wrap_free(void *Ptr);      // Called by user, defined in memory.c
void *__wrap_realloc(void *ptr, size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);
void printMemoryStats();     // Defined in memory.c
void runGC();                // Defined in mem.s

#define mymalloc  __wrap_malloc
#define myfree    __wrap_free
#define mycalloc  __wrap_calloc
#define myrealloc __wrap_realloc

#endif
