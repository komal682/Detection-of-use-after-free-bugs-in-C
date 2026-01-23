#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stddef.h>

void *mymalloc(size_t Size); // Called by user, defined in mem.s
void myfree(void *Ptr);      // Called by user, defined in memory.c
void printMemoryStats();     // Defined in memory.c
void runGC();                // Defined in mem.s

#endif