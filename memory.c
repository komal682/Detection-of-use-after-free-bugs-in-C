#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <elf.h>
#include <assert.h>
#include <pthread.h>
#include "memory.h"

typedef unsigned long long ulong64;
#define NUM_SIZE_CLASSES 10
#define SEGMENT_SIZE (4ULL << 30)
#define PAGE_SIZE 4096
#define COMMIT_SIZE PAGE_SIZE
#define BITS_TO_BYTES(x) (((x) + 7) / 8)
#define Align(x, y) (((x) + (y-1)) & ~(y-1))
#define ADDR_TO_PAGE(x) (char*)(((ulong64)(x)) & ~(PAGE_SIZE-1))
#define ADDR_TO_SEGMENT(x) (Segment*)(((ulong64)(x)) & ~(SEGMENT_SIZE-1))

long long NumGCTriggered = 0;
long long NumBytesFreed = 0;
long long NumBytesAllocated = 0;

static const size_t SizeClasses[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128,
    256, 512, 1024, 2048, 4096
};

static inline unsigned char *get_slot_bitmap(void *page_start) {
    return (unsigned char *)page_start;
}

static inline int is_slot_used(unsigned char *bm, int slot) {
    return bm[slot / 8] & (1 << (slot % 8));
}

static inline void set_slot_used(unsigned char *bm, int slot) {
    bm[slot / 8] |= (1 << (slot % 8));
}

static inline void clear_slot_used(unsigned char *bm, int slot) {
    bm[slot / 8] &= ~(1 << (slot % 8));
}

static inline size_t slot_bitmap_bytes(size_t slots) {
    return BITS_TO_BYTES(slots);
}

struct OtherMetadata {
    char *AllocPtr;
    char *CommitPtr;
    char *ReservePtr;
    char *DataPtr;
    int BigAlloc;
};

typedef struct Segment {
    struct OtherMetadata Other;
    size_t object_size;        
    size_t objects_per_page;   
    unsigned char *byte_map;     
    struct Segment *Next;
} Segment;

static Segment *SizeClassSegments[NUM_SIZE_CLASSES] = {NULL};

static void setDataPtr(Segment *Seg, char *Ptr) { Seg->Other.DataPtr = Ptr; }
static void setAllocPtr(Segment *Seg, char *Ptr) { Seg->Other.AllocPtr = Ptr; }
static void setCommitPtr(Segment *Seg, char *Ptr) { Seg->Other.CommitPtr = Ptr; }
static void setReservePtr(Segment *Seg, char *Ptr) { Seg->Other.ReservePtr = Ptr; }
static char* getDataPtr(Segment *Seg) { return Seg->Other.DataPtr; }
static char* getAllocPtr(Segment *Seg) { return Seg->Other.AllocPtr; }
static char* getCommitPtr(Segment *Seg) { return Seg->Other.CommitPtr; }
static char* getReservePtr(Segment *Seg) { return Seg->Other.ReservePtr; }
static void setBigAlloc(Segment *Seg, int BigAlloc) { Seg->Other.BigAlloc = BigAlloc; }
void consolidate_sparse_pages(Segment *Seg);


static void addToSizeClassList(int cls, Segment *Seg) {
    Seg->Next = SizeClassSegments[cls];
    SizeClassSegments[cls] = Seg;
}

static void allowAccess(void *Ptr, size_t Size) {
    int Ret = mprotect(Ptr, Size, PROT_READ|PROT_WRITE);
    if (Ret == -1) {
        exit(1);
    }
}

static void reclaimMemory(void *Ptr, size_t Size) {
    mprotect(Ptr, Size, PROT_NONE);
    madvise(Ptr, Size, MADV_DONTNEED);
}

static Segment* allocateSegment(int BigAlloc, int cls) {
    void *Base = mmap(NULL, SEGMENT_SIZE * 2, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (Base == MAP_FAILED) exit(1);

    Segment *Seg = (Segment*)Align((ulong64)Base, SEGMENT_SIZE);
    allowAccess(Seg, PAGE_SIZE);

    char *SegmentStart = (char*)Seg;
    char *SegmentEnd = SegmentStart + SEGMENT_SIZE;

    setDataPtr(Seg, SegmentStart);
    setAllocPtr(Seg, SegmentStart);     
    setCommitPtr(Seg, SegmentStart);
    setReservePtr(Seg, SegmentEnd);
    setBigAlloc(Seg, BigAlloc);

    Seg->object_size = SizeClasses[cls];
    size_t max_slots = PAGE_SIZE / Seg->object_size;
    size_t bitmap_bytes = slot_bitmap_bytes(max_slots);
    Seg->objects_per_page = (PAGE_SIZE - bitmap_bytes) / Seg->object_size;
    
    size_t num_pages = SEGMENT_SIZE / PAGE_SIZE;
    size_t bytemap_needed = Align(num_pages, PAGE_SIZE);
    Seg->byte_map = (unsigned char *)(SegmentEnd - bytemap_needed);

    allowAccess(Seg->byte_map, bytemap_needed);

    for (size_t i = 0; i < num_pages; i++) {
        Seg->byte_map[i] = (unsigned char)Seg->objects_per_page;
    }

    addToSizeClassList(cls, Seg);
    return Seg;
}

void myfree(void *Ptr) {
    if (Ptr == NULL) return;

    Segment *Seg = ADDR_TO_SEGMENT(Ptr);
    
    if (Seg->Other.BigAlloc) {
        size_t *sz_ptr = (size_t*)Ptr - 1;
        size_t actual_size = *sz_ptr;
        NumBytesFreed += actual_size;
        munmap((char*)Ptr - PAGE_SIZE, actual_size + PAGE_SIZE);
        return;
    }

    char *page_start = ADDR_TO_PAGE(Ptr);
    size_t page_index = (page_start - getDataPtr(Seg)) / PAGE_SIZE;
    unsigned char *slot_bm = get_slot_bitmap(page_start);
    size_t bitmap_bytes = slot_bitmap_bytes(PAGE_SIZE / Seg->object_size);
    char *obj_base = page_start + bitmap_bytes;
    size_t slot_index = ((char *)Ptr - obj_base) / Seg->object_size;

    if (is_slot_used(slot_bm, slot_index)) {
        clear_slot_used(slot_bm, slot_index);
        Seg->byte_map[page_index]++;
		if (NumBytesFreed % (SEGMENT_SIZE / 1024) == 0) { 
    		consolidate_sparse_pages(Seg);
		}
        NumBytesFreed += Seg->object_size;

        if (Seg->byte_map[page_index] == Seg->objects_per_page) {
            reclaimMemory(page_start, PAGE_SIZE);
        }
    }
}

static void* BigAlloc(size_t Size) {
    size_t TotalSize = Align(Size + PAGE_SIZE, PAGE_SIZE);
    void *Ptr = mmap(NULL, TotalSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (Ptr == MAP_FAILED) return NULL;

    Segment *DummySeg = (Segment*)Ptr;
    setBigAlloc(DummySeg, 1);
    
    size_t *sz_ptr = (size_t*)((char*)Ptr + PAGE_SIZE);
    *(sz_ptr - 1) = Size;

    NumBytesAllocated += TotalSize;
    return (void*)sz_ptr;
}

void *_mymalloc(size_t Size) {
    if (Size == 0) return NULL;
    if (Size > SizeClasses[NUM_SIZE_CLASSES - 1]) return BigAlloc(Size);

    int cls = -1;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (Size <= SizeClasses[i]) {
            cls = i;
            break;
        }
    }

    Segment *Seg = SizeClassSegments[cls];
    while (Seg) {
        size_t num_pages = SEGMENT_SIZE / PAGE_SIZE;
        for (size_t p = 1; p < num_pages - 1; p++) {
            if (Seg->byte_map[p] == 0) continue;

            char *page_start = getDataPtr(Seg) + p * PAGE_SIZE;
            unsigned char *bm = get_slot_bitmap(page_start);
            size_t bm_bytes = slot_bitmap_bytes(PAGE_SIZE / Seg->object_size);

            if (Seg->byte_map[p] == Seg->objects_per_page) {
                allowAccess(page_start, PAGE_SIZE);
                memset(bm, 0, bm_bytes);
            }

            char *obj_base = page_start + bm_bytes;
            for (size_t s = 0; s < Seg->objects_per_page; s++) {
                if (!is_slot_used(bm, s)) {
                    set_slot_used(bm, s);
                    Seg->byte_map[p]--;
                    NumBytesAllocated += Seg->object_size;
                    return obj_base + s * Seg->object_size;
                }
            }
        }
        Seg = Seg->Next;
    }

    allocateSegment(0, cls);
    return _mymalloc(Size);
}

void consolidate_mremap(void *src_page, void *dst_page) {
    mremap(src_page, PAGE_SIZE, PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, dst_page);
}

void consolidate_sparse_pages(Segment *Seg) {
    size_t num_pages = SEGMENT_SIZE / PAGE_SIZE;
    size_t bytemap_needed = Align(num_pages, PAGE_SIZE);
    size_t last_usable_page = num_pages - (bytemap_needed / PAGE_SIZE) - 1;

    for (size_t i = 1; i < last_usable_page; i++) {
        if (Seg->byte_map[i] == Seg->objects_per_page || Seg->byte_map[i] == 0) {
            continue;
        }

        for (size_t j = i + 1; j < last_usable_page; j++) {
            if (Seg->byte_map[j] == Seg->objects_per_page || Seg->byte_map[j] == 0) {
                continue;
            }

            size_t used_i = Seg->objects_per_page - Seg->byte_map[i];
            size_t used_j = Seg->objects_per_page - Seg->byte_map[j];

            if (used_i + used_j <= Seg->objects_per_page) {
                void *src_addr = getDataPtr(Seg) + (j * PAGE_SIZE);
                void *dst_addr = getDataPtr(Seg) + (i * PAGE_SIZE);

                unsigned char *src_bm = get_slot_bitmap(src_addr);
                unsigned char *dst_bm = get_slot_bitmap(dst_addr);
                size_t bm_bytes = slot_bitmap_bytes(Seg->objects_per_page);

                for (size_t s = 0; s < Seg->objects_per_page; s++) {
                    if (is_slot_used(src_bm, s)) {
                        size_t bm_offset = slot_bitmap_bytes(PAGE_SIZE / Seg->object_size);
                        void *src_obj = (char*)src_addr + bm_offset + (s * Seg->object_size);
                        
                        for (size_t d = 0; d < Seg->objects_per_page; d++) {
                            if (!is_slot_used(dst_bm, d)) {
                                void *dst_obj = (char*)dst_addr + bm_offset + (d * Seg->object_size);
                                memcpy(dst_obj, src_obj, Seg->object_size);
                                set_slot_used(dst_bm, d);
                                clear_slot_used(src_bm, s);
                                Seg->byte_map[i]--;
                                Seg->byte_map[j]++;
                                break;
                            }
                        }
                    }
                }

                if (Seg->byte_map[j] == Seg->objects_per_page) {
                    reclaimMemory(src_addr, PAGE_SIZE);
                }
            }
        }
    }
}

void trigger_mremap_move(void *src_page, void *dst_page) {
    void *res = mremap(src_page, PAGE_SIZE, PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, dst_page);
    if (res == MAP_FAILED) {
        return;
    }
}

void printMemoryStats() {
    printf("Num Bytes Allocated: %lld\n", NumBytesAllocated);
    printf("Num Bytes Freed: %lld\n", NumBytesFreed);
}