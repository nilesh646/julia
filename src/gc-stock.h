// This file is a part of Julia. License is MIT: https://julialang.org/license

/*
  allocation and garbage collection
  . non-moving, precise mark and sweep collector
  . pool-allocates small objects, keeps big objects on a simple list
*/
#ifndef JL_GC_H
#define JL_GC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include "julia.h"
#include "julia_threads.h"
#include "julia_internal.h"
#include "julia_assert.h"
#include "threading.h"
#include "gc-common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GC_SMALL_PAGE
#define GC_PAGE_LG2 12 // log2(size of a page)
#else
#define GC_PAGE_LG2 14 // log2(size of a page)
#endif
#define GC_PAGE_SZ (1 << GC_PAGE_LG2)
#define GC_PAGE_OFFSET (JL_HEAP_ALIGNMENT - (sizeof(jl_taggedvalue_t) % JL_HEAP_ALIGNMENT))

// Used by GC_DEBUG_ENV
typedef struct {
    uint64_t num;
    uint64_t next;
    uint64_t min;
    uint64_t interv;
    uint64_t max;
    unsigned short random[3];
} jl_alloc_num_t;

typedef struct {
    int wait_for_debugger;
    jl_alloc_num_t pool;
    jl_alloc_num_t other;
    jl_alloc_num_t print;
} jl_gc_debug_env_t;

// Array chunks (work items representing suffixes of
// large arrays of pointers left to be marked)

typedef enum {
    GC_empty_chunk = 0, // for sentinel representing no items left in chunk queue
    GC_objary_chunk,    // for chunk of object array
    GC_ary8_chunk,      // for chunk of array with 8 bit field descriptors
    GC_ary16_chunk,     // for chunk of array with 16 bit field descriptors
    GC_finlist_chunk,   // for chunk of finalizer list
} gc_chunk_id_t;

typedef struct _jl_gc_chunk_t {
    gc_chunk_id_t cid;
    struct _jl_value_t *parent; // array owner
    struct _jl_value_t **begin; // pointer to first element that needs scanning
    struct _jl_value_t **end;   // pointer to last element that needs scanning
    void *elem_begin;           // used to scan pointers within objects when marking `ary8` or `ary16`
    void *elem_end;             // used to scan pointers within objects when marking `ary8` or `ary16`
    uint32_t step;              // step-size used when marking objarray
    uintptr_t nptr;             // (`nptr` & 0x1) if array has young element and (`nptr` & 0x2) if array owner is old
} jl_gc_chunk_t;

#define GC_CHUNK_BATCH_SIZE (1 << 16)       // maximum number of references that can be processed
                                            // without creating a chunk

#define GC_PTR_QUEUE_INIT_SIZE (1 << 18)    // initial size of queue of `jl_value_t *`
#define GC_CHUNK_QUEUE_INIT_SIZE (1 << 14)  // initial size of chunk-queue

#define GC_REMSET_PTR_TAG (0x1)             // lowest bit of `jl_value_t *` is tagged if it's in the remset

// layout for big (>2k) objects

extern uintptr_t gc_bigval_sentinel_tag;

// pool page metadata
typedef struct _jl_gc_pagemeta_t {
    // next metadata structure in per-thread list
    // or in one of the `jl_gc_page_stack_t`
    struct _jl_gc_pagemeta_t *next;
    // index of pool that owns this page
    uint8_t pool_n;
    // Whether any cell in the page is marked
    // This bit is set before sweeping iff there are live cells in the page.
    // Note that before marking or after sweeping there can be live
    // (and young) cells in the page for `!has_marked`.
    uint8_t has_marked;
    // Whether any cell was live and young **before sweeping**.
    // For a normal sweep (quick sweep that is NOT preceded by a
    // full sweep) this bit is set iff there are young or newly dead
    // objects in the page and the page needs to be swept.
    //
    // For a full sweep, this bit should be ignored.
    //
    // For a quick sweep preceded by a full sweep. If this bit is set,
    // the page needs to be swept. If this bit is not set, there could
    // still be old dead objects in the page and `nold` and `prev_nold`
    // should be used to determine if the page needs to be swept.
    uint8_t has_young;
    // number of old objects in this page
    uint16_t nold;
    // number of old objects in this page during the previous full sweep
    uint16_t prev_nold;
    // number of free objects in this page.
    // invalid if pool that owns this page is allocating objects from this page.
    uint16_t nfree;
    uint16_t osize;           // size of each object in this page
    uint16_t fl_begin_offset; // offset of first free object in this page
    uint16_t fl_end_offset;   // offset of last free object in this page
    uint16_t thread_n;        // thread id of the heap that owns this page
    char *data;
} jl_gc_pagemeta_t;

extern jl_gc_page_stack_t global_page_pool_lazily_freed;
extern jl_gc_page_stack_t global_page_pool_clean;
extern jl_gc_page_stack_t global_page_pool_freed;

// Lock-free stack implementation taken
// from Herlihy's "The Art of Multiprocessor Programming"
// XXX: this is not a general-purpose lock-free stack. We can
// get away with just using a CAS and not implementing some ABA
// prevention mechanism since once a node is popped from the
// `jl_gc_page_stack_t`, it may only be pushed back to them
// in the sweeping phase, which also doesn't push a node into the
// same stack after it's popped

STATIC_INLINE void push_lf_back_nosync(jl_gc_page_stack_t *pool, jl_gc_pagemeta_t *elt) JL_NOTSAFEPOINT
{
    jl_gc_pagemeta_t *old_back = jl_atomic_load_relaxed(&pool->bottom);
    elt->next = old_back;
    jl_atomic_store_relaxed(&pool->bottom, elt);
}

STATIC_INLINE jl_gc_pagemeta_t *pop_lf_back_nosync(jl_gc_page_stack_t *pool) JL_NOTSAFEPOINT
{
    jl_gc_pagemeta_t *old_back = jl_atomic_load_relaxed(&pool->bottom);
    if (old_back == NULL) {
        return NULL;
    }
    jl_atomic_store_relaxed(&pool->bottom, old_back->next);
    return old_back;
}

STATIC_INLINE void push_lf_back(jl_gc_page_stack_t *pool, jl_gc_pagemeta_t *elt) JL_NOTSAFEPOINT
{
    while (1) {
        jl_gc_pagemeta_t *old_back = jl_atomic_load_relaxed(&pool->bottom);
        elt->next = old_back;
        if (jl_atomic_cmpswap(&pool->bottom, &old_back, elt)) {
            break;
        }
        jl_cpu_pause();
    }
}

#define MAX_POP_ATTEMPTS (1 << 10)

STATIC_INLINE jl_gc_pagemeta_t *try_pop_lf_back(jl_gc_page_stack_t *pool) JL_NOTSAFEPOINT
{
    for (int i = 0; i < MAX_POP_ATTEMPTS; i++) {
        jl_gc_pagemeta_t *old_back = jl_atomic_load_relaxed(&pool->bottom);
        if (old_back == NULL) {
            return NULL;
        }
        if (jl_atomic_cmpswap(&pool->bottom, &old_back, old_back->next)) {
            return old_back;
        }
        jl_cpu_pause();
    }
    return NULL;
}

STATIC_INLINE jl_gc_pagemeta_t *pop_lf_back(jl_gc_page_stack_t *pool) JL_NOTSAFEPOINT
{
    while (1) {
        jl_gc_pagemeta_t *old_back = jl_atomic_load_relaxed(&pool->bottom);
        if (old_back == NULL) {
            return NULL;
        }
        if (jl_atomic_cmpswap(&pool->bottom, &old_back, old_back->next)) {
            return old_back;
        }
        jl_cpu_pause();
    }
}
typedef struct {
    jl_gc_page_stack_t stack;
    // pad to 128 bytes to avoid false-sharing
#ifdef _P64
    void *_pad[15];
#else
    void *_pad[31];
#endif
} jl_gc_padded_page_stack_t;
static_assert(sizeof(jl_gc_padded_page_stack_t) == 128, "jl_gc_padded_page_stack_t is not 128 bytes");

typedef struct {
    _Atomic(size_t) n_freed_objs;
    _Atomic(size_t) n_pages_allocd;
} gc_fragmentation_stat_t;

#ifdef GC_SMALL_PAGE
#ifdef _P64
#define REGION0_PG_COUNT (1 << 16)
#define REGION1_PG_COUNT (1 << 18)
#define REGION2_PG_COUNT (1 << 18)
#define REGION0_INDEX(p) (((uintptr_t)(p) >> 12) & 0xFFFF) // shift by GC_PAGE_LG2
#define REGION1_INDEX(p) (((uintptr_t)(p) >> 28) & 0x3FFFF)
#define REGION_INDEX(p)  (((uintptr_t)(p) >> 46) & 0x3FFFF)
#else
#define REGION0_PG_COUNT (1 << 10)
#define REGION1_PG_COUNT (1 << 10)
#define REGION2_PG_COUNT (1 << 0)
#define REGION0_INDEX(p) (((uintptr_t)(p) >> 12) & 0x3FF) // shift by GC_PAGE_LG2
#define REGION1_INDEX(p) (((uintptr_t)(p) >> 22) & 0x3FF)
#define REGION_INDEX(p)  (0)
#endif
#else
#ifdef _P64
#define REGION0_PG_COUNT (1 << 16)
#define REGION1_PG_COUNT (1 << 16)
#define REGION2_PG_COUNT (1 << 18)
#define REGION0_INDEX(p) (((uintptr_t)(p) >> 14) & 0xFFFF) // shift by GC_PAGE_LG2
#define REGION1_INDEX(p) (((uintptr_t)(p) >> 30) & 0xFFFF)
#define REGION_INDEX(p)  (((uintptr_t)(p) >> 46) & 0x3FFFF)
#else
#define REGION0_PG_COUNT (1 << 8)
#define REGION1_PG_COUNT (1 << 10)
#define REGION2_PG_COUNT (1 << 0)
#define REGION0_INDEX(p) (((uintptr_t)(p) >> 14) & 0xFF) // shift by GC_PAGE_LG2
#define REGION1_INDEX(p) (((uintptr_t)(p) >> 22) & 0x3FF)
#define REGION_INDEX(p)  (0)
#endif
#endif

// define the representation of the levels of the page-table (0 to 2)
typedef struct {
    uint8_t meta[REGION0_PG_COUNT];
} pagetable0_t;

typedef struct {
    pagetable0_t *meta0[REGION1_PG_COUNT];
} pagetable1_t;

typedef struct {
    pagetable1_t *meta1[REGION2_PG_COUNT];
} pagetable_t;

typedef struct {
    _Atomic(size_t) bytes_mapped;
    _Atomic(size_t) bytes_resident;
    _Atomic(size_t) heap_size;
    _Atomic(size_t) heap_target;
} gc_heapstatus_t;

#define GC_PAGE_UNMAPPED        0
#define GC_PAGE_ALLOCATED       1
#define GC_PAGE_LAZILY_FREED    2
#define GC_PAGE_FREED           3

extern pagetable_t alloc_map;

STATIC_INLINE uint8_t gc_alloc_map_is_set(char *_data) JL_NOTSAFEPOINT
{
    uintptr_t data = ((uintptr_t)_data);
    unsigned i;
    i = REGION_INDEX(data);
    pagetable1_t *r1 = alloc_map.meta1[i];
    if (r1 == NULL)
        return 0;
    i = REGION1_INDEX(data);
    pagetable0_t *r0 = r1->meta0[i];
    if (r0 == NULL)
        return 0;
    i = REGION0_INDEX(data);
    return (r0->meta[i] == GC_PAGE_ALLOCATED);
}

STATIC_INLINE void gc_alloc_map_set(char *_data, uint8_t v) JL_NOTSAFEPOINT
{
    uintptr_t data = ((uintptr_t)_data);
    unsigned i;
    i = REGION_INDEX(data);
    pagetable1_t *r1 = alloc_map.meta1[i];
    assert(r1 != NULL);
    i = REGION1_INDEX(data);
    pagetable0_t *r0 = r1->meta0[i];
    assert(r0 != NULL);
    i = REGION0_INDEX(data);
    r0->meta[i] = v;
}

STATIC_INLINE void gc_alloc_map_maybe_create(char *_data) JL_NOTSAFEPOINT
{
    uintptr_t data = ((uintptr_t)_data);
    unsigned i;
    i = REGION_INDEX(data);
    pagetable1_t *r1 = alloc_map.meta1[i];
    if (r1 == NULL) {
        r1 = (pagetable1_t*)calloc_s(sizeof(pagetable1_t));
        alloc_map.meta1[i] = r1;
    }
    i = REGION1_INDEX(data);
    pagetable0_t *r0 = r1->meta0[i];
    if (r0 == NULL) {
        r0 = (pagetable0_t*)calloc_s(sizeof(pagetable0_t));
        r1->meta0[i] = r0;
    }
}

// Page layout:
//  Metadata pointer: sizeof(jl_gc_pagemeta_t*)
//  Padding: GC_PAGE_OFFSET - sizeof(jl_gc_pagemeta_t*)
//  Blocks: osize * n
//    Tag: sizeof(jl_taggedvalue_t)
//    Data: <= osize - sizeof(jl_taggedvalue_t)

STATIC_INLINE char *gc_page_data(void *x) JL_NOTSAFEPOINT
{
    return (char*)(((uintptr_t)x >> GC_PAGE_LG2) << GC_PAGE_LG2);
}

STATIC_INLINE jl_gc_pagemeta_t *page_metadata_unsafe(void *_data) JL_NOTSAFEPOINT
{
    return *(jl_gc_pagemeta_t**)(gc_page_data(_data));
}

STATIC_INLINE jl_gc_pagemeta_t *page_metadata(void *_data) JL_NOTSAFEPOINT
{
    if (!gc_alloc_map_is_set((char*)_data)) {
        return NULL;
    }
    return page_metadata_unsafe(_data);
}

STATIC_INLINE void set_page_metadata(jl_gc_pagemeta_t *pg) JL_NOTSAFEPOINT
{
    *(jl_gc_pagemeta_t**)(pg->data) = pg;
}

STATIC_INLINE void push_page_metadata_back(jl_gc_pagemeta_t **ppg, jl_gc_pagemeta_t *elt) JL_NOTSAFEPOINT
{
    elt->next = *ppg;
    *ppg = elt;
}

STATIC_INLINE jl_gc_pagemeta_t *pop_page_metadata_back(jl_gc_pagemeta_t **ppg) JL_NOTSAFEPOINT
{
    jl_gc_pagemeta_t *v = *ppg;
    if (*ppg != NULL) {
        *ppg = (*ppg)->next;
    }
    return v;
}

extern bigval_t *oldest_generation_of_bigvals;
extern int64_t buffered_pages;
extern int gc_first_tid;
extern gc_heapstatus_t gc_heap_stats;

STATIC_INLINE int gc_first_parallel_collector_thread_id(void) JL_NOTSAFEPOINT
{
    if (jl_n_markthreads == 0) {
        return 0;
    }
    return gc_first_tid;
}

STATIC_INLINE int gc_last_parallel_collector_thread_id(void) JL_NOTSAFEPOINT
{
    if (jl_n_markthreads == 0) {
        return -1;
    }
    return gc_first_tid + jl_n_markthreads - 1;
}

STATIC_INLINE int gc_ith_parallel_collector_thread_id(int i) JL_NOTSAFEPOINT
{
    assert(i >= 0 && i < jl_n_markthreads);
    return gc_first_tid + i;
}

STATIC_INLINE int gc_is_parallel_collector_thread(int tid) JL_NOTSAFEPOINT
{
    return tid >= gc_first_tid && tid <= gc_last_parallel_collector_thread_id();
}

STATIC_INLINE int gc_is_concurrent_collector_thread(int tid) JL_NOTSAFEPOINT
{
    if (jl_n_sweepthreads == 0) {
        return 0;
    }
    int last_parallel_collector_thread_id = gc_last_parallel_collector_thread_id();
    int concurrent_collector_thread_id = last_parallel_collector_thread_id + 1;
    return tid == concurrent_collector_thread_id;
}

STATIC_INLINE int gc_random_parallel_collector_thread_id(jl_ptls_t ptls) JL_NOTSAFEPOINT
{
    assert(jl_n_markthreads > 0);
    int v = gc_first_tid + (int)cong(jl_n_markthreads, &ptls->rngseed); // cong is [0, n)
    assert(v >= gc_first_tid && v <= gc_last_parallel_collector_thread_id());
    return v;
}

STATIC_INLINE int gc_parallel_collector_threads_enabled(void) JL_NOTSAFEPOINT
{
    return jl_n_markthreads > 0;
}

STATIC_INLINE void gc_check_ptls_of_parallel_collector_thread(jl_ptls_t ptls) JL_NOTSAFEPOINT
{
    (void)ptls;
    assert(gc_parallel_collector_threads_enabled());
    assert(ptls != NULL);
    assert(jl_atomic_load_relaxed(&ptls->gc_state) == JL_GC_PARALLEL_COLLECTOR_THREAD);
}

STATIC_INLINE bigval_t *bigval_header(jl_taggedvalue_t *o) JL_NOTSAFEPOINT
{
    return container_of(o, bigval_t, header);
}

STATIC_INLINE jl_taggedvalue_t *page_pfl_beg(jl_gc_pagemeta_t *p) JL_NOTSAFEPOINT
{
    return (jl_taggedvalue_t*)(p->data + p->fl_begin_offset);
}

STATIC_INLINE jl_taggedvalue_t *page_pfl_end(jl_gc_pagemeta_t *p) JL_NOTSAFEPOINT
{
    return (jl_taggedvalue_t*)(p->data + p->fl_end_offset);
}

FORCE_INLINE void gc_big_object_unlink(const bigval_t *node) JL_NOTSAFEPOINT
{
    assert(node != oldest_generation_of_bigvals);
    assert(node->header != gc_bigval_sentinel_tag);
    assert(node->prev != NULL);
    if (node->next != NULL) {
        node->next->prev = node->prev;
    }
    node->prev->next = node->next;
}

FORCE_INLINE void gc_big_object_link(bigval_t *sentinel_node, bigval_t *node) JL_NOTSAFEPOINT
{
    assert(sentinel_node != NULL);
    assert(sentinel_node->header == gc_bigval_sentinel_tag);
    assert(sentinel_node->prev == NULL);
    assert(node->header != gc_bigval_sentinel_tag);
    // a new node gets linked in at the head of the list
    node->next = sentinel_node->next;
    node->prev = sentinel_node;
    if (sentinel_node->next != NULL) {
        sentinel_node->next->prev = node;
    }
    sentinel_node->next = node;
}

// Must be kept in sync with `base/timing.jl`
#define FULL_SWEEP_REASON_SWEEP_ALWAYS_FULL (0)
#define FULL_SWEEP_REASON_FORCED_FULL_SWEEP (1)
#define FULL_SWEEP_REASON_USER_MAX_EXCEEDED (2)
#define FULL_SWEEP_REASON_LARGE_PROMOTION_RATE (3)
#define FULL_SWEEP_NUM_REASONS (4)

extern JL_DLLEXPORT uint64_t jl_full_sweep_reasons[FULL_SWEEP_NUM_REASONS];
STATIC_INLINE void gc_count_full_sweep_reason(int reason) JL_NOTSAFEPOINT
{
    assert(reason >= 0 && reason < FULL_SWEEP_NUM_REASONS);
    jl_full_sweep_reasons[reason]++;
}

extern uv_mutex_t gc_perm_lock;
extern uv_mutex_t gc_threads_lock;
extern uv_cond_t gc_threads_cond;
extern uv_sem_t gc_sweep_assists_needed;
extern _Atomic(int) gc_n_threads_marking;
extern _Atomic(int) gc_n_threads_sweeping_pools;
extern _Atomic(int) n_threads_running;
extern uv_barrier_t thread_init_done;
void gc_mark_queue_all_roots(jl_ptls_t ptls, jl_gc_markqueue_t *mq);
void gc_mark_finlist_(jl_gc_markqueue_t *mq, jl_value_t *fl_parent, jl_value_t **fl_begin, jl_value_t **fl_end) JL_NOTSAFEPOINT;
void gc_mark_finlist(jl_gc_markqueue_t *mq, arraylist_t *list, size_t start) JL_NOTSAFEPOINT;
void gc_mark_loop_serial_(jl_ptls_t ptls, jl_gc_markqueue_t *mq);
void gc_mark_loop_serial(jl_ptls_t ptls);
void gc_mark_loop_parallel(jl_ptls_t ptls, int master);
void gc_sweep_pool_parallel(jl_ptls_t ptls);
void gc_free_pages(void);
void sweep_stack_pool_loop(void) JL_NOTSAFEPOINT;
void jl_gc_debug_init(void);

// GC pages

extern uv_mutex_t gc_pages_lock;
void jl_gc_init_page(void) JL_NOTSAFEPOINT;
NOINLINE jl_gc_pagemeta_t *jl_gc_alloc_page(void) JL_NOTSAFEPOINT;
void jl_gc_free_page(jl_gc_pagemeta_t *p) JL_NOTSAFEPOINT;

// GC debug

#if defined(GC_TIME) || defined(GC_FINAL_STATS)
void gc_settime_premark_end(void);
void gc_settime_postmark_end(void);
#else
#define gc_settime_premark_end()
#define gc_settime_postmark_end()
#endif

#ifdef GC_FINAL_STATS
void gc_final_count_page(size_t pg_cnt);
void gc_final_pause_end(int64_t t0, int64_t tend);
#else
#define gc_final_count_page(pg_cnt)
#define gc_final_pause_end(t0, tend)
#endif

#ifdef GC_TIME
void gc_time_pool_start(void) JL_NOTSAFEPOINT;
void gc_time_count_page(int freedall, int pg_skpd) JL_NOTSAFEPOINT;
void gc_time_pool_end(int sweep_full) JL_NOTSAFEPOINT;
void gc_time_sysimg_end(uint64_t t0) JL_NOTSAFEPOINT;

void gc_time_big_start(void) JL_NOTSAFEPOINT;
void gc_time_count_big(int old_bits, int bits) JL_NOTSAFEPOINT;
void gc_time_big_end(void) JL_NOTSAFEPOINT;

void gc_time_mallocd_memory_start(void) JL_NOTSAFEPOINT;
void gc_time_count_mallocd_memory(int bits) JL_NOTSAFEPOINT;
void gc_time_mallocd_memory_end(void) JL_NOTSAFEPOINT;

void gc_time_mark_pause(int64_t t0, int64_t scanned_bytes,
                        int64_t perm_scanned_bytes);
void gc_time_sweep_pause(uint64_t gc_end_t, int64_t actual_allocd,
                         int64_t live_bytes, int64_t estimate_freed,
                         int sweep_full);
void gc_time_summary(int sweep_full, uint64_t start, uint64_t end,
                     uint64_t freed, uint64_t live, uint64_t interval,
                     uint64_t pause, uint64_t ttsp, uint64_t mark,
                     uint64_t sweep);
void gc_heuristics_summary(
        uint64_t old_alloc_diff, uint64_t alloc_mem,
        uint64_t old_mut_time, uint64_t alloc_time,
        uint64_t old_freed_diff, uint64_t gc_mem,
        uint64_t old_pause_time, uint64_t gc_time,
        int thrash_counter, const char *reason,
        uint64_t current_heap, uint64_t target_heap);
#else
#define gc_time_pool_start()
STATIC_INLINE void gc_time_count_page(int freedall, int pg_skpd) JL_NOTSAFEPOINT
{
    (void)freedall;
    (void)pg_skpd;
}
#define gc_time_pool_end(sweep_full) (void)(sweep_full)
#define gc_time_sysimg_end(t0) (void)(t0)
#define gc_time_big_start()
STATIC_INLINE void gc_time_count_big(int old_bits, int bits) JL_NOTSAFEPOINT
{
    (void)old_bits;
    (void)bits;
}
#define gc_time_big_end()
#define gc_time_mallocd_memory_start()
STATIC_INLINE void gc_time_count_mallocd_memory(int bits) JL_NOTSAFEPOINT
{
    (void)bits;
}
#define gc_time_mallocd_memory_end()
#define gc_time_mark_pause(t0, scanned_bytes, perm_scanned_bytes)
#define gc_time_sweep_pause(gc_end_t, actual_allocd, live_bytes,        \
                            estimate_freed, sweep_full)
#define  gc_time_summary(sweep_full, start, end, freed, live,           \
                         interval, pause, ttsp, mark, sweep)
#define gc_heuristics_summary( \
        old_alloc_diff, alloc_mem, \
        old_mut_time, alloc_time, \
        old_freed_diff, gc_mem, \
        old_pause_time, gc_time, \
        thrash_counter, reason, \
        current_heap, target_heap)
#endif

#ifdef MEMFENCE
void gc_verify_tags(void);
#else
static inline void gc_verify_tags(void)
{
}
#endif

#ifdef GC_VERIFY
extern jl_value_t *lostval;
void gc_verify(jl_ptls_t ptls);
void add_lostval_parent(jl_value_t *parent);
#define verify_val(v) do {                                              \
        if (lostval == (jl_value_t*)(v) && (v) != 0) {                  \
            jl_printf(JL_STDOUT,                                        \
                      "Found lostval %p at %s:%d oftype: ",             \
                      (void*)(lostval), __FILE__, __LINE__);            \
            jl_static_show(JL_STDOUT, jl_typeof(v));                    \
            jl_printf(JL_STDOUT, "\n");                                 \
        }                                                               \
    } while(0);

#define verify_parent(ty, obj, slot, args...) do {                      \
        if (gc_ptr_clear_tag(*(void**)(slot), 3) == (void*)lostval &&   \
            (jl_value_t*)(obj) != lostval) {                            \
            jl_printf(JL_STDOUT, "Found parent %p %p at %s:%d\n",       \
                      (void*)(ty), (void*)(obj), __FILE__, __LINE__);   \
            jl_printf(JL_STDOUT, "\tloc %p : ", (void*)(slot));         \
            jl_printf(JL_STDOUT, args);                                 \
            jl_printf(JL_STDOUT, "\n");                                 \
            jl_printf(JL_STDOUT, "\ttype: ");                           \
            jl_static_show(JL_STDOUT, jl_typeof(obj));                  \
            jl_printf(JL_STDOUT, "\n");                                 \
            add_lostval_parent((jl_value_t*)(obj));                     \
        }                                                               \
    } while(0);

#define verify_parent1(ty,obj,slot,arg1) verify_parent(ty,obj,slot,arg1)
#define verify_parent2(ty,obj,slot,arg1,arg2) verify_parent(ty,obj,slot,arg1,arg2)
extern int gc_verifying;
#else
#define gc_verify(ptls)
#define verify_val(v)
#define verify_parent1(ty,obj,slot,arg1) do {} while (0)
#define verify_parent2(ty,obj,slot,arg1,arg2) do {} while (0)
#define gc_verifying (0)
#endif

int gc_slot_to_fieldidx(void *_obj, void *slot, jl_datatype_t *vt) JL_NOTSAFEPOINT;
int gc_slot_to_arrayidx(void *_obj, void *begin) JL_NOTSAFEPOINT;
NOINLINE void gc_mark_loop_unwind(jl_ptls_t ptls, jl_gc_markqueue_t *mq, int offset) JL_NOTSAFEPOINT;

#ifdef GC_DEBUG_ENV
JL_DLLEXPORT extern jl_gc_debug_env_t jl_gc_debug_env;
int jl_gc_debug_check_other(void);
int gc_debug_check_pool(void);
void jl_gc_debug_print(void);
void gc_scrub_record_task(jl_task_t *ta) JL_NOTSAFEPOINT;
void gc_scrub(void);
#else
static inline int jl_gc_debug_check_other(void)
{
    return 0;
}
static inline int gc_debug_check_pool(void)
{
    return 0;
}
static inline void jl_gc_debug_print(void)
{
}
static inline void gc_scrub_record_task(jl_task_t *ta) JL_NOTSAFEPOINT
{
    (void)ta;
}
static inline void gc_scrub(void)
{
}
#endif

#ifdef MEMPROFILE
void gc_stats_all_pool(void);
void gc_stats_big_obj(void);
#else
#define gc_stats_all_pool()
#define gc_stats_big_obj()
#endif

// For debugging
void gc_count_pool(void);

JL_DLLEXPORT void jl_enable_gc_logging(int enable);
JL_DLLEXPORT int jl_is_gc_logging_enabled(void);
JL_DLLEXPORT uint32_t jl_get_num_stack_mappings(void);
void _report_gc_finished(uint64_t pause, uint64_t freed, int full, int recollect, int64_t live_bytes) JL_NOTSAFEPOINT;

#ifdef __cplusplus
}
#endif

#endif
