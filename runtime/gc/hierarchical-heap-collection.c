/* Copyright (C) 2015 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file hierarchical-heap-collection.c
 *
 * @author Ram Raghunathan
 *
 * This file implements the hierarchical heap collection interface described in
 * hierarchical-heap-collection.h.
 */

#include "hierarchical-heap-collection.h"

/******************************/
/* Static Function Prototypes */
/******************************/
/**
 * Compute the size of the object, how much of it has to be copied, as well as
 * how much metadata it has.
 *
 * @param s GC state
 * @param p The pointer to copy
 * @param objectSize Where to store the size of the object (in bytes)
 * @param copySize Where to store the number of bytes to copy
 * @param metaDataSize Where to store the metadata size (in bytes)
 *
 * @return the tag of the object
 */
GC_objectTypeTag computeObjectCopyParameters(GC_state s, pointer p,
                                             size_t *objectSize,
                                             size_t *copySize,
                                             size_t *metaDataSize);

/**
 * Copies the object into the new level list of the hierarchical heap provided.
 *
 * @param p The pointer to copy
 * @param objectSize The size of the object
 * @param copySize The number of bytes to copy
 * @param toChunkList The ChunkList at which 'p' will be copied.
 *
 * @return pointer to the copied object
 */
pointer copyObject(pointer p,
                   size_t objectSize,
                   size_t copySize,
                   void* toChunkList);

/**
 * Populates 'holes' with the current global heap holes from all processors.
 *
 * @attention
 * Must be run within an enter/leave in order to ensure correctness.
 *
 * @param s The GC_state to use
 * @param holes The holes to populate.
 */
void populateGlobalHeapHoles(GC_state s, struct GlobalHeapHole* holes);

/**
 * ObjptrPredicateFunction for skipping stacks and threads in the hierarchical
 * heap.
 *
 * @note This function takes as additional arguments the
 * struct SSATOPredicateArgs
 */
struct SSATOPredicateArgs {
  pointer expectedStackPointer;
  pointer expectedThreadPointer;
};
bool skipStackAndThreadObjptrPredicate(GC_state s,
                                       pointer p,
                                       void* rawArgs);

/************************/
/* Function Definitions */
/************************/
#if (defined (MLTON_GC_INTERNAL_BASIS))
void HM_HHC_registerQueue(uint32_t processor, pointer queuePointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  assert(processor < s->numberOfProcs);
  assert(isObjptrInGlobalHeap(s, pointerToObjptr (queuePointer,
                                                  s->heap->start)));

  s->procStates[processor].wsQueue = pointerToObjptr (queuePointer,
                                                      s->heap->start);
}

void HM_HHC_registerQueueLock(uint32_t processor, pointer queueLockPointer) {
  GC_state s = pthread_getspecific (gcstate_key);

  assert(processor < s->numberOfProcs);
  assert(isObjptrInGlobalHeap(s, pointerToObjptr (queueLockPointer,
                                                  s->heap->start)));

  s->procStates[processor].wsQueueLock = pointerToObjptr (queueLockPointer,
                                                          s->heap->start);
}
#endif /* MLTON_GC_INTERNAL_BASIS */

#if (defined (MLTON_GC_INTERNAL_BASIS))
void HM_HHC_collectLocal(void) {
  GC_state s = pthread_getspecific (gcstate_key);
  struct HM_HierarchicalHeap* hh = HM_HH_getCurrent(s);
  struct rusage ru_start;
  Pointer wsQueueLock = objptrToPointer(s->wsQueueLock, s->heap->start);
  bool queueLockHeld = FALSE;
  uint64_t oldObjectCopied;

  if (NONE == s->controls->hhCollectionLevel) {
    /* collection disabled */
    return;
  }

  if (Parallel_alreadyLockedByMe(wsQueueLock)) {
    /* in a scheduler critical section, so cannot collect */
    LOG(LM_HH_COLLECTION, LL_DEBUG,
        "Queue locked by mutator/scheduler");
    queueLockHeld = TRUE;
  }

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "START");

  Trace0(EVENT_GC_ENTER);
  TraceResetCopy();

  if (needGCTime(s)) {
    startTiming (RUSAGE_THREAD, &ru_start);
  }
  s->cumulativeStatistics->numHHLocalGCs++;

  /* used needs to be set because the mutator has changed s->stackTop. */
  getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed (s);
  getThreadCurrent(s)->exnStack = s->exnStack;

  int processor = Proc_processorNumber (s);

  HM_debugMessage(s,
                  "[%d] HM_HH_collectLocal(): Starting Local collection on "
                  "HierarchicalHeap = %p\n",
                  processor,
                  ((void*)(hh)));
  HM_debugDisplayHierarchicalHeap(s, hh);

  assert (!hh->newLevelList);

  /* lock queue to prevent steals */
  if (!queueLockHeld) {
    Parallel_lockTake(wsQueueLock);
  }
  lockWriterHH(hh);

  assertInvariants(s, hh, LIVE);
  assert(hh->thread == s->currentThread);

  /* copy roots */
  struct ForwardHHObjptrArgs forwardHHObjptrArgs = {
    .hh = hh,
    .minLevel = HM_HH_getLowestPrivateLevel(s, hh),
    .maxLevel = hh->level,
    .tgtChunkList = NULL,
    .bytesCopied = 0,
    .objectsCopied = 0,
    .stacksCopied = 0
  };

  if (SUPERLOCAL == s->controls->hhCollectionLevel) {
    forwardHHObjptrArgs.minLevel = hh->level;
  }

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "collecting hh %p (SL: %u L: %u):\n"
      "  local scope is %u -> %u\n"
      "  lchs %"PRIu64" lcs %"PRIu64,
      ((void*)(hh)),
      hh->stealLevel,
      hh->level,
      forwardHHObjptrArgs.minLevel,
      forwardHHObjptrArgs.maxLevel,
      hh->locallyCollectibleHeapSize,
      hh->locallyCollectibleSize);

  LOG(LM_HH_COLLECTION, LL_DEBUG, "START root copy");

  /* forward contents of stack */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(getStackCurrentObjptr(s),
                                        s->heap->start),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from stack",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward contents of thread (hence including stack) */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(getThreadCurrentObjptr(s),
                                        s->heap->start),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from thread",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward thread itself */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  forwardHHObjptr(s, &(s->currentThread), &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      (1 == (forwardHHObjptrArgs.objectsCopied - oldObjectCopied)) ?
      "Copied thread from GC_state" : "Did not copy thread from GC_state");
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);


#if ASSERT
  /* forward thread from hh */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  forwardHHObjptr(s, &(hh->thread), &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      (1 == (forwardHHObjptrArgs.objectsCopied - oldObjectCopied)) ?
      "Copied thread from HH" : "Did not copy thread from HH");
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);
  assert(hh->thread == s->currentThread);
#else
  /* update thread in hh */
  hh->thread = s->currentThread;
#endif

  /* forward contents of deque */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(s->wsQueue,
                                        s->heap->start),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from deque",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward retVal pointer if necessary */
  if (NULL != hh->retVal) {
    objptr root = pointerToObjptr(hh->retVal, s->heap->start);

    oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
    forwardHHObjptr(s, &root, &forwardHHObjptrArgs);
    LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from hh->retVal",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);

    hh->retVal = objptrToPointer(root, s->heap->start);
  }

  LOG(LM_HH_COLLECTION, LL_DEBUG, "END root copy");

  /* do copy-collection */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  /*
   * I skip the stack and thread since they are already forwarded as roots
   * above
   */
  struct SSATOPredicateArgs ssatoPredicateArgs = {
    .expectedStackPointer = objptrToPointer(getStackCurrentObjptr(s),
                                            s->heap->start),
    .expectedThreadPointer = objptrToPointer(getThreadCurrentObjptr(s),
                                             s->heap->start)
  };
  HM_forwardHHObjptrsInLevelList(
      s,
      &(hh->newLevelList),
      &skipStackAndThreadObjptrPredicate,
      ((void*)(&ssatoPredicateArgs)),
      &forwardHHObjptrArgs,
      false);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects in copy-collection",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" stacks in copy-collection",
      forwardHHObjptrArgs.stacksCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  assertInvariants(s, hh, LIVE);

  /*
   * RAM_NOTE: Add hooks to forwardHHObjptr and freeChunks to count from/toBytes
   * instead of iterating
   */
#if 0
  if (DEBUG_HEAP_MANAGEMENT or s->controls->HMMessages) {
    /* count number of from-bytes */
    size_t fromBytes = 0;
    for (void* chunkList = hh->levelList;
         (NULL != chunkList) && (HM_getChunkListLevel(chunkList) >=
                                 forwardHHObjptrArgs.minLevel);
         chunkList = HM_getChunkInfo(chunkList)->split.levelHead.nextHead) {
      for (void* chunk = chunkList;
           NULL != chunk;
           chunk = HM_getChunkInfo(chunk)->nextChunk) {
        fromBytes += HM_getChunkLimit(chunk) - HM_getChunkStart(chunk);
      }
    }

    /* count number of to-chunks */
    size_t toBytes = 0;
    for (void* chunkList = hh->newLevelList;
         NULL != chunkList;
         chunkList = HM_getChunkInfo(chunkList)->split.levelHead.nextHead) {
      for (void* chunk = chunkList;
           NULL != chunk;
           chunk = HM_getChunkInfo(chunk)->nextChunk) {
        toBytes += HM_getChunkLimit(chunk) - HM_getChunkStart(chunk);
      }
    }

    LOG(LM_HH_COLLECTION, LL_INFO,
        "Collection went from %zu bytes to %zu bytes",
        fromBytes,
        toBytes);
  }
#endif

  /* free old chunks */
  HM_freeChunks(&(hh->levelList), forwardHHObjptrArgs.minLevel);

  /* merge newLevelList back in */
  HM_updateLevelListPointers(hh->newLevelList, hh);
  HM_mergeLevelList(&(hh->levelList), hh->newLevelList, hh, true);

  /* reset hh->newLevelList */
  hh->newLevelList = NULL;

  /*
   * RAM_NOTE: Really should get this off of forwardHHObjptrArgs instead of
   * summing up
   */
  /* update locally collectible size */
  /* off-by-one loop to prevent underflow and infinite loop */
  hh->locallyCollectibleSize = 0;
  Word32 level;
  for (level = hh->level;
       level > (HM_HH_getHighestStolenLevel(s, hh) + 1);
       level--) {
    hh->locallyCollectibleSize += HM_getLevelSize(hh->levelList, level);
  }
  hh->locallyCollectibleSize += HM_getLevelSize(hh->levelList, level);

  /* update lastAllocatedChunk and associated */
  void* lastChunk = HM_getChunkListLastChunk(hh->levelList);
  if (NULL == lastChunk) {
    /* empty lists, so reset hh */
    hh->lastAllocatedChunk = NULL;
  } else {
    /* we have a last chunk */
    hh->lastAllocatedChunk = lastChunk;
  }

  assertInvariants(s, hh, LIVE);

  /* RAM_NOTE: This can be moved earlier? */
  /* unlock hh and queue */
  unlockWriterHH(hh);
  if (!queueLockHeld) {
    Parallel_lockRelease(wsQueueLock);
  }

  HM_debugMessage(s,
                  "[%d] HM_HH_collectLocal(): Finished Local collection on "
                  "HierarchicalHeap = %p\n",
                  processor,
                  ((void*)(hh)));

  s->cumulativeStatistics->bytesHHLocaled += forwardHHObjptrArgs.bytesCopied;

  /* enter statistics if necessary */
  if (needGCTime(s)) {
    if (detailedGCTime(s)) {
      stopTiming(RUSAGE_THREAD, &ru_start, &s->cumulativeStatistics->ru_gcHHLocal);
    }

    /*
     * RAM_NOTE: small extra here since I recompute delta, but probably not a
     * big deal...
     */
    stopTiming(RUSAGE_THREAD, &ru_start, &s->cumulativeStatistics->ru_gc);
  }

  TraceResetCopy();
  Trace0(EVENT_GC_LEAVE);

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "END");
}

void forwardHHObjptr (GC_state s,
                      objptr* opp,
                      void* rawArgs) {
  struct ForwardHHObjptrArgs* args = ((struct ForwardHHObjptrArgs*)(rawArgs));
  objptr op = *opp;
  pointer p = objptrToPointer (op, s->heap->start);
  bool inPromotion = (args->tgtChunkList != NULL);

  if (DEBUG_DETAILED) {
    fprintf (stderr,
             "forwardHHObjptr  opp = "FMTPTR"  op = "FMTOBJPTR"  p = "
             ""FMTPTR"\n",
             (uintptr_t)opp,
             op,
             (uintptr_t)p);
  }

  LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
      "opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR,
      (uintptr_t)opp,
      op,
      (uintptr_t)p);

  if (!isObjptr(op) || isObjptrInGlobalHeap(s, op)) {
    /* does not point to an HH objptr, so not in scope for collection */
    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "skipping opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR": not in HH.",
        (uintptr_t)opp,
        op,
        (uintptr_t)p);
    return;
  }

  // if it's not in the global heap, then it must be in the HH
  assert(HM_HH_objptrInHierarchicalHeap(s, op));

  struct HM_ObjptrInfo opInfo;
  HM_getObjptrInfo(s, op, &opInfo);

  if (opInfo.level > args->maxLevel) {
    DIE("entanglement detected during %s: %p is at level %u, below %u",
        inPromotion ? "promotion" : "collection",
        (void *)p,
        opInfo.level,
        args->maxLevel);
  }

  /* RAM_NOTE: This is more nuanced with non-local collection */
  if ((opInfo.level > args->maxLevel) ||
      /* cannot forward any object below 'args->minLevel' */
      (opInfo.level < args->minLevel)) {
      LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
          "skipping opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR
          ": level %d not in [minLevel %d, maxLevel %d].",
          (uintptr_t)opp,
          op,
          (uintptr_t)p,
          opInfo.level,
          args->minLevel,
          args->maxLevel);
      LOCAL_USED_FOR_ASSERT objptr oppop =
        pointerToObjptr((pointer)opp, s->heap->start);
      assert ((inPromotion && HM_isObjptrInToSpace(s, oppop))
              || HM_objptrIsAboveHH(s, p, args->hh));
      return;
  }

  /* We look for the top-most collectible replica (tmcr) of p present in the
   * collection range, that is in [arg->maxLevel, arg->minLevel]. Depending on
   * what we find, we might have to create a copy or not. There are three cases:
   *
   * 1/ The tmcr is in to-space. No copying is needed, the address of the tmcr
   * is the new location of p.
   *
   * 2/ The tmcr is in from-space and has been forwarded. No copy is needed, the
   * target of the tmcr's forwarding pointer (necessarily outside collection
   * range) is the new location of p.
   *
   * 3/ The tmcr is in from-space and has no forwarding pointer. The tmcr must
   * be copied in to-space; the resulting address is the new location of p.
   */

  /* find the top-most collectible replica */
  p = HM_followForwardPointerUntilNullOrBelowLevel(s,
                                                   p,
                                                   args->minLevel);
  op = pointerToObjptr(p, s->heap->start);
  HM_getObjptrInfo(s, op, &opInfo);

  if (HM_isObjptrInToSpace(s, op)) {
    /* just use p/op */
    *opp = op;

    if (DEBUG_DETAILED) {
      fprintf (stderr, "  already FORWARDED\n");
    }

    /* objects in to-space should not themselves have forwarding pointers */
    assert (!hasFwdPtr(p));
    /* to-space should be copying */
    assert(COPY_OBJECT_HH_VALUE == opInfo.hh);
    /* should not have copy-forwarded anything below 'args->minLevel'! */
    assert (opInfo.level >= args->minLevel);
  } else if (hasFwdPtr(p)) {
    /* just use the forwarding pointer of p */
    *opp = getFwdPtr(p);

    /* should point outside collectible zone */
#if ASSERT
    HM_getObjptrInfo(s, *opp, &opInfo);
    assert (opInfo.level < args->minLevel);
#endif
  } else {
    /* forward the object */
    GC_objectTypeTag tag;
    size_t metaDataBytes;
    size_t objectBytes;
    size_t copyBytes;

    /* compute object size and bytes to be copied */
    tag = computeObjectCopyParameters(s,
                                      p,
                                      &objectBytes,
                                      &copyBytes,
                                      &metaDataBytes);

    switch (tag) {
    case STACK_TAG:
        args->stacksCopied++;
        break;
    case WEAK_TAG:
        die(__FILE__ ":%d: "
            "forwardHHObjptr() does not support WEAK_TAG objects!",
            __LINE__);
        break;
    default:
        break;
    }

    /* We copy the object to args->tgtChunkList during promotion, or preserve
     * the level of the object otherwise. */
    void *toChunkList = args->tgtChunkList;

    if (!inPromotion) {
        toChunkList = HM_getChunkListToChunkList(opInfo.chunkList);

#if ASSERT
        if (NULL == toChunkList) {
            void* cursor;
            for (cursor = args->hh->newLevelList;
                 (NULL != cursor) &&
                     (HM_getChunkListLevel(cursor) > opInfo.level);
                 cursor = HM_getChunkInfo(cursor)->split.levelHead.nextHead) {
            }
            assert((NULL == cursor) ||
                   (HM_getChunkListLevel(cursor) != opInfo.level));
        } else {
            assert(HM_getChunkListLevel(toChunkList) == opInfo.level);

            void* cursor;
            for (cursor = args->hh->newLevelList;
                 (NULL != cursor) &&
                     (HM_getChunkListLevel(cursor) > opInfo.level);
                 cursor = HM_getChunkInfo(cursor)->split.levelHead.nextHead) {
            }
            assert(toChunkList == cursor);
        }
#endif
        if (toChunkList == NULL) {
            /* Level does not exist, so create it */
            toChunkList = HM_allocateLevelHeadChunk(&(args->hh->newLevelList),
                                                    objectBytes,
                                                    opInfo.level,
                                                    COPY_OBJECT_HH_VALUE);
            if (NULL == toChunkList) {
                DIE("Ran out of space for Hierarchical Heap!");
            }
            HM_getChunkInfo(toChunkList)->split.levelHead.isInToSpace = true;

            /* update toChunkList for fast access later */
            HM_setChunkListToChunkList(opInfo.chunkList, toChunkList);
        }
    }

    assert (!hasFwdPtr(p));

    LOG(LM_HH_COLLECTION, LL_INFO,
        "during %s, copying pointer %p at level %u to level list %p",
        (inPromotion ? "promotion" : "collection"),
        (void *)p,
        opInfo.level,
        (void *)toChunkList);

    pointer copyPointer = copyObject(p - metaDataBytes,
                                     objectBytes,
                                     copyBytes,
                                     toChunkList);

    args->bytesCopied += copyBytes;
    args->objectsCopied++;
    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "%p --> %p", ((void*)(p - metaDataBytes)), ((void*)(copyPointer)));

    /* Store the forwarding pointer in the old object metadata. */
    *(getFwdPtrp(p)) = pointerToObjptr (copyPointer + metaDataBytes,
                                        s->heap->start);
    assert (hasFwdPtr(p));

    /* use the forwarding pointer */
    *opp = getFwdPtr(p);

#if ASSERT
    /* args->hh->newLevelList has containingHH set to COPY_OBJECT_HH_VALUE
     * during a copy-collection. */
    HM_getObjptrInfo(s, *opp, &opInfo);
    /* TODO have a more precise assert that also handles the promotion case. */
    assert (inPromotion || COPY_OBJECT_HH_VALUE == opInfo.hh);
#endif
  }

  LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
      "opp "FMTPTR" set to "FMTOBJPTR,
      ((uintptr_t)(opp)),
      *opp);
}
#endif /* MLTON_GC_INTERNAL_BASIS */

GC_objectTypeTag computeObjectCopyParameters(GC_state s, pointer p,
                                             size_t *objectSize,
                                             size_t *copySize,
                                             size_t *metaDataSize) {
    GC_header header;
    GC_objectTypeTag tag;
    uint16_t bytesNonObjptrs;
    uint16_t numObjptrs;
    header = getHeader(p);
    splitHeader(s, header, &tag, NULL, &bytesNonObjptrs, &numObjptrs);

    if (GC_HIERARCHICAL_HEAP_HEADER == header) {
        die(__FILE__ ":%d: "
            "computeObjectCopyParameters() does not support"
            " GC_HIERARCHICAL_HEAP_HEADER objects!",
            __LINE__);
    }

    /* Compute the space taken by the metadata and object body. */
    if ((NORMAL_TAG == tag) or (WEAK_TAG == tag)) { /* Fixed size object. */
      if (WEAK_TAG == tag) {
        die(__FILE__ ":%d: "
            "computeObjectSizeAndCopySize() #define does not support"
            " WEAK_TAG objects!",
            __LINE__);
      }
      *metaDataSize = GC_NORMAL_METADATA_SIZE;
      *objectSize = bytesNonObjptrs + (numObjptrs * OBJPTR_SIZE);
      *copySize = *objectSize;
    } else if (ARRAY_TAG == tag) {
      *metaDataSize = GC_ARRAY_METADATA_SIZE;
      *objectSize = sizeofArrayNoMetaData (s, getArrayLength (p),
                                           bytesNonObjptrs, numObjptrs);
      *copySize = *objectSize;
    } else {
      /* Stack. */
      bool current;
      size_t reservedNew;
      GC_stack stack;

      assert (STACK_TAG == tag);
      *metaDataSize = GC_STACK_METADATA_SIZE;
      stack = (GC_stack)p;

      /* RAM_NOTE: This changes with non-local collection */
      /* Check if the pointer is the current stack of my processor. */
      current = getStackCurrent(s) == stack;

      reservedNew = sizeofStackShrinkReserved (s, stack, current);
      if (reservedNew < stack->reserved) {
        LOG(LM_HH_COLLECTION, LL_DEBUG,
            "Shrinking stack of size %s bytes to size %s bytes"
            ", using %s bytes.",
            uintmaxToCommaString(stack->reserved),
            uintmaxToCommaString(reservedNew),
            uintmaxToCommaString(stack->used));
        stack->reserved = reservedNew;
      }
      *objectSize = sizeof (struct GC_stack) + stack->reserved;
      *copySize = sizeof (struct GC_stack) + stack->used;
    }

    *objectSize += *metaDataSize;
    *copySize += *metaDataSize;

    return tag;
}

pointer copyObject(pointer p,
                   size_t objectSize,
                   size_t copySize,
                   void* toChunkList) {
  assert (toChunkList);
  assert (copySize <= objectSize);
  assert (HM_getChunkInfo(toChunkList)->level != CHUNK_INVALID_LEVEL);

  /* get the chunk to allocate in */
  void* chunk = HM_getChunkListLastChunk(toChunkList);
  void* frontier = HM_getChunkFrontier(chunk);
  void* limit = HM_getChunkLimit(chunk);

  if (((size_t)(((char*)(limit)) - ((char*)(frontier)))) < objectSize) {
      /* need to allocate a new chunk */
      chunk = HM_allocateChunk(toChunkList, objectSize);
      if (NULL == chunk) {
          die(__FILE__ ":%d: Ran out of space for Hierarchical Heap!", __LINE__);
      }
      frontier = HM_getChunkFrontier(chunk);
  }

  GC_memcpy(p, frontier, copySize);
  void* newFrontier = ((void*)(((char*)(frontier)) + objectSize));
  if (alignDown((size_t)newFrontier, 512ULL * 1024) != chunk) {
    chunk = HM_allocateChunk(toChunkList, 42); /* I just need to extend with a new chunk... size is arbitrary. */
    if (NULL == chunk) {
      die(__FILE__ ":%d: Ran out of space for Hierarchical Heap!", __LINE__);
    }
    newFrontier = HM_getChunkFrontier(chunk);
  }
  HM_updateChunkValues(chunk, newFrontier);
  assert(ChunkPool_find_checked(newFrontier) == alignDown((size_t)newFrontier, 512ULL * 1024));

  return frontier;
}

void populateGlobalHeapHoles(GC_state s, struct GlobalHeapHole* holes) {
  DIE('populateGlobalHeapHoles deprecated');

//   for (uint32_t i = 0; i < s->numberOfProcs; i++) {
//     spinlock_lock(&(s->procStates[i].lock), Proc_processorNumber(s));

//     pointer start = s->procStates[i].frontier;
//     pointer end = s->procStates[i].limitPlusSlop + GC_GAP_SLOP;

//     if (HM_HH_objptrInHierarchicalHeap(s, pointerToObjptr(start,
//                                                           s->heap->start))) {
// #if 0
//       assert(HM_HH_objptrInHierarchicalHeap(s,
//                                             pointerToObjptr(end,
//                                                             s->heap->start)));
// #endif

//       /* use the saved global frontier */
//       start = s->procStates[i].globalFrontier;
//       end = s->procStates[i].globalLimitPlusSlop + GC_GAP_SLOP;
//     }

//     holes[i].start = start;
//     holes[i].end = end;

//     spinlock_unlock(&(s->procStates[i].lock));
//   }
}

bool skipStackAndThreadObjptrPredicate(GC_state s,
                                       pointer p,
                                       void* rawArgs) {
  /* silence compliler */
  ((void)(s));

  /* extract expected stack */
  LOCAL_USED_FOR_ASSERT const struct SSATOPredicateArgs* args =
      ((struct SSATOPredicateArgs*)(rawArgs));

  /* run through FALSE cases */
  GC_header header;
  header = getHeader(p);
  if (header == GC_STACK_HEADER) {
    assert(args->expectedStackPointer == p);
    return FALSE;
  } else if (header == GC_THREAD_HEADER) {
    assert(args->expectedThreadPointer == p);
    return FALSE;
  }

  return TRUE;
}
