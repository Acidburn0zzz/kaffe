/*
 * main.c
 * Kick off program.
 *
 * Copyright (c) 1996-2000
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * Cross-language profiling changes contributed by
 * the Flux Research Group, Department of Computer Science,
 * University of Utah, http://www.cs.utah.edu/flux/
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

#include "config.h"
#include "config-std.h"
#include "config-mem.h"
#include "debug.h"
#include "classMethod.h"
#include "kaffe/jtypes.h"
#include "native.h"
#include "constants.h"
#include "support.h"
#include "errors.h"
#include "thread.h"
#include "system.h"
#include "md.h"
#include "ltdl.h"
#include "version.h"
#include "debugFile.h"
#include "xprofiler.h"
#include "fileSections.h"
#include "feedback.h"
#include "methodCache.h"
#include "external.h"

#if defined(KAFFE_PROFILER)
extern int profFlag;
#endif

#include "jni.h"

JavaVMInitArgs vmargs;
JNIEnv* global_env;
JavaVM* global_vm;
static int isJar = 0;
static char *jvm_onload;

static int options(char**, int);
static void usage(void);
static size_t parseSize(char*);
static int checkException(void);
static int main2(JNIEnv* env, char *argv[], int farg, int argc);

#define	KAFFEHOME	"KAFFEHOME"
#define	CLASSPATH1	"KAFFECLASSPATH"
#define	LIBRARYPATH1	"KAFFELIBRARYPATH"

#define	CLASSPATH2	"CLASSPATH"
#define	LIBRARYPATH2	"LD_LIBRARY_PATH"

#define BOOTCLASSPATH 	"BOOTCLASSPATH"

/*
 * MAIN
 */
int
main(int argc, char* argv[])
{
	int farg;
	const char* cp;

#if defined(MAIN_MD)
	/* Machine specific main first */
	MAIN_MD;
#endif
	vmargs.version = JAVA_VERSION_HEX;

#if defined(KAFFE_PROFILER)
	profFlag = 0;
#endif

	JNI_GetDefaultJavaVMInitArgs(&vmargs);

	/* set up libtool/libltdl dlopen emulation */
	LTDL_SET_PRELOADED_SYMBOLS();

#if defined(KAFFE_VMDEBUG)
	cp = getenv("KAFFE_VMDEBUG");
	if (cp != 0)
		dbgSetMaskStr(cp);
#endif

	cp = getenv(BOOTCLASSPATH);
	vmargs.bootClasspath = cp;

	cp = getenv(CLASSPATH1);
	if (cp == 0) {
		cp = getenv(CLASSPATH2);
#if defined(DEFAULT_CLASSPATH)
		if (cp == 0) {
			cp = DEFAULT_CLASSPATH;
		}
#endif
	}
	vmargs.classpath = cp;

        cp = getenv(LIBRARYPATH1);
	if (cp == 0) {
		cp = getenv(LIBRARYPATH2);
	}
        vmargs.libraryhome = cp;

        cp = getenv(KAFFEHOME);
        if (cp == 0) {
#if defined(DEFAULT_KAFFEHOME)
                cp = DEFAULT_KAFFEHOME;
#endif
        }
        vmargs.classhome = cp;

	/* Process program options */
	farg = options(argv, argc);
	argc = argc - farg;

#if defined(KAFFE_XPROFILER)
	if( xProfFlag )
	{
		if( !enableXCallGraph() )
		{
			fprintf(stderr, 
				"Unable to initialize cross "
				"language profiling\n");
			xProfFlag = 0;
		}
	}
#endif

	/* Get the class name to start with */
	if (argv[farg] == 0) {
		usage();
		exit(1);
	}

	/* Initialise */
	JNI_CreateJavaVM(&global_vm, &global_env, &vmargs);

	/* Handle the '-Xrun' argument. */
	if( jvm_onload != NULL )
	{
		char *libpath, *libargs;
		char errbuf[512];
		int i;

		/* XXX Pull findLibrary() from the JanosVM. */
		libpath = &jvm_onload[2];
		libpath[0] = 'l';
		libpath[1] = 'i';
		libpath[2] = 'b';

		if( (libargs = strchr(jvm_onload, ':')) != NULL )
		{
			*libargs = '\0';
			libargs += 1;
		}
		
		i = loadNativeLibrary(libpath, errbuf, sizeof(errbuf));
		if( i > 0 )
		{
			jint (*onload_func)(JavaVM *jvm, char *, void *);

			if( (onload_func =
			     loadNativeLibrarySym("JVM_OnLoad")) != NULL )
			{
				(void)onload_func(global_vm, libargs, NULL);
			}
		}
		else
		{
			fprintf(stderr,
				"Unable to load %s: %s\n",
				libpath,
				errbuf);
			exit(1);
		}
	}

	return (main2(global_env, argv, farg, argc));
}

/*
 * Note:
 * Why do we split main in two parts?
 *
 * During initialisation, which is invoked in JNI_CreateJavaVM, we will
 * estimate the upper end of the main stack by taking the address of a
 * local variable.  The upper end of the main stack is important since
 * it tells the conservative garbage collector the upper boundary up to
 * which it must scan the stack (stackEnd in the thread context).
 * (Replace stackEnd with stackBase and upper with lower if your stack
 *  grows upward.)
 *
 * To ensure that no references will be stored on the stack above that
 * estimated point, we will allocate 1K on the stack as a safe zone.
 *
 * The old approach would have the initialisation code guess how much
 * it must add to the address of the local variable to find the actual
 * upper end up to which the gc must look for local variables.
 * The problem with that approach is what if the code guesses wrong?
 * If it guesses too low, we will lose objects.  If it guesses too
 * high, however, the gc might segfault when trying to scan the stack.
 *
 * With the new approach some guessing is still involved:
 * we guess how big the safe zone should be.  If we guess too small,
 * we will lose objects.  If we guess too big, however, all we do is to
 * waste memory on the main stack.
 * Weighing the consequences, the new approach seems better.
 * Does anybody have a better solution?
 */

/*
 * MAIN, part II
 */
static int
main2(JNIEnv* env, char *argv[], int farg, int argc)
{
	char gc_safe_zone[1024];
	jarray args;
	jclass lcls;
	jclass cls;
	jclass mcls;
	jmethodID cmth;
	jmethodID lmth;
	jmethodID mmth;
	jobject str;
	jobject loader;
	int i;
	const char* exec;

	/* make sure no compiler optimizes this away */
	gc_safe_zone[0] = gc_safe_zone[sizeof gc_safe_zone - 1] = 0;

	/* Executable is a JAR?  Use the JAR launcher */
	if (isJar != 0) {
		exec = "kaffe.jar.ExecJar";
		
		mcls = (*env)->FindClass(env, exec);
		if (checkException())
			goto done;
	}
	else {
		exec = argv[farg];
		farg++;
		argc--;

		/* Get the application class loader class */
		lcls = (*env)->FindClass(env, "kaffe.lang.AppClassLoader");
		if (checkException())
			goto done;
		
		/* ... and then get the singleton. */
		cmth = (*env)->GetStaticMethodID(env,
						 lcls,
						 "getSingleton",
						 "()Ljava/lang/ClassLoader;");
		if (checkException())
			goto done;
		
		loader = (*env)->CallStaticObjectMethod(env,
							lcls,
							cmth);
		if (checkException())
			goto done;
		
		/* Load the main class into the AppClassLoader */
		lmth = (*env)->GetMethodID(env,
					   lcls,
					   "loadClass",
					   "(Ljava/lang/String;Z)Ljava/lang/Class;");
		if (checkException())
			goto done;
		
DBG(VMCLASSLOADER,
    /* Announce when VM calls class loaders.. */
    dprintf("Calling user-defined \"startup\" class loader "
	    "kaffe/lang/AppClassLoader - loadClass(%s)\n", exec);
    )
	
		mcls = (*env)->CallObjectMethod(env,
						loader,
						lmth,
						(*env)->NewStringUTF(env, exec),
						false);
		if (checkException())
			goto done;
	}
	
	/* ... and run main. */
	mmth = (*env)->GetStaticMethodID(env,
	    mcls, "main", "([Ljava/lang/String;)V");
	if (checkException())
		goto done;

	/* Build an array of strings as the arguments */
	cls = (*env)->FindClass(env, "java/lang/String");
	if (checkException())
		goto done;
	args = (*env)->NewObjectArray(env, (unsigned)argc, cls, 0);
	if (checkException())
		goto done;
	for (i = 0; i < argc; i++) {
		str = (*env)->NewStringUTF(env, argv[farg+i]);
		if (checkException())
			goto done;
		(*env)->SetObjectArrayElement(env, args, (unsigned)i, str);
		if (checkException())
			goto done;
	}

	/* Call method, check for errors and then exit */
	(*env)->CallStaticVoidMethod(env, mcls, mmth, args);
	(void)checkException();

done:
	/* We're done. We are the "main thread" and so are required to call
	   (*vm)->DestroyJavaVM() instead of (*vm)->DetachCurrentThread() */
	(*global_vm)->DestroyJavaVM(global_vm);
	return (0);
}

static int
checkException(void)
{
	jobject e;
	jclass eiic;

	/* Display exception stack trace */
	if ((e = (*global_env)->ExceptionOccurred(global_env)) == NULL)
		return (0);
	(*global_env)->ExceptionDescribe(global_env);
	(*global_env)->ExceptionClear(global_env);

	/* Display inner exception in ExceptionInInitializerError case */
	eiic = (*global_env)->FindClass(global_env, "java/lang/ExceptionInInitializerError");
	if ((*global_env)->ExceptionOccurred(global_env) != NULL) {
		(*global_env)->ExceptionClear(global_env);
		return (1);
	}
	if ((*global_env)->IsInstanceOf(global_env, e, eiic)) {
		e = (*global_env)->CallObjectMethod(global_env, e,
		    (*global_env)->GetMethodID(global_env, (*global_env)->GetObjectClass(global_env, e),
			"getException", "()Ljava/lang/Throwable;"));
		if ((*global_env)->ExceptionOccurred(global_env) != NULL) {
			(*global_env)->ExceptionClear(global_env);
			return (1);
		}
		if (e != NULL) {
			(*global_env)->Throw(global_env, e);
			return (checkException());
		}
	}
	return (1);
}

/*
 * Process program's flags.
 */
static
int
options(char** argv, int argc)
{
	int i;
	unsigned int j;
	size_t sz;
	userProperty* prop;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			break;
		}

		if (strcmp(argv[i], "-help") == 0) {
			usage();
			exit(0);
		}
		else if (strcmp(argv[i], "-version") == 0) {
			printShortVersion();
			exit(0);
		}
		else if (strcmp(argv[i], "-fullversion") == 0) {
			printFullVersion();
			exit(0);
		}
#if defined(__ia64__)
		else if (strcmp(argv[i], "-ia32") == 0) {
			i++;
			/* FIXME: skip, case handled by the calle script */
		}
#endif
		else if ((strcmp(argv[i], "-addclasspath") == 0)
			 || (strcmp(argv[i], "-classpath") == 0)
			 || (strcmp(argv[i], "-cp") == 0)) {
			char	*newcpath;
			unsigned int      cpathlength;

			i++;
			if (argv[i] == 0) {
				fprintf(stderr, 
				    "Error: No path found for %s option.\n",
				    argv[i - 1]);
				exit(1);
			}

			cpathlength = ((vmargs.classpath != NULL) ? strlen(vmargs.classpath) : 0)
				+ strlen(path_separator)
				+ strlen(argv[i])
				+ 1;

			/* Get longer buffer FIXME:  free the old one */
			if ((newcpath = malloc(cpathlength)) == NULL) {
				fprintf(stderr,  "Error: out of memory.\n");
				exit(1);
			}

			/* Construct new classpath */
			if( vmargs.classpath != 0 )
			  	strcpy(newcpath, vmargs.classpath);
			else
				newcpath[0] = '\0';
			strcat(newcpath, path_separator);
			strcat(newcpath, argv[i]);

			/* set the new classpath */
			vmargs.classpath = newcpath;
		}
		else if (strncmp(argv[i], "-Xbootclasspath/p:", (j=18)) == 0) {
			char	*newbootcpath;
			unsigned int      bootcpathlength;

			bootcpathlength = strlen(&argv[i][j])
				+ strlen(path_separator)
				+ ((vmargs.bootClasspath != NULL) ?
					strlen(vmargs.bootClasspath) : 0)
				+ 1;

			/* Get longer buffer FIXME:  free the old one */
			if ((newbootcpath = malloc(bootcpathlength)) == NULL) {
				fprintf(stderr,  "Error: out of memory.\n");
				exit(1);
			}

			/* Construct new boot classpath */
			strcpy(newbootcpath, &argv[i][j]);
			strcat(newbootcpath, path_separator);
			if( vmargs.bootClasspath != 0 )
			  	strcat(newbootcpath, vmargs.bootClasspath);

			/* set the new boot classpath */
			vmargs.bootClasspath = newbootcpath;
		}
		else if (strncmp(argv[i], "-Xbootclasspath/a:", (j=18)) == 0) {
			char	*newbootcpath;
			unsigned int      bootcpathlength;

			bootcpathlength = strlen(&argv[i][j])
				+ strlen(path_separator)
				+ ((vmargs.bootClasspath != NULL) ?
					strlen(vmargs.bootClasspath) : 0)
				+ 1;

			/* Get longer buffer FIXME:  free the old one */
			if ((newbootcpath = malloc(bootcpathlength)) == NULL) {
				fprintf(stderr,  "Error: out of memory.\n");
				exit(1);
			}

			/* Construct new boot classpath */
			if( vmargs.bootClasspath != 0 ) {
			  	strcpy(newbootcpath, vmargs.bootClasspath);
				strcat(newbootcpath, path_separator);
			}
			strcat(newbootcpath, &argv[i][j]);

			/* set the new boot classpath */
			vmargs.bootClasspath = newbootcpath;
		}
		else if (strncmp(argv[i], "-Xbootclasspath:", (j=16)) == 0) {
			char	*newbootcpath;
			unsigned int      bootcpathlength;

			bootcpathlength = strlen(&argv[i][j]) + 1;

			/* Get longer buffer FIXME:  free the old one */
			if ((newbootcpath = malloc(bootcpathlength)) == NULL) {
				fprintf(stderr,  "Error: out of memory.\n");
				exit(1);
			}

			/* Construct new boot classpath */
			strcpy(newbootcpath, &argv[i][j]);

			/* set the new boot classpath */
			vmargs.bootClasspath = newbootcpath;
		}
		else if ((strncmp(argv[i], "-ss", (j=3)) == 0) 
			 || (strncmp(argv[i], "-Xss", (j=4)) == 0)) {
			if (argv[i][j] == 0) {
				i++;
				if (argv[i] == 0) {
					fprintf(stderr,  "Error: No stack size found for -ss option.\n");
					exit(1);
				}
				sz = parseSize(argv[i]);
			} else {
				sz = parseSize(&argv[i][j]);
			}
			if (sz < THREADSTACKSIZE) {
				fprintf(stderr,  "Warning: Attempt to set stack size smaller than %d - ignored.\n", THREADSTACKSIZE);
			}
			else {
				vmargs.nativeStackSize = sz;
			}
		}
		else if ((strncmp(argv[i], "-mx", (j=3)) == 0)
			 || (strncmp(argv[i], "-Xmx", (j=4)) == 0)) {
			if (argv[i][j] == 0) {
				i++;
				if (argv[i] == 0) {
					fprintf(stderr,  "Error: No heap size found for -mx option.\n");
					exit(1);
				}
				vmargs.maxHeapSize = parseSize(argv[i]);
			} else {
				vmargs.maxHeapSize = parseSize(&argv[i][j]);
			}
		}
		else if ((strncmp(argv[i], "-ms", (j=3)) == 0)
			 || (strncmp(argv[i], "-Xms", (j=4)) == 0)) {
			if (argv[i][j] == 0) {
				i++;
				if (argv[i] == 0) {
					fprintf(stderr,  "Error: No heap size found for -ms option.\n");
					exit(1);
				}
				vmargs.minHeapSize = parseSize(argv[i]);
			} else {
				vmargs.minHeapSize = parseSize(&argv[i][j]);
			}
		}
		else if (strncmp(argv[i], "-as", 3) == 0) {
			if (argv[i][3] == 0) {
				i++;
				if (argv[i] == 0) {
					fprintf(stderr,  "Error: No heap size found for -as option.\n");
					exit(1);
				}
				vmargs.allocHeapSize = parseSize(argv[i]);
			} else {
				vmargs.allocHeapSize = parseSize(&argv[i][3]);
			}
		}
		else if (strcmp(argv[i], "-verify") == 0) {
			vmargs.verifyMode = 3;
		}
		else if (strcmp(argv[i], "-verifyremote") == 0) {
			vmargs.verifyMode = 2;
		}
		else if (strcmp(argv[i], "-noverify") == 0) {
			vmargs.verifyMode = 0;
		}
		else if (strcmp(argv[i], "-verbosegc") == 0) {
			vmargs.enableVerboseGC = 1;
		}
		else if (strcmp(argv[i], "-noclassgc") == 0) {
			vmargs.enableClassGC = 0;
		}
		else if (strcmp(argv[i], "-verbosejit") == 0) {
			vmargs.enableVerboseJIT = 1;
		}
		else if (strcmp(argv[i], "-verbosemem") == 0) {
			vmargs.enableVerboseGC = 2;
		}
		else if (strcmp(argv[i], "-verbosecall") == 0) {
			vmargs.enableVerboseCall = 1;
		}
		else if (strcmp(argv[i], "-verbose") == 0 || strcmp(argv[i], "-v") == 0) {
			vmargs.enableVerboseClassloading = 1;
		}
                else if (strcmp(argv[i], "-jar") == 0) {
                        isJar = 1;
                }
		else if (strncmp(argv[i], "-Xrun", 5) == 0) {
			jvm_onload = argv[i];
		}
#if defined(KAFFE_PROFILER)
		else if (strcmp(argv[i], "-prof") == 0) {
			profFlag = 1;
			vmargs.enableClassGC = 0;
		}
#endif
#if defined(KAFFE_XPROFILER)
		else if (strcmp(argv[i], "-Xxprof") == 0) {
			xProfFlag = 1;
			vmargs.enableClassGC = 0;
		}
		else if (strcmp(argv[i], "-Xxprof_syms") == 0) {
			i++;
			if (argv[i] == 0) {
				fprintf(stderr, 
					"Error: -Xxprof_syms option requires "
					"a file name.\n");
			}
			else if( !profileSymbolFile(argv[i]) )
			{
				fprintf(stderr, 
					"Unable to create profiler symbol "
					"file %s.\n",
					argv[i]);
			}
		}
		else if (strcmp(argv[i], "-Xxprof_gmon") == 0) {
			i++;
			if (argv[i] == 0) {
				fprintf(stderr, 
					"Error: -Xxprof_gmon option requires "
					"a file name.\n");
			}
			else if (!profileGmonFile(argv[i]))
			{
				fprintf(stderr, 
					"Unable to create gmon file %s.\n",
					argv[i]);
			}
		}
#endif
#if defined(KAFFE_XDEBUGGING)
		else if (strcmp(argv[i], "-Xxdebug") == 0) {
			/* Use a default name */
			machine_debug_filename = "xdb.as";
		}
		else if (strcmp(argv[i], "-Xxdebug_file") == 0) {
			i++;
			if (argv[i] == 0) {
				fprintf(stderr, 
					"Error: -Xxdebug_file option requires "
					"a file name.\n");
			}
			else
			{
				machine_debug_filename = argv[i];
			}
		}
#endif
#if defined(KAFFE_FEEDBACK)
		else if (strcmp(argv[i], "-Xfeedback") == 0) {
			i++;
			if (argv[i] == 0) {
				fprintf(stderr, 
					"Error: -Xfeedback option requires a "
					"file name.\n");
			}
			else
			{
				feedback_filename = argv[i];
			}
		}
#endif
		else if (strcmp(argv[i], "-nodeadlock") == 0) {
			deadlockDetection = 0;
		}
#if defined(KAFFE_STATS)
                else if (strcmp(argv[i], "-vmstats") == 0) {
			extern void statsSetMaskStr(char *);
                        i++;
                        if (argv[i] == 0) { /* forgot second arg */
                                fprintf(stderr, 
					"Error: -vmstats option requires a "
					"second arg.\n");
                                exit(1);
                        }
                        statsSetMaskStr(argv[i]);
                }
#endif
#if defined(KAFFE_VMDEBUG)
                else if (strcmp(argv[i], "-vmdebug") == 0) {
                        i++;
                        if (argv[i] == 0) { /* forgot second arg */
                                fprintf(stderr, 
					"Error: -vmdebug option requires a "
					"debug flag. Use `list' for a list.\n");
                                exit(1);
                        }
                        if (!dbgSetMaskStr(argv[i]))
				exit(1);
                }
#endif
		else if (argv[i][1] ==  'D') {
			/* Set a property */
			prop = malloc(sizeof(userProperty));
			assert(prop != 0);
			prop->next = userProperties;
			userProperties = prop;
			for (sz = 2; argv[i][sz] != 0; sz++) {
				if (argv[i][sz] == '=') {
					argv[i][sz] = 0;
					sz++;
					break;
				}
			}
			prop->key = &argv[i][2];
			prop->value = &argv[i][sz];
		}
		else if (argv[i][1] == 'X') {
			fprintf(stderr, 
				"Error: Unrecognized JVM specific option "
				"`%s'.\n",
				argv[i]);
		}
		/* The following options are not supported and will be
		 * ignored for compatibility purposes.
		 */
		else if (strcmp(argv[i], "-noasyncgc") == 0 ||
		   strcmp(argv[i], "-cs") == 0 ||
		   strcmp(argv[i], "-checksource")) {
		}
		else if (strcmp(argv[i], "-oss") == 0) {
			i++;
		}
		else {
			fprintf(stderr,  "Unknown flag: %s\n", argv[i]);
		}
	}

	/* Return first no-flag argument */
	return (i);
}

/*
 * Print usage message.
 */
static
void
usage(void)
{
	fprintf(stderr, "usage: kaffe [-options] class\n");
	fprintf(stderr, "Options are:\n");
	fprintf(stderr, "	-help			 Print this message\n");
	fprintf(stderr, "	-version		 Print version number\n");
	fprintf(stderr, "	-fullversion		 Print verbose version info\n");
#if defined(__ia64__)
	fprintf(stderr, "	-ia32			 Execute the ia32 version of Kaffe\n");
#endif
	fprintf(stderr, "	-ss <size>		 Maximum native stack size\n");
	fprintf(stderr, "	-mx <size> 		 Maximum heap size\n");
	fprintf(stderr, "	-ms <size> 		 Initial heap size\n");
	fprintf(stderr, "	-as <size> 		 Heap increment\n");
	fprintf(stderr, "	-classpath <path>        Set classpath\n");
	fprintf(stderr, "	-Xbootclasspath:<path>   Set bootclasspath\n");
	fprintf(stderr, "	-Xbootclasspath:/a<path> Append path to bootclasspath\n");
	fprintf(stderr, "	-Xbootclasspath:/p<path> Prepend path to bootclasspath\n");
	fprintf(stderr, "	-D<property>=<value>     Set a property\n");
	fprintf(stderr, "	-verify *		 Verify all bytecode\n");
	fprintf(stderr, "	-verifyremote *		 Verify bytecode loaded from network\n");
	fprintf(stderr, "	-noverify		 Do not verify any bytecode\n");
	fprintf(stderr, "	-noclassgc		 Disable class garbage collection\n");
	fprintf(stderr, "	-verbosegc		 Print message during garbage collection\n");
	fprintf(stderr, "	-v, -verbose		 Be verbose\n");
	fprintf(stderr, "	-verbosejit		 Print message during JIT code generation\n");
	fprintf(stderr, "	-verbosemem		 Print detailed memory allocation statistics\n");
	fprintf(stderr, "	-verbosecall		 Print detailed call flow information\n");
	fprintf(stderr, "	-nodeadlock		 Disable deadlock detection\n");
#if defined(KAFFE_PROFILER)
	fprintf(stderr, "	-prof			 Enable profiling of Java methods\n");
#endif
#if defined(KAFFE_XPROFILER)
	fprintf(stderr, "	-Xxprof			 Enable cross language profiling\n");
	fprintf(stderr, "	-Xxprof_syms <file>	 Name of the profiling symbols file [Default: kaffe-jit-symbols.s]\n");
	fprintf(stderr, "	-Xxprof_gmon <file>	 Base name for gmon files [Default: xgmon.out]\n");
#endif
#if defined(KAFFE_XDEBUGGING)
	fprintf(stderr, "	-Xxdebug_file <file>	 Name of the debugging symbols file\n");
#endif
#if defined(KAFFE_FEEDBACK)
	fprintf(stderr, "	-Xfeedback <file>	 The file name to write feedback data to\n");
#endif
	fprintf(stderr, "	-debug * 		 Trace method calls\n");
	fprintf(stderr, "	-noasyncgc *		 Do not garbage collect asynchronously\n");
	fprintf(stderr, "	-cs, -checksource *	 Check source against class files\n");
	fprintf(stderr, "	-oss <size> *		 Maximum java stack size\n");
        fprintf(stderr, "	-jar                     Executable is a JAR\n");
#ifdef KAFFE_VMDEBUG
        fprintf(stderr, "	-vmdebug <flag{,flag}>	 Internal VM debugging.  Set flag=list for a list\n");
#endif
#ifdef KAFFE_STATS
        fprintf(stderr, "	-vmstats <flag{,flag}>	 Print VM statistics.  Set flag=all for all\n");
#endif
	fprintf(stderr, "  * Option currently ignored.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Compatibility options:\n");
	fprintf(stderr, "	-Xss <size>		 Maximum native stack size\n");
	fprintf(stderr, "	-Xmx <size> 		 Maximum heap size\n");
	fprintf(stderr, "	-Xms <size> 		 Initial heap size\n");
	fprintf(stderr, "	-cp <path> 		 Set classpath\n");
}

static
size_t
parseSize(char* arg)
{
	size_t sz;
	char* narg;

	sz = strtol(arg, &narg, 0);
	switch (narg[0]) {
	case 'b': case 'B':
		break;

	case 'k': case 'K':
		sz *= 1024;
		break;

	case 'm': case 'M':
		sz *= 1024 * 1024;
		break;
	}

	return (sz);
}
