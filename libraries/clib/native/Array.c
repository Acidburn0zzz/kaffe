/*     
 * java.lang.reflect.Array.c
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#include "config.h"
#include "config-std.h"
#include "config-mem.h"
#include "../../../kaffe/kaffevm/object.h"
#include "../../../kaffe/kaffevm/classMethod.h"
#include "../../../kaffe/kaffevm/baseClasses.h"
#include "../../../kaffe/kaffevm/itypes.h"
#include "../../../kaffe/kaffevm/soft.h"
#include "../../../kaffe/kaffevm/exception.h"
#include "java_lang_reflect_Array.h"
#include "java_lang_Boolean.h"
#include "java_lang_Byte.h"
#include "java_lang_Character.h"
#include "java_lang_Short.h"
#include "java_lang_Integer.h"
#include "java_lang_Long.h"
#include "java_lang_Float.h"
#include "java_lang_Double.h"
#include <native.h>
#include "defs.h"

jint
java_lang_reflect_Array_getLength(struct Hjava_lang_Object* obj)
{
	if (!CLASS_IS_ARRAY(OBJECT_CLASS(obj)))
		SignalError("java.lang.IllegalArgumentException", "");

	return (obj_length((HArrayOfObject*)obj));
}

struct Hjava_lang_Object*
java_lang_reflect_Array_newArray(struct Hjava_lang_Class* clazz, jint size)
{
	if (size < 0) {
		SignalError("java.lang.NegativeArraySizeException", "");
	} else {
		return (newArray(clazz, (size_t) size));
	}
}

