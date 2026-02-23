#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#include <math.h>

#define SEG_SIZE (4ULL << 30)
#define PAGE_SIZE 4096
#define ALIGN(x, y) (((x) + (y - 1)) & ~(y - 1))
#define FRAG_TRIGGER_BYTES 1

static const int size_classes[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
#define NUM_OBJ_SIZES 10

typedef struct Segment
{
    void *base;
    char *data_start;
    char *data_end;
    unsigned long long *bitmap;
    size_t alloc_idx;
    int object_size;
    int obj_per_page;
    uint16_t *page_objfree_count;
    struct Segment *next;
} Segment;

typedef struct GlobalPageNode
{
    Segment *seg;
    size_t page_index;
    struct GlobalPageNode *next;
} GlobalPageNode;

static Segment *seg_list[NUM_OBJ_SIZES];
static GlobalPageNode *global_page_list[NUM_OBJ_SIZES];

typedef struct BigAlloc
{
    void *addr;
    size_t size;
    struct BigAlloc *next;
} BigAlloc;
#define BIGALLOC_HASH_SIZE 1024
static BigAlloc *BigAllocTable[BIGALLOC_HASH_SIZE];

static size_t NumBytesAllocated = 0;
static size_t NumBytesFreed = 0;
static size_t NumPagesReclaimed = 0;

static int get_size_class(int size)
{
    for (int i = 0; i < NUM_OBJ_SIZES; i++)
        if (size <= size_classes[i])
            return i;
    return -1;
}

static size_t bigalloc_hash(void *addr)
{
    return ((uintptr_t)addr >> 12) % BIGALLOC_HASH_SIZE;
}

static void bigalloc_insert(void *addr, size_t size)
{
    size_t h = bigalloc_hash(addr);
    /* mmap requires a page-aligned length; sizeof(BigAlloc) is only 24 bytes.*/
    BigAlloc *n = mmap(NULL, PAGE_SIZE,
                       PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (n == MAP_FAILED)
        return;
    n->addr = addr;
    n->size = size;
    n->next = BigAllocTable[h];
    BigAllocTable[h] = n;
}

static BigAlloc *bigalloc_find(void *addr)
{
    for (BigAlloc *b = BigAllocTable[bigalloc_hash(addr)]; b; b = b->next)
        if (b->addr == addr)
            return b;
    return NULL;
}

static void bigalloc_remove(void *addr)
{
    BigAlloc **p = &BigAllocTable[bigalloc_hash(addr)];
    while (*p)
    {
        if ((*p)->addr == addr)
        {
            BigAlloc *tmp = *p;
            *p = tmp->next;
            munmap(tmp, PAGE_SIZE);
            return;
        }
        p = &(*p)->next;
    }
}

static void *big_alloc(size_t size)
{
    size_t alloc_size = ALIGN(size, PAGE_SIZE);
    void *ptr = mmap(NULL, alloc_size,
                     PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    bigalloc_insert(ptr, alloc_size);
    NumBytesAllocated += alloc_size;
    return ptr;
}

// Memory layout inside the segment (low → high):
// [  header  PAGE_SIZE  ]  ← Segment struct lives here
// [  object data        ]  ← data_start .. data_end
// [  page_objfree_count    ]  ← pfc_bytes, page-aligned
// [  bitmap             ]  ← bm_bytes,  page-aligned

static Segment *allocateSeg(int size)
{
    void *addrs = mmap(NULL, SEG_SIZE * 2,
                       PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
    if (addrs == MAP_FAILED)
        return NULL;

    Segment *seg = (Segment *)ALIGN((uintptr_t)addrs, SEG_SIZE);

    seg->base = (void *)seg;
    seg->data_start = (char *)seg + PAGE_SIZE;

    // int sc = get_size_class(size);
    // if (sc < 0) return NULL;

    seg->object_size = size;
    size_t seg_pages = (SEG_SIZE / PAGE_SIZE) - 1;
    seg->obj_per_page = PAGE_SIZE / seg->object_size;
    size_t total_objs = seg_pages * seg->obj_per_page;

    size_t bm_bytes = ALIGN((total_objs + 7) / 8, PAGE_SIZE);
    int bm_pages = bm_bytes / PAGE_SIZE;

    size_t pfc_bytes = ALIGN((seg_pages - bm_pages) * sizeof(uint16_t), PAGE_SIZE);

    seg->bitmap = (unsigned long long *)((char *)seg + SEG_SIZE - bm_bytes);
    seg->page_objfree_count = (uint16_t *)((char *)seg->bitmap - pfc_bytes);
    seg->data_end = (char *)seg->page_objfree_count;

    memset(seg->bitmap, 0, bm_bytes);

    for (size_t p = 0; p < seg_pages - bm_pages; p++)
        seg->page_objfree_count[p] = (uint16_t)seg->obj_per_page;

    seg->alloc_idx = 0;
    seg->next = NULL;
    return seg;
}

static void add_global_page(int sc, Segment *seg, size_t page_idx)
{
    GlobalPageNode *node = mmap(NULL, sizeof(GlobalPageNode),
                                PROT_READ | PROT_WRITE,
                                MAP_ANON | MAP_PRIVATE, -1, 0);
    if (node == MAP_FAILED)
        return;

    node->seg = seg;
    node->page_index = page_idx;
    node->next = global_page_list[sc];
    global_page_list[sc] = node;
}

static void remove_global_page(int sc, Segment *seg, size_t page_idx)
{
    GlobalPageNode **curr = &global_page_list[sc];

    while (*curr)
    {
        if ((*curr)->seg == seg &&
            (*curr)->page_index == page_idx)
        {
            GlobalPageNode *tmp = *curr;
            *curr = tmp->next;
            munmap(tmp, sizeof(GlobalPageNode));
            return;
        }
        curr = &(*curr)->next;
    }
}

static void merge_pages(Segment *A, size_t pA,
                        Segment *B, size_t pB)
{
    int opp = A->obj_per_page;

    size_t baseA = pA * opp;
    size_t baseB = pB * opp;

    size_t startA = baseA / 64;
    size_t startB = baseB / 64;

    size_t words = (opp + 63) / 64;

    // Check overlap
    for (size_t w = 0; w < words; w++)
    {
        if (A->bitmap[startA + w] &
            B->bitmap[startB + w])
            return;
    }
    printf("MERGE: SegmentA %p Page %zu  -->  SegmentB %p Page %zu\n",
           (void *)A, pA, (void *)B, pB);

    // Merge exact positions
    for (size_t i = 0; i < opp; i++)
    {
        size_t slotA = baseA + i;
        size_t slotB = baseB + i;

        if (A->bitmap[slotA / 64] & (1ULL << (slotA % 64)))
        {
            memcpy(B->data_start +
                       slotB * B->object_size,
                   A->data_start +
                       slotA * A->object_size,
                   A->object_size);
        }
    }

    // Update bitmap
    for (size_t w = 0; w < words; w++)
    {
        B->bitmap[startB + w] |=
            A->bitmap[startA + w];

        A->bitmap[startA + w] = 0;
    }

    int live = 0;
    for (size_t w = 0; w < words; w++)
    {
        live += __builtin_popcountll(
            B->bitmap[startB + w]);
    }
    B->page_objfree_count[pB] = opp - live;
    A->page_objfree_count[pA] = opp;

    void *page_base =
        A->data_start + pA * PAGE_SIZE;

    munmap(page_base, PAGE_SIZE);

    NumPagesReclaimed++;
}

static void brute_force_merge(int sc)
{
    GlobalPageNode *prevA = NULL;
    GlobalPageNode *nodeA = global_page_list[sc];

    while (nodeA)
    {
        GlobalPageNode *nodeB = nodeA->next;
        int removedA = 0;

        while (nodeB)
        {
            if (nodeA->seg != nodeB->seg)
            {
                merge_pages(nodeA->seg, nodeA->page_index, nodeB->seg, nodeB->page_index);

                if (nodeA->seg->page_objfree_count[nodeA->page_index] == nodeA->seg->obj_per_page)
                {
                    GlobalPageNode *to_delete = nodeA;

                    if (prevA)
                        prevA->next = nodeA->next;
                    else
                        global_page_list[sc] = nodeA->next;

                    nodeA = nodeA->next;

                    munmap(to_delete, sizeof(GlobalPageNode));

                    removedA = 1;
                    break;
                }
            }

            nodeB = nodeB->next;
        }

        if (!removedA)
        {
            prevA = nodeA;
            nodeA = nodeA->next;
        }
    }
}

void *__wrap_malloc(size_t size)
{
    printf("Wrap mallocs");
    if (size == 0)
        return NULL;
    if (size > PAGE_SIZE)
        return big_alloc(size);

    int sc = get_size_class(size);
    if (sc < 0)
        return NULL;

    Segment *seg = seg_list[sc];
    if (!seg)
    {
        seg = allocateSeg(size_classes[sc]);
        if (!seg)
            return NULL;
        seg->next = NULL;
        seg_list[sc] = seg;
    }

    char *obj = seg->data_start + seg->alloc_idx * seg->object_size;

    /* segment full — prepend a fresh one */
    if (obj + seg->object_size > seg->data_end)
    {
        Segment *new_seg = allocateSeg(size_classes[sc]);
        if (!new_seg)
            return NULL;
        new_seg->next = seg_list[sc];
        seg_list[sc] = new_seg;
        seg = new_seg;
        obj = seg->data_start + seg->alloc_idx * seg->object_size;
    }

    /* mark slot live in bitmap */
    size_t wi = seg->alloc_idx / 64;
    size_t bi = seg->alloc_idx % 64;
    seg->bitmap[wi] |= (1ULL << bi);

    /* one fewer free slot on this page */
    size_t page_idx = (size_t)(obj - seg->data_start) / PAGE_SIZE;
    // if page was free before allocated
    if (seg->page_objfree_count[page_idx] == seg->obj_per_page)
    {
        add_global_page(sc, seg, page_idx);
    }
    seg->page_objfree_count[page_idx]--;

    seg->alloc_idx++;
    NumBytesAllocated += seg->object_size;
    printf("Wrap mallocs");
    return obj;
}

void __wrap_free(void *ptr)
{
    if (!ptr)
        return;

    BigAlloc *big = bigalloc_find(ptr);
    if (big)
    {
        munmap(big->addr, big->size);
        NumBytesFreed += big->size;
        bigalloc_remove(ptr);
        return;
    }

    Segment *seg = (Segment *)((uintptr_t)ptr & ~(SEG_SIZE - 1));

    if (seg->base != (void *)seg)
        return;

    size_t offset = (char *)ptr - seg->data_start;
    size_t idx = offset / seg->object_size;

    size_t i = idx / 64;
    size_t j = idx % 64;

    seg->bitmap[i] &= ~(1ULL << j);
    NumBytesFreed += seg->object_size;

    size_t page_idx = ((char *)ptr - seg->data_start) / PAGE_SIZE;
    seg->page_objfree_count[page_idx]++;

    if (seg->page_objfree_count[page_idx] == (uint16_t)seg->obj_per_page)
    {
        int sc = get_size_class(seg->object_size);
        remove_global_page(sc, seg, page_idx);

        void *page_base = seg->data_start + page_idx * PAGE_SIZE;
        madvise(page_base, PAGE_SIZE, MADV_FREE);
        // munmap(page_base, PAGE_SIZE);
        NumPagesReclaimed++;
    }

    size_t reclaimed_bytes = NumPagesReclaimed * PAGE_SIZE;
    size_t frag_bytes = (NumBytesFreed > reclaimed_bytes)
                            ? NumBytesFreed - reclaimed_bytes
                            : 0;

    if (frag_bytes >= FRAG_TRIGGER_BYTES)
    {
        int sc = get_size_class(seg->object_size);
        if (sc >= 0)
            brute_force_merge(sc);
    }
    printf("Wrap free");
}

void *__wrap_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = __wrap_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return __wrap_malloc(size);
    if (!size)
    {
        __wrap_free(ptr);
        return NULL;
    }

    BigAlloc *big = bigalloc_find(ptr);
    if (big)
    {
        if (size <= big->size)
            return ptr;
        void *np = __wrap_malloc(size);
        if (!np)
            return NULL;
        memcpy(np, ptr, big->size);
        __wrap_free(ptr);
        return np;
    }

    Segment *seg = (Segment *)((uintptr_t)ptr & ~(SEG_SIZE - 1));
    if (seg->base != (void *)seg)
        return NULL;
    int sc = get_size_class(size);
    if (sc < 0)
        return NULL;

    size_t new_size = size_classes[sc];
    size_t old_size = (size_t)seg->object_size;

    if (new_size <= old_size)
        return ptr;

    void *np = __wrap_malloc(new_size);
    if (!np)
        return NULL;
    memcpy(np, ptr, old_size);
    __wrap_free(ptr);
    return np;
}

/* ── printMemoryStats ─────────────────────────────────────────────────────*/
void printMemoryStats(void)
{
    printf("\n=== Memory Stats ===\n");
    printf("Allocated:       %zu\n", NumBytesAllocated);
    printf("Freed:           %zu\n", NumBytesFreed);
    printf("Pages Reclaimed: %zu (%zu KiB)\n",
           NumPagesReclaimed, (NumPagesReclaimed * PAGE_SIZE) / 1024);
    printf("In Use:          %zu\n", NumBytesAllocated - NumBytesFreed);
}

/* ── libc aliases ─────────────────────────────────────────────────────────*/
void *malloc(size_t size) __attribute__((alias("__wrap_malloc")));
void free(void *ptr) __attribute__((alias("__wrap_free")));
void *realloc(void *p, size_t s) __attribute__((alias("__wrap_realloc")));
void *calloc(size_t n, size_t s) __attribute__((alias("__wrap_calloc")));