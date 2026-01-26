#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
<<<<<<< HEAD
=======
#include <pthread.h>
#include "memory.h"
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5

#define SEGMENT_SIZE (4ULL << 30)   // 4GB
#define PAGE_SIZE 4096
<<<<<<< HEAD
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
=======
#define BITS_TO_BYTES(x) (((x) + 7) / 8)
#define Align(x, y) (((x) + (y-1)) & ~(y-1))
#define ADDR_TO_PAGE(x) (char*)(((ulong64)(x)) & ~(PAGE_SIZE-1))
#define ADDR_TO_SEGMENT(x) (Segment*)(((ulong64)(x)) & ~(SEGMENT_SIZE-1))

long long NumGCTriggered = 0;
long long NumBytesFreed = 0;
long long NumBytesAllocated = 0;

static const size_t SizeClasses[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

typedef struct Segment {
    size_t object_size;
    size_t objects_per_page;
    struct Segment *Next;
    unsigned char *byte_map;
    unsigned char *slot_bitmaps;
    size_t slot_bitmap_size;
    char *data_start;
    char *data_end;
    size_t num_data_pages;
    int is_big_alloc;
    pthread_mutex_t lock;
} Segment;

static Segment *SizeClassSegments[NUM_SIZE_CLASSES] = {NULL};
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

static inline unsigned char *get_page_slot_bitmap(Segment *Seg, size_t page_index) {
    return Seg->slot_bitmaps + (page_index * Seg->slot_bitmap_size);
}

static inline int is_slot_used(unsigned char *bm, size_t slot) {
    return bm[slot / 8] & (1 << (slot % 8));
}

static inline void set_slot_used(unsigned char *bm, size_t slot) {
    bm[slot / 8] |= (1 << (slot % 8));
}

static inline void clear_slot_used(unsigned char *bm, size_t slot) {
    bm[slot / 8] &= ~(1 << (slot % 8));
}

static inline void *get_object_addr(Segment *Seg, size_t page_index, size_t slot_index) {
    char *page_start = Seg->data_start + (page_index * PAGE_SIZE);
    return page_start + (slot_index * Seg->object_size);
}

static void allowAccess(void *Ptr, size_t Size) {
    if (mprotect(Ptr, Size, PROT_READ | PROT_WRITE) == -1) {
        perror("mprotect allow");
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
        exit(1);
    }
}

static void reclaimMemory(void *Ptr, size_t Size) {
    mprotect(Ptr, Size, PROT_NONE);
    madvise(Ptr, Size, MADV_DONTNEED);
}

<<<<<<< HEAD
/* Allocate new segment */
static Segment* allocateSegment() {
=======
static Segment* allocateSegment(int cls) {
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
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

<<<<<<< HEAD
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
=======
    Seg->object_size = SizeClasses[cls];
    Seg->is_big_alloc = 0;
    pthread_mutex_init(&Seg->lock, NULL);

    Seg->objects_per_page = PAGE_SIZE / Seg->object_size;
    Seg->slot_bitmap_size = BITS_TO_BYTES(Seg->objects_per_page);

    size_t total_pages = SEGMENT_SIZE / PAGE_SIZE;
    size_t byte_map_size = Align(total_pages, PAGE_SIZE);
    size_t total_bitmaps_size = Align(Seg->slot_bitmap_size * total_pages, PAGE_SIZE);
    size_t total_metadata = byte_map_size + total_bitmaps_size;

    Seg->slot_bitmaps = (unsigned char*)(SegmentEnd - total_metadata);
    Seg->byte_map = Seg->slot_bitmaps + total_bitmaps_size;

    Seg->data_start = SegmentStart + PAGE_SIZE;
    Seg->data_end = (char*)Seg->slot_bitmaps;
    Seg->num_data_pages = (Seg->data_end - Seg->data_start) / PAGE_SIZE;

    allowAccess(Seg->slot_bitmaps, total_metadata);

    for (size_t i = 0; i < Seg->num_data_pages; i++) {
        Seg->byte_map[i] = (unsigned char)Seg->objects_per_page;
    }

    memset(Seg->slot_bitmaps, 0, total_bitmaps_size);

    pthread_mutex_lock(&global_lock);
    Seg->Next = SizeClassSegments[cls];
    SizeClassSegments[cls] = Seg;
    pthread_mutex_unlock(&global_lock);

    return Seg;
}

static void* BigAlloc(size_t Size) {
    size_t TotalSize = Align(Size + sizeof(Segment) + sizeof(size_t), PAGE_SIZE);
    void *Ptr = mmap(NULL, TotalSize, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    if (Ptr == MAP_FAILED) {
        perror("mmap big alloc");
        return NULL;
    }

    Segment *Meta = (Segment*)Ptr;
    memset(Meta, 0, sizeof(Segment));
    Meta->is_big_alloc = 1;

    size_t *size_ptr = (size_t*)((char*)Ptr + sizeof(Segment));
    *size_ptr = TotalSize;

    __sync_fetch_and_add(&NumBytesAllocated, TotalSize);

    return (char*)Ptr + sizeof(Segment) + sizeof(size_t);
}

void *_mymalloc(size_t Size) {
    if (Size == 0) return NULL;

    if (Size > SizeClasses[NUM_SIZE_CLASSES - 1]) {
        return BigAlloc(Size);
    }

    int cls = -1;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (Size <= SizeClasses[i]) {
            cls = i;
            break;
        }
    }

    pthread_mutex_lock(&global_lock);
    Segment *Seg = SizeClassSegments[cls];
    pthread_mutex_unlock(&global_lock);

    while (Seg) {
        pthread_mutex_lock(&Seg->lock);

        for (size_t p = 0; p < Seg->num_data_pages; p++) {
            if (Seg->byte_map[p] == 0) continue;

            char *page_start = Seg->data_start + (p * PAGE_SIZE);
            unsigned char *slot_bm = get_page_slot_bitmap(Seg, p);

            if (Seg->byte_map[p] == Seg->objects_per_page) {
                allowAccess(page_start, PAGE_SIZE);
            }

            for (size_t s = 0; s < Seg->objects_per_page; s++) {
                if (!is_slot_used(slot_bm, s)) {
                    set_slot_used(slot_bm, s);
                    Seg->byte_map[p]--;
                    __sync_fetch_and_add(&NumBytesAllocated, Seg->object_size);

                    void *obj = get_object_addr(Seg, p, s);
                    pthread_mutex_unlock(&Seg->lock);
                    return obj;
                }
            }
        }

        pthread_mutex_unlock(&Seg->lock);
        Seg = Seg->Next;
    }

    Seg = allocateSegment(cls);
    if (!Seg) return NULL;
    return _mymalloc(Size);
}

static int pages_have_overlap(Segment *Seg, size_t page1_idx, size_t page2_idx) {
    unsigned char *bm1 = get_page_slot_bitmap(Seg, page1_idx);
    unsigned char *bm2 = get_page_slot_bitmap(Seg, page2_idx);
    
    for (size_t s = 0; s < Seg->objects_per_page; s++) {
        if (is_slot_used(bm1, s) && is_slot_used(bm2, s)) {
            return 1;
        }
    }
    return 0;
}

static size_t merge_pages(Segment *Seg, size_t dst_page_idx, size_t src_page_idx) {
    unsigned char *dst_bm = get_page_slot_bitmap(Seg, dst_page_idx);
    unsigned char *src_bm = get_page_slot_bitmap(Seg, src_page_idx);
    
    char *dst_page = Seg->data_start + (dst_page_idx * PAGE_SIZE);
    char *src_page = Seg->data_start + (src_page_idx * PAGE_SIZE);
    
    if (Seg->byte_map[dst_page_idx] == Seg->objects_per_page) {
        allowAccess(dst_page, PAGE_SIZE);
    }
    if (Seg->byte_map[src_page_idx] == Seg->objects_per_page) {
        allowAccess(src_page, PAGE_SIZE);
    }
    
    size_t objects_copied = 0;
    
    for (size_t s = 0; s < Seg->objects_per_page; s++) {
        if (is_slot_used(src_bm, s)) {
            void *src_obj = src_page + (s * Seg->object_size);
            void *dst_obj = dst_page + (s * Seg->object_size);
            
            memcpy(dst_obj, src_obj, Seg->object_size);
            
            set_slot_used(dst_bm, s);
            clear_slot_used(src_bm, s);
            
            Seg->byte_map[dst_page_idx]--;
            Seg->byte_map[src_page_idx]++;
            
            objects_copied++;
        }
    }
    
    return objects_copied;
}

static void consolidate_segment(Segment *Seg) {
    if (!Seg || Seg->num_data_pages < 2) return;
    
    pthread_mutex_lock(&Seg->lock);
    
    size_t pages_consolidated = 0;
    size_t total_objects_moved = 0;
    
    for (size_t page1 = 0; page1 < Seg->num_data_pages; page1++) {
        size_t page1_used = Seg->objects_per_page - Seg->byte_map[page1];
        
        if (page1_used == 0 || page1_used == Seg->objects_per_page) {
            continue;
        }
        
        for (size_t page2 = page1 + 1; page2 < Seg->num_data_pages; page2++) {
            size_t page2_used = Seg->objects_per_page - Seg->byte_map[page2];
            
            if (page2_used == 0 || page2_used == Seg->objects_per_page) {
                continue;
            }
            
            size_t total_used = page1_used + page2_used;
            
            if (total_used <= Seg->objects_per_page) {
                if (!pages_have_overlap(Seg, page1, page2)) {
                    size_t dst_idx, src_idx;
                    size_t objects_to_move;
                    
                    if (page1_used < page2_used) {
                        src_idx = page1;
                        dst_idx = page2;
                        objects_to_move = page1_used;
                    } else {
                        src_idx = page2;
                        dst_idx = page1;
                        objects_to_move = page2_used;
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
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
<<<<<<< HEAD
=======
            }
        }
    }
    
    pthread_mutex_unlock(&Seg->lock);
    
    if (pages_consolidated > 0) {
        printf("Consolidated %zu pages (moved %zu objects) in segment (obj_size=%zu)\n", 
               pages_consolidated, total_objects_moved, Seg->object_size);
    }
}

static void consolidate_size_class(int cls) {
    pthread_mutex_lock(&global_lock);
    Segment *Seg = SizeClassSegments[cls];
    pthread_mutex_unlock(&global_lock);
    
    while (Seg) {
        consolidate_segment(Seg);
        Seg = Seg->Next;
    }
}

void myfree(void *Ptr) {
    if (Ptr == NULL) return;

    char *meta_base = (char*)Ptr - sizeof(Segment) - sizeof(size_t);
    Segment *potential_seg = (Segment*)meta_base;
    
    Segment *Seg = ADDR_TO_SEGMENT(Ptr);
    
    if ((char*)Ptr >= (char*)Seg + sizeof(Segment) + sizeof(size_t) &&
        (char*)Ptr < (char*)Seg + PAGE_SIZE &&
        potential_seg == Seg && Seg->is_big_alloc) {
        
        size_t *size_ptr = (size_t*)((char*)Seg + sizeof(Segment));
        size_t total_size = *size_ptr;
        __sync_fetch_and_add(&NumBytesFreed, total_size);
        munmap(Seg, total_size);
        return;
    }

    pthread_mutex_lock(&Seg->lock);

    char *page_start = ADDR_TO_PAGE(Ptr);
    
    if (page_start < Seg->data_start || page_start >= Seg->data_end) {
        pthread_mutex_unlock(&Seg->lock);
        fprintf(stderr, "Error: free() invalid pointer %p\n", Ptr);
        return;
    }

    size_t page_index = (page_start - Seg->data_start) / PAGE_SIZE;
    size_t offset_in_page = (char*)Ptr - page_start;
    
    if (offset_in_page % Seg->object_size != 0) {
        pthread_mutex_unlock(&Seg->lock);
        fprintf(stderr, "Error: free() misaligned pointer %p\n", Ptr);
        return;
    }

    size_t slot_index = offset_in_page / Seg->object_size;
    unsigned char *slot_bm = get_page_slot_bitmap(Seg, page_index);

    if (!is_slot_used(slot_bm, slot_index)) {
        pthread_mutex_unlock(&Seg->lock);
        fprintf(stderr, "Error: double free detected at %p\n", Ptr);
        return;
    }

    clear_slot_used(slot_bm, slot_index);
    Seg->byte_map[page_index]++;
    __sync_fetch_and_add(&NumBytesFreed, Seg->object_size);

    if (Seg->byte_map[page_index] == Seg->objects_per_page) {
        reclaimMemory(page_start, PAGE_SIZE);
    }

    pthread_mutex_unlock(&Seg->lock);
    
    static long long last_consolidation = 0;
    if (NumBytesFreed - last_consolidation > 10 * 1024 * 1024) {
        last_consolidation = NumBytesFreed;
        
        for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
            if (Seg->object_size == SizeClasses[cls]) {
                consolidate_size_class(cls);
                break;
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
            }
            B = B->next;
        }
        A = A->next;
    }

<<<<<<< HEAD
=======
void runGC() {
    printf("Running manual garbage collection (consolidation)...\n");
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        consolidate_size_class(cls);
    }
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
    NumGCTriggered++;
}

void printMemoryStats() {
<<<<<<< HEAD
    printf("\n=== Memory Stats ===\n");
    printf("Allocated: %lld\n", NumBytesAllocated);
    printf("Freed:     %lld\n", NumBytesFreed);
    printf("In Use:    %lld\n", NumBytesAllocated - NumBytesFreed);
    printf("GC Runs:   %lld\n", NumGCTriggered);
=======
    printf("\n=== Memory Allocator Statistics ===\n");
    printf("Bytes Allocated: %lld\n", NumBytesAllocated);
    printf("Bytes Freed:     %lld\n", NumBytesFreed);
    printf("Bytes In Use:    %lld\n", NumBytesAllocated - NumBytesFreed);
    
    printf("\nSegments per size class:\n");
    for (int cls = 0; cls < NUM_SIZE_CLASSES; cls++) {
        int count = 0;
        long long bytes_used = 0;
        long long sparse_pages = 0;
        long long empty_pages = 0;
        
        pthread_mutex_lock(&global_lock);
        Segment *Seg = SizeClassSegments[cls];
        pthread_mutex_unlock(&global_lock);
        
        while (Seg) {
            count++;
            pthread_mutex_lock(&Seg->lock);
            for (size_t p = 0; p < Seg->num_data_pages; p++) {
                size_t used = Seg->objects_per_page - Seg->byte_map[p];
                bytes_used += used * Seg->object_size;
                
                if (Seg->byte_map[p] == Seg->objects_per_page) {
                    empty_pages++;
                } else if (Seg->byte_map[p] > 0) {
                    sparse_pages++;
                }
            }
            pthread_mutex_unlock(&Seg->lock);
            Seg = Seg->Next;
        }
        
        if (count > 0) {
            printf("  %4zu bytes: %3d segments, %lld bytes in use, %lld sparse pages, %lld empty pages\n",
                   SizeClasses[cls], count, bytes_used, sparse_pages, empty_pages);
        }
    }
>>>>>>> c99fc8a3c46b91877b3e8b45c7cd9ac0315b99a5
}