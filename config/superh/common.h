/*
 * superh/common.h
 * Common Super-H configuration information.
 *
 * Copyright (c) 2001
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * Copyright (c) 2003
 *	Kaffe.org contributors. See ChangeLog for details.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#ifndef __superh_common_h
#define __superh_common_h

#include <stddef.h>

/*
 * Do an atomic compare and exchange.  The address 'A' is checked against
 * value 'O' and if they match it's exchanged with value 'N'.
 * We return '1' if the exchange is sucessful, otherwise 0.
 *
 * Copied from "config/mips/common.h".
 */
#define COMPARE_AND_EXCHANGE(A,O,N)		\
({						\
    int ret = 0;				\
    jthread_suspendall();			\
						\
    if (*(A) == (O)) {				\
	*(A) = (N);				\
	ret = 1;				\
    }						\
    jthread_unsuspendall();			\
    ret;					\
})

#endif
