/* gc-mem.c
 * The heap manager.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#include "debug.h"
/* undefine this to revert to old tile scheme */
#define	PREDEFINED_NUMBER_OF_TILES

#include "config.h"
#include "config-std.h"
#include "config-mem.h"
#include "gtypes.h"
#include "baseClasses.h"
#include "support.h"
#include "stats.h"
#include "locks.h"
#include "thread.h"
#include "gc.h"
#include "gc-mem.h"
#include "jni.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(HAVE_MPROTECT) && defined(KAFFE_VMDEBUG)
#include <sys/mman.h>
#endif

static iStaticLock	gc_heap_lock;

#if defined(KAFFE_STATS)
static counter gcpages;
#endif
static gc_block* gc_small_block(size_t);
static gc_block* gc_large_block(size_t);

static gc_block* gc_primitive_alloc(size_t);
void gc_primitive_free(gc_block*);

/**
 * A preallocated block for small objects.
 *
 * @list list of gc_blocks available for objects of the same size
 * @sz   the size of the objects that can be stored in @list. 
 */
typedef struct {
	gc_block* list;
	uint16	  sz;
} gc_freelist;

/**
 * Array of preallocated blocks.
 *  
 */
static gc_freelist freelist[NR_FREELISTS+1]
#ifdef PREDEFINED_NUMBER_OF_TILES
	= {
#define	S(sz)	{ 0, sz }
	S(16),
	S(24),
	S(32),
	S(40),
	S(48),
	S(56),
	S(64),
	S(80),
	S(96),
	S(112),
	S(128),
	S(160),
	S(192),
	S(224),
	S(240),
	S(496),
	S(1000),
	S(2016),
	S(4040),
	{ (gc_block *)-1, 0 }
}
#endif /* PREDEFINED_NUMBER_OF_TILES */
;

/**
 * Maps a given size to a freelist entry. 
 *
 */
static struct {
	uint16	list;
} sztable[MAX_SMALL_OBJECT_SIZE+1];
static int max_freelist;

static size_t max_small_object_size;

size_t gc_heap_total;		/* current size of the heap */
size_t gc_heap_allocation_size;	/* amount of memory by which to grow heap */
size_t gc_heap_initial_size;	/* amount of memory to initially allocate */
size_t gc_heap_limit;		/* maximum size to which heap should grow */
uintp gc_heap_base;
uintp gc_heap_range;

#ifndef gc_pgsize
size_t gc_pgsize;
int gc_pgbits;
#endif

#ifdef KAFFE_VMDEBUG
int gc_system_alloc_cnt;
#endif

extern struct Hjava_lang_Thread* garbageman;

#ifdef KAFFE_VMDEBUG
/*
 * analyze the slack incurred by small objects
 */
static int totalslack;
static int totalsmallobjs;

static void 
printslack(void)
{
	dprintf(
		"allocated %d small objects, total slack %d, slack/per "
		"object %8.2f\n", 
		totalsmallobjs, totalslack, totalslack/(double)totalsmallobjs);
}


/*
 * check whether the heap is still in a consistent state
 */
static void
gc_heap_check(void)
{
	int i; 

	for (i = 0; i < NR_FREELISTS; i++) {
		gc_block* blk = freelist[i].list;
		if (blk == 0 || blk == (gc_block*)-1) {
			continue;
		} else {
			gc_freeobj* mem = blk->free;

			assert(GCBLOCKINUSE(blk));
			assert(blk->avail < blk->nr);
			assert(blk->funcs == (uint8*)GCBLOCK2BASE(blk));
			assert(blk->state == (uint8*)(blk->funcs + blk->nr));
			assert(blk->data  == (uint8*)ROUNDUPALIGN(blk->state + blk->nr));

			while (mem) {
				ASSERT_ONBLOCK(mem, blk);
				mem = mem->next;
			}
		}
	}
}

#endif /* KAFFE_VMDEBUG */

/*
 * Initialise allocator.
 */
void
gc_heap_initialise(void)
{
#ifndef gc_pgsize
	gc_pgsize = getpagesize();
	for (gc_pgbits = 0;
	     (1 << gc_pgbits) != gc_pgsize && gc_pgbits < 64;
	     gc_pgbits++)
		;
	assert(gc_pgbits < 64);
#endif

	gc_heap_allocation_size = Kaffe_JavaVMArgs[0].allocHeapSize;
	gc_heap_initial_size = Kaffe_JavaVMArgs[0].minHeapSize;
	gc_heap_limit = Kaffe_JavaVMArgs[0].maxHeapSize;

	/*
	 * Perform some sanity checks.
	 */
	if (gc_heap_initial_size > gc_heap_limit) {
		dprintf(
		    "Initial heap size (%dK) > Maximum heap size (%dK)\n",
		    (int) (gc_heap_initial_size/1024), (int)(gc_heap_limit/1024));
		EXIT(-1);
	}

#ifndef PREDEFINED_NUMBER_OF_TILES
    {
	int i;
	int l;
	int b;
	int t;

	/* old scheme, where number of tiles was approximated by a series
	 * of powers of two
	 */
#define	OBJSIZE(NR) \
	((gc_pgsize-ROUNDUPALIGN(1)-(NR*(2+sizeof(void*))))/NR)

	/* For a given number of tiles in a block, work out the size of
	 * the allocatable units which'll fit in them and build a translation
	 * table for the sizes.
	 */
	i = 1;
	max_small_object_size = ROUNDDOWNALIGN(OBJSIZE(i));
	l = max_small_object_size;
	for (;;) {
		b = ROUNDDOWNALIGN(OBJSIZE(i));
		if (b >= MIN_OBJECT_SIZE) {
			for (t = l; t > b; t--) {
				sztable[t].list = l;
			}
			l = t;
			i <<= 1;
		}
		else {
			for (t = l; t > MIN_OBJECT_SIZE; t--) {
				sztable[t].list = l;
			}
			for (t = 0; t <= MIN_OBJECT_SIZE; t++) {
				sztable[t].list = MIN_OBJECT_SIZE;
			}
			break;
		}
	}

	/* Translate table into list numbers */
	i = -1;
	b = -1;
	for (l = 0; l <= max_small_object_size; l++) {
		if (sztable[l].list != b) {
			b = sztable[l].list;
			i++;
			freelist[i].sz = b;
		}
		sztable[l].list = i;
	}
	max_freelist = i;
    }
#else
	/* PREDEFINED_NUMBER_OF_TILES */
	{
		/*
		 * Use the preinitialized freelist table to initialize
		 * the sztable.
		 */
		int sz = 0;
		uint16 flidx = 0;
		while (freelist[flidx].list == 0) {
			for (; sz <= freelist[flidx].sz; sz++)
				sztable[sz].list = flidx;
			flidx++;
		}
		max_small_object_size = sz - 1;
		max_freelist = flidx;
	}
#endif

DBG(SLACKANAL,
	atexit(printslack);
    )

#undef	OBJSIZE

	/* Round 'gc_heap_allocation_size' up to pagesize */
	gc_heap_allocation_size = ROUNDUPPAGESIZE(gc_heap_allocation_size);

	/* Round 'gc_heap_initial_size' up to pagesize */
	gc_heap_initial_size = ROUNDUPPAGESIZE(gc_heap_initial_size);

	/* allocate heap of initial size from system */
	gc_heap_grow(gc_heap_initial_size);
}

/**
 * Allocate a piece of memory.
 */
void*
gc_heap_malloc(size_t sz)
{
	size_t lnr;
	gc_freeobj* mem = 0;
	gc_block** mptr;
	gc_block* blk;
	size_t nsz;
	int iLockRoot;

	lockStaticMutex(&gc_heap_lock);

DBG(SLACKANAL,
	if (GC_SMALL_OBJECT(sz)) {
		totalslack += (freelist[sztable[sz].list].sz - sz);
		totalsmallobjs++;
	}
    )

DBG(GCDIAG, 
	gc_heap_check();
    )

	if (GC_SMALL_OBJECT(sz)) {

		/* Translate size to object free list */
		lnr = sztable[sz].list;
		nsz = freelist[lnr].sz;

		/* No available objects? Allocate some more */
		mptr = &freelist[lnr].list;
		if (*mptr != 0) {
			blk = *mptr;
			assert(blk->free != 0);
DBG(GCALLOC,		dprintf("gc_heap_malloc: freelist %ld at %p free %p\n", 
				(long) sz, *mptr, blk->free);)
		}
		else {
			blk = gc_small_block(nsz);
			if (blk == 0) {
				goto out;
			}
			blk->next = *mptr;
			*mptr = blk;

DBG(GCALLOC,		dprintf("gc_heap_malloc: small block %ld at %p free %p\n", 
				(long) sz, *mptr, blk->free);)
		}

		/* Unlink free one and return it */
		mem = blk->free;

		DBG(GCDIAG,
		    assert(blk->magic == GC_MAGIC);
		    ASSERT_ONBLOCK(mem, blk);
		    if (mem->next) ASSERT_ONBLOCK(mem->next, blk));

		blk->free = mem->next;

		GC_SET_STATE(blk, GCMEM2IDX(blk, mem), GC_STATE_NORMAL);

		/* Once we use all the sub-blocks up, remove the whole block
		 * from the freelist.
		 */
		assert(blk->nr >= blk->avail);
		assert(blk->avail > 0);
		blk->avail--;
		if (blk->avail == 0) {
			*mptr = blk->next;
		}
	}
	else {
		nsz = sz;
		blk = gc_large_block(nsz);
		if (blk == 0) {
			goto out;
		}
		mem = GCBLOCK2FREE(blk, 0);
		GC_SET_STATE(blk, 0, GC_STATE_NORMAL);
DBG(GCALLOC,	dprintf("gc_heap_malloc: large block %ld at %p\n", 
			(long) sz, mem);	)
		blk->avail--;
		assert(blk->avail == 0);
	}

	/* Clear memory */
	memset(mem, 0, nsz);

	assert(GC_OBJECT_SIZE(mem) >= sz);

	out:
	unlockStaticMutex(&gc_heap_lock);

	return (mem);
}

/**
 * Free a piece of memory.
 */
void
gc_heap_free(void* mem)
{
	gc_block* info;
	gc_freeobj* obj;
	int lnr;
	int msz;
	int idx;
	int iLockRoot;

	info = GCMEM2BLOCK(mem);
	idx = GCMEM2IDX(info, mem);

	DBG(GCDIAG,
	    gc_heap_check();
	    assert(info->magic == GC_MAGIC);
	    assert(GC_GET_COLOUR(info, idx) != GC_COLOUR_FREE));

	GC_SET_COLOUR(info, idx, GC_COLOUR_FREE);

DBG(GCFREE,
	dprintf("gc_heap_free: memory %p size %d\n", mem, info->size);	)

	lockStaticMutex(&gc_heap_lock);

	if (GC_SMALL_OBJECT(info->size)) {
		lnr = sztable[info->size].list;
	
		info->avail++;
		DBG(GCDIAG,
		    /* write pattern in memory to see when live objects were
		     * freed - Note that (f4f4f4f4 == -185273100)
		     */
		    memset(mem, 0xf4, info->size));
		obj = GCMEM2FREE(mem);
		obj->next = info->free;
		info->free = obj;

		ASSERT_ONBLOCK(obj, info);

		/* If we free all sub-blocks, free the block */
		assert(info->avail <= info->nr);
		if (info->avail == info->nr) {
			/*
			 * note that *finfo==0 is ok if we free a block
			 * whose small object is so large that it can
			 * only contain one object.
			 */
			gc_block** finfo = &freelist[lnr].list;
			for (;*finfo;) {
				if (*finfo == info) {
					(*finfo) = info->next;
					break;
				}
				finfo = &(*finfo)->next;
			}

			info->size = gc_pgsize;
			gc_primitive_free(info);
		} else if (info->avail==1) {
			/*
			 * If this block contains no free sub-blocks yet, attach
			 * it to freelist. 
			 */
			gc_block **finfo = &freelist[lnr].list;

			info->next = *finfo; 
			*finfo = info;
		}

	}
	else {
		/* Calculate true size of block */
		msz = info->size + 2 + ROUNDUPALIGN(1);
		msz = ROUNDUPPAGESIZE(msz);
		info->size = msz;
		gc_primitive_free(info);
	}

	unlockStaticMutex(&gc_heap_lock);

DBG(GCDIAG,
	gc_heap_check();
    )

}

/*
 * Allocate a new block of GC'ed memory.  The block will contain 'nr' objects
 * each of 'sz' bytes.
 */
static
gc_block*
gc_small_block(size_t sz)
{
	gc_block* info;
	int i;
	int nr;

	info = gc_primitive_alloc(gc_pgsize);
	if (info == 0) {
		return (0);
	}

	/* Calculate number of objects in this block */
	nr = (gc_pgsize-ROUNDUPALIGN(1))/(sz+2);

	/* Setup the meta-data for the block */
	DBG(GCDIAG, info->magic = GC_MAGIC);

	info->size = sz;
	info->nr = nr;
	info->avail = nr;
	info->funcs = (uint8*)GCBLOCK2BASE(info);
	info->state = (uint8*)(info->funcs + nr);
	info->data = (uint8*)ROUNDUPALIGN(info->state + nr);

	DBG(GCDIAG, memset(info->data, 0, sz * nr));

	/* Build the objects into a free list */
	for (i = nr-1; i-- > 0;) {
		GCBLOCK2FREE(info, i)->next = GCBLOCK2FREE(info, i+1);
		GC_SET_COLOUR(info, i, GC_COLOUR_FREE);
		GC_SET_STATE(info, i, GC_STATE_NORMAL);
	}
	GCBLOCK2FREE(info, nr-1)->next = 0;
	GC_SET_COLOUR(info, nr-1, GC_COLOUR_FREE);
	GC_SET_STATE(info, nr-1, GC_STATE_NORMAL);
	info->free = GCBLOCK2FREE(info, 0);
DBG(SLACKANAL,
	int slack = ((void *)info) 
		+ gc_pgsize - (void *)(GCBLOCK2MEM(info, nr));
	totalslack += slack;
    )
	return (info);
}

/*
 * Allocate a new block of GC'ed memory.  The block will contain one object
 */
static
gc_block*
gc_large_block(size_t sz)
{
	gc_block* info;
	size_t msz;

	/* Add in management overhead */
	msz = sz+2+ROUNDUPALIGN(1);
	/* Round size up to a number of pages */
	msz = ROUNDUPPAGESIZE(msz);

	info = gc_primitive_alloc(msz);
	if (info == 0) {
		return (0);
	}

	/* Setup the meta-data for the block */
	DBG(GCDIAG, info->magic = GC_MAGIC);

	info->size = sz;
	info->nr = 1;
	info->avail = 1;
	info->funcs = (uint8*)GCBLOCK2BASE(info);
	info->state = (uint8*)(info->funcs + 1);
	info->data = (uint8*)ROUNDUPALIGN(info->state + 1);
	info->free = 0;

	DBG(GCDIAG, memset(info->data, 0, sz));

	GCBLOCK2FREE(info, 0)->next = 0;

	/*
	 * XXX gc_large_block only called during a block allocation.
	 * The following is just going to get overwritten. (Right?)
	 */
	GC_SET_COLOUR(info, 0, GC_COLOUR_FREE);
	GC_SET_STATE(info, 0, GC_STATE_NORMAL);

	return (info);
}

/*
 * Primitive block management:  Allocating and freeing whole pages.
 *
 * Each primitive block of the heap consists of one or more contiguous
 * pages. Pages of unused primitive blocks are marked unreadable when
 * kaffe is compiled with debugging enabled. Whether a block is in use
 * can be determined by its nr field: when it's in use, its nr field
 * will be > 0.
 *
 * All primitive blocks are chained through their pnext / pprev fields,
 * no matter whether or not they are in use. This makes the necessary
 * check for mergable blocks as cheap as possible. Merging small blocks
 * is necessary so that single unused primitive blocks in the heap are
 * always as large as possible. The first block in the list is stored
 * in gc_block_base, the last block in the list is gc_last_block.
 *
 * In order to speed up the search for the primitive block that fits
 * a given allocation request best, small primitive blocks are stored
 * in several lists (one per size). If no primitive blocks of a given
 * size are left, a larger one is splitted instead. 
 */
#define GC_PRIM_LIST_COUNT 20

uintp gc_block_base;
static gc_block *gc_last_block;
static gc_block *gc_prim_freelist[GC_PRIM_LIST_COUNT+1];


#ifndef PROT_NONE
#define PROT_NONE 0
#endif

#if !defined(HAVE_MPROTECT) || !defined(KAFFE_VMDEBUG)
#define mprotect(A,L,P)
#define ALL_PROT
#define NO_PROT
#else
/* In a sense, this is backwards. */
#define ALL_PROT PROT_READ|PROT_WRITE|PROT_EXEC
#define NO_PROT  PROT_NONE
#endif

/* Mark a primitive block as used */
static inline void 
gc_block_add(gc_block *b)
{
	b->nr = 1;
	mprotect(GCBLOCK2BASE(b), b->size, ALL_PROT);
}

/* Mark a primitive block as unused */
static inline void 
gc_block_rm(gc_block *b)
{
	b->nr = 0;
	mprotect(GCBLOCK2BASE(b), b->size, NO_PROT);
}

/* return the prim list blk belongs to */
static inline gc_block **
gc_get_prim_freelist (gc_block *blk)
{
	size_t sz = blk->size >> gc_pgbits;

	if (sz <= GC_PRIM_LIST_COUNT)
	{
		return &gc_prim_freelist[sz-1];
	}

	return &gc_prim_freelist[GC_PRIM_LIST_COUNT];
}

/* add a primitive block to the correct freelist */
static inline void
gc_add_to_prim_freelist(gc_block *blk)
{
	gc_block **list = gc_get_prim_freelist (blk);

	/* insert the block int the list, sorting by ascending addresses */
	while (*list && blk > *list)
	{
		list = &(*list)->next;
	}

	if (*list) {
		(*list)->free = (gc_freeobj *)&blk->next;
	}

	blk->next = *list;
	*list = blk;
	blk->free = (gc_freeobj *)list;
}

/* remove a primitive block from its freelist */
static inline void
gc_remove_from_prim_freelist(gc_block *blk)
{
	*( (gc_block **) blk->free ) = blk->next;

	if (blk->next) {
		blk->next->free = blk->free;
	}
}
 
/*
 * Allocate a block of memory from the free list or, failing that, the
 * system pool.
 */
static
gc_block*
gc_primitive_alloc(size_t sz)
{
	size_t diff = 0;
	gc_block* best_fit = NULL;
	size_t i = sz >> gc_pgbits;

	assert(sz % gc_pgsize == 0);

	DBG(GCPRIM, dprintf("\ngc_primitive_alloc: got to allocate 0x%x bytes\n", sz); )

	/* try freelists for small primitive blocks first */
	if (i <= GC_PRIM_LIST_COUNT) {
		for (i-=1; i<GC_PRIM_LIST_COUNT; i++) {
			if (gc_prim_freelist[i]) {
				best_fit = gc_prim_freelist[i]; 
				diff = gc_prim_freelist[i]->size - sz;
				break;
			}
		}
	}

	/* if that fails, try the big remaining list */
	if (!best_fit) {
		gc_block *ptr;
		for (ptr = gc_prim_freelist[GC_PRIM_LIST_COUNT]; ptr != 0; ptr=ptr->next) {

			/* Best fit */
			if (sz == ptr->size) {
				diff = 0;
				best_fit = ptr;
				break;
			} else if (sz < ptr->size) {
				size_t left = ptr->size - sz;
		
				if (best_fit==NULL || left<diff) {
					diff = left;
					best_fit = ptr;
				}		
			}
		}
	}

	/* if we found a block, remove it from the list and check if splitting is necessary */
	if (best_fit) {
		gc_remove_from_prim_freelist (best_fit);

		DBG(GCPRIM, dprintf ("gc_primitive_alloc: found best_fit %p diff 0x%x (0x%x - 0x%x)\n",
				     best_fit, diff, best_fit->size, sz); )
		assert ( diff % gc_pgsize == 0 );

		if (diff > 0) {
			gc_block *nptr;

			best_fit->size = sz;
		
			nptr = GCBLOCKEND(best_fit);
			nptr->size = diff;
			gc_block_rm (nptr);

			DBG(GCPRIM, dprintf ("gc_primitive_alloc: splitted remaining 0x%x bytes @ %p\n", diff, nptr); )

			DBG(GCDIAG, nptr->magic = GC_MAGIC);

			/* maintain list of primitive blocks */
			nptr->pnext = best_fit->pnext;
			nptr->pprev = best_fit;

			best_fit->pnext = nptr;

			if (nptr->pnext) {
				nptr->pnext->pprev = nptr;
			} else {
				gc_last_block = nptr;
			}

			/* and add nptr to one of the freelists */
			gc_add_to_prim_freelist (nptr);
		}

DBG(GCPRIM,	dprintf("gc_primitive_alloc: 0x%x bytes from freelist @ %p\n", best_fit->size, best_fit); )
		gc_block_add(best_fit);
		return (best_fit);
	}
DBG(GCPRIM,	dprintf("gc_primitive_alloc: no suitable block found!\n"); )

	/* Nothing found on free list */
	return (0);
}

/*
 * merge a primitive block with its successor.
 */
static inline void
gc_merge_with_successor (gc_block *blk)
{
	gc_block *next_blk = blk->pnext;

	assert (next_blk);

	blk->size += next_blk->size;
	blk->pnext = next_blk->pnext;

	/*
	 * if the merged block has a successor, update its pprev field.
	 * otherwise, the merged block is the last block in the primitive
	 * chain.
	 */
	if (blk->pnext) {
		blk->pnext->pprev = blk;
	} else {
		gc_last_block = blk;
	}
}


/*
 * Return a block of memory to the free list.
 */
void
gc_primitive_free(gc_block* mem)
{
	gc_block *blk;

	assert(mem->size % gc_pgsize == 0);

	/* Remove from object hash */
	gc_block_rm(mem);

	DBG(GCPRIM, dprintf ("\ngc_primitive_free: freeing block %p (%x bytes, %x)\n", mem, mem->size, mem->size >> gc_pgbits); )

	/*
	 * Test whether this block is mergable with its successor.
	 * We need to do the GCBLOCKEND check, since the heap may not be a continuous
	 * memory area and thus two consecutive blocks need not be mergable. 
	 */
	if ((blk=mem->pnext) &&
	    !GCBLOCKINUSE(blk) &&
	    GCBLOCKEND(mem)==blk) {
		DBG(GCPRIM, dprintf ("gc_primitive_free: merging %p with its successor (%p, %u)\n", mem, blk, blk->size);)

		gc_remove_from_prim_freelist(blk);

		gc_merge_with_successor (mem);
	}

	if ((blk=mem->pprev) &&
	    !GCBLOCKINUSE(blk) &&
	    GCBLOCKEND(blk)==mem) {
		DBG(GCPRIM, dprintf ("gc_primitive_free: merging %p with its predecessor (%p, %u)\n", mem, blk, blk->size); )

		gc_remove_from_prim_freelist(blk);

		mem = blk;

		gc_merge_with_successor (mem);
	}

	gc_add_to_prim_freelist (mem);

	DBG(GCPRIM, dprintf ("gc_primitive_free: added 0x%x bytes @ %p to freelist %u @ %p\n", mem->size, mem,
			     gc_get_prim_freelist(mem)-&gc_prim_freelist[0], gc_get_prim_freelist(mem)); )
}

/*
 * Try to reserve some memory for OOM exception handling.  Gc once at
 * the beginning.  We start out looking for an arbitrary number of
 * pages (4), and cut our expectations in half until we are able to
 * meet them.
 */
gc_block *
gc_primitive_reserve(void)
{
	gc_block *r = 0;
	size_t size = 4 * gc_pgsize;
	
	while (size >= gc_pgsize && !(r = gc_primitive_alloc(size))) {
		if (size == gc_pgsize) {
			break;
		}
		size /= 2;
	}
	return r;
}

/*
 * System memory management:  Obtaining additional memory from the
 * OS.  This looks more complicated than it is, since it does not require
 * sbrk.
 */
/* Get some page-aligned memory from the system. */
static uintp
pagealloc(size_t size)
{
	void* ptr;

#define	CHECK_OUT_OF_MEMORY(P)	if ((P) == 0) return 0;

#if defined(HAVE_SBRK)

	/* Our primary choice for basic memory allocation is sbrk() which
	 * should avoid any unsee space overheads.
	 */
	for (;;) {
		int missed;
		ptr = sbrk(size);
		if (ptr == (void*)-1) {
			ptr = 0;
			break;
		}
		if ((uintp)ptr % gc_pgsize == 0) {
			break;
		}
		missed = gc_pgsize - ((uintp)ptr % gc_pgsize);
		DBG(GCSYSALLOC,
		    dprintf("unaligned sbrk %p, missed %d bytes\n",
			    ptr, missed));
		sbrk(-size + missed);
	}
	CHECK_OUT_OF_MEMORY(ptr);

#elif defined(HAVE_MEMALIGN)

        ptr = memalign(gc_pgsize, size);
	CHECK_OUT_OF_MEMORY(ptr);

#elif defined(HAVE_VALLOC)

        ptr = valloc(size);
	CHECK_OUT_OF_MEMORY(ptr);

#else

	/* Fallback ...
	 * Allocate memory using malloc and align by hand.
	 */
	size += gc_pgsize;

        ptr = malloc(size);
	CHECK_OUT_OF_MEMORY(ptr);
	ptr = (void*)((((uintp)ptr) + gc_pgsize - 1) & -gc_pgsize);

#endif
	addToCounter(&gcpages, "gcmem-system pages", 1, size);
	return ((uintp) ptr);
}

/* Free memory allocated with pagealloc */
static void pagefree(uintp base, size_t size)
{
#ifdef HAVE_SBRK
	sbrk(-size);
#else
	/* it must have been allocated with memalign, valloc or malloc */
	free((void *)base);
#endif
}

/*
 * Allocate size bytes of heap memory, and return the corresponding
 * gc_block *.
 */
static void *
gc_block_alloc(size_t size)
{
	int size_pg = (size>>gc_pgbits);
	static int n_live = 0;	/* number of pages in java heap */
	static int nblocks;	/* number of gc_blocks in array */
	uintp heap_addr;
	static uintp last_addr;

	if (!gc_block_base) {
		nblocks = (gc_heap_limit+gc_pgsize-1)>>gc_pgbits;

		gc_block_base = (uintp) malloc(nblocks * sizeof(gc_block));
		if (!gc_block_base) return 0;
		memset((void *)gc_block_base, 0, nblocks * sizeof(gc_block));
	}

	DBG(GCSYSALLOC, dprintf("pagealloc(%ld)", (long) size));
	heap_addr = pagealloc(size);
	DBG(GCSYSALLOC, dprintf(" => %p\n", (void *) heap_addr));

	if (!heap_addr) return 0;
	
	if (!gc_heap_base) {
		gc_heap_base = heap_addr;
	}

	if (GCMEM2BLOCK(heap_addr + size)
	    > ((gc_block *)gc_block_base) + nblocks
	    || heap_addr < gc_heap_base) {
		uintp old_blocks = gc_block_base;
		int onb = nblocks;
		int min_nb;	/* minimum size of array to hold heap_addr */
#if defined(KAFFE_STATS)
		static timespent growtime;
#endif

		startTiming(&growtime, "gctime-blockrealloc");
		/* Pick a new size for the gc_block array.  Remember,
		   malloc does not simply grow a memory segment.

		   We can extrapolate how many gc_blocks we need for
		   the entire heap based on how many heap pages
		   currently fit in the gc_block array.  But, we must
		   also make sure to allocate enough blocks to cover
		   the current allocation */
		nblocks = (nblocks * (gc_heap_limit >> gc_pgbits))
			/ n_live;
		if (heap_addr < gc_heap_base) 
			min_nb = nblocks
			  + ((gc_heap_base - heap_addr) >> gc_pgbits);
		else
			min_nb = ((heap_addr + size) - gc_heap_base) >>
			  gc_pgbits;
		nblocks = MAX(nblocks, min_nb);
		DBG(GCSYSALLOC,
		    dprintf("growing block array from %d to %d elements\n",
			    onb, nblocks));

		jthread_spinon(0);
		gc_block_base = (uintp) realloc((void *) old_blocks,
						nblocks * sizeof(gc_block));
		if (!gc_block_base) {
			/* roll back this call */
			pagefree(heap_addr, size);
			gc_block_base = old_blocks;
			nblocks = onb;
			jthread_spinoff(0);
			return 0;
		}

		/* If the array's address has changed, we have to fix
		   up the pointers in the gc_blocks, as well as all
		   external pointers to the gc_blocks.  We can only
		   fix gc_prim_freelist and the size-freelist array.
		   There should be no gc_block *'s on any stack
		   now. */ 
		if (gc_block_base != old_blocks) {
			int i;
			gc_block *b = (void *) gc_block_base;
			uintp delta = gc_block_base - old_blocks;
#define R(X) if (X) ((uintp) (X)) += delta

			DBG(GCSYSALLOC,
			    dprintf("relocating gc_block array\n"));
			for (i = 0; i < onb; i++) R(b[i].next);
			memset(b + onb, 0,
			       (nblocks - onb) * sizeof(gc_block));

			for (i = 0; i<=GC_PRIM_LIST_COUNT; i++)
				R(gc_prim_freelist[i]);

			for (i = 0; freelist[i].list != (void*)-1; i++) 
				R(freelist[i].list);
#undef R
		}
		jthread_spinoff(0);
		stopTiming(&growtime);
	}
	n_live += size_pg;
	last_addr = MAX(last_addr, heap_addr + size);
	gc_heap_range = last_addr - gc_heap_base;
	DBG(GCSYSALLOC, dprintf("%ld unused bytes in heap addr range\n",
				(long) (gc_heap_range - gc_heap_total)));
	mprotect((void *) heap_addr, size, NO_PROT);
	return GCMEM2BLOCK(heap_addr);
}

/**
 * Grows the heap.
 *
 * @param sz minimum number of bytes to grow.
 * @return 0 in case of an error, otherwise != 0
 */
void *
gc_heap_grow(size_t sz)
{
	gc_block* blk;
	int iLockRoot;

	if (GC_SMALL_OBJECT(sz)) {
		sz = gc_pgsize;
	} else {
		sz = sz + 2 + ROUNDUPALIGN(1);
		sz = ROUNDUPPAGESIZE(sz);
	}

	if (sz < gc_heap_allocation_size) {
		sz = gc_heap_allocation_size;
	}

	assert(sz % gc_pgsize == 0);

	lockStaticMutex(&gc_heap_lock);

	if (gc_heap_total == gc_heap_limit) {
		unlockStaticMutex(&gc_heap_lock);
		return (0);
	} else if (gc_heap_total + sz > gc_heap_limit) {
		/* take as much memory as we can */
		sz = gc_heap_limit - gc_heap_total;
		assert(sz % gc_pgsize == 0);
		DBG(GCSYSALLOC, dprintf("allocating up to limit\n"));
	}
#ifdef KAFFE_VMDEBUG
	gc_system_alloc_cnt++;
#endif

	blk = gc_block_alloc(sz);

	DBG(GCSYSALLOC,
	    dprintf("gc_system_alloc: %ld byte at %p\n", (long) sz, blk); )

	if (blk == 0) {
		unlockStaticMutex(&gc_heap_lock);
		return (0);
	}

	gc_heap_total += sz;
	assert(gc_heap_total <= gc_heap_limit);

	/* Place block into the freelist for subsequent use */
	DBG(GCDIAG, blk->magic = GC_MAGIC);
	blk->size = sz;

	/* maintain list of primitive blocks */
	if (gc_last_block) {
		gc_last_block->pnext = blk;
		blk->pprev = gc_last_block;
	} else {
		gc_last_block = blk;
	}

	/* Free block into the system */
	gc_primitive_free(blk);

	unlockStaticMutex(&gc_heap_lock);

	return (blk);
}
