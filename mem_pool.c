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
    if(_mem_resize_pool_store() != ALLOC_OK){
        return  NULL;
    }

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
    poolMgr->pool.num_gaps = 0;
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
    _mem_add_to_gap_ix(poolMgr, size, &nodeHeap[0]);

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
    if (poolMgr->pool.num_gaps > 1) {
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
    pool_mgr_pt poolMgr = (pool_mgr_pt) pool;

    // check if any gaps, return null if none
    if (poolMgr->gap_ix_capacity == 0){
        return NULL;
    }

    // expand heap node, if necessary, quit on error
    if (_mem_resize_node_heap(poolMgr) !=ALLOC_OK){
        return NULL;
    }

    // check used nodes fewer than total nodes, quit on error
    if (poolMgr->total_nodes <= poolMgr->used_nodes){
        return NULL;
    }
    // get a node for allocation:
    node_pt nodeForAlloc = NULL;

    // if FIRST_FIT, then find the first sufficient node in the node heap
    if (poolMgr->pool.policy==FIRST_FIT){
        nodeForAlloc = poolMgr->node_heap;
        while (nodeForAlloc->allocated == 1 || nodeForAlloc->alloc_record.size < size){
            if(nodeForAlloc->next == NULL){
                return NULL;
            }
            nodeForAlloc = nodeForAlloc->next;
        }
    }

    // if BEST_FIT, then find the first sufficient node in the gap index
    else {
        int check = 1;
        int nodeIndex = 0;
        while (check && nodeIndex < poolMgr->pool.num_gaps){
            if (poolMgr->gap_ix[nodeIndex].size >= size){
                check = 0;
                nodeForAlloc = poolMgr->gap_ix[nodeIndex].node;
            }
            else{
                nodeIndex =  nodeIndex +1;
            }
        }

        // check if node found
        if(nodeIndex == poolMgr->pool.num_gaps) {
            return NULL;
        }
    }

    // update metadata (num_allocs, alloc_size)
    poolMgr->pool.num_allocs++;
    poolMgr->pool.alloc_size = poolMgr->pool.alloc_size + size;

    // calculate the size of the remaining gap, if any
    size_t remainingSize = nodeForAlloc->alloc_record.size - size;

    // remove node from gap index
    _mem_remove_from_gap_ix(poolMgr, nodeForAlloc->alloc_record.size, nodeForAlloc);

    // convert gap_node to an allocation node of given size
    nodeForAlloc->alloc_record.size = size;
    nodeForAlloc->allocated = 1;

    // adjust node heap:
    //   if remaining gap, need a new node
    //   find an unused one in the node heap
    if (remainingSize > 0){
        node_pt newGapNode = NULL;
        for (int i = 0; i < poolMgr->total_nodes; ++i) {
            if (poolMgr->node_heap[i].used == 0){
                newGapNode = &poolMgr->node_heap[i];
                break;
            }
        }

        //   initialize it to a gap node
        newGapNode->used = 1;
        newGapNode->alloc_record.size = remainingSize;
        newGapNode->alloc_record.mem = nodeForAlloc->alloc_record.mem + size;
        newGapNode->allocated = 0;

        //   update metadata (used_nodes)
        poolMgr->used_nodes++;

        //   update linked list (new node right after the node for allocation)
        newGapNode->next = nodeForAlloc->next;
        newGapNode->prev = nodeForAlloc;
        if (nodeForAlloc->next != NULL){
            nodeForAlloc->next->prev = newGapNode;
        }
        nodeForAlloc->next = newGapNode;

        //   add to gap index
        if(_mem_add_to_gap_ix(poolMgr,newGapNode->alloc_record.size, newGapNode) != ALLOC_OK){
            return NULL;
        }
    }
    //   make sure one was found
    //for (int i = 0; i < poolMgr->total_nodes; ++i) {
    //   if(&poolMgr->node_heap[i] == NULL){
    //
    //    }
    //}

    // return allocation record by casting the node to (alloc_pt)
    return (alloc_pt)nodeForAlloc;
}

alloc_status mem_del_alloc(pool_pt pool, void * alloc) {
    // get mgr from pool by casting the pointer to (pool_mgr_pt)
    pool_mgr_pt poolMgr = (pool_mgr_pt) pool;
    // get node from alloc by casting the pointer to (node_pt)
    node_pt nodePt = (node_pt)alloc;
    // find the node in the node heap
    // this is node-to-delete (nodePt)
    // make sure it's found
    if(nodePt == NULL){
        return ALLOC_FAIL;
    }
    // convert to gap node
    nodePt->allocated = 0;
    // update metadata (num_allocs, alloc_size)
    poolMgr->pool.num_allocs--;
    poolMgr->pool.alloc_size = poolMgr->pool.alloc_size - nodePt->alloc_record.size;
    // if the next node in the list is also a gap, merge into node-to-delete
    if (nodePt->next !=NULL && nodePt->next->allocated == 0 && nodePt->next->used){
        node_pt next = nodePt->next;
        //   remove the next node from gap index
        //   check success
        if(_mem_remove_from_gap_ix(poolMgr,next->alloc_record.size, next) != ALLOC_OK){
            return ALLOC_FAIL;
        }
        //   add the size to the node-to-delete
        nodePt->alloc_record.size = nodePt->alloc_record.size + next->alloc_record.size;
        //   update node as unused
        next->used = 0;
        //   update metadata (used nodes)
        poolMgr->used_nodes--;
        //   update linked list:

        if (next->next) {
            next->next->prev = nodePt;
            nodePt->next = next->next;
        } else {
            nodePt->next = NULL;
        }
        next->next = NULL;
        next->prev = NULL;

    }
    // this merged node-to-delete might need to be added to the gap index
    // but one more thing to check...
    // if the previous node in the list is also a gap, merge into previous!
    if(nodePt->prev != NULL && nodePt->prev->allocated == 0 && nodePt->prev->used){
        node_pt prev = nodePt->prev;
        //   remove the previous node from gap index
        //   check success
        if(_mem_remove_from_gap_ix(poolMgr,prev->alloc_record.size, prev) != ALLOC_OK){
            return ALLOC_FAIL;
        }
        //   add the size of node-to-delete to the previous
        prev->alloc_record.size = prev->alloc_record.size + nodePt->alloc_record.size;
        //   update node-to-delete as unused
        nodePt->used = 0;
        //   update metadata (used_nodes)
        poolMgr->used_nodes--;
        //   update linked list
        if (nodePt->next) {
            prev->next = nodePt->next;
            nodePt->next->prev = prev;
        } else {
            prev->next = NULL;
        }
        nodePt->next = NULL;
        nodePt->prev = NULL;
        // change the node to add to the previous node!
        nodePt = prev;
    }
    // add the resulting node to the gap index
    // check success
    if(_mem_add_to_gap_ix(poolMgr, nodePt->alloc_record.size, nodePt) != ALLOC_OK){
        return ALLOC_FAIL;
    }
    return ALLOC_OK;
}

void mem_inspect_pool(pool_pt pool,
                      pool_segment_pt *segments,
                      unsigned *num_segments) {
    // get the mgr from the pool
    pool_mgr_pt poolMgr = (pool_mgr_pt) pool;

    // allocate the segments array with size == used_nodes
    // check successful
    pool_segment_pt segmentArray = calloc(poolMgr->used_nodes, sizeof(pool_segment_t));
    if (segmentArray == NULL){
        segments = NULL;
        return;
    }

    // loop through the node heap and the segments array
    //    for each node, write the size and allocated in the segment
    node_pt currentNode = poolMgr->node_heap;
    for (int i = 0; i < poolMgr->used_nodes; i++){
        segmentArray[i].size = currentNode->alloc_record.size;
        segmentArray[i].allocated = currentNode->allocated;
        currentNode = currentNode->next;
    }

    // "return" the values:
    *segments = segmentArray;
    *num_segments = poolMgr->used_nodes;
}



/***********************************/
/*                                 */
/* Definitions of static functions */
/*                                 */
/***********************************/
static alloc_status _mem_resize_pool_store() {
    // check if necessary

    if (((float) pool_store_size / pool_store_capacity)
        > MEM_POOL_STORE_FILL_FACTOR) {
        pool_store = realloc(pool_store, sizeof(pool_mgr_pt)*(pool_store_size * MEM_POOL_STORE_EXPAND_FACTOR));
        pool_store_capacity = pool_store_capacity + (pool_store_size*MEM_POOL_STORE_EXPAND_FACTOR - pool_store_size);
        pool_store_size = pool_store_size *MEM_POOL_STORE_EXPAND_FACTOR;
    }

    // don't forget to update capacity variables

    return ALLOC_OK;
}

static alloc_status _mem_resize_node_heap(pool_mgr_pt pool_mgr) {
    // see above
    if(((float) pool_mgr->used_nodes/pool_mgr->total_nodes) > MEM_NODE_HEAP_FILL_FACTOR){
        //create new heap
        node_pt tempNodeHeap = calloc(pool_mgr->total_nodes*MEM_NODE_HEAP_EXPAND_FACTOR, sizeof(node_t));
        _mem_invalidate_gap_ix(pool_mgr);
        node_pt currentNode = pool_mgr->node_heap;
        unsigned check = 1;
        unsigned newNodeIX = 0;
        while(check){
            if (currentNode == NULL){
                check = 0;
            }
            else{
                if(newNodeIX > 0) {
                    tempNodeHeap[newNodeIX].prev = &tempNodeHeap[newNodeIX-1];
                    tempNodeHeap[newNodeIX-1].next = &tempNodeHeap[newNodeIX];
                }
                tempNodeHeap[newNodeIX].used = currentNode->used;
                tempNodeHeap[newNodeIX].allocated = currentNode->allocated;
                tempNodeHeap[newNodeIX].alloc_record.size = currentNode->alloc_record.size;
                tempNodeHeap[newNodeIX].alloc_record.mem = currentNode->alloc_record.mem;

                if(currentNode->used && (currentNode->allocated == 0)){
                    _mem_add_to_gap_ix(pool_mgr, tempNodeHeap[newNodeIX].alloc_record.size, &tempNodeHeap[newNodeIX]);
                }

                newNodeIX ++;
                currentNode = currentNode->next;
            }
        }

        pool_mgr->total_nodes = pool_mgr->total_nodes*MEM_NODE_HEAP_EXPAND_FACTOR;
        free(pool_mgr->node_heap);
        pool_mgr->node_heap = tempNodeHeap;

    }

    return ALLOC_OK;
}

static alloc_status _mem_resize_gap_ix(pool_mgr_pt pool_mgr) {
    // see above
    if(((float)pool_mgr->pool.num_gaps/pool_mgr->gap_ix_capacity)>MEM_GAP_IX_FILL_FACTOR){
        pool_mgr->gap_ix = realloc(pool_mgr->gap_ix, sizeof(gap_t)*pool_mgr->gap_ix_capacity*MEM_GAP_IX_EXPAND_FACTOR);
        pool_mgr->gap_ix_capacity = pool_mgr->gap_ix_capacity*MEM_GAP_IX_EXPAND_FACTOR;
    }
    return ALLOC_OK;
}

static alloc_status _mem_add_to_gap_ix(pool_mgr_pt pool_mgr,
                                       size_t size,
                                       node_pt node) {

    if(_mem_resize_gap_ix(pool_mgr) != ALLOC_OK){
        return ALLOC_FAIL;
    }
    

    // add the entry at the end
    // check success
    //TODO: How to Check Success?
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = size;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = node;

    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps ++;

    // sort the gap index (call the function)
    if(_mem_sort_gap_ix(pool_mgr) != ALLOC_OK) {
        return ALLOC_FAIL;
    }

    //return result;
    return ALLOC_OK;
}

static alloc_status _mem_remove_from_gap_ix(pool_mgr_pt pool_mgr,
                                            size_t size,
                                            node_pt node) {
    // find the position of the node in the gap index
    unsigned found = 0;
    int gapNodeIndex = 0;
    for (gapNodeIndex = 0; gapNodeIndex < pool_mgr->pool.num_gaps; ++gapNodeIndex) {
        if (pool_mgr->gap_ix[gapNodeIndex].node == node) {
            found = 1;
            break;
        }
    }
    if(!found) {
        return ALLOC_FAIL;
    }

    // loop from there to the end of the array:
    //    pull the entries (i.e. copy over) one position up
    //    this effectively deletes the chosen node
    while(gapNodeIndex < pool_mgr->pool.num_gaps) {
        pool_mgr->gap_ix[gapNodeIndex].size = pool_mgr->gap_ix[gapNodeIndex+1].size;
        pool_mgr->gap_ix[gapNodeIndex].node = pool_mgr->gap_ix[gapNodeIndex+1].node;
        gapNodeIndex++;
    }

    // update metadata (num_gaps)
    pool_mgr->pool.num_gaps--;
    // zero out the element at position num_gaps!
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].size = 0;
    pool_mgr->gap_ix[pool_mgr->pool.num_gaps].node = NULL;
    return ALLOC_OK;
}

// note: only called by _mem_add_to_gap_ix, which appends a single entry
static alloc_status _mem_sort_gap_ix(pool_mgr_pt pool_mgr) {
    if(pool_mgr->pool.num_gaps == 0){
        return ALLOC_FAIL;
    }
    // the new entry is at the end, so "bubble it up"
    // loop from num_gaps - 1 until but not including 0:
    for(int i = pool_mgr->pool.num_gaps - 1; i > 0; i--){
        //    if the size of the current entry is less than the previous (u - 1)
        //    or if the sizes are the same but the current entry points to a
        //    node with a lower address of pool allocation address (mem)
        gap_pt current = &pool_mgr->gap_ix[i];
        gap_pt previous = &pool_mgr->gap_ix[i-1];

        if
        (current->size < previous->size ||
        (current->size == previous->size &&
         current->node->alloc_record.mem < previous->node->alloc_record.mem))
        {
            //   swap them (by copying) (remember to use a temporary variable)
            gap_t temp;
            temp.node = current->node;
            temp.size = current->size;

            current->node = previous->node;
            current->size = previous->size;
            previous->node = temp.node;
            previous->size = temp.size;
        }
    }

    return ALLOC_OK;
}

static alloc_status _mem_invalidate_gap_ix(pool_mgr_pt pool_mgr) {
    for (int i = 0; i < pool_mgr->pool.num_gaps; ++i) {
        pool_mgr->gap_ix[i].size = 0;
        pool_mgr->gap_ix[i].node = NULL;
    }
    pool_mgr->pool.num_gaps = 0;
    return ALLOC_OK;
}

