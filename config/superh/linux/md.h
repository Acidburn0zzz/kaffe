/*    
 * superh/linux/md.h
 * Linux SuperH configuration information.
 *
 * Copyright (c) 2001
 *      Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */
 
#ifndef __superh_linux_md_h
#define __superh_linux_md_h
 
#include "superh/common.h"
#include "superh/threads.h"

#if defined(TRANSLATOR)
#include "jit-md.h"
#endif

#define EXCEPTIONPROTO  	int sig, int d1, int d2, int d3, struct sigcontext ctx

#define	EXCEPTIONFIXRETURN()	asm volatile("mov.l %2,@%0\n\tmov.l %1,@(4,%0)" : : "r" ((int)&ctx - 8), "r" (ctx.sc_regs[14]), "r" (ctx.sc_pc));

#define	EXCEPTIONPC()		(ctx.sc_pc)

/* The Linux implementation of this provides a large amount
 * of information for real-time stuff.  We don't need that and
 * the standard version returns the sigcontext which is all
 * we're interested in.  So just turn this flag off.
 */
#if defined(SA_SIGINFO)
#undef  SA_SIGINFO
#endif

/*
 * No floating point support - so emulate
 */
#define	HAVE_NO_FLOATING_POINT	1

#endif
