/* Copyright (C) 2014,2015 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file hierarchical-heap.c
 *
 * @author Ram Raghunathan
 *
 * This file implements the utility functions for the HierarchicalHeap object
 * described in hierarchical-heap.h.
 */

#include "hierarchical-heap.h"

/******************************/
/* Static Function Prototypes */
/******************************/
/**
 * Asserts all of the invariants assumed for the struct HM_HierarchicalHeap.
 *
 * @attention
 * If an assertion fails, this function aborts the program, as per the assert()
 * macro.
 *
 * @param s The GC_state to use
 * @param hh The struct HM_HierarchicalHeap to assert invariants for
 */
static void HM_HH_assertInvariants(GC_state s,
                                   const struct HM_HierarchicalHeap* hh);

/**
 * This function converts a hierarchical heap objptr to the struct
 * HM_HierarchicalHeap
 *
 * @param s The GC_state to use
 * @param hhObjptr the objptr to convert
 *
 * @return the contained struct HM_HierarchicalHeap if hhObjptr is a valid
 * objptr, NULL otherwise
 */
static struct HM_HierarchicalHeap* HHObjptrToStruct(GC_state s,
                                                    objptr hhObjptr);

/**
 * Gets the lock on 'hh'
 *
 * @param hh the struct HM_HierarchicalHeap* to lock
 */
static void lockHH(struct HM_HierarchicalHeap* hh);

/**
 * Releases the lock on 'hh'
 *
 * @param hh the struct HM_HierarchicalHeap* to unlock
 */
static void unlockHH(struct HM_HierarchicalHeap* hh);

/************************/
/* Function Definitions */
/************************/
#if (defined (MLTON_GC_INTERNAL_BASIS))
void HM_HH_appendChild(pointer parentHHPointer, pointer childHHPointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  objptr parentHHObjptr = pointerToObjptr (parentHHPointer, s->heap->start);
  struct HM_HierarchicalHeap* parentHH = HHObjptrToStruct(s, parentHHObjptr);

  objptr childHHObjptr = pointerToObjptr (childHHPointer, s->heap->start);
  struct HM_HierarchicalHeap* childHH = HHObjptrToStruct(s, childHHObjptr);

  lockHH(parentHH);
  lockHH(childHH);

  /* cannot assert parentHH as it is still running! */
  HM_HH_assertInvariants(s, childHH);

  /* childHH should be a orphan! */
  assert (BOGUS_OBJPTR == childHH->parentHH);
  assert (BOGUS_OBJPTR == childHH->nextChildHH);

  /*
   * If childHH's will be merged back in LIFO order, this sets up
   * parentHH->childHHList in that order
   */
  childHH->parentHH = parentHHObjptr;
  childHH->nextChildHH = parentHH->childHHList;
  parentHH->childHHList = childHHObjptr;

  /* cannot assert parentHH as it is still running! */
  HM_HH_assertInvariants(s, childHH);

  unlockHH(childHH);
  unlockHH(parentHH);
}

size_t HM_HH_getLevel(pointer hhPointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  objptr hhObjptr = pointerToObjptr (hhPointer, s->heap->start);
  const struct HM_HierarchicalHeap* hh = HHObjptrToStruct(s, hhObjptr);

  return hh->level;
}

void HM_HH_mergeIntoParent(pointer hhPointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  objptr hhObjptr = pointerToObjptr (hhPointer, s->heap->start);
  struct HM_HierarchicalHeap* hh = HHObjptrToStruct(s, hhObjptr);

  assert (BOGUS_OBJPTR != hh->parentHH);
  struct HM_HierarchicalHeap* parentHH = HHObjptrToStruct(s, hh->parentHH);

  lockHH(hh);
  lockHH(parentHH);

  HM_HH_assertInvariants(s, parentHH);
  HM_HH_assertInvariants(s, hh);
  /* can only merge at join point! */
  assert(hh->level == parentHH->level);

  /* remove hh from parentHH->childHHList */
  /*
   * This assert assumes that all merges happen in LIFO order, as per the
   * comment in HM_appendChildHH ()
   */
  objptr* cursor;
  for (cursor = &(parentHH->childHHList);
#if ASSERT
       (BOGUS_OBJPTR != *cursor) &&
#endif
                (hhObjptr != *cursor);
       cursor = &(HHObjptrToStruct(s, *cursor)->nextChildHH)) {
  }
  assert(BOGUS_OBJPTR != *cursor);
  *cursor = hh->nextChildHH;

  /* merge level lists */
  HM_mergeLevelList(&(parentHH->levelList), hh->levelList);

  HM_HH_assertInvariants(s, parentHH);
  /* don't assert hh here as it should be thrown away! */

  unlockHH(parentHH);
  unlockHH(hh);
}

void HM_HH_promoteChunks(pointer hhPointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  objptr hhObjptr = pointerToObjptr (hhPointer, s->heap->start);
  struct HM_HierarchicalHeap* hh = HHObjptrToStruct(s, hhObjptr);

  assert(HM_getHighestLevel(hh->levelList) <= hh->level);
  HM_promoteChunks(&(hh->levelList), hh->level);
}

void HM_HH_setLevel(pointer hhPointer, size_t level) {
  GC_state s = pthread_getspecific (gcstate_key);

  objptr hhObjptr = pointerToObjptr (hhPointer, s->heap->start);
  struct HM_HierarchicalHeap* hh = HHObjptrToStruct(s, hhObjptr);

  hh->level = level;
}
#endif /* MLTON_GC_INTERNAL_BASIS */

#if (defined (MLTON_GC_INTERNAL_FUNCS))
void HM_HH_display (
    const struct HM_HierarchicalHeap* hh,
    FILE* stream) {
  fprintf (stream,
           "\t\tsavedFrontier = %p\n"
           "\t\tlimit = %p\n"
           "\t\tlastAllocatedChunk = %p\n"
           "\t\tlevelList = %p\n"
           "\t\tparentHH = "FMTOBJPTR"\n"
           "\t\tnextChildHH = "FMTOBJPTR"\n"
           "\t\tchildHHList= "FMTOBJPTR"\n",
           hh->savedFrontier,
           hh->limit,
           hh->lastAllocatedChunk,
           hh->levelList,
           hh->parentHH,
           hh->nextChildHH,
           hh->childHHList);
}

void HM_HH_ensureNotEmpty(struct HM_HierarchicalHeap* hh) {
  if (NULL == hh->levelList) {
    assert(NULL == hh->savedFrontier);
    assert(NULL == hh->limit);
    assert(NULL == hh->lastAllocatedChunk);

    /* add in one chunk */
    if (!HM_HH_extend(hh, GC_HEAP_LIMIT_SLOP)) {
      die(__FILE__ ":%d: Ran out of space for Hierarchical Heap!", __LINE__);
    }
  }

  HM_HH_assertInvariants(pthread_getspecific(gcstate_key), hh);
}

bool HM_HH_extend(struct HM_HierarchicalHeap* hh, size_t bytesRequested) {
  size_t level = HM_getHighestLevel(hh->levelList);
  void* chunk;
  void* chunkEnd;
  assert((CHUNK_INVALID_LEVEL == level) || (hh->level >= level));

  if (ChunkPool_overHalfAllocated()) {
    /* collect first to free up some space */
    HM_HHC_collectLocal();
  }

  if ((CHUNK_INVALID_LEVEL == level) || (hh->level > level)) {
    chunk = HM_allocateLevelHeadChunk(&(hh->levelList),
                                      &chunkEnd,
                                      bytesRequested,
                                      hh->level,
                                      hh);
  } else {
    chunk = HM_allocateChunk(hh->levelList, &chunkEnd, bytesRequested);
  }

  if (NULL == chunk) {
    return FALSE;
  }

  hh->limit = chunkEnd;
  hh->savedFrontier = HM_getChunkFrontier(chunk);
  hh->lastAllocatedChunk = chunk;

  return TRUE;
}

struct HM_HierarchicalHeap* HM_HH_getContaining(GC_state s, objptr object) {
  assert(HM_HH_objptrInHierarchicalHeap(s, object));

  return HM_getContainingHierarchicalHeap(object);
}

struct HM_HierarchicalHeap* HM_HH_getCurrent(GC_state s) {
  return HHObjptrToStruct(s, s->currentHierarchicalHeap);
}

Word32 HM_HH_getObjptrLevel(GC_state s, objptr object) {
  return HM_getObjptrLevel(s, object);
}

void* HM_HH_getSavedFrontier(
    const struct HM_HierarchicalHeap* hh) {
  return hh->savedFrontier;
}

void* HM_HH_getLimit(const struct HM_HierarchicalHeap* hh) {
  return hh->limit;
}

/* RAM_NOTE: should this be moved to local-heap.h? */
bool HM_HH_objptrInHierarchicalHeap(GC_state s, objptr candidateObjptr) {
  pointer candidatePointer = objptrToPointer (candidateObjptr, s->heap->start);
  return ChunkPool_pointerInChunkPool(candidatePointer);
}

/* RAM_NOTE: Should be able to compute once and save result */
size_t HM_HH_offsetof(GC_state s) {
  return (HM_HH_sizeof(s) - (GC_NORMAL_HEADER_SIZE +
                             sizeof (struct HM_HierarchicalHeap)));
}

void HM_HH_updateValues(struct HM_HierarchicalHeap* hh,
                        void* frontier) {
  hh->savedFrontier = frontier;
  HM_updateChunkValues(hh->lastAllocatedChunk, frontier);
}

/* RAM_NOTE: Should be able to compute once and save result */
size_t HM_HH_sizeof(GC_state s) {
  size_t result = GC_NORMAL_HEADER_SIZE + sizeof (struct HM_HierarchicalHeap);
  result = align (result, s->alignment);

  if (DEBUG) {
    uint16_t bytesNonObjptrs;
    uint16_t numObjptrs;
    splitHeader (s,
                 GC_HIERARCHICAL_HEAP_HEADER,
                 NULL,
                 NULL,
                 &bytesNonObjptrs,
                 &numObjptrs);

    size_t check = GC_NORMAL_HEADER_SIZE +
                   (bytesNonObjptrs + (numObjptrs * OBJPTR_SIZE));

    if (DEBUG_DETAILED) {
      fprintf (
          stderr,
          "sizeofHierarchicalHeap: result = %"PRIuMAX"  check = %"PRIuMAX"\n",
          (uintmax_t)result,
          (uintmax_t)check);
    }

    assert (check == result);
  }
  assert (isAligned (result, s->alignment));

  return result;
}

void HM_HH_updateLevelListPointers(objptr hhObjptr) {
  GC_state s = pthread_getspecific (gcstate_key);
  struct HM_HierarchicalHeap* hh = HHObjptrToStruct(s, hhObjptr);

  HM_updateLevelListPointers(hh->levelList, hh);
}
#endif /* MLTON_GC_INTERNAL_FUNCS */

#if ASSERT
void HM_HH_assertInvariants(GC_state s,
                            const struct HM_HierarchicalHeap* hh) {
  HM_assertLevelListInvariants(hh->levelList);
  if (NULL != hh->limit) {
    assert(ChunkPool_find(((char*)(hh->limit)) - 1) == hh->lastAllocatedChunk);
    assert(ChunkPool_find(hh->savedFrontier) == hh->lastAllocatedChunk);
    assert(NULL != hh->levelList);
  } else {
    assert(NULL == hh->savedFrontier);
    assert(NULL == hh->lastAllocatedChunk);
    assert(NULL == hh->levelList);
  }

  struct HM_HierarchicalHeap* parentHH = HHObjptrToStruct(s, hh->parentHH);
  if (NULL != parentHH) {
    /* Make sure I am in parentHH->childHHList */
    bool foundInParentList = FALSE;
    for (struct HM_HierarchicalHeap* childHH =
             HHObjptrToStruct(s, parentHH->childHHList);
         NULL != childHH;
         childHH = HHObjptrToStruct(s, childHH->nextChildHH)) {
      if (hh == childHH) {
        foundInParentList = TRUE;
        break;
      }
    }
    assert(foundInParentList);
  }

  for (struct HM_HierarchicalHeap* childHH = HHObjptrToStruct(s,
                                                              hh->childHHList);
       NULL != childHH;
       childHH = HHObjptrToStruct(s, childHH->nextChildHH)) {
    assert(HHObjptrToStruct(s, childHH->parentHH) == hh);
  }
}
#else
void HM_HH_assertInvariants(GC_state s,
                            const struct HM_HierarchicalHeap* hh) {
  ((void)(s));
  ((void)(hh));
}
#endif /* ASSERT */

struct HM_HierarchicalHeap* HHObjptrToStruct(GC_state s, objptr hhObjptr) {
  if (BOGUS_OBJPTR == hhObjptr) {
    return NULL;
  }

  pointer hhPointer = objptrToPointer (hhObjptr, s->heap->start);
  return ((struct HM_HierarchicalHeap*)(hhPointer +
                                        HM_HH_offsetof(s)));
}

void lockHH(struct HM_HierarchicalHeap* hh) {
  do {
  } while ((hh->lock == HM_HH_LOCK_LOCKED) ||
           (!__sync_bool_compare_and_swap (&(hh->lock),
                                           HM_HH_LOCK_UNLOCKED,
                                           HM_HH_LOCK_LOCKED)));
}

void unlockHH(struct HM_HierarchicalHeap* hh) {
  assert(HM_HH_LOCK_LOCKED == hh->lock);
  __sync_bool_compare_and_swap (&(hh->lock),
                                HM_HH_LOCK_LOCKED,
                                HM_HH_LOCK_UNLOCKED);
}
