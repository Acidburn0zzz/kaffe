/*
 * external.h
 * Handle method calls to other languages.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#ifndef __external_h
#define __external_h

#define	MAXSTUBLEN	1024
#define	MAXLIBPATH	1024
#define	MAXLIBS		16

/* Default library info. */
#if !defined(LIBRARYPATH)
#define	LIBRARYPATH	"KAFFELIBRARYPATH"
#endif
#define	NATIVELIBRARY	"libnative"

struct _methods;
struct _errorInfo;

void	initNative(void);
int	loadNativeLibrary(char*, char*, size_t);
void	unloadNativeLibrary(int);
void*	loadNativeLibrarySym(char*);
bool	native(struct _methods*, struct _errorInfo*);
void	addNativeFunc(char*, void*);
char*	getLibraryPath(void);

#endif
