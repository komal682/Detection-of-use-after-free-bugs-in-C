#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SEGMENT_SIZE (4ULL << 30)   // 4GB
#define PAGE_SIZE 4096
#define OBJECT_SIZE 8

#define OBJECTS_PER_PAGE (PAGE_SIZE / OBJECT_SIZE) // 512
#define SLOT_BITMAP_SIZE (OBJECTS_PER_PAGE / 8)    // 64 bytes
#define TOTAL_PAGES (SEGMENT_SIZE / PAGE_SIZE)

#define Align(x, y) (((x) + (y-1)) & ~(y-1))
#define ADDR_TO_PAGE(x) (char*)(((unsigned long long)(x)) & ~(PAGE_SIZE-1))
#define ADDR_TO_SEGMENT(x) (Segment*)(((unsigned long long)(x)) & ~(SEGMENT_SIZE-1))

typedef struct Segment {
    unsigned char *page_byte_map;   // 1 byte per page
    unsigned char *slot_bitmaps;    // bitmap per page
    char *data_start;
    char *data_end;
    size_t curr_page;
    size_t curr_slot;
    struct Segment *next;
} Segment;

static Segment *SegmentList = NULL;
static Segment *CurrentSegment = NULL;

long long NumBytesAllocated = 0;
long long NumBytesFreed = 0;
long long NumGCTriggered = 0;

static void allowAccess(void *Ptr, size_t Size) {
    if (mprotect(Ptr, Size, PROT_READ | PROT_WRITE) == -1) {
        perror("mprotect");
        exit(1);
    }
}

static void reclaimMemory(void *Ptr, size_t Size) {
    mprotect(Ptr, Size, PROT_NONE);
    madvise(Ptr, Size, MADV_DONTNEED);
}

/* Allocate new segment */
static Segment* allocateSegment() {
    void *Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (Base == MAP_FAILED) {
        perror("mmap segment");
        return NULL;
    }

    Segment *Seg = (Segment*)Align((unsigned long long)Base, SEGMENT_SIZE);
    allowAccess(Seg, PAGE_SIZE);
    memset(Seg, 0, sizeof(Segment));

    char *SegmentStart = (char*)Seg;
    char *SegmentEnd = SegmentStart + SEGMENT_SIZE;

    size_t byte_map_size = Align(TOTAL_PAGES, PAGE_SIZE);
    size_t bitmap_size = Align(TOTAL_PAGES * SLOT_BITMAP_SIZE, PAGE_SIZE);
    size_t metadata_size = byte_map_size + bitmap_size;

    Seg->slot_bitmaps = (unsigned char*)(SegmentEnd - metadata_size);
    Seg->page_byte_map = Seg->slot_bitmaps + bitmap_size;

    Seg->data_start = SegmentStart + PAGE_SIZE;
    Seg->data_end = (char*)Seg->slot_bitmaps;

    allowAccess(Seg->slot_bitmaps, metadata_size);

    memset(Seg->page_byte_map, 0, byte_map_size);
    memset(Seg->slot_bitmaps, 0, bitmap_size);

    Seg->curr_page = 0;
    Seg->curr_slot = 0;

    Seg->next = SegmentList;
    SegmentList = Seg;

    return Seg;
}

/* Allocate 8 bytes */
void *mymalloc(size_t size) {
    if (!CurrentSegment || CurrentSegment->curr_page >= TOTAL_PAGES) {
        CurrentSegment = allocateSegment();
        if (!CurrentSegment) return NULL;
    }

    Segment *Seg = CurrentSegment;

    size_t p = Seg->curr_page;
    size_t s = Seg->curr_slot;

    char *page_start = Seg->data_start + p * PAGE_SIZE;
    allowAccess(page_start, PAGE_SIZE);

    unsigned char *bm = Seg->slot_bitmaps + p * SLOT_BITMAP_SIZE;
    bm[s / 8] |= (1 << (s % 8));
    Seg->page_byte_map[p]++;

    void *obj = page_start + s * OBJECT_SIZE;

    memset(obj, 0, OBJECT_SIZE);

    Seg->curr_slot++;
    if (Seg->curr_slot == OBJECTS_PER_PAGE) {
        Seg->curr_slot = 0;
        Seg->curr_page++;
    }

    NumBytesAllocated += OBJECT_SIZE;
    return obj;
}

/* Free object (no reuse) */
void myfree(void *Ptr) {
    if (!Ptr) return;

    Segment *Seg = ADDR_TO_SEGMENT(Ptr);
    char *page = ADDR_TO_PAGE(Ptr);

    if (page < Seg->data_start || page >= Seg->data_end) {
        printf("Invalid free: %p\n", Ptr);
        return;
    }

    size_t p = (page - Seg->data_start) / PAGE_SIZE;
    size_t offset = (char*)Ptr - page;
    size_t slot = offset / OBJECT_SIZE;

    unsigned char *bm = Seg->slot_bitmaps + p * SLOT_BITMAP_SIZE;
    if (!(bm[slot/8] & (1 << (slot%8)))) {
        printf("Double free: %p\n", Ptr);
        return;
    }

    bm[slot/8] &= ~(1 << (slot%8));
    Seg->page_byte_map[p]--;
    NumBytesFreed += OBJECT_SIZE;

    if (Seg->page_byte_map[p] == 0) {
        reclaimMemory(page, PAGE_SIZE);
    }
}

/* Check if two pages have overlapping slots */
static int pages_have_overlap(Segment *dst, Segment *src, size_t page_idx) {
    unsigned char *dst_bm = dst->slot_bitmaps + page_idx * SLOT_BITMAP_SIZE;
    unsigned char *src_bm = src->slot_bitmaps + page_idx * SLOT_BITMAP_SIZE;

    for (size_t i = 0; i < SLOT_BITMAP_SIZE; i++) {
        if (dst_bm[i] & src_bm[i]) {
            return 1;  // Conflict found
        }
    }
    return 0;
}

/* Merge page idx of src into dst */
static void merge_pages(Segment *dst, Segment *src, size_t page_idx) {
    unsigned char *dst_bm = dst->slot_bitmaps + page_idx * SLOT_BITMAP_SIZE;
    unsigned char *src_bm = src->slot_bitmaps + page_idx * SLOT_BITMAP_SIZE;

    char *dst_page = dst->data_start + page_idx * PAGE_SIZE;
    char *src_page = src->data_start + page_idx * PAGE_SIZE;

    // Only allow access if pages have objects
    if (dst->page_byte_map[page_idx] > 0) {
        allowAccess(dst_page, PAGE_SIZE);
    }
    if (src->page_byte_map[page_idx] > 0) {
        allowAccess(src_page, PAGE_SIZE);
    }

    size_t objects_moved = 0;

    for (size_t i = 0; i < OBJECTS_PER_PAGE; i++) {
        if (src_bm[i/8] & (1 << (i%8))) {
            memcpy(dst_page + i*OBJECT_SIZE,
                   src_page + i*OBJECT_SIZE,
                   OBJECT_SIZE);
            dst_bm[i/8] |= (1 << (i%8));
            src_bm[i/8] &= ~(1 << (i%8));
            objects_moved++;
        }
    }

    // Update byte maps
    dst->page_byte_map[page_idx] += objects_moved;
    src->page_byte_map[page_idx] = 0;
    
    reclaimMemory(src_page, PAGE_SIZE);
}

/* Garbage collection */
void runGC() {
    Segment *A = SegmentList;
    while (A) {
        Segment *B = A->next;
        while (B) {
            for (size_t p = 0; p < TOTAL_PAGES; p++) {
                // Skip if either page is empty or if merging would overflow
                if (A->page_byte_map[p] == 0 && B->page_byte_map[p] == 0) {
                    continue;
                }
                
                if (A->page_byte_map[p] + B->page_byte_map[p] <= OBJECTS_PER_PAGE) {
                    // Check for slot conflicts before merging
                    if (!pages_have_overlap(A, B, p)) {
                        merge_pages(A, B, p);
                    }
                    
                    size_t moved = merge_pages(Seg, dst_idx, src_idx);
                    total_objects_moved += moved;
                    
                    if (Seg->byte_map[src_idx] == Seg->objects_per_page) {
                        char *src_page = Seg->data_start + (src_idx * PAGE_SIZE);
                        reclaimMemory(src_page, PAGE_SIZE);
                        pages_consolidated++;
                    }
                    
                    break;
                }
            }
            B = B->next;
        }
        A = A->next;
    }

    NumGCTriggered++;
}

void printMemoryStats() {
    printf("\n=== Memory Stats ===\n");
    printf("Allocated: %lld\n", NumBytesAllocated);
    printf("Freed:     %lld\n", NumBytesFreed);
    printf("In Use:    %lld\n", NumBytesAllocated - NumBytesFreed);
    printf("GC Runs:   %lld\n", NumGCTriggered);
}