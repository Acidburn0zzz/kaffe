/* gc-incremental.c
 * The garbage collector.
 * The name is misleading.  GC is non-incremental at this point.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#define	SUPPORT_VERBOSEMEM

#include "config.h"
#include "debug.h"
#include "config-std.h"
#include "config-mem.h"
#include "gtypes.h"
#include "gc.h"
#include "gc-mem.h"
#include "locks.h"
#include "thread.h"
#include "jthread.h"
#include "errors.h"
#include "md.h"
#include "stats.h"
#include "classMethod.h"

/* Avoid recursively allocating OutOfMemoryError */
#define OOM_ALLOCATING		((void *) -1)

#define GCSTACKSIZE		16384
#define FINALIZERSTACKSIZE	THREADSTACKSIZE

/*
 * Object implementing the collector interface.
 */
static struct CollectorImpl {
	Collector 	collector;
	/* XXX include static below here for encapsulation */
} gc_obj;

/* XXX don't use these types ! */
Hjava_lang_Thread* garbageman;
static Hjava_lang_Thread* finalman;

static gcList gclists[5];
static const int mustfree = 4;		/* temporary list */
static const int white = 3;
static const int grey = 2;
static const int black = 1;
static const int finalise = 0;

static int gc_init = 0;
static volatile int gcRunning = 0;
static volatile bool finalRunning = false;
static void (*walkRootSet)(Collector*);
#if defined(KAFFE_STATS)
static timespent gc_time;
static timespent sweep_time;
static counter gcgcablemem;
static counter gcfixedmem;
#endif /* KAFFE_STATS */

/* Is this pointer within our managed heap? */
#define IS_A_HEAP_POINTER(from) \
        ((uintp) (from) >= gc_heap_base && \
	 (uintp) (from) < gc_heap_base + gc_heap_range)

static void *reserve;
static void *outOfMem;
static void *outOfMem_allocator;

#if defined(SUPPORT_VERBOSEMEM)

static void objectStatsChange(gc_unit*, int);
static void objectStatsPrint(void);

#define	OBJECTSTATSADD(M)	objectStatsChange(M, 1)
#define	OBJECTSTATSREMOVE(M)	objectStatsChange(M, -1)
#define	OBJECTSTATSPRINT()	objectStatsPrint()

#else

#define	OBJECTSTATSADD(M)
#define	OBJECTSTATSREMOVE(M)
#define	OBJECTSTATSPRINT()

#endif

/* For statistics gathering, record how many objects and how 
 * much memory was marked.
 */
static inline
void record_marked(int nr_of_objects, uint32 size)
{       
        gcStats.markedobj += nr_of_objects;
        gcStats.markedmem += size;
} 

iLock* gcman;
iLock* finman;
iLock* gc_lock;			/* allocator mutex */

static void gcFree(Collector* gcif, void* mem);

/* Standard GC function sets.  We call them "allocation types" now */
static gcFuncs gcFunctions[GC_ALLOC_MAX_INDEX];

/* Number of registered allocation types */
static int nrTypes;

/*
 * register an allocation type under a certain index
 * NB: we could instead return a pointer to the record describing the
 * allocation type.  This would give us more flexibility, but it wouldn't
 * allow us to use compile-time constants.
 */
static void
registerTypeByIndex(int index, walk_func_t walk, final_func_t final,
	destroy_func_t destroy,
	const char *description)
{
	/* once only, please */
	assert (gcFunctions[index].description == 0);
	/* don't exceed bounds */
	assert (index >= 0 && 
		index < sizeof(gcFunctions)/sizeof(gcFunctions[0]));
	gcFunctions[index].walk = walk;
	gcFunctions[index].final = final;
	gcFunctions[index].destroy = destroy;
	gcFunctions[index].description = description;
	if (index >= nrTypes) {
		nrTypes = index + 1;
	}
}

/*
 * Register a fixed allocation type.  The only reason we tell them apart
 * is for statistical purposes.
 */
static void
gcRegisterFixedTypeByIndex(Collector* gcif, 
	int index, const char *description)
{
	registerTypeByIndex(index, 0, GC_OBJECT_FIXED, 0, description);
}

/*
 * Register a allocation type that is subject to gc.  
 */
static void
gcRegisterGcTypeByIndex(Collector* gcif,
	int index, walk_func_t walk, final_func_t final,
	destroy_func_t destroy,
	const char *description)
{
	registerTypeByIndex(index, walk, final, destroy, description);
}

struct _gcStats gcStats;

static void startGC(Collector *gcif);
static void finishGC(Collector *gcif);
static void startFinalizer(void);
static void markObjectDontCheck(gc_unit *unit, gc_block *info, int idx);

/* Return true if gc_unit is pointer to an allocated object */
static inline int
gc_heap_isobject(gc_block *info, gc_unit *unit)
{
	uintp p = (uintp) UTOMEM(unit) - gc_heap_base;
	int idx;

	if (!(p & (MEMALIGN - 1)) && p < gc_heap_range && info->inuse) {
		/* Make sure 'unit' refers to the beginning of an
		 * object.  We do this by making sure it is correctly
		 * aligned within the block.
		 */
		idx = GCMEM2IDX(info, unit);
		if (idx < info->nr && GCBLOCK2MEM(info, idx) == unit
		    && (GC_GET_COLOUR(info, idx) & GC_COLOUR_INUSE) ==
		    GC_COLOUR_INUSE) {
			return 1;
		}
	}
	return 0;
}

static inline void
markObjectDontCheck(gc_unit *unit, gc_block *info, int idx)
{
	/* If object's been traced before, don't do it again */
	if (GC_GET_COLOUR(info, idx) != GC_COLOUR_WHITE) {
		return;
	}
DBG(GCWALK,	
	dprintf("  marking @%08p: %s\n", UTOMEM(unit),
			describeObject(UTOMEM(unit)));
    )

	DBG(GCSTAT,
	    switch (GC_GET_FUNCS(info, idx)) {
	    case GC_ALLOC_NORMALOBJECT:
	    case GC_ALLOC_FINALIZEOBJECT:
	    case GC_ALLOC_PRIMARRAY:
	    case GC_ALLOC_REFARRAY: {
		    Hjava_lang_Class *c
			    = OBJECT_CLASS((Hjava_lang_Object *) (unit+1));
		    c->live_count++;
	    }});
	    

	/* If we found a new white object, mark it as grey and
	 * move it into the grey list.
	 */
	GC_SET_COLOUR(info, idx, GC_COLOUR_GREY);
	UREMOVELIST(unit);
	UAPPENDLIST(gclists[grey], unit);
}

/*
 * Mark the memory given by an address if it really is an object.
 */
static void
gcMarkAddress(Collector* gcif, const void* mem)
{
	gc_block* info;
	gc_unit* unit;

	/*
	 * First we check to see if the memory 'mem' is in fact the
	 * beginning of an object.  If not we just return.
	 */

	/* Get block info for this memory - if it exists */
	info = GCMEM2BLOCK(mem);
	unit = UTOUNIT(mem);
	if (gc_heap_isobject(info, unit)) {
		markObjectDontCheck(unit, info, GCMEM2IDX(info, unit));
	}
}

/*
 * Mark an object.  Argument is assumed to point to a valid object,
 * and never, ever, be null.
 */
static void
gcMarkObject(Collector* gcif, const void* objp)
{
  gc_unit *unit = UTOUNIT(objp);
  gc_block *info = GCMEM2BLOCK(unit);
  DBG(GCDIAG, assert(gc_heap_isobject(info, unit)));
  markObjectDontCheck(unit, info, GCMEM2IDX(info, unit));
}

static void
gcWalkConservative(Collector* gcif, const void* base, uint32 size)
{
	int8* mem;

DBG(GCWALK,	
	dprintf("scanning %d bytes conservatively from %p-%p\n", 
		size, base, base+size);
    )

	record_marked(1, size);

	if (size > 0) {
		for (mem = ((int8*)base) + (size & -ALIGNMENTOF_VOIDP) - sizeof(void*); (void*)mem >= base; mem -= ALIGNMENTOF_VOIDP) {
			void *p = *(void **)mem;
			if (p) {
				gcMarkAddress(gcif, p);
			}
		}
	}
}

/*
 * Like walkConservative, except that length is computed from the block size
 * of the object.  Must be called with pointer to object allocated by gc.
 */
static
uint32
gcGetObjectSize(Collector* gcif, const void* mem)
{
	return (GCBLOCKSIZE(GCMEM2BLOCK(UTOUNIT(mem))));
}

static
int
gcGetObjectIndex(Collector* gcif, const void* mem)
{
	gc_unit* unit = UTOUNIT(mem);
	gc_block* info = GCMEM2BLOCK(unit);
	if (!gc_heap_isobject(info, unit)) {
		return (-1);
	} else {
		return (GC_GET_FUNCS(info, GCMEM2IDX(info, unit)));
	}
}

/*
 * Given a pointer within an object, find the base of the object.
 * This works for both gcable and fixed object types.
 *
 * This method uses many details of the allocator implementation.
 * Specifically, it relies on the contiguous layout of block infos
 * and the way GCMEM2BLOCK and GCMEM2IDX are implemented.
 */
static
void*
gcGetObjectBase(Collector *gcif, const void* mem)
{
	int idx;
	gc_block* info;

	/* quickly reject pointers that are not part of this heap */
	if (!IS_A_HEAP_POINTER(mem)) {
		return (0);
	}

	info = GCMEM2BLOCK(mem);
	if (!info->inuse) {
		/* go down block list to find out whether we were hitting
		 * in a large object
		 */
		while (!info->inuse && (uintp)info > (uintp)gc_block_base) {
			info--;
		}
		/* must be a large block, hence nr must be 1 */
		if (!info->inuse || info->nr != 1) {
			return (0);
		}
	}

	idx = GCMEM2IDX(info, mem);

	/* report fixed objects as well */
	if (idx < info->nr && 
	    ((GC_GET_COLOUR(info, idx) & GC_COLOUR_INUSE) || 
	     (GC_GET_COLOUR(info, idx) & GC_COLOUR_FIXED))) 
	{
	    	return (UTOMEM(GCBLOCK2MEM(info, idx)));
	}
	return (0);
}

static
const char*
gcGetObjectDescription(Collector* gcif, const void* mem)
{
	return (gcFunctions[gcGetObjectIndex(gcif, mem)].description);
}

/*
 * Walk a bit of memory.
 */
static
void
gcWalkMemory(Collector* gcif, void* mem)
{
	gc_block* info;
	int idx;
	gc_unit* unit;
	uint32 size;
	walk_func_t walkf;

	unit = UTOUNIT(mem);
	info = GCMEM2BLOCK(unit);
	idx = GCMEM2IDX(info, unit);

	UREMOVELIST(unit);
	UAPPENDLIST(gclists[black], unit);
	GC_SET_COLOUR(info, idx, GC_COLOUR_BLACK);

	assert(GC_GET_FUNCS(info, idx) < 
		sizeof(gcFunctions)/sizeof(gcFunctions[0]));
	size = GCBLOCKSIZE(info);
	record_marked(1, size);
	walkf = gcFunctions[GC_GET_FUNCS(info, idx)].walk;
	if (walkf != 0) {
DBG(GCWALK,	
		dprintf("walking %d bytes @%08p: %s\n", size, mem, 
			describeObject(mem));
    )
		walkf(gcif, mem, size);
	}
}

#ifdef DEBUG
static int
gcClearCounts(Hjava_lang_Class *c, void *_)
{
	c->live_count = 0;
	return 0;
}

static int
gcDumpCounts(Hjava_lang_Class *c, void *_)
{
	if (c->live_count)
		dprintf("%7d %s\n", c->live_count,	c->name->data);
	return 0;
}
#endif

/*
 * The Garbage Collector sits in a loop starting a collection, waiting
 * until it's finished incrementally, then tidying up before starting
 * another one.
 */
static void
gcMan(void* arg)
{
	gc_unit* unit;
	gc_unit* nunit;
	gc_block* info;
	int idx;
	Collector *gcif = (Collector*)arg;
	int iLockRoot;

	/* Wake up anyone waiting for the GC to finish every time we're done */
	for (;;) {
		lockStaticMutex(&gcman);

		while (gcRunning == 0) {
			waitStaticCond(&gcman, 0);
		}
		/* We have observed that gcRunning went from 0 to 1 or 2 
		 * One thread requested a gc.  We will decide whether to gc
		 * or not, and then we will set gcRunning back to 0 and
		 * inform the calling thread of the change
		 */
		assert(gcRunning > 0);

		/* 
		 * gcRunning will either be 1 or 2.  If it's 1, we can apply
		 * some heuristics for when we skip a collection.
		 * If it's 2, we must collect.  See gcInvokeGC.
		 */

		/* First, since multiple thread can wake us up without 
		 * coordinating with each other, we must make sure that we
		 * don't collect multiple times in a row.
		 */
                if (gcRunning == 1 && gcStats.allocmem == 0) {
			/* XXX: If an application runs out of memory, it may be 
			 * possible that an outofmemory error was raised and the
			 * application in turn dropped some references.  Then
			 * allocmem will be 0, yet a gc would be in order.
			 * Once we implement OOM Errors properly, we will fix 
			 * this; for now, this guards against wakeups by 
			 * multiple threads.
			 */
DBG(GCSTAT,
			dprintf("skipping collection cause allocmem==0...\n");
    )
			goto gcend;
                }

		/*
		 * Now try to decide whether we should postpone the gc and get
		 * some memory from the system instead.
		 *
		 * If we already use the maximum amount of memory, we must gc.
		 *
		 * Otherwise, wait until the newly allocated memory is at 
		 * least 1/4 of the total memory in use.  Assuming that the
		 * gc will collect all newly allocated memory, this would 
		 * asymptotically converge to a memory usage of approximately
		 * 4/3 the amount of long-lived and fixed data combined.
		 *
		 * Feel free to tweak this parameter.
		 * NB: Boehm calls this the liveness factor, we stole the
		 * default 1/4 setting from there.
		 *
		 * XXX: make this a run-time configurable parameter.
		 */
		if (gcRunning == 1 && gc_heap_total < gc_heap_limit && 
		    gcStats.allocmem * 4 < gcStats.totalmem * 1) {
DBG(GCSTAT,
			dprintf("skipping collection since alloc/total "
				"%dK/%dK = %.2f < 1/3\n",
				gcStats.allocmem/1024, 
				gcStats.totalmem/1024,
				gcStats.allocmem/(double)gcStats.totalmem);
    )
			goto gcend;
		}

		lockStaticMutex(&gc_lock);
		DBG(GCSTAT, walkClassPool(gcClearCounts, 0));
		    
		startGC(gcif);

		for (unit = gclists[grey].cnext; unit != &gclists[grey]; unit = gclists[grey].cnext) {
			gcWalkMemory(gcif, UTOMEM(unit));
		}
		/* Now walk any white objects which will be finalized.  They
		 * may get reattached, so anything they reference must also
		 * be live just in case.
		 */
		for (unit = gclists[white].cnext; unit != &gclists[white]; unit = nunit) {
			nunit = unit->cnext;
			info = GCMEM2BLOCK(unit);
			idx = GCMEM2IDX(info, unit);
			if (GC_GET_STATE(info, idx) == GC_STATE_NEEDFINALIZE) {
				/* this assert is somewhat expensive */
				DBG(GCDIAG,
				    assert(gc_heap_isobject(info, unit)));
				GC_SET_STATE(info, idx, GC_STATE_INFINALIZE);
				markObjectDontCheck(unit, info, idx);
			}
		}
		/* We may now have more grey objects, so walk them */
		for (unit = gclists[grey].cnext; unit != &gclists[grey]; unit = gclists[grey].cnext) {
			gcWalkMemory(gcif, UTOMEM(unit));
		}

		finishGC(gcif);

		DBG(GCSTAT,
		    dprintf("REACHABLE OBJECT HISTOGRAM\n");
		    dprintf("%-7s %s\n", "COUNT", "CLASS");
		    dprintf("%-7s %s\n", "-------",
			    "-----------------------------------"
			    "-----------------------------------");
		    walkClassPool(gcDumpCounts, 0));
		    
		unlockStaticMutex(&gc_lock);

		startFinalizer();

		if (Kaffe_JavaVMArgs[0].enableVerboseGC > 0) {
			/* print out all the info you ever wanted to know */
			fprintf(stderr, 
			    "<GC: heap %dK, total before %dK,"
			    " after %dK (%d/%d objs)\n %2.1f%% free,"
			    " alloced %dK (#%d), marked %dK, "
			    "swept %dK (#%d)\n"
			    " %d objs (%dK) awaiting finalization>\n",
			(int)(gc_heap_total/1024), 
			gcStats.totalmem/1024, 
			(gcStats.totalmem-gcStats.freedmem)/1024, 
			gcStats.totalobj,
			gcStats.totalobj-gcStats.freedobj,
			(1.0 - ((gcStats.totalmem-gcStats.freedmem)/
				(double)gc_heap_total)) * 100.0,
			gcStats.allocmem/1024,
			gcStats.allocobj,
			gcStats.markedmem/1024, 
			gcStats.freedmem/1024,
			gcStats.freedobj,
			gcStats.finalobj,
			gcStats.finalmem/1024);
		}
		if (Kaffe_JavaVMArgs[0].enableVerboseGC > 1) {
			OBJECTSTATSPRINT();
		}
		gcStats.totalmem -= gcStats.freedmem;
		gcStats.totalobj -= gcStats.freedobj;
		gcStats.allocobj = 0;
		gcStats.allocmem = 0;

gcend:;
		/* now signal any waiters */
		gcRunning = 0;
		broadcastStaticCond(&gcman);
		unlockStaticMutex(&gcman);
	}
}

/*
 * Start the GC process by scanning the root and thread stack objects.
 */
static
void
startGC(Collector *gcif)
{
	gc_unit* unit;
	gc_unit* nunit;

	gcStats.freedmem = 0;
	gcStats.freedobj = 0;
	gcStats.markedobj = 0;
	gcStats.markedmem = 0;

	/* disable the mutator to protect colour lists */
	STOPWORLD();

	/* measure time */
	startTiming(&gc_time, "gctime-scan");

	/* Walk all objects on the finalizer list */
	for (unit = gclists[finalise].cnext;
	     unit != &gclists[finalise]; unit = nunit) {
		nunit = unit->cnext;
		gcMarkObject(gcif, UTOMEM(unit));
	}

	(*walkRootSet)(gcif);
}

/*
 * Finish off the GC process.  Any unreached (white) objects are moved
 * for finalising and the finaliser woken.
 * The reached (black) objects are moved onto the now empty white list
 * and recoloured white.
 */
static
void
finishGC(Collector *gcif)
{
	gc_unit* unit;
	gc_block* info;
	int idx;

	/* There shouldn't be any grey objects at this point */
	assert(gclists[grey].cnext == &gclists[grey]);

	/* 
	 * Any white objects should now be freed, but we cannot call
	 * gc_heap_free here because we might block in gc_heap_free, 
	 * which would leave the white list unprotected.
	 * So we move them to a 'mustfree' list from where we'll pull them
	 * off later.
	 *
	 * XXX: this is so silly it hurts.  Jason has a fix.
	 */
	while (gclists[white].cnext != &gclists[white]) {
		unit = gclists[white].cnext;
		UREMOVELIST(unit);

		info = GCMEM2BLOCK(unit);
		idx = GCMEM2IDX(info, unit);

		assert(GC_GET_COLOUR(info, idx) == GC_COLOUR_WHITE);
		assert(GC_GET_STATE(info, idx) == GC_STATE_NORMAL);
		gcStats.freedmem += GCBLOCKSIZE(info);
		gcStats.freedobj += 1;
		UAPPENDLIST(gclists[mustfree], unit);
		OBJECTSTATSREMOVE(unit);
	}

	/* 
	 * Now move the black objects back to the white queue for next time.
	 * Note that all objects that were eligible for finalization are now
	 * black - this is so because we marked and then walked them.
	 * We recognize them by their "INFINALIZE" state, however, and put
	 * them on the finalise list.
	 */
	while (gclists[black].cnext != &gclists[black]) {
		unit = gclists[black].cnext;
		UREMOVELIST(unit);

		info = GCMEM2BLOCK(unit);
		idx = GCMEM2IDX(info, unit);

		assert(GC_GET_COLOUR(info, idx) == GC_COLOUR_BLACK);

		if (GC_GET_STATE(info, idx) == GC_STATE_INFINALIZE) {
			gcStats.finalmem += GCBLOCKSIZE(info);
			gcStats.finalobj += 1;
			UAPPENDLIST(gclists[finalise], unit);
		}
		else {
			UAPPENDLIST(gclists[white], unit);
		}
		GC_SET_COLOUR(info, idx, GC_COLOUR_WHITE);
	}

	/* this is where we'll stop locking out other threads 
	 * measure gc time until here.  This is not quite accurate, as
	 * it excludes the time to sweep objects, but lacking
	 * per-thread timing it's a reasonable thing to do.
	 */
	stopTiming(&gc_time);

	/* 
	 * Now that all lists that the mutator manipulates are in a
	 * consistent state, we can reenable the mutator here 
	 */
	RESUMEWORLD();

	/* 
	 * Now free the objects.  We can block here since we're the only
	 * thread manipulating the "mustfree" list.
	 */
	startTiming(&sweep_time, "gctime-sweep");

	while (gclists[mustfree].cnext != &gclists[mustfree]) {
		destroy_func_t destroy;
		unit = gclists[mustfree].cnext;

		/* invoke destroy function before freeing the object */
		info = GCMEM2BLOCK(unit);
		idx = GCMEM2IDX(info, unit);
		destroy = gcFunctions[GC_GET_FUNCS(info,idx)].destroy;
		if (destroy != 0) {
			destroy(gcif, UTOMEM(unit));
		}

		UREMOVELIST(unit);
		addToCounter(&gcgcablemem, "gcmem-gcable objects", 1, 
			-((jlong)GCBLOCKSIZE(info)));
		gc_heap_free(unit);
	}
	stopTiming(&sweep_time);
}

static
void
startFinalizer(void)
{
	int iLockRoot;
	int start;

        start = 0;

	lockStaticMutex(&gc_lock);
	/* If there's stuff to be finalised then we'd better do it */
	if (gclists[finalise].cnext != &gclists[finalise]) {
		start = 1;
	}
	unlockStaticMutex(&gc_lock);

	lockStaticMutex(&finman);
	if (start != 0 && finalRunning == false) {
		finalRunning = true;
		signalStaticCond(&finman);
	}
	unlockStaticMutex(&finman);

}

/*
 * The finaliser sits in a loop waiting to finalise objects.  When a
 * new finalised list is available, it is woken by the GC and finalises
 * the objects in turn.  An object is only finalised once after which
 * it is deleted.
 */
static void
finaliserMan(void* arg)
{
	gc_block* info;
	gc_unit* unit;
	int idx;
	Collector *gcif = (Collector*)arg;
	int iLockRoot;


	for (;;) {
		lockStaticMutex(&finman);

		finalRunning = false;
		while (finalRunning == false) {
			waitStaticCond(&finman, 0);
		}
		assert(finalRunning == true);

		while (gclists[finalise].cnext != &gclists[finalise]) {
			lockStaticMutex(&gc_lock);
			unit = gclists[finalise].cnext;
			UREMOVELIST(unit);
			UAPPENDLIST(gclists[grey], unit);

			info = GCMEM2BLOCK(unit);
			idx = GCMEM2IDX(info, unit);
			gcStats.finalmem -= GCBLOCKSIZE(info);
			gcStats.finalobj -= 1;

			assert(GC_GET_STATE(info,idx) == GC_STATE_INFINALIZE);
			/* Objects are only finalised once */
			GC_SET_STATE(info, idx, GC_STATE_FINALIZED);
			GC_SET_COLOUR(info, idx, GC_COLOUR_GREY);
			unlockStaticMutex(&gc_lock);

			/* Call finaliser */
			unlockStaticMutex(&finman);
			(*gcFunctions[GC_GET_FUNCS(info,idx)].final)(gcif, UTOMEM(unit));
			lockStaticMutex(&finman);
		}

		/* Wake up anyone waiting for the finalizer to finish */
		broadcastStaticCond(&finman);
		unlockStaticMutex(&finman);
	}
}

/*
 * Explicity invoke the garbage collector and wait for it to complete.
 */
static
void
gcInvokeGC(Collector* gcif, int mustgc)
{
	int iLockRoot;

	lockStaticMutex(&gcman);
	if (gcRunning == 0) {
		gcRunning = mustgc ? 2 : 1;
		signalStaticCond(&gcman);
	}
	unlockStaticMutex(&gcman);

	lockStaticMutex(&gcman);
	while (gcRunning != 0) {
		waitStaticCond(&gcman, 0);
	}
	unlockStaticMutex(&gcman);
}

/*
 * GC and invoke the finalizer.  Used to run finalizers on exit.
 */
static
void
gcInvokeFinalizer(Collector* gcif)
{
	int iLockRoot;

	/* First invoke the GC */
	GC_invoke(gcif, 1);

	/* Run the finalizer (if might already be running as a result of
	 * the GC)
	 */
	lockStaticMutex(&finman);
	if (finalRunning == false) {
		finalRunning = true;
		signalStaticCond(&finman); 
	}
	waitStaticCond(&finman, 0);
	unlockStaticMutex(&finman);
}

/*
 * Allocate a new object.  The object is attached to the white queue.
 * After allocation, if incremental collection is active we peform
 * a little garbage collection.  If we finish it, we wakeup the garbage
 * collector.
 */

void throwOutOfMemory(void) __NORETURN__;

static
void*
gcMalloc(Collector* gcif, size_t size, int fidx)
{
	gc_block* info;
	gc_unit* unit;
	void * volatile mem;	/* needed on SGI, see comment below */
	int i;
	size_t bsz;
	int iLockRoot;

	assert(gc_init != 0);
	assert(fidx < nrTypes && size != 0);

	unit = gc_heap_malloc(size + sizeof(gc_unit));

	/* keep pointer to object */
	mem = UTOMEM(unit);
	if (unit == 0) {
		return 0;
	}

	lockStaticMutex(&gc_lock);

	info = GCMEM2BLOCK(mem);
	i = GCMEM2IDX(info, unit);

	bsz = GCBLOCKSIZE(info);
	gcStats.totalmem += bsz;
	gcStats.totalobj += 1;
	gcStats.allocmem += bsz;
	gcStats.allocobj += 1;

	GC_SET_FUNCS(info, i, fidx);

	OBJECTSTATSADD(unit);

	/* Determine whether we need to finalise or not */
	if (gcFunctions[fidx].final == GC_OBJECT_NORMAL || gcFunctions[fidx].final == GC_OBJECT_FIXED) {
		GC_SET_STATE(info, i, GC_STATE_NORMAL);
	}
	else {
		GC_SET_STATE(info, i, GC_STATE_NEEDFINALIZE);
	}

	/* If object is fixed, we give it the fixed colour and do not
	 * attach it to any lists.  This object is not part of the GC
	 * regime and must be freed explicitly.
	 */
	if (gcFunctions[fidx].final == GC_OBJECT_FIXED) {
		addToCounter(&gcfixedmem, "gcmem-fixed objects", 1, bsz);
		GC_SET_COLOUR(info, i, GC_COLOUR_FIXED);
	}
	else {
		addToCounter(&gcgcablemem, "gcmem-gcable objects", 1, bsz);
		/*
		 * Note that as soon as we put the object on the white list,
		 * the gc might come along and free the object if it can't
		 * find any references to it.  This is why we need to keep
		 * a reference in `mem'.  Note that keeping a reference in
		 * `unit' will not do because markObject performs a UTOUNIT()!
		 * In addition, on some architectures (SGI), we must tell the
		 * compiler to not delay computing mem by defining it volatile.
		 */
		GC_SET_COLOUR(info, i, GC_COLOUR_WHITE);
		UAPPENDLIST(gclists[white], unit);
	}
	if (!reserve) {
		reserve = gc_primitive_reserve();
	}

	/* It is not safe to allocate java objects the first time
	 * gcMalloc is called, but it should be safe after gcEnable
	 * has been called.
	 */
	if (garbageman && !outOfMem && !outOfMem_allocator) {
		outOfMem_allocator = jthread_current();
	}

	unlockStaticMutex(&gc_lock);

	/* jthread_current() will be null in some window before we
	 * should try allocating java objects
	 */
	if (!outOfMem && outOfMem_allocator
	    && outOfMem_allocator == jthread_current()) { 
		outOfMem = OOM_ALLOCATING;
		outOfMem = OutOfMemoryError; /* implicit allocation */
		outOfMem_allocator = 0;
		gc_add_ref(outOfMem);
	}
	return (mem);
}

static
Hjava_lang_Throwable *
gcThrowOOM(Collector *gcif)
{
	Hjava_lang_Throwable *ret = 0;
	int reffed;
	int iLockRoot;

	/*
	 * Make sure we are the only thread to use this exception
	 * object.
	 */
	lockStaticMutex(&gc_lock);
	ret = outOfMem;
	reffed = outOfMem != 0;
	outOfMem = 0;
	/* We try allocating reserved pages before we allocate the
	 * outOfMemory error.  We can use some or all of the reserved
	 * pages to actually grab an error.
	 */
	if (reserve) {
		gc_primitive_free(reserve);
		reserve = 0;
		if (!ret || ret == OOM_ALLOCATING) {
			unlockStaticMutex(&gc_lock);
			ret = OutOfMemoryError; /* implicit allocation */
			lockStaticMutex(&gc_lock);
		}
	}
	if (ret == OOM_ALLOCATING || ret == 0) {
		/* die now */
		unlockStaticMutex(&gc_lock);
		fprintf(stderr,
			"Not enough memory to throw OutOfMemoryError!\n");
		ABORT();
	}
	unlockStaticMutex(&gc_lock);
	if (reffed) gc_rm_ref(ret);
	return ret;
}

/*
 * Reallocate an object.
 */
static
void*
gcRealloc(Collector* gcif, void* mem, size_t size, int fidx)
{
	gc_block* info;
	int idx;
	void* newmem;
	gc_unit* unit;
	int osize;
	int iLockRoot;

	assert(gcFunctions[fidx].final == GC_OBJECT_FIXED);

	/* If nothing to realloc from, just allocate */
	if (mem == NULL) {
		return (gcMalloc(gcif, size, fidx));
	}

	lockStaticMutex(&gc_lock);
	unit = UTOUNIT(mem);
	info = GCMEM2BLOCK(unit);
	idx = GCMEM2IDX(info, unit);
	osize = GCBLOCKSIZE(info);

	/* Can only handled fixed objects at the moment */
	assert(GC_GET_COLOUR(info, idx) == GC_COLOUR_FIXED);
	info = 0;
	unlockStaticMutex(&gc_lock);

	/* If we'll fit into the current space, just send it back */
	if (osize >= size + sizeof(gc_unit)) {
		return (mem);
	}

	/* Allocate new memory, copy data, and free the old */
	newmem = gcMalloc(gcif, size, fidx);
	memcpy(newmem, mem, osize);
	gcFree(gcif, mem);

	return (newmem);
}

/*
 * Explicitly free an object.
 */
static
void
gcFree(Collector* gcif, void* mem)
{
	gc_block* info;
	int idx;
	gc_unit* unit;
	int iLockRoot;

	if (mem != 0) {
		lockStaticMutex(&gc_lock);
		unit = UTOUNIT(mem);
		info = GCMEM2BLOCK(unit);
		idx = GCMEM2IDX(info, unit);

		if (GC_GET_COLOUR(info, idx) == GC_COLOUR_FIXED) {
			size_t sz = GCBLOCKSIZE(info);

			OBJECTSTATSREMOVE(unit);

			/* Keep the stats correct */
			gcStats.totalmem -= sz;
			gcStats.totalobj -= 1;
			addToCounter(&gcfixedmem, "gcmem-fixed objects", 1, -(jlong)sz);

			gc_heap_free(unit);
		}
		else {
			assert(!!!"Attempt to explicitly free nonfixed object");
		}
		unlockStaticMutex(&gc_lock);
	}
}

static
void
gcInit(Collector *collector)
{
	gc_init = 1;
}

/*
 * Start gc threads, which enable collection
 */
static 
void
/* ARGSUSED */
gcEnable(Collector* collector)
{
	errorInfo info;

        if (DBGEXPR(NOGC, false, true))
        {
                /* Start the GC daemons we need */
                finalman = createDaemon(&finaliserMan, "finaliser", 
				collector, THREAD_MAXPRIO,
					FINALIZERSTACKSIZE, &info);
                garbageman = createDaemon(&gcMan, "gc", 
				collector, THREAD_MAXPRIO,
					  GCSTACKSIZE, &info);
		assert(finalman && garbageman);
        }
}

#if defined(SUPPORT_VERBOSEMEM)

/* --------------------------------------------------------------------- */
/* The following functions are strictly for statistics gathering	 */

static
void
objectStatsChange(gc_unit* unit, int diff)
{
	gc_block* info;
	int idx;

	info = GCMEM2BLOCK(unit);
	idx = GC_GET_FUNCS(info, GCMEM2IDX(info, unit));

	assert(idx >= 0 && idx < nrTypes);
	gcFunctions[idx].nr += diff * 1;
	gcFunctions[idx].mem += diff * GCBLOCKSIZE(info);
}

static void
objectStatsPrint(void)
{
	int cnt = 0;

	fprintf(stderr, "Memory statistics:\n");
	fprintf(stderr, "------------------\n");

	while (cnt < nrTypes) {
		fprintf(stderr, "%14.14s: Nr %6d  Mem %6dK",
			gcFunctions[cnt].description, 
			gcFunctions[cnt].nr, 
			gcFunctions[cnt].mem/1024);
		if (++cnt % 2 != 0) {
			fprintf(stderr, "   ");
		} else {
			fprintf(stderr, "\n");
		}
	}

	if (cnt % 2 != 0) {
		fprintf(stderr, "\n");
	}
}
#endif

/*
 * vtable for object implementing the collector interface.
 */
static struct GarbageCollectorInterface_Ops GC_Ops = {
	0,		/* reserved */
	0,		/* reserved */
	0,		/* reserved */
	gcMalloc,
	gcRealloc,
	gcFree,
	gcInvokeGC,
	gcInvokeFinalizer,
	gcInit,
	gcEnable,
	gcMarkAddress,
	gcMarkObject,
	gcGetObjectSize,
	gcGetObjectDescription,
	gcGetObjectIndex,
	gcGetObjectBase,
	gcWalkMemory,
	gcWalkConservative,
	gcRegisterFixedTypeByIndex,
	gcRegisterGcTypeByIndex,
	gcThrowOOM
};

/*
 * Initialise the Garbage Collection system.
 */
Collector* 
createGC(void (*_walkRootSet)(Collector*))
{
	walkRootSet = _walkRootSet;
	URESETLIST(gclists[white]);
	URESETLIST(gclists[grey]);
	URESETLIST(gclists[black]);
	URESETLIST(gclists[finalise]);
	URESETLIST(gclists[mustfree]);
	gc_obj.collector.ops = &GC_Ops;
	return (&gc_obj.collector);
}

