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
#define FRAG_TRIGGER_BYTES (64ULL * 1024)

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

static Segment *seg_list[NUM_OBJ_SIZES];

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

/* ── get_size_class ───────────────────────────────────────────────────────*/
static int get_size_class(int size)
{
    for (int i = 0; i < NUM_OBJ_SIZES; i++)
        if (size <= size_classes[i])
            return i;
    return -1;
}

/* ── big-alloc hash table ─────────────────────────────────────────────────
 * Allocations larger than PAGE_SIZE bypass the slab entirely and go
 * straight to mmap. We track them in a hash table keyed by address so
 * __wrap_free can identify and munmap them.
 * --------------------------------------------------------------------------*/
static size_t bigalloc_hash(void *addr)
{
    return ((uintptr_t)addr >> 12) % BIGALLOC_HASH_SIZE;
}

static void bigalloc_insert(void *addr, size_t size)
{
    size_t h = bigalloc_hash(addr);
    /* mmap requires a page-aligned length; sizeof(BigAlloc) is only 24 bytes.
     * munmap with a sub-page size causes SIGABRT on some systems. */
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

/* ── allocateSeg ──────────────────────────────────────────────────────────
 * Reserves a 4 GiB-aligned slab segment and carves the tail into a bitmap
 * and a page_objfree_count array.
 *
 * Memory layout inside the segment (low → high):
 *   [  header  PAGE_SIZE  ]  ← Segment struct lives here
 *   [  object data        ]  ← data_start .. data_end
 *   [  page_objfree_count    ]  ← pfc_bytes, page-aligned
 *   [  bitmap             ]  ← bm_bytes,  page-aligned
 * --------------------------------------------------------------------------*/
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

/* ---------------------------------------------------------
   Merge two pages from two segments (same size class)
   --------------------------------------------------------- */
static void merge_pages(Segment *segA, size_t pageA, Segment *segB, size_t pageB)
{

    int opp = segA->obj_per_page;

    /* count live objects */
    int liveA = opp - segA->page_objfree_count[pageA];
    int liveB = opp - segB->page_objfree_count[pageB];

    if (liveA == 0 || liveB == 0)
        return;

    if (liveA + liveB > opp)
        return;

    /* choose smaller page as source */
    Segment *src = (liveA <= liveB) ? segA : segB;
    Segment *dst = (src == segA) ? segB : segA;
    size_t src_pg = (src == segA) ? pageA : pageB;
    size_t dst_pg = (src == segA) ? pageB : pageA;

    for (int i = 0; i < opp; i++)
    {
        int src_slot = src_pg * opp + i;

        if (!(src->bitmap[src_slot / 64] &
              (1ULL << (src_slot % 64))))
            continue;

        /* find free slot in dst */
        for (int j = 0; j < opp; j++)
        {
            int dst_slot = dst_pg * opp + j;

            if (!(dst->bitmap[dst_slot / 64] &
                  (1ULL << (dst_slot % 64))))
            {
                memcpy(
                    dst->data_start +
                        (size_t)dst_slot * dst->object_size,
                    src->data_start +
                        (size_t)src_slot * src->object_size,
                    src->object_size);

                src->bitmap[src_slot / 64] &=
                    ~(1ULL << (src_slot % 64));
                dst->bitmap[dst_slot / 64] |=
                    (1ULL << (dst_slot % 64));

                src->page_objfree_count[src_pg]++;
                dst->page_objfree_count[dst_pg]--;

                break;
            }
        }
    }

    /* reclaim source page if empty */
    if (src->page_objfree_count[src_pg] == opp)
    {
        void *page_base =
            src->data_start + src_pg * PAGE_SIZE;

        madvise(page_base, PAGE_SIZE, MADV_FREE);
        NumPagesReclaimed++;
    }
}

/* ---------------------------------------------------------
   Brute force segment-vs-segment merge
   --------------------------------------------------------- */
   static void brute_force_merge(int sc)
   {
       Segment *segA = seg_list[sc];
   
       while (segA)
       {
           size_t pagesA =
               (segA->data_end - segA->data_start) / PAGE_SIZE;
   
           Segment *segB = segA->next;
   
           while (segB)
           {
               size_t pagesB =
                   (segB->data_end - segB->data_start) / PAGE_SIZE;
   
               int threshold = segA->obj_per_page / 4;
               if (threshold < 1) threshold = 1;
   
               for (size_t pA = 0; pA < pagesA; pA++)
               {
                   /* Skip empty pages */
                   if (segA->page_objfree_count[pA] ==
                       segA->obj_per_page)
                       continue;
   
                   /* 25% free threshold */
                   if (segA->page_objfree_count[pA] < threshold)
                       continue;
   
                   for (size_t pB = 0; pB < pagesB; pB++)
                   {
                       if (segB->page_objfree_count[pB] ==
                           segB->obj_per_page)
                           continue;
   
                       if (segB->page_objfree_count[pB] < threshold)
                           continue;
   
                       merge_pages(segA, pA,
                                   segB, pB);
                   }
               }
   
               segB = segB->next;
           }
   
           segA = segA->next;
       }
   }
   

/* ── __wrap_malloc ────────────────────────────────────────────────────────*/
void *__wrap_malloc(size_t size)
{
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
    seg->page_objfree_count[page_idx]--;

    seg->alloc_idx++;
    NumBytesAllocated += seg->object_size;
    return obj;
}

/* ── __wrap_free ──────────────────────────────────────────────────────────*/
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

    /* Validate this pointer actually came from one of our segments.
     * When the malloc alias intercepts libc-internal free() calls for
     * pointers we never allocated, seg->base will not equal seg itself.
     * Silently ignore such pointers rather than corrupting random memory. */
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
        void *page_base = seg->data_start + page_idx * PAGE_SIZE;
        madvise(page_base, PAGE_SIZE, MADV_FREE);
        NumPagesReclaimed++;
    }

    /* BUG FIX — saturating subtraction.
     *
     * After compact_size_class moves live objects out of a sparse page and
     * madvises it, NumPagesReclaimed * PAGE_SIZE can exceed NumBytesFreed
     * (the moved page contributed only its *dead* slots to NumBytesFreed, not
     * the live slot that was evacuated). A raw unsigned subtraction wraps to
     * ~SIZE_MAX, instantly re-fires compaction, and stack-overflows.
     * The ternary clamps the result to 0 instead. */
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
}

/* ══════════════════════════════════════════════════════════════════════════
 * compact_size_class
 *
 * For every segment in size class sc, scan pages from the HIGH end toward
 * the front. Any page with >= 25% free slots is a compaction candidate.
 * Live objects on that page are memcpy'd into the earliest page that still
 * has room (safe because the allocator guarantees no external pointers into
 * slab memory). Once a page is fully evacuated it is returned to the OS via
 * madvise(MADV_FREE) and NumPagesReclaimed is incremented.
 *
 * Helpers:
 *   find_free_slot — first bitmap-0 slot on a page, or -1
 *   find_live_slot — first bitmap-1 slot on a page, or -1
 *   move_object    — memcpy + bitmap update + page_objfree_count update
 *
 * Complexity: O(active_pages²) per segment — acceptable for Model 0.
 * ══════════════════════════════════════════════════════════════════════════*/

/* ── __wrap_calloc ────────────────────────────────────────────────────────*/
void *__wrap_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = __wrap_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

/* ── __wrap_realloc ───────────────────────────────────────────────────────*/
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
            return ptr; /* still fits */
        void *np = __wrap_malloc(size);
        if (!np)
            return NULL;
        memcpy(np, ptr, big->size);
        __wrap_free(ptr);
        return np;
    }

    Segment *seg = (Segment *)((uintptr_t)ptr & ~(SEG_SIZE - 1));
    if (seg->base != (void *)seg)
        return NULL; /* not our pointer */
    int sc = get_size_class(size);
    if (sc < 0)
        return NULL;

    size_t new_size = size_classes[sc];
    size_t old_size = (size_t)seg->object_size;

    if (new_size <= old_size)
        return ptr; /* fits in same size class */

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