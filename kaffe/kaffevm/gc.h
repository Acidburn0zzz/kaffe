/*
 * gc.h
 * The garbage collector.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#ifndef __gc_h
#define __gc_h

#include "gtypes.h"

/*
 * Default values for initial and maximum heap size and heap increment.
 */
#define	MIN_HEAPSIZE	(5*1024*1024)
#define	MAX_HEAPSIZE	(64*1024*1024)
#define	ALLOC_HEAPSIZE	(1024*1024)

/* 
 * We do not support incremental collection as this time.
 */
#undef	KGC_INCREMENTAL

typedef struct _Collector Collector;
typedef void (*walk_func_t)(struct _Collector*, void*, uint32);
typedef void (*final_func_t)(struct _Collector*, void*);
typedef void (*destroy_func_t)(struct _Collector*, void*);

#define	KGC_OBJECT_NORMAL	((final_func_t)0)
#define	KGC_OBJECT_FIXED		((final_func_t)1)

/*
 * Garbage collector interface.
 */
typedef enum {
	/* allocation types for different kinds of java objects */
	KGC_ALLOC_JAVASTRING,
	KGC_ALLOC_NOWALK,
	KGC_ALLOC_NORMALOBJECT,
	KGC_ALLOC_PRIMARRAY,
	KGC_ALLOC_REFARRAY,
	KGC_ALLOC_FINALIZEOBJECT,
	KGC_ALLOC_JAVALOADER,

	/* allocation types related to the translator engines */
	KGC_ALLOC_JITCODE,
	KGC_ALLOC_JITTEMP,
	KGC_ALLOC_JIT_SEQ,
	KGC_ALLOC_JIT_CONST,
	KGC_ALLOC_JIT_ARGS,
	KGC_ALLOC_JIT_FAKE_CALL,
	KGC_ALLOC_JIT_SLOTS,
	KGC_ALLOC_JIT_CODEBLOCK,
	KGC_ALLOC_JIT_LABELS,
	KGC_ALLOC_TRAMPOLINE,
	
	/* allocation types used for java.lang.Class and its parts */
	KGC_ALLOC_CLASSOBJECT,
	KGC_ALLOC_BYTECODE,
	KGC_ALLOC_EXCEPTIONTABLE,
	KGC_ALLOC_STATICDATA,
	KGC_ALLOC_CONSTANT,
	KGC_ALLOC_DISPATCHTABLE,
	KGC_ALLOC_METHOD,
	KGC_ALLOC_FIELD,
	KGC_ALLOC_INTERFACE,
	KGC_ALLOC_LINENRTABLE,
	KGC_ALLOC_LOCALVARTABLE,
	KGC_ALLOC_DECLAREDEXC,
	KGC_ALLOC_CLASSMISC,

	/* miscelanious allocation types */
	KGC_ALLOC_FIXED,
	KGC_ALLOC_UTF8CONST,
	KGC_ALLOC_LOCK,
	KGC_ALLOC_THREADCTX,
	KGC_ALLOC_REF,
	KGC_ALLOC_JAR,
	KGC_ALLOC_CODEANALYSE,
	KGC_ALLOC_CLASSPOOL,
	KGC_ALLOC_VERIFIER,
	KGC_ALLOC_NATIVELIB,
	KGC_ALLOC_MAX_INDEX
} gc_alloc_type_t;

/*
 * Define a COM-like GC interface.
 */
struct GarbageCollectorInterface_Ops;

struct _Collector {
	struct GarbageCollectorInterface_Ops *ops;
};

struct GarbageCollectorInterface_Ops {

	void*   reserved1;
	void*   reserved2;
	void*   reserved3;
	void*	(*malloc)(Collector *, size_t size, gc_alloc_type_t type);
	void*	(*realloc)(Collector *, void* addr, size_t size, gc_alloc_type_t type);
	void	(*free)(Collector *, void* addr);

	void	(*invoke)(Collector *, int mustgc);
	void	(*invokeFinalizer)(Collector *);
	void	(*init)(Collector *);
	void	(*enable)(Collector *);

	void	(*markAddress)(Collector *, const void* addr);
	void	(*markObject)(Collector *, const void* obj);
	uint32	(*getObjectSize)(Collector *, const void* obj);
	const char* (*getObjectDescription)(Collector *, const void* obj);
	int	(*getObjectIndex)(Collector *, const void* obj);
	void*	(*getObjectBase)(Collector *, const void* obj);

	void	(*walkMemory)(Collector *, void *addr);
	void	(*walkConservative)(Collector *, 
			const void* addr, uint32 length);

	void	(*registerFixedTypeByIndex)(Collector *, 
			gc_alloc_type_t gc_index, const char *description);

	void 	(*registerGcTypeByIndex)(Collector *, 
			gc_alloc_type_t gc_index,
			walk_func_t walk, final_func_t final, 
			destroy_func_t destroy, const char
					 *description);
	struct Hjava_lang_Throwable *(*throwOOM)(Collector *);

	void 	(*enableGC)(Collector *);
	void 	(*disableGC)(Collector *);
  
        uintp   (*getHeapLimit)(Collector *);
        uintp   (*getHeapTotal)(Collector *);
};

Collector* createGC(void (*_walkRootSet)(Collector*));

/*
 * Convenience macros
 */
#define KGC_malloc(G, size, type)	\
    ((G)->ops->malloc)((Collector*)(G), (size), (type))
#define KGC_realloc(G, addr, size, type)	\
    ((G)->ops->realloc)((Collector*)(G), (addr), (size), (type))
#define KGC_free(G, addr)		\
    ((G)->ops->free)((Collector*)(G), (addr))
#define KGC_invoke(G, mustgc)		\
    ((G)->ops->invoke)((Collector*)(G), (mustgc))
#define KGC_invokeFinalizer(G)		\
    ((G)->ops->invokeFinalizer)((Collector*)(G))
#define KGC_init(G)		\
    ((G)->ops->init)((Collector*)(G))
#define KGC_enable(G)		\
    ((G)->ops->enable)((Collector*)(G))
#define KGC_throwOOM(G)		\
    ((G)->ops->throwOOM)((Collector*)(G))
#define KGC_markAddress(G, addr)		\
    ((G)->ops->markAddress)((Collector*)(G), (addr))

#if !defined(KAFFEH)
static inline void KGC_markObject(void *g, void *addr)
{
	if (addr)
		((Collector*) g)->ops->markObject((Collector*) g, addr);
}
#endif

#define KGC_getObjectSize(G, obj)	\
    ((G)->ops->getObjectSize)((Collector*)(G), (obj))
#define KGC_getObjectDescription(G, obj)	\
    ((G)->ops->getObjectDescription)((Collector*)(G), (obj))
#define KGC_getObjectIndex(G, obj)	\
    ((G)->ops->getObjectIndex)((Collector*)(G), (obj))
#define KGC_getObjectBase(G, obj)	\
    ((G)->ops->getObjectBase)((Collector*)(G), (obj))
#define KGC_walkMemory(G, addr)	\
    ((G)->ops->walkMemory)((Collector*)(G), (addr))
#define KGC_walkConservative(G, addr, len)		\
    ((G)->ops->walkConservative)((Collector*)(G), (addr), (len))
#define KGC_registerFixedTypeByIndex(G, idx, desc)	\
    ((G)->ops->registerFixedTypeByIndex)((Collector*)(G),   \
				(idx), (desc))
#define KGC_registerGcTypeByIndex(G, idx, walk, final, destroy, desc)	\
    ((G)->ops->registerGcTypeByIndex)((Collector*)(G), 	     \
				(idx), (walk), (final), (destroy), (desc))
#define KGC_enableGC(G)		\
    ((G)->ops->enableGC)((Collector*)(G));
#define KGC_disableGC(G)		\
    ((G)->ops->disableGC)((Collector*)(G));

#define KGC_getHeapLimit(G) \
    ((G)->ops->getHeapLimit)((Collector *)(G));
#define KGC_getHeapTotal(G) \
    ((G)->ops->getHeapTotal)((Collector *)(G));
#define KGC_WRITE(a, b)

/*
 * Compatibility macros to access GC functions
 */
extern Collector* main_collector;

#define	gc_malloc(A,B)	    KGC_malloc(main_collector,A,B)
#define	gc_calloc(A,B,C)    KGC_malloc(main_collector,(A)*(B),C)
#define	gc_realloc(A,B,C)   KGC_realloc(main_collector,(A),(B),C)
#define	gc_free(A)	    KGC_free(main_collector,(A))

#define	invokeGC()	    KGC_invoke(main_collector,1)
#define	adviseGC()	    KGC_invoke(main_collector,0)
#define	invokeFinalizer()   KGC_invokeFinalizer(main_collector)

#define gc_throwOOM()	    KGC_throwOOM(main_collector)

#define gc_enableGC()	    KGC_enableGC(main_collector)
#define gc_disableGC()	    KGC_disableGC(main_collector)

#include "gcRefs.h"
extern char* describeObject(const void* mem);
#endif
