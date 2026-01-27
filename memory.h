#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stddef.h>

void *__wrap_malloc(size_t Size); // Called by user, defined in mem.s
void __wrap_free(void *Ptr);      // Called by user, defined in memory.c
void printMemoryStats();     // Defined in memory.c
void runGC();                // Defined in mem.s

#endif
