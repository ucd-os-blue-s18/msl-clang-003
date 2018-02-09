/*
 * Created by Ivo Georgiev on 2/9/16.
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h> // for perror()

#include "mem_pool.h"

/*************/
/*           */
/* Constants */
/*           */
/*************/
static const float      MEM_FILL_FACTOR                 = 0.75;
static const unsigned   MEM_EXPAND_FACTOR               = 2;

static const unsigned   MEM_POOL_STORE_INIT_CAPACITY    = 20;
static const float      MEM_POOL_STORE_FILL_FACTOR      = 0.75;
static const unsigned   MEM_POOL_STORE_EXPAND_FACTOR    = 2;

static const unsigned   MEM_NODE_HEAP_INIT_CAPACITY     = 40;
static const float      MEM_NODE_HEAP_FILL_FACTOR       = 0.75;
static const unsigned   MEM_NODE_HEAP_EXPAND_FACTOR     = 2;

static const unsigned   MEM_GAP_IX_INIT_CAPACITY        = 40;
static const float      MEM_GAP_IX_FILL_FACTOR          = 0.75;
static const unsigned   MEM_GAP_IX_EXPAND_FACTOR        = 2;



/*********************/
/*                   */
/* Type declarations */
/*                   */
/*********************/
typedef struct _alloc {
    char *mem;
    size_t size;
} alloc_t, *alloc_pt;

typedef struct _node {
    alloc_t alloc_record;
    unsigned used;
    unsigned allocated;
    struct _node *next, *prev; // doubly-linked list for gap deletion
} node_t, *node_pt;

typedef struct _gap {
    size_t size;
    node_pt node;
} gap_t, *gap_pt;

typedef struct _pool_mgr {
    pool_t pool;
    node_pt node_heap;
    unsigned total_nodes;
    unsigned used_nodes;
    gap_pt gap_ix;
    unsigned gap_ix_capacity;
} pool_mgr_t, *pool_mgr_pt;



/***************************/
/*                         */
/* Static global variables */
/*                         */
/***************************/
static pool_mgr_pt *pool_store = NULL; // an array of pointers, only expand
static unsigned pool_store_size = 0;
static unsigned pool_store_capacity = 0;



/********************************************/
/*                                          */
/* Forward declarations of static functions */
/*                                          */
/********************************************/
static alloc_status _mem_resize_pool_store();
static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr);
static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status
        _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                           size_t size,
                           node_pt node);
static alloc_status
        _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                size_t size,
                                node_pt node);
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr);
static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr);



/****************************************/
/*                                      */
/* Definitions of user-facing functions */
/*                                      */
/****************************************/
alloc_status mem_init() {
    // ensure that it's called only once until mem_free
    // allocate the pool store with initial capacity
    // note: holds pointers only, other functions to allocate/deallocate
    if(pool_store != NULL){
        return ALLOC_CALLED_AGAIN;
    }
    else {
        pool_store = calloc(MEM_POOL_STORE_INIT_CAPACITY, sizeof(pool_mgr_pt));
        //update tracking items ie static variables!
        pool_store_size = 0;
        pool_store_capacity = MEM_POOL_STORE_INIT_CAPACITY;
        return ALLOC_OK;
    }
}

alloc_status mem_free() {
    // ensure that it's called only once for each mem_init
    // make sure all pool managers have been deallocated
    // can free the pool store array
    // update static variables
    if(pool_store == NULL){
        return ALLOC_CALLED_AGAIN;
    }
    else{
        for (int i = 0; i < pool_store_size; ++i) {
            if (pool_store[i] != NULL){
                return ALLOC_NOT_FREED;
            }
        }
        free(pool_store);
        pool_store = NULL;
        pool_store_size = 0;
        pool_store_capacity = 0;
        return ALLOC_OK;
    }
}

pool_pt mem_pool_open(size_t size, alloc_policy policy) {
    // make sure there the pool store is allocated
    if(pool_store == NULL){
        return NULL;
    }

    // TODO: (bonus) expand the pool store, if necessary

    // allocate a new mem pool mgr
    // check success, on error return null
    pool_mgr_pt poolMgr = malloc(sizeof(pool_mgr_t));
    if(poolMgr == NULL) {
        return NULL;
    }

    // allocate a new memory pool
    // check success, on error deallocate mgr and return null
    char* poolMem = malloc(size);
    if (poolMem== NULL) {
        free(poolMgr);
        return NULL;
    }

    // allocate a new node heap
    // check success, on error deallocate mgr/pool and return null
    node_pt nodeHeap = calloc(MEM_NODE_HEAP_INIT_CAPACITY, sizeof(node_t));
    if (nodeHeap == NULL) {
        free(poolMem);
        free(poolMgr);
        return NULL;
    }

    // allocate a new gap index
    // check success, on error deallocate mgr/pool/heap and return null
    gap_pt gapIx = calloc(MEM_GAP_IX_INIT_CAPACITY, sizeof(gap_t));
    if(gapIx == NULL) {
        free(nodeHeap);
        free(poolMem);
        free(poolMgr);
        return NULL;
    }

    // assign all the pointers and update meta data:
    poolMgr->pool.mem = poolMem;
    poolMgr->pool.alloc_size = 0;
    poolMgr->pool.num_allocs = 0;
    poolMgr->pool.num_gaps = 1;
    poolMgr->pool.policy = policy;
    poolMgr->pool.total_size = size;

    poolMgr->node_heap = nodeHeap;
    poolMgr->total_nodes = MEM_NODE_HEAP_INIT_CAPACITY;
    poolMgr->used_nodes = 1;

    poolMgr->gap_ix = gapIx;
    poolMgr->gap_ix_capacity = MEM_GAP_IX_INIT_CAPACITY;


    //   initialize top node of node heap
    nodeHeap[0].used = 1;
    nodeHeap[0].allocated = 0;
    nodeHeap[0].alloc_record.mem = poolMem;
    nodeHeap[0].alloc_record.size = size;
    nodeHeap[0].next = NULL;
    nodeHeap[0].prev = NULL;

    //   initialize top node of gap index
    // TODO: do this by calling _mem_add_to_gap_ix (not yet implemented)
    gapIx[0].size = size;
    gapIx[0].node = &nodeHeap[0];

    //   initialize pool mgr
    //   link pool mgr to pool store
    pool_store[pool_store_size] = poolMgr;
    pool_store_size++;

    // return the address of the mgr, cast to (pool_pt)
    return (pool_pt) poolMgr;
}

alloc_status mem_pool_close(pool_pt pool) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt poolMgr = (pool_mgr_pt) pool;

    // check if this pool is allocated
    if (poolMgr == NULL) {
        return ALLOC_NOT_FREED;
    }

    // check if pool has only one gap
    // TODO: not sure if this is even the right check here
    if (poolMgr->pool.num_gaps != 1) {
        return ALLOC_NOT_FREED;
    }

    // check if it has zero allocations
    if (poolMgr->pool.num_allocs != 0) {
        return ALLOC_NOT_FREED;
    }

    // free memory pool
    free(poolMgr->pool.mem);

    // free node heap
    free(poolMgr->node_heap);

    // free gap index
    free(poolMgr->gap_ix);

    // find mgr in pool store and set to null
    for (int i = 0; i < pool_store_size; i++) {
        if(pool_store[i] == poolMgr) {
            pool_store[i] = NULL;
            break;
        }
    }

    // note: don't decrement pool_store_size, because it only grows
    // free mgr
    free(poolMgr);

    return ALLOC_OK;
}

void * mem_new_alloc(pool_pt pool, size_t size) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // check if any gaps, return null if none
    // expand heap node, if necessary, quit on error
    // check used nodes fewer than total nodes, quit on error
    // get a node for allocation:
    // if FIRST_FIT, then find the first sufficient node in the node heap
    // if BEST_FIT, then find the first sufficient node in the gap index
    // check if node found
    // update metadata (num_allocs, alloc_size)
    // calculate the size of the remaining gap, if any
    // remove node from gap index
    // convert gap_node to an allocation node of given size
    // adjust node heap:
    //   if remaining gap, need a new node
    //   find an unused one in the node heap
    //   make sure one was found
    //   initialize it to a gap node
    //   update metadata (used_nodes)
    //   update linked list (new node right after the node for allocation)
    //   add to gap index
    //   check if successful
    // return allocation record by casting the node to (alloc_pt)

    return NULL;
}

alloc_status mem_del_alloc(pool_pt pool, void * alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    // get node from alloc by casting the pointer to (node_pt)
    // find the node in the node heap
    // this is node-to-delete
    // make sure it's found
    // convert to gap node
    // update metadata (num_allocs, alloc_size)
    // if the next node in the list is also a gap, merge into node-to-delete
    //   remove the next node from gap index
    //   check success
    //   add the size to the node-to-delete
    //   update node as unused
    //   update metadata (used nodes)
    //   update linked list:
    /*
                    if (next->next) {
                        next->next->prev = node_to_del;
                        node_to_del->next = next->next;
                    } else {
                        node_to_del->next = NULL;
                    }
                    next->next = NULL;
                    next->prev = NULL;
     */

    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    //   remove the previous node from gap index
    //   check success
    //   add the size of node-to-delete to the previous
    //   update node-to-delete as unused
    //   update metadata (used_nodes)
    //   update linked list
    /*
                    if (node_to_del->next) {
                        prev->next = node_to_del->next;
                        node_to_del->next->prev = prev;
                    } else {
                        prev->next = NULL;
                    }
                    node_to_del->next = NULL;
                    node_to_del->prev = NULL;
     */
    //   change the node to add to the previous node!
    // add the resulting node to the gap index
    // check success

    return ALLOC_FAIL;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    // allocate the segments array with size == used_nodes
    // check successful
    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    // "return" the values:
    /*
                    *segments = segs;
                    *num_segments = pool_mgr->used_nodes;
     */
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary
    /*
                if (((float) pool_store_size / pool_store_capacity)
                    > MEM_POOL_STORE_FILL_FACTOR) {...}
     */
    // don't forget to update capacity variables

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    // see above

    return ALLOC_FAIL;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    // TODO: (bonus) expand the gap index, if necessary (call the function)
    // add the entry at the end
    // update metadata (num_gaps)
    // sort the gap index (call the function)
    // check success

    return ALLOC_FAIL;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index
    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    // update metadata (num_gaps)
    // zero out the element at position num_gaps!

    return ALLOC_FAIL;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    //    if the size of the current entry is less than the previous (u - 1)
    //    or if the sizes are the same but the current entry points to a
    //    node with a lower address of pool allocation address (mem)
    //       swap them (by copying) (remember to use a temporary variable)

    return ALLOC_FAIL;
}

static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr) {
    return ALLOC_FAIL;
}

