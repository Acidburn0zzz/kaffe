/*
 * thread-impl.h - pthread based ThreadInterface implementation
 *
 * Copyright (c) 1998
 *      Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

#ifndef __thread_internal_h
#define __thread_internal_h

#include <pthread.h>
#include <semaphore.h>

#include "gtypes.h"

#if !defined(STACKREDZONE)
#define STACKREDZONE    8192
#endif

/* suspend states (these are exclusive) */
typedef enum {
  SS_PENDING_SUSPEND =  0x01,  /* suspend signal has been sent, but not handled */
  SS_SUSPENDED       =  0x02,  /* suspend signal has been handled */
  SS_PENDING_RESUME  =  0x04   /* resume signal  has been sent */
} suspend_state_t;

/* blocking states (might be accumulative) */
typedef enum {
  BS_THREAD          =  0x01,  /* blocked on tLock (thread system internal) */
  BS_MUTEX           =  0x02,  /* blocked on a external mutex lock */
  BS_CV              =  0x04,  /* blocked on a external convar wait */
  BS_CV_TO           =  0x08   /* blocked on a external convar timeout wait */
} block_state_t;

/*
 * 'nativeThread' is our link between native and Java thread objects.
 * It also serves as a container for our pthread extensions (namely
 * enumeration, and inter-thread suspend)
 */
typedef struct _nativeThread {
  /* these are our links to the native pthread implementation */
  pthread_t             tid;
  pthread_attr_t        attr;

  /* this is our Java Thread object */
  void			*jlThread;

  /* wether this is a daemon thread */
  int			daemon;

  /* convars and mutexes aren't useful in signal handlers, semaphores are */
  sem_t                 sem;

  /* the following fields hold our extensions */
  int                   active;         /* are we in our user thread function 'func'? */
  suspend_state_t       suspendState;   /* are we suspended for a critSection?  */
  block_state_t         blockState;     /* are we in a Lwait or Llock (can handle signals)? */

  void                  (*func)(void*);  /* this kicks off the user thread func */
  void                  *stackMin;
  void                  *stackCur;      /* just useful if blocked or suspended */
  void                  *stackMax;

  struct _nativeThread  *next;
} *jthread_t;

extern pthread_key_t   ntKey;

/**
 * Returns the current native thread.
 *
 */
static inline          
jthread_t jthread_current(void)      
{
  return (jthread_t)pthread_getspecific(ntKey);
}

/**
 * Disable stopping the calling thread.
 *
 * Needed to avoid stopping a thread while it holds a lock.
 */
static inline
void jthread_disable_stop(void)
{
}

/**
 * Enable stopping the calling thread.
 *
 * Needed to avoid stopping a thread while it holds a lock.
 */
static inline
void jthread_enable_stop(void)
{
}

/** 
 * Stop a thread.
 * 
 * @param tid the thread to stop.
 */
static inline
void jthread_stop(jthread_t tid)
{
}

/**
 * Interrupt a thread.
 * 
 * @param tid the thread to interrupt
 */
static inline
void jthread_interrupt(jthread_t tid)
{
}

/**
 * Register a function to be called when the vm exits.
 * 
 * @param func the func to execute.
 */
static inline
void jthread_atexit(void* func)
{
}

/**
 * Dump some information about a thread to stderr.
 *
 * @param tid the thread whose info is to be dumped.
 */
static inline
void jthread_dumpthreadinfo(jthread_t tid)
{
}

/**
 * Return the java.lang.Thread instance attached to a thread
 *
 * @param tid the native thread whose corresponding java thread
 *            is to be returned.
 * @return the java.lang.Thread instance.
 */
static inline
void* jthread_getcookie(jthread_t tid)
{
        return (tid->jlThread);
}

/**
 * Test whether an address is on the stack of the calling thread.
 *
 * @param p the address to check
 *
 * @return true if address is on the stack
 *
 * Needed for locking and for exception handling.
 */
static inline
bool jthread_on_current_stack(void* p)
{
  jthread_t nt = jthread_current();
  if (nt == 0 || (p > nt->stackMin && p < nt->stackMax)) {
	return (true);
  }
  else {
	return (false);
  }
}

/**
 * Check for room on stack.
 *
 * @param left number of bytes that are needed
 *
 * @return true if @left bytes are free, otherwise false 
 *
 * Needed by intrp in order to implement stack overflow checking.
 */
static inline
bool jthread_stackcheck(int left)
{
	int rc;
#if defined(STACK_GROWS_UP)
        rc = jthread_on_current_stack((char*)&rc + left);
#else
        rc = jthread_on_current_stack((char*)&rc - left);
#endif
	return (rc);
}

/**
 * Extract the range of the stack that's in use.
 * 
 * @param tid the thread whose stack is to be examined
 * @param from storage for the address of the start address
 * @param len storage for the size of the used range
 *
 * @return true if successful, otherwise false
 *
 * Needed by the garbage collector.
 */
static inline
bool jthread_extract_stack(jthread_t tid, void** from, unsigned* len)
{
  if (tid->active == 0) {
    return false;
  }
  assert(tid->suspendState == SS_SUSPENDED);
#if defined(STACK_GROWS_UP)
  *from = tid->stackMin;
  *len = tid->stackCur - tid->stackMin;
#else
  *from = tid->stackCur;
  *len = tid->stackMax - tid->stackCur;
#endif
  return true;
}

/**
 * Returns the upper bound of the stack of the calling thread.
 *
 * Needed by support.c in order to implement stack overflow checking. 
 */
static inline
void* jthread_stacklimit(void)
{
  jthread_t nt = jthread_current();
#if defined(STACK_GROWS_UP)
  return (nt->stackMax - STACKREDZONE);
#else
  return (nt->stackMin + STACKREDZONE);
#endif
}

/*
 * Get the current stack limit.
 * Adapted from kaffe/kaffevm/systems/unix-jthreads/jthread.h
 */
static inline 
void jthread_relaxstack(int yes)
{
	if( yes )
	{
#if defined(STACK_GROWS_UP)
		jthread_current()->stackMax += STACKREDZONE;
#else
		jthread_current()->stackMin -= STACKREDZONE;
#endif
	}
	else
	{
#if defined(STACK_GROWS_UP)
		jthread_current()->stackMax -= STACKREDZONE;
#else
		jthread_current()->stackMin += STACKREDZONE;
#endif
	}
}

/**
 * yield.
 *
 */
static inline
void jthread_yield (void)
{
  sched_yield();
}

/**
 * Acquire a spin lock.
 *
 */
static inline
void jthread_spinon(int dummy)
{
}

/**
 * Release a spin lock.
 *
 */
static inline
void jthread_spinoff(int dummy)
{
}

struct _exceptionFrame;
typedef void (*exchandler_t)(struct _exceptionFrame*);

/**
 * Initialize handlers for null pointer accesses and div by zero        
 *
 */             
void jthread_initexceptions(exchandler_t _nullHandler,
			    exchandler_t _floatingHandler);

/**
 * Initialize the thread subsystem.
 *
 */
void jthread_init(int preemptive,                 /* preemptive scheduling */
		  int maxpr,                      /* maximum priority */
		  int minpr,                      /* minimum priority */
		  void *(*_allocator)(size_t),    /* memory allocator */
		  void (*_deallocator)(void*),    /* memory deallocator */
		  void (*_destructor1)(void*),    /* called when a thread exits */
		  void (*_onstop)(void),          /* called when a thread is stopped */
		  void (*_ondeadlock)(void));     /* called when we detect deadlock */


/**
 * Bind the main thread of the vm to a java.lang.Thread instance.
 *
 */
jthread_t jthread_createfirst(size_t, unsigned char, void*);

/**
 * Create a new native thread.
 *
 */
jthread_t jthread_create (unsigned char pri, void* func, int daemon,
			  void* jlThread, size_t threadStackSize );


/**
 * Set the priority of a native thread.
 *
 */
void jthread_setpriority (jthread_t thread, jint prio);

/**
 * Called by thread.c when a thread is finished. 
 * 
 */
void jthread_exit ( void );

/**
 * Destroys the a native thread.
 *
 * @param thread the thread to destroy.
 *
 * Called when finalizing a java.lang.Thread instance.
 */
void jthread_destroy (jthread_t thread);

/**
 * Suspends all threads but the calling one. 
 *
 * Currently needed by the garbage collector.
 */
void jthread_suspendall (void);

/**
 * Unsuspends all threads but the calling one. 
 *
 * Currently needed by the garbage collector.
 */
void jthread_unsuspendall (void);

/**
 * Call a function once for each active thread.
 *
 */
void jthread_walkLiveThreads (void(*)(void*));

#endif /* __thread_impl_h */
