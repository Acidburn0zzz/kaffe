/*
 * classPool.c
 * Methods dealing with the class entry pool
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

#include "config.h"
#include "debug.h"
#include "config-std.h"
#include "config-mem.h"
#include "errors.h"
#include "itypes.h"
#include "access.h"
#include "file.h"
#include "thread.h"
#include "jthread.h"
#include "classMethod.h"
#include "md.h"
#include "support.h"
#include "stringSupport.h"
#include "stats.h"
#include "gc.h"
#include "locks.h"
#include "md.h"

#define	CLASSHASHSZ	256	/* Must be a power of two */
static iStaticLock	classHashLock;
static classEntry* classEntryPool[CLASSHASHSZ];
#if defined(KAFFE_STATS)
statobject classStats;
void statClassPool(void);
#endif

/*
 * Lookup an entry for a given (name, loader) pair.
 * Return null if none is found.
 */
classEntry*
lookupClassEntryInternal(Utf8Const* name, Hjava_lang_ClassLoader* loader)
{
	classEntry* entry;

	entry = classEntryPool[utf8ConstHashValue(name) & (CLASSHASHSZ-1)];
	for (; entry != 0; entry = entry->next) {
		if (utf8ConstEqual(name, entry->name) && loader == entry->loader) {
			return (entry);
		}
	}
	return (0);
}

/*
 * Lookup an entry for a given (name, loader) pair.
 * Create one if none is found.
 */
classEntry*
lookupClassEntry(Utf8Const* name, Hjava_lang_ClassLoader* loader,
		 errorInfo *einfo)
{
	classEntry* entry;
	classEntry** entryp;
	int iLockRoot;
	static int f = 0;

        if (f == 0) {
		f++;
		registerUserCounter(&classStats, "class-pool", statClassPool);
        }

	entry = lookupClassEntryInternal(name, loader);
	if (entry != 0)
		return (entry);

	/* Failed to find class entry - create a new one */
	entry = gc_malloc(sizeof(classEntry), GC_ALLOC_CLASSPOOL);
	if (entry == 0) {
		postOutOfMemory(einfo);
		return (0);
	}
	entry->name = name;
	entry->loader = loader;
	entry->data.cl = 0;
	entry->next = 0;

	/* Lock the class table and insert entry into it (if not already
	   there) */
        lockStaticMutex(&classHashLock);

	entryp = &classEntryPool[utf8ConstHashValue(name) & (CLASSHASHSZ-1)];
	for (; *entryp != 0; entryp = &(*entryp)->next) {
		if (utf8ConstEqual(name, (*entryp)->name) && loader == (*entryp)->loader) {
			/* Someone else added it - discard ours and return
			   the new one. */
			unlockStaticMutex(&classHashLock);
			KFREE(entry);
			return (*entryp);
		}
	}

	/* Add ours to end of hash */
	*entryp = entry;
	addToCounter(&cpemem, "vmmem-class entry pool", 1, GCSIZEOF(entry));

	/*
	 * This reference to the utf8 name will be released if and when this
	 * class entry is freed in destroyClassLoader.
	 */
	utf8ConstAddRef(entry->name);

        unlockStaticMutex(&classHashLock);

	return (entry);
}

#if 0
#if defined(TRANSLATOR) && !defined(JIT3)
/*
 * Find method containing pc.
 * This is the old version that linearly scans the class entry pool.
 *
 * It is only used in JIT2.  JIT3 uses a faster implementation
 * in methodCache.c.  JIT2 could use that implementation too if calls
 * to makeMethodActive are added.
 *
 * For now, let's leave this code here.  It could be used a potential
 * fallback for gc systems that would not support gcGetObjectBase().
 *
 * Note that the only reason this code is in this file is that
 * the classEntryPool is a private structure --- what we'd really need
 * here is an "map" function over the elements in the class entry pool.
 */
Method*
findMethodFromPC(uintp pc)
{
	classEntry* entry;
	Method* ptr;
	int ipool;
	int imeth;

	for (ipool = CLASSHASHSZ;  --ipool >= 0; ) {
		for (entry = classEntryPool[ipool];  entry != NULL; entry = entry->next) {
			if (entry->data.cl != 0) {
				imeth = CLASS_NMETHODS(entry->data.cl);
				ptr = CLASS_METHODS(entry->data.cl);
				for (; --imeth >= 0;  ptr++) {
					uintp ncode;
					if (!METHOD_TRANSLATED(ptr)) {
						continue;
					}
					ncode = (uintp)METHOD_NATIVECODE(ptr);
					if (pc >= ncode && pc < (uintp)ptr->c.ncode.ncode_end) {
						return (ptr);
					}
				}
			}
		}
	}
	return (NULL);
}
#endif
#endif

void
walkClassEntries(Collector *collector, Hjava_lang_ClassLoader* loader)
{
        classEntry* entry;
        int ipool;

        for (ipool = CLASSHASHSZ;  --ipool >= 0; ) {
                for (entry = classEntryPool[ipool]; entry != NULL;
                     entry = entry->next)
                {
                        if (entry->loader == loader && entry->state >= NMS_LOADING) {
                                GC_markObject(collector, entry->data.cl);
                        }
                }
        }
}

/*
 * Remove all entries from the class entry pool that belong to a given
 * class.  Return the number of entries removed.
 */
int
removeClassEntries(Hjava_lang_ClassLoader* loader)
{
	classEntry** entryp;
	classEntry* entry;
	int ipool;
	int totalent = 0;
	int iLockRoot;

        lockStaticMutex(&classHashLock);
	for (ipool = CLASSHASHSZ;  --ipool >= 0; ) {
		entryp = &classEntryPool[ipool];
		while (*entryp != NULL) {
			/* loop invariant: entryp points at the next non-null
			 * entry that must be examined.
			 */
			entry = *entryp;
			if (entry->loader == loader) {
				/*
				 * If class gc is turned off, no classloader
				 * should ever be finalized because they're all
				 * kept alive by their respective classes.
				 */
				assert(entry->data.cl == 0 ||
					Kaffe_JavaVMArgs[0].enableClassGC != 0);
DBG(CLASSGC,
				dprintf("removing %s l=%p/c=%p\n",
				    entry->name->data, loader, entry->data.cl);
    )
				/* release reference to name */
				utf8ConstRelease(entry->name);
				(*entryp) = entry->next;
				addToCounter(&cpemem, "vmmem-class entry pool",
					1, -(jlong)GCSIZEOF(entry));
				gc_free(entry);
				totalent++;
			} else {
				entryp = &(*entryp)->next;
			}
		}
	}
        unlockStaticMutex(&classHashLock);
	return (totalent);
}

/*
 * Finalize a classloader and remove its entries in the class entry pool.
 */
void
/* ARGSUSED */
destroyClassLoader(Collector *c UNUSED, void* _loader)
{
	Hjava_lang_ClassLoader* loader = _loader;
        int rmoved;

        rmoved = removeClassEntries(loader);

	if (Kaffe_JavaVMArgs[0].enableVerboseGC > 0) {
		dprintf("<GC: destroying classloader "
			"@%p (%d entries removed)>\n", loader, rmoved);
   	}
}

static nameDependency *dependencies;
static iStaticLock	mappingLock;

static
nameDependency *findNameDependency(jthread_t jt)
{
	nameDependency *curr, *retval = 0;

	for( curr = dependencies; curr && !retval; curr = curr->next )
	{
		if( curr->thread == jt )
		{
			retval = curr;
		}
	}
	return( retval );
}

static
int addNameDependency(nameDependency *nd)
{
	int retval = 1;
	int iLockRoot;

	assert(nd != 0);

	lockStaticMutex(&mappingLock);
	{
		nameDependency *curr;
		
		nd->next = dependencies;
		dependencies = nd;

		for( curr = findNameDependency(nd->mapping->data.thread);
		     curr && retval;
		     curr = findNameDependency(curr->mapping->data.thread) )
		{
			if( curr->mapping->data.thread == nd->thread )
			{
				retval = 0;
			}
		}
	}
	unlockStaticMutex(&mappingLock);
	return( retval );
}

static
void remNameDependency(classEntry *ce)
{
	int iLockRoot;

	assert(ce != 0);
	
	lockStaticMutex(&mappingLock);
	{
		nameDependency **last, *curr;

		last = &dependencies;
		curr = dependencies;
		while( curr && (curr->mapping != ce) )
		{
			last = &curr->next;
			curr = curr->next;
		}
		if( curr )
		{
			*last = curr->next;
		}
	}
	unlockStaticMutex(&mappingLock);
}

int classMappingSearch(classEntry *ce,
		       Hjava_lang_Class **out_cl,
		       errorInfo *einfo)
{
	int done = 0, retval = 1;
	nameDependency nd;
	jthread_t jt;
	int iLockRoot;

	jt = jthread_current();
	while( !done )
	{
		lockMutex(ce);
		switch( ce->state )
		{
		case NMS_EMPTY:
			/* This thread's responsibility. */
			ce->state = NMS_SEARCHING;
			ce->data.thread = jt;
			done = 1;
			break;
		case NMS_SEARCHING:
			if( ce->data.thread == jt )
			{
				done = 1;
				break;
			}
			waitCond(ce, 0);
			break;
		case NMS_LOADING:
			/*
			 * Another thread is loading it, make sure there is not
			 * a cycle.
			 */
			nd.thread = jt;
			nd.mapping = ce;
			if( (ce->data.thread == jt) ||
			    !addNameDependency(&nd) )
			{
				/* Circularity. */
				done = 1;
				retval = 0;
				postExceptionMessage(
					einfo,
					JAVA_LANG(ClassCircularityError),
					"%s",
					ce->name->data);
			}
			else
			{
				waitCond(ce, 0);
			}
			remNameDependency(ce);
			break;
		case NMS_LOADED:
			/*
			 * Another thread loaded it, however, its not finished
			 * linking yet.
			 */
			waitCond(ce, 0);
			break;
		case NMS_DONE:
			/* Its already been loaded and linked. */
			*out_cl = ce->data.cl;
			done = 1;
			break;
		}
		unlockMutex(ce);
	}
	return( retval );
}

int classMappingLoad(classEntry *ce,
		     Hjava_lang_Class **out_cl,
		     errorInfo *einfo)
{
	int done = 0, retval = 1;
	nameDependency nd;
	jthread_t jt;
	int iLockRoot;

	*out_cl = 0;
	jt = jthread_current();
	while( !done )
	{
		lockMutex(ce);
		switch( ce->state )
		{
		case NMS_EMPTY:
		case NMS_SEARCHING:
			/* This thread's responsibility. */
			ce->state = NMS_LOADING;
			ce->data.thread = jt;
			done = 1;
			break;
		case NMS_LOADING:
			/*
			 * Another thread is loading it, make sure there is not
			 * a cycle.
			 */
			nd.thread = jt;
			nd.mapping = ce;
			if( (ce->data.thread == jt) ||
			    !addNameDependency(&nd) )
			{
				/* Circularity. */
				done = 1;
				retval = 0;
				postExceptionMessage(
					einfo,
					JAVA_LANG(ClassCircularityError),
					"%s",
					ce->name->data);
			}
			else
			{
				waitCond(ce, 0);
			}
			remNameDependency(ce);
			break;
		case NMS_LOADED:
			/*
			 * Another thread loaded it, however, its not finished
			 * linking yet.
			 */
			waitCond(ce, 0);
			break;
		case NMS_DONE:
			/* Its already been loaded and linked. */
			*out_cl = ce->data.cl;
			done = 1;
			break;
		}
		unlockMutex(ce);
	}
	return( retval );
}

Hjava_lang_Class *classMappingLoaded(classEntry *ce, Hjava_lang_Class *cl)
{
	Hjava_lang_Class *retval = 0;
	int iLockRoot;

	assert(ce != 0);
	assert(cl != 0);
	
	lockMutex(ce);
	{
		switch( ce->state )
		{
		case NMS_LOADING:
			/* FALLTHROUGH */
		case NMS_SEARCHING:
			if( cl->state < CSTATE_PREPARED )
			{
				ce->state = NMS_LOADED;
			}
			else
			{
				ce->state = NMS_DONE;
			}
			ce->data.cl = cl;
			retval = cl;
			break;
		default:
			/* Ignore. */
			retval = ce->data.cl;
			break;
		}
		broadcastCond(ce);
	}
	unlockMutex(ce);

	return( retval );
}

void setClassMappingState(classEntry *ce, name_mapping_state_t nms)
{
	int iLockRoot;
	
	assert(ce != 0);
	
	lockMutex(ce);
	{
		switch( ce->state )
		{
		case NMS_SEARCHING:
			ce->state = nms;
			break;
		case NMS_LOADING:
			/* FALLTHROUGH */
		case NMS_LOADED:
			ce->state = nms;
			break;
		case NMS_DONE:
			break;
		case NMS_EMPTY:
			break;
		default:
			assert(0);
			break;
		}
		broadcastCond(ce);
	}
	unlockMutex(ce);
}

#if defined(KAFFE_STATS) || defined(KAFFE_PROFILER) || defined(KAFFE_VMDEBUG)
/**
 * Walk the class pool and invoke walker() for each classes
 */
void
walkClassPool(int (*walker)(Hjava_lang_Class *clazz, void *), void *param)
{
	int ipool;
	classEntry* entry;

	assert(walker != NULL);

	for (ipool = CLASSHASHSZ;  --ipool >= 0; ) {
		entry = classEntryPool[ipool];
		for (; entry != NULL; entry = entry->next) {
			if (entry->data.cl
			    && entry->loader == entry->data.cl->loader) {
				walker(entry->data.cl, param);
			}
		}
	}
}
#endif


#if defined(KAFFE_STATS)
static void
statClass(Hjava_lang_Class *clazz, int *total)
{
	Collector *c = main_collector;
	int misc = 0;
	int miscfixed = 0;
	int bytecodemem = 0, jitmem = 0;

#define SIZE_IFNONZERO(x)  ((x) ? GC_getObjectSize(c, (x)) : 0)

	/* compute sizes of miscellaneous data */
	/* we tally up the memory for KMALLOC'ed objects (they'll appear
	 * under "other-fixed".) and the memory for other misc objects
	 */
	misc += SIZE_IFNONZERO(clazz);		/* myself */
	if (!CLASS_IS_PRIMITIVE(clazz)) {
		/* For primitives, vtable is -1 and methods is the
		 * class of the corresponding array
		 */
		misc += SIZE_IFNONZERO(clazz->vtable);
		misc += SIZE_IFNONZERO(CLASS_CONSTANTS(clazz)->data);
		misc += SIZE_IFNONZERO(CLASS_FIELDS(clazz));
		misc += SIZE_IFNONZERO(CLASS_STATICDATA(clazz));

		miscfixed += SIZE_IFNONZERO(clazz->if2itable);
		miscfixed += SIZE_IFNONZERO(clazz->itable2dtable);
		miscfixed += SIZE_IFNONZERO(clazz->gc_layout);
		miscfixed += SIZE_IFNONZERO(clazz->sourcefile);
		miscfixed += SIZE_IFNONZERO(clazz->implementors);
		miscfixed += SIZE_IFNONZERO(clazz->inner_classes);
	}
	if (!CLASS_IS_ARRAY(clazz) && !CLASS_IS_PRIMITIVE(clazz)) {
		misc += SIZE_IFNONZERO(clazz->interfaces);
	}

	/* methods only if we have some */
	if (!CLASS_IS_PRIMITIVE(clazz) && !CLASS_IS_ARRAY(clazz)
		&& CLASS_METHODS(clazz) != 0)
	{
		Method *m = CLASS_METHODS(clazz);
		int i, n = CLASS_NMETHODS(clazz);
		misc += SIZE_IFNONZERO(CLASS_METHODS(clazz));
		for (i = 0; i < n; m++, i++) {
			miscfixed += SIZE_IFNONZERO(m->parsed_sig);
			miscfixed += SIZE_IFNONZERO(m->lines);
			miscfixed += SIZE_IFNONZERO(m->declared_exceptions);
			misc += SIZE_IFNONZERO(m->exception_table);
			/* bytecode or jitted code */
			if ((m->accflags & ACC_NATIVE) == 0) {
				if (METHOD_TRANSLATED(m)) {
					jitmem += SIZE_IFNONZERO(m->c.ncode.ncode_start);
				} else {
					bytecodemem += SIZE_IFNONZERO(m->c.bcode.code);
				}
			}
		}
	}

	dprintf("%7d %7d %7d %7d %s\n",
		misc,
		miscfixed,
		jitmem,
		bytecodemem,
		CLASS_CNAME(clazz));

	total[0] += misc;
	total[1] += miscfixed;
	total[2] += jitmem;
	total[3] += bytecodemem;
}

void
statClassPool(void)
{
	int total[20];

	memset(total, 0, sizeof total);
	dprintf("#DUMPING CLASSPOOL MEMORY\n");
	dprintf("%-7s %-7s %-7s %-7s %-7s\n",
		"#MISC", "MISCFIX", "JCODE", "BCODE", "CLASS");
	dprintf("%-7s %-7s %-7s %-7s %-7s\n",
		"#------", "-------", "-------", "-------", "--------------\n");
	walkClassPool(statClass, total);
	dprintf("%7.2f %7.2f %7.2f %7.2f KBytes\n",
		total[0]/1024.0, total[1]/1024.0, total[2]/1024.0,
		total[3]/1024.0);
	fflush(stderr);
}
#endif

