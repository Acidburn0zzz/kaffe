/*
 * classMethod.h
 * Class, method and field tables.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

#ifndef __classmethod_h
#define __classmethod_h

#include "access.h"
#include "gtypes.h"
#include "object.h"
#include "md.h"	 /* XXX: need this here so KAFFE_PROFILER is accurately def'd */
#include "constants.h"
#include "errors.h"
#include "jthread.h"

#define	MAXMETHOD		64

/* Class state */
typedef enum {
	CSTATE_FAILED = -1,
	CSTATE_UNLOADED,
	CSTATE_LOADED,
	CSTATE_LOADED_SUPER,
	CSTATE_PRELOADED,
	CSTATE_VERIFIED,
	CSTATE_DOING_PREPARE,
	CSTATE_PREPARED,
	CSTATE_LINKED,
	CSTATE_CONSTINIT,
	CSTATE_DOING_SUPER,
	CSTATE_USABLE,
	CSTATE_DOING_INIT,
	CSTATE_COMPLETE,
} class_state_t;

struct _classEntry;
struct _innerClass;
struct Hjava_lang_String;

#include <java_lang_ClassLoader.h>

struct Hjava_lang_Class {
	Hjava_lang_Object	head;		/* A class is an object too */

	struct _iLock*		lock;		/* Lock for internal use */

	/* Link to class entry */
	struct _classEntry*	centry;

	Utf8Const*		name;
	char*			sourcefile;	/* source file name if known */
	accessFlags		accflags;

	/* If non-NULL, a pointer to the superclass.
	 * However, if state < CSTATE_DOING_PREPARE, then
	 * (int) superclass is a constant pool index. */
	struct Hjava_lang_Class* superclass;

	struct _constants	constants;

	/* For regular classes, an array of the methods defined in this class.
	   For array types, used for CLASS_ELEMENT_TYPE.
	   For primitive types, used by CLASS_ARRAY_CACHE. */
	Method*			methods;
	short			nmethods;

	/* Number of methods in the dtable. */
	/* If CLASS_IS_PRIMITIVE, then the CLASS_PRIM_SIG. */
	short			msize;

	/* Pointer to array of Fields, on for each field.
	   Static fields come first. */
	Field*			fields;

	/* The size of the non-static fields, in bytes.
	   For a primitive type, the length in bytes.
	   Also used temporarily while reading fields.  */
	int			bfsize;

	/* Number of fields, including static fields. */
	short			nfields;
	/* Number of static fields. */
	short			nsfields;

	struct _dispatchTable*	dtable;

	/* all interfaces supported by this class */
        struct Hjava_lang_Class** interfaces;
	short*			if2itable;	/* redundant now */
	void**			itable2dtable;
	short			interface_len;
	short			total_interface_len;
	/* indices for all classes implementing this interface */
	short*			implementors;	/* interfaces only */
	int			impl_index;

	Hjava_lang_ClassLoader*	loader;

	/* A bitmap describing the layout of instances of that class.
	   It contains CLASS_FSIZE/ALIGNMENTVOIDP bits.
	   The MSB corresponds to the dtable field.
	 */
	int*			gc_layout;
	class_state_t		state;
	void*			processingThread;
	Method*			finalizer;
	int			alloc_type;	/* allocation type */

	/* array containing static data */
	void*			static_data;

	/* InnerClasses attribute */
	short			this_index;
	short			nr_inner_classes;
	struct _innerClass*	inner_classes;

	void*			gcjPeer;	/* only needed if GCJ_SUPPORT */
#ifdef KAFFE_VMDEBUG
	int			live_count;
#endif
};

#ifndef __DEFINED_CLASS
#define __DEFINED_CLASS
typedef struct Hjava_lang_Class Hjava_lang_Class;
#endif

/*
 * Functions for locking and waiting on *internal* class
 * bits.  This is not for static+synchronized methods.
 * Use the centry lock, as it is convenient and available.
 */
#define lockClass(C) lockMutex(((C)))
#define unlockClass(C) unlockMutex(((C)))
#define waitOnClass(C) waitCond(((C)), 0)
#define signalOnClass(C) signalCond(((C)))
#define broadcastOnClass(C) broadcastCond(((C)))

#define METHOD_TRANSLATED(M)		((M)->accflags & ACC_TRANSLATED)
#define METHOD_JITTED(M)		((M)->accflags & ACC_JITTED)

#if defined(TRANSLATOR)
#define	METHOD_NATIVECODE(M)		(((M)->idx == -1) ? \
		((M)->ncode) : \
		((M)->class->dtable->method[(M)->idx]))
#else
/* INTERPRETER */
#define	METHOD_NATIVECODE(M)		((M)->ncode)
#endif

/* Like METHOD_NATIVECODE, except we take the address ---
 * gcc doesn't compile &( ? : ) expressions for some reason
 */
#define	PMETHOD_NATIVECODE(M)		(((M)->idx == -1) ? \
		(void*)&((M)->ncode) : \
		&((M)->class->dtable->method[(M)->idx]))

#define	METHOD_CODE_START(M)		((M)->c.ncode.ncode_start)
#define	SET_METHOD_NATIVECODE(M, C)	METHOD_NATIVECODE(M) = (C); \
					(M)->accflags |= ACC_TRANSLATED
#define	SET_METHOD_JITCODE(M, C)	METHOD_NATIVECODE(M) = (C); \
					(M)->accflags |= ACC_TRANSLATED|ACC_JITTED

#if defined(TRANSLATOR)
#define	METHOD_INDIRECTMETHOD(M)	METHOD_NATIVECODE(M)
#else
#define	METHOD_INDIRECTMETHOD(M)	(M)
#endif

/*
 * Stats for the nameMapping object.
 *
 * searching - The system is searching for this particular class.
 * loading - The system is loading this particular class and/or its parents.
 *   If a circularity exists in the hierarchy it will be detected in this
 *   state.
 * loaded - The class and its parents have been loaded.
 * done - The class have been loaded and verified.
 */
typedef enum {
	NMS_EMPTY,
	NMS_SEARCHING,
	NMS_LOADING,
	NMS_LOADED,
	NMS_DONE,
} name_mapping_state_t;

/*
 * Class hash entry.
 */
typedef struct _classEntry {
	struct _classEntry*	next;
	Utf8Const*		name;
    
        struct _iLock*          lock;
	name_mapping_state_t state;
	Hjava_lang_ClassLoader*	loader;
	union {
		jthread_t thread;
		struct Hjava_lang_Class *cl;
	} data;
} classEntry;

/*
 * The nameDependency structure is used to detect cycles between classes.
 * Basically, its used as a simple deadlock detection system.
 *
 * next - The next object in the list.
 * thread - The thread that is blocked on "mapping".
 * mapping - The mapping that "thread" is waiting for.
 */
typedef struct _nameDependency {
	struct _nameDependency *next;
	jthread_t thread;
	classEntry *mapping;
} nameDependency;

typedef struct _innerClass {
	u2			outer_class;
	u2			inner_class;
	u2			inner_class_accflags;
} innerClass;

typedef struct _parsed_signature {
	Utf8Const*		signature;
	u2			nargs;
	u2			ret_and_args[1]; /* index into signature */
	/* ret_and_args[0]: return value
	   ret_and_args[1]: first argument
	   etc */
} parsed_signature_t;

typedef struct _methods {
	Utf8Const*		name;
	parsed_signature_t*	parsed_sig;
	accessFlags		accflags;
	short			idx;	/* Index into class->dtable */
	u2			stacksz;
	u2			localsz;
	/* Only used for static/final/constructor methods */
	nativecode*		ncode;
	union {
	  struct {
		nativecode*	ncode_start;
		nativecode*	ncode_end;
	  } ncode;
	  struct {
		unsigned char*	code;
		int		codelen;
	  } bcode;
	} c;
	Hjava_lang_Class*	class;
	struct _lineNumbers*	lines;
	struct _jexception*	exception_table;
	int			ndeclared_exceptions;
	union {
		constIndex*		local_exceptions;
		struct _methods*	remote_exceptions;
	} declared_exceptions_u;
#define declared_exceptions declared_exceptions_u.local_exceptions
	int			framesize;	/* JIT only: size of frame */

#if defined(KAFFE_PROFILER)
	profiler_click_t	jitClicks;
	profiler_click_t	totalClicks;
	profiler_click_t	totalChildrenClicks;
	int			callsCount;
#endif
} methods;

#define PSIG_UTF8(sig)		((sig)->signature)
#define PSIG_DATA(sig)		(PSIG_UTF8((sig))->data)
#define PSIG_RET(sig)		((sig)->ret_and_args[0])
#define PSIG_NARGS(sig)		(sig->nargs)
#define PSIG_ARG(sig,n)		((sig)->ret_and_args[1+n])

#define METHOD_PSIG(M)		((M)->parsed_sig)
#define METHOD_SIG(M)		(PSIG_UTF8(METHOD_PSIG((M))))
#define METHOD_SIGD(M)		(PSIG_DATA(METHOD_PSIG((M))))
#define METHOD_RET_TYPE(M)	(METHOD_SIGD((M))+PSIG_RET(METHOD_PSIG((M))))
#define METHOD_NARGS(M)		(PSIG_NARGS(METHOD_PSIG((M))))
#define METHOD_ARG_TYPE(M,N)    (METHOD_SIGD((M))+PSIG_ARG(METHOD_PSIG((M)),(N)))

#define	METHOD_BYTECODE_LEN(M)	((M)->c.bcode.codelen)
#define	METHOD_BYTECODE_CODE(M)	((M)->c.bcode.code)

typedef struct _dispatchTable {
	Hjava_lang_Class*	class;
	void*			__dummy0; /* For GCJ/C++ compatibility */
	void*			method[1];
} dispatchTable;

#define	DTABLE_CLASS		0
#define	DTABLE_METHODOFFSET	(sizeof(void*)*2)
#define	DTABLE_METHODSIZE	(sizeof(void*))

typedef struct _fields {
	Utf8Const*		name;
	/* The type of the field, if FIELD_RESOLVED.
	   If !FIELD_RESOLVED:  The fields's signature as a (Utf8Const*). */
	Hjava_lang_Class*	type;
	accessFlags		accflags;
	u2			bsize;		/* in bytes */
	union {
		int		boffset;	/* offset in bytes (object) */
		void*		addr;		/* address (static) */
		u2		idx;		/* constant value index */
	} info;
} fields;

#define FIELD_UNRESOLVED_FLAG   0x8000
#define FIELD_CONSTANT_VALUE    0x4000

#define FIELD_RESOLVED(FLD)	((FLD)->type != 0 && !((FLD)->accflags & FIELD_UNRESOLVED_FLAG))

/* Type of field FLD.  Only valid if FIELD_RESOLVED(FLD). */
#define FIELD_TYPE(FLD)		((FLD)->type)
/* Size of field FLD, in bytes. */
#define FIELD_SIZE(FLD)		((FLD)->bsize)
#define FIELD_WSIZE(FLD)	((FLD)->bsize <= sizeof(jint) ? 1 : 2)
#define FIELD_BOFFSET(FLD)	((FLD)->info.boffset)
#define FIELD_ADDRESS(FLD)	((FLD)->info.addr)
#define	FIELD_CONSTIDX(FLD)	((FLD)->info.idx)
#define FIELD_ISPRIM(FLD)	(FIELD_RESOLVED(FLD)			\
				 && CLASS_IS_PRIMITIVE(FIELD_TYPE(FLD)))
#define FIELD_ISREF(FLD)	(!FIELD_ISPRIM(FLD)			\
				 && FIELD_TYPE(FLD) != PtrClass)

#define	CLASSMAXSIG		256

struct _Code;
struct _method_info;
struct _field_info;
struct classFile;

#define CLASS_METHODS(CLASS)  ((CLASS)->methods)
#define CLASS_NMETHODS(CLASS)  ((CLASS)->nmethods)

/* An array containing all the Fields, static fields first. */
#define CLASS_FIELDS(CLASS)   ((CLASS)->fields)

/* An array containing all the static Fields. */
#define CLASS_SFIELDS(CLASS)  ((CLASS)->fields)

/* The static data of this class */
#define CLASS_STATICDATA(CLASS)  ((CLASS)->static_data)

/* An array containing all the instance (non-static) Fields. */
#define CLASS_IFIELDS(CL)     (&(CL)->fields[CLASS_NSFIELDS(CL)])

/* Total number of fields (instance and static). */
#define CLASS_NFIELDS(CLASS)  ((CLASS)->nfields)
/* Number of instance (non-static) fields. */
#define CLASS_NIFIELDS(CLASS) ((CLASS)->nfields - (CLASS)->nsfields)
/* Number of static fields. */
#define CLASS_NSFIELDS(CLASS) ((CLASS)->nsfields)

/* Size of a class fields (including header), in words. */
#define CLASS_WFSIZE(CLASS)   ((CLASS)->bfsize / sizeof(jint))

/* Size of a class's fields (including header), in bytes. */
#define CLASS_FSIZE(CLASS)    ((CLASS)->bfsize)

#define OBJECT_CLASS(OBJ)     ((OBJ)->dtable->class)
#define CLASS_CNAME(CL)  ((CL)->name->data)
#define CLASS_SOURCEFILE(CL)	\
	((CL)->sourcefile == 0 ? "source file unknown" : (CL)->sourcefile)
#define _PRIMITIVE_DTABLE ((struct _dispatchTable*)(-1))
#define CLASS_IS_PRIMITIVE(CL) ((CL)->dtable == _PRIMITIVE_DTABLE)

/* Assuming CLASS_IS_PRIMITIVE(CL), return the 1-letter signature code. */
#define CLASS_PRIM_SIG(CL) ((CL)->msize)
#define	CLASS_PRIM_NAME(CL) (*(Utf8Const**)&(CL)->fields)

/* A freshly born class that does have its name set, but the collector
 * may already want to know whether it'll be a boy or a girl.
 */
#define CLASS_IS_ARRAY(CL) ((CL)->name && CLASS_CNAME(CL)[0] == '[')

#define	CLASS_IS_INTERFACE(CL) ((CL)->accflags & ACC_INTERFACE)
#define	CLASS_IS_ABSTRACT(CL) ((CL)->accflags & ACC_ABSTRACT)
#define	CLASS_IS_FINAL(CL) ((CL)->accflags & ACC_FINAL)

/* For an array type, the types of the elements. */
#define CLASS_ELEMENT_TYPE(ARRAYCLASS) (*(Hjava_lang_Class**)&(ARRAYCLASS)->methods)

/* Used by the lookupArray function. */
#define CLASS_ARRAY_CACHE(PRIMTYPE) (*(Hjava_lang_Class**)&(PRIMTYPE)->methods)

#define TYPE_PRIM_SIZE(CL) ((CL)->bfsize)
#define TYPE_SIZE(CL) \
  (CLASS_IS_PRIMITIVE(CL) ? TYPE_PRIM_SIZE (CL) : PTR_TYPE_SIZE)

#define	METHOD_IS_STATIC(METH) ((METH)->accflags & ACC_STATIC)
#define	METHOD_IS_CONSTRUCTOR(METH) ((METH)->accflags & ACC_CONSTRUCTOR)
#define	METHOD_IS_FINAL(METH) ((METH)->accflags & ACC_FINAL)
#define	METHOD_IS_PRIVATE(METH) ((METH)->accflags & ACC_PRIVATE)
#define	METHOD_IS_NATIVE(METH) ((METH)->accflags & ACC_NATIVE)

#define CLASS_GCJ(C)		((C)->accflags & ACC_GCJ)
#define SET_CLASS_GCJ(C)	(C)->accflags |= ACC_GCJ

/* For manipulating the constant pool in a class */
#define CLASS_CONSTANTS(CL) (&(CL)->constants)
#define CLASS_CONST_SIZE(CL) ((CL)->constants.size)
#define CLASS_CONST_TAG(CL, IDX) ((CL)->constants.tags[IDX])
#define CLASS_CONST_DATA(CL, IDX) ((CL)->constants.data[IDX])
#define CLASS_CONST_UTF8(CL, IDX) WORD2UTF(CLASS_CONST_DATA(CL, IDX))
#define CLASS_CONST_INT(CL, IDX) ((int32) CLASS_CONST_DATA(CL, IDX))
#if SIZEOF_VOIDP == 8
#define CLASS_CONST_LONG(CL, IDX) \
  ((uint64) CLASS_CONST_DATA(CL, IDX))
#else
#define CLASS_CONST_LONG(CL, IDX) \
  WORDS_TO_LONG ((CL)->constants.data[IDX], (CL)->constants.data[(IDX)+1])
#endif
#define	CLASS_CONST_DOUBLE(CL, IDX) \
  CLASS_CONST_LONG(CL, IDX)
/* The first uint16 of the INDEX'th constant pool entry. */
#define CLASS_CONST_USHORT1(CL, INDEX) ((CL)->constants.data[INDEX] & 0xFFFF)
/* The second uint16 of the INDEX'th constant pool entry. */
#define CLASS_CONST_USHORT2(CL, INDEX) \
  ((uint16)((CL)->constants.data[INDEX] >> 16))


/*
 * 'processClass' is the core of the class initialiser and can prepare a
 * class from the cradle to the grave.
 */
bool			processClass(Hjava_lang_Class*, int, errorInfo *einfo);

Hjava_lang_Class*	loadClass(Utf8Const*, Hjava_lang_ClassLoader*, errorInfo *einfo);
Hjava_lang_Class*	loadArray(Utf8Const*, Hjava_lang_ClassLoader*, errorInfo *einfo);
Hjava_lang_Class* 	findClass(struct _classEntry* centry, errorInfo *einfo);

void			loadStaticClass(Hjava_lang_Class**, const char*);

Hjava_lang_Class*	setupClass(Hjava_lang_Class*, constIndex,
				   constIndex, u2, Hjava_lang_ClassLoader*, errorInfo*);
bool 			addSourceFile(Hjava_lang_Class* c, int idx, errorInfo*);
bool			addInnerClasses(Hjava_lang_Class* c, uint32 len, struct classFile* fp, errorInfo *info);
int			startMethods(Hjava_lang_Class*, u2 methct, errorInfo*);
Method*			addMethod(Hjava_lang_Class*, u2 access_flags,
				  u2 name_index, u2 signature_index, errorInfo*);
Method*			addExceptionMethod(Hjava_lang_Class*, Utf8Const*, Utf8Const*);
void 			addMethodCode(Method*, struct _Code*);
Field*        		addField(Hjava_lang_Class*, u2 access_flags,
				 u2 name_index, u2 signature_index, errorInfo* einfo);
void			addInterfaces(Hjava_lang_Class*, u2, Hjava_lang_Class**);
void			setFieldValue(Hjava_lang_Class*, Field*, u2);
Hjava_lang_Class*	resolveFieldType(Field*, Hjava_lang_Class*, errorInfo*);
bool			getInheritedMethodIndex(Hjava_lang_Class *clazz, Method *meth);

classEntry*		lookupClassEntry(Utf8Const*, Hjava_lang_ClassLoader*,
				errorInfo *info);
classEntry*		lookupClassEntryInternal(Utf8Const*,
				Hjava_lang_ClassLoader*);
int			removeClassEntries(Hjava_lang_ClassLoader*);
void 			walkClassEntries(Collector *collector, Hjava_lang_ClassLoader*);

Collector* 		initCollector(void);

Hjava_lang_Class*	lookupClass(const char*, Hjava_lang_ClassLoader*,
				errorInfo*);
Hjava_lang_Class*	lookupArray(Hjava_lang_Class*, errorInfo*);
Hjava_lang_Class*	lookupObjectArrayClass(Hjava_lang_Class*);
Field*			lookupClassField(Hjava_lang_Class*, Utf8Const*, bool, errorInfo *einfo);

Hjava_lang_Class*	getClass(constIndex, Hjava_lang_Class*, errorInfo *einfo);

void			countInsAndOuts(const char*, short*, short*, char*);
int			sizeofSigChar(char, bool);
int			sizeofSigItem(const char**, bool);
int			sizeofSig(const char**, bool);
int			sizeofSigMethod(Method *, bool);
int			sizeofSigClass(Hjava_lang_Class*, bool);
void			establishMethod(Method*);
Hjava_lang_Class*	getClassFromSignature(const char*, Hjava_lang_ClassLoader*, errorInfo*);
Hjava_lang_Class*	getClassFromSignaturePart(const char*, Hjava_lang_ClassLoader*, errorInfo*);
int			countArgsInSignature(const char *);
parsed_signature_t*	parseSignature(Utf8Const *, errorInfo*);

int			startFields(Hjava_lang_Class*, u2 fieldct, errorInfo*);
void			finishFields(Hjava_lang_Class*);
Method*			findMethodFromPC(uintp);

void			destroyClassLoader(Collector *, void *);
struct Hjava_lang_String* resolveString(Hjava_lang_Class* clazz, int idx,
					errorInfo *einfo);
/*
 * Start a search for a class.  If no other thread is searching for this
 * mapping then the responsibility falls on the current thread.
 *
 * ce - The mapping to start searching for.
 * out_cl - A placeholder for the class if it has already been bound.
 * einfo - An uninitialized errorInfo object.
 * returns - True, if the class is already bound or if this thread should be
 *   responsible for searching/loading the class.  False, if searching for this
 *   class would result in a class circularity.
 */
int classMappingSearch(classEntry *ce,
		       Hjava_lang_Class **out_cl,
		       errorInfo *einfo);
/*
 * Start loading a class.
 *
 * ce - The mapping to start searching for.
 * out_cl - A placeholder for the class if it has already been bound.
 * einfo - An uninitialized errorInfo object.
 * returns - True, if the class is already bound or if this thread should be
 *   responsible for searching/loading the class.  False, if searching for this
 *   class would result in a class circularity.
 */
int classMappingLoad(classEntry *ce,
		     Hjava_lang_Class **out_cl,
		     errorInfo *einfo);
/*
 * Transition a mapping to the loaded state.
 *
 * ce - The name mapping whose state should be updated.
 * cl - The class object that should be bound to this mapping.
 * return - The value of "cl" if the mapping wasn't already updated, otherwise,
 *   it will be the value previously stored in the mapping.
 */
Hjava_lang_Class *classMappingLoaded(classEntry *ce, Hjava_lang_Class *cl);
/*
 * Force a mapping to a particular state.
 *
 * ce - The name mapping whose state should be updated.
 * nms - The state the mapping should be set to.
 */
void setClassMappingState(classEntry *ce, name_mapping_state_t nms);

void walkClassPool(int (*walker)(Hjava_lang_Class *clazz, void *), void *param);

extern Utf8Const* init_name;		/* "<clinit>" */
extern Utf8Const* constructor_name;	/* "<init>" */
extern Utf8Const* final_name;		/* "finalize" */
extern Utf8Const* void_signature;	/* "()V" */
extern Utf8Const* Code_name;		/* "Code" */
extern Utf8Const* LineNumberTable_name;	/* "LineNumberTable" */
extern Utf8Const* ConstantValue_name;	/* "ConstantValue" */
extern Utf8Const* Exceptions_name;	/* "Exceptions" */
extern Utf8Const* SourceFile_name;	/* "SourceFile" */
extern Utf8Const* InnerClasses_name;	/* "InnerClasses" */

#endif
