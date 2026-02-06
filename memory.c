#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>


#define SEGMENT_SIZE (4ULL << 30)
#define PAGE_SIZE 4096

#define Align(x, y) (((x) + ((y) - 1)) & ~((y) - 1))

#define ADDR_TO_PAGE(x) ((char *)((uintptr_t)(x) & ~(PAGE_SIZE - 1)))

#define ADDR_TO_SEGMENT(x) ((Segment *)((uintptr_t)(x) & ~(SEGMENT_SIZE - 1)))

long long NumBytesAllocated = 0;
long long NumBytesFreed     = 0;

static const size_t size_classes[] = {
    8, 16, 32, 64,
    128, 256, 512,
    1024, 2048, 4096
};

#define NUM_SIZE_CLASSES 10


typedef struct Segment {
    size_t slot_size;            // size of each object
    size_t objects_per_page;     // how many objects fit in one page

    
    uint64_t *slot_bitmap;

    char *data_start;            // start of object pages
    char *data_end;              // start of bitmap region

    size_t curr_page;            // current page for allocation
    size_t curr_slot;            // current slot inside the page

    struct Segment *next;        // linked list
} Segment;


typedef struct BigAlloc {
    void *addr;
    size_t size;
    struct BigAlloc *next;
} BigAlloc;

#define BIGALLOC_HASH_SIZE 1024
static BigAlloc *BigAllocTable[BIGALLOC_HASH_SIZE];

static size_t bigalloc_hash(void *addr) {
    return ((uintptr_t)addr >> 12) % BIGALLOC_HASH_SIZE;
}

static void bigalloc_insert(void *addr, size_t size) {
    size_t h = bigalloc_hash(addr);
    BigAlloc *n = malloc(sizeof(BigAlloc));
    n->addr = addr;
    n->size = size;
    n->next = BigAllocTable[h];
    BigAllocTable[h] = n;
}

static BigAlloc *bigalloc_find(void *addr) {
    for (BigAlloc *b = BigAllocTable[bigalloc_hash(addr)]; b; b = b->next)
        if (b->addr == addr) return b;
    return NULL;
}

static void bigalloc_remove(void *addr) {
    BigAlloc **p = &BigAllocTable[bigalloc_hash(addr)];
    while (*p) {
        if ((*p)->addr == addr) {
            BigAlloc *tmp = *p;
            *p = tmp->next;
            free(tmp);
            return;
        }
        p = &(*p)->next;
    }
}


static void *big_alloc(size_t size) {
    size_t alloc_size = Align(size, PAGE_SIZE);

    void *ptr = mmap(NULL, alloc_size,
                     PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);

    if (ptr == MAP_FAILED)
        return NULL;

    bigalloc_insert(ptr, alloc_size);
    NumBytesAllocated += alloc_size;
    return ptr;
}



static Segment *SegmentLists[NUM_SIZE_CLASSES];
static Segment *CurrentSegments[NUM_SIZE_CLASSES];


static void allowAccess(void *ptr, size_t size) {
    mprotect(ptr, size, PROT_READ | PROT_WRITE);
}

static void reclaimMemory(void *ptr, size_t size) {
    mprotect(ptr, size, PROT_NONE);
    madvise(ptr, size, MADV_DONTNEED);
}

static int get_size_class(size_t size) {
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
        if (size <= size_classes[i])
            return i;
    return -1;
}


static Segment *allocateSegment(size_t slot_size) {
    void *base = mmap(NULL, SEGMENT_SIZE * 2,
                      PROT_NONE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    Segment *seg = (Segment *)Align((uintptr_t)base, SEGMENT_SIZE);

    allowAccess(seg, PAGE_SIZE); //accessing metadata page

    memset(seg, 0, sizeof(*seg));

    seg->slot_size = slot_size;

    seg->objects_per_page = PAGE_SIZE / slot_size;

    char *start = (char *)seg;
    char *end   = start + SEGMENT_SIZE;

    size_t pages = SEGMENT_SIZE / PAGE_SIZE;

    size_t bitmap_size = Align(pages * sizeof(uint64_t), PAGE_SIZE);

    seg->slot_bitmap = (uint64_t *)(end - bitmap_size); //bitmap is at the end of the segment

    seg->data_start = start + PAGE_SIZE;

    seg->data_end = (char *)seg->slot_bitmap;
    allowAccess(seg->slot_bitmap, bitmap_size);
    memset(seg->slot_bitmap, 0, bitmap_size);

    return seg;
}

void *__wrap_malloc(size_t size) {
    if (size > PAGE_SIZE)
        return big_alloc(size);

    int sc = get_size_class(size);
    if (sc < 0)
        return NULL;

    Segment *seg = CurrentSegments[sc];
    if (!seg) {
        seg = allocateSegment(size_classes[sc]);
        seg->next = SegmentLists[sc];
        SegmentLists[sc] = seg;
        CurrentSegments[sc] = seg;
    }

    char *page = seg->data_start + seg->curr_page * PAGE_SIZE;
    allowAccess(page, PAGE_SIZE);

    uint64_t *bm = &seg->slot_bitmap[seg->curr_page];

    *bm |= (1ULL << seg->curr_slot); // allocate slot

     
    assert(*bm != 0); //page will not be empty after allocation

    void *obj = page + seg->curr_slot * seg->slot_size;
    memset(obj, 0, seg->slot_size);

    if (++seg->curr_slot == seg->objects_per_page) {
        seg->curr_slot = 0;
        seg->curr_page++;
    } // moving to the next slot/page

    NumBytesAllocated += seg->slot_size;
    return obj;
}

void __wrap_free(void *ptr) {
    if (!ptr)
        return;

    BigAlloc *ba = bigalloc_find(ptr);
    if (ba) {
        munmap(ptr, ba->size);
        NumBytesFreed += ba->size;
        bigalloc_remove(ptr);
        return;
    }

    Segment *seg = ADDR_TO_SEGMENT(ptr);
    char *page   = ADDR_TO_PAGE(ptr);

    size_t p = (page - seg->data_start) / PAGE_SIZE;
    size_t slot = ((char *)ptr - page) / seg->slot_size;

    uint64_t *bm = &seg->slot_bitmap[p];

    assert(*bm != 0); // page must be allocated if we are freeing

    // /* ignore double free */
    // if (!(*bm & (1ULL << slot)))
    //     return;

    *bm &= ~(1ULL << slot);
    NumBytesFreed += seg->slot_size;

    
    if (*bm == 0) // if page becomes empty
        reclaimMemory(page, PAGE_SIZE);
}

void *__wrap_calloc(size_t n, size_t size) {
    size_t total = n * size;
    void *p = __wrap_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *__wrap_realloc(void *ptr, size_t size) {
    if (!ptr)
        return __wrap_malloc(size);

    if (size == 0) {
        __wrap_free(ptr);
        return NULL;
    }

    BigAlloc *ba = bigalloc_find(ptr);
    if (ba) {
        void *n = big_alloc(size);
        memcpy(n, ptr, ba->size < size ? ba->size : size);
        munmap(ptr, ba->size);
        bigalloc_remove(ptr);
        return n;
    }

    Segment *seg = ADDR_TO_SEGMENT(ptr);
    void *n = __wrap_malloc(size);
    memcpy(n, ptr, seg->slot_size < size ? seg->slot_size : size);
    __wrap_free(ptr);
    return n;
}


void printMemoryStats() {
    printf("\n=== Memory Stats ===\n");
    printf("Allocated: %lld\n", NumBytesAllocated);
    printf("Freed:     %lld\n", NumBytesFreed);
    printf("In Use:    %lld\n", NumBytesAllocated - NumBytesFreed);
}

void *malloc(size_t size)   __attribute__((alias("__wrap_malloc")));
void free(void *ptr)        __attribute__((alias("__wrap_free")));
void *realloc(void *p, size_t s)
                           __attribute__((alias("__wrap_realloc")));
void *calloc(size_t n, size_t s)
                           __attribute__((alias("__wrap_calloc")));
