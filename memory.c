#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>


#define SEG_SIZE (4ULL << 30)
#define PAGE_SIZE 4096
#define ALIGN(x, y) (((x) + (y - 1)) & ~(y - 1))



static const int size_classes[] = { 8, 16, 32, 64,128, 256, 512,1024, 2048, 4096 };
#define NUM_OBJ_SIZES 10

typedef struct Segment {
    void *base;
    char *data_start;
    char *data_end;
    unsigned long long *bitmap;
    size_t alloc_idx;
    int object_size;
    int obj_per_page; 
    struct Segment *next;
}Segment;

static Segment *seg_list[NUM_OBJ_SIZES];
// static Segment *curr_seg[NUM_OBJ_SIZES];

static int get_size_class(int size) {
    for (int i = 0; i < NUM_OBJ_SIZES; i++)
        if (size <= size_classes[i])
            return i;
    return -1;
}


static Segment *allocateSeg(int size){
    void *addrs = mmap(NULL, SEG_SIZE * 2,
                      PROT_NONE,
                      MAP_ANON | MAP_PRIVATE, -1, 0);
    if (addrs == MAP_FAILED)
        return NULL;

    
    Segment *seg = (Segment *)ALIGN((uintptr_t)addrs, SEG_SIZE);

    if (mprotect(seg, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        munmap(addrs, SEG_SIZE * 2);
        return NULL;
    }

    
    seg->base = (void *)seg;
    seg->data_start = (char *)seg + PAGE_SIZE;
    
    int sc = get_size_class(size);
    if(sc < 0) return NULL;
    
    seg->object_size = size_classes[sc];
    size_t seg_pages = SEG_SIZE / PAGE_SIZE;
    seg->obj_per_page = PAGE_SIZE / seg->object_size;
    size_t total_objs = seg_pages * seg->obj_per_page;

    size_t bm_bytes = ceil(total_objs/8);

    bm_bytes = ALIGN(bm_bytes, PAGE_SIZE);

    seg->data_end = (char *)seg + SEG_SIZE - bm_bytes;
    seg->bitmap  = (unsigned long long *)seg->data_end;
    
    if (mprotect(seg->bitmap, bm_bytes,PROT_READ | PROT_WRITE) != 0) {
            munmap(addrs, SEG_SIZE * 2);
            return NULL;
    }

    memset(seg->bitmap, 0, bm_bytes);
    
    seg->alloc_idx = 0;
    seg->next = NULL;


    return seg;

}


void *__wrap_malloc(size_t size){
    if(size == 0) return NULL;
    if(size > PAGE_SIZE) return NULL; //big allocations here

    int sc = get_size_class(size);
    if(sc < 0 ) return NULL;

    Segment *seg = seg_list[sc];
    if(!seg){
        seg = allocateSeg(size_classes[sc]);
        if(!seg) return NULL;
        seg->next = NULL;
        seg_list[sc] = seg;
    }     
    char *obj = seg->data_start + seg->alloc_idx * seg->object_size;
    
    
    if(obj + seg->object_size > seg->data_end){
        Segment *new_seg = allocateSeg(size_classes[sc]);
        if(!new_seg) return NULL;
        new_seg->next = seg_list[sc];
        seg_list[sc] = new_seg;
        seg = new_seg;
        obj = seg->data_start + seg->alloc_idx * seg->object_size;
        
    }
    

    int i = (int)(seg->alloc_idx/64);
    int j = (int)(seg->alloc_idx%64);

    seg->bitmap[i] |= (1ULL << j);
    seg->alloc_idx++;

    return obj;
}

void __wrap_free(void *ptr){
    if(!ptr) return;

    Segment *seg = (Segment *)((uintptr_t)ptr & ~(SEG_SIZE - 1));

    size_t offset = (char *)ptr - seg->data_start;
    size_t idx = offset / seg->object_size;

    size_t i = idx / 64;
    size_t j = idx % 64;

    seg->bitmap[i] &= ~(1ULL << j);
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

    Segment *seg = (Segment *)((uintptr_t)ptr & ~(SEG_SIZE - 1));
    int sc = get_size_class(size);
    if (sc < 0)
        return NULL;

    size_t new_size = size_classes[sc];
    size_t old_size = seg->object_size;

    if(new_size <= old_size) return ptr;

    else{
        void *new_ptr = __wrap_malloc(new_size);
        if(!new_ptr) return NULL;
        memcpy(new_ptr, ptr, old_size);  
        __wrap_free(ptr); 
        return new_ptr;
    }

    return NULL;
}