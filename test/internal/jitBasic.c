/*
 * jitBasic.c
 *
 * Copyright (c) 2003 University of Utah and the Flux Group.
 * All rights reserved.
 *
 * This file is licensed under the terms of the GNU Public License.
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * Contributed by the Flux Research Group, Department of Computer Science,
 * University of Utah, http://www.cs.utah.edu/flux/
 */

#include "config.h"
#include "object.h"
#include "support.h"
#include "utf8const.h"
#include "classMethod.h"
#include "stringParsing.h"

#if !defined(TRANSLATOR)

int internal_test(char *class_name)
{
	return( 0 );
}

#else

#include <machine.h>
#include <slots.h>
#include <seq.h>
#include <labels.h>
#include <codeproto.h>
#include <basecode.h>

Method *findMethodNoSig(Hjava_lang_Class *cl, Utf8Const *name)
{
	Method *retval = 0;
	int lpc;

	assert(cl != 0);
	assert(name != 0);

	for( lpc = 0; (lpc < CLASS_NMETHODS(cl)) && !retval; lpc++ )
	{
		if( CLASS_METHODS(cl)[lpc].name == name )
			retval = &CLASS_METHODS(cl)[lpc];
	}

	assert(retval != 0);
	
	return( retval );
}

#define MAX_TEST_FIELD_COMPONENTS 64

struct testField {
	char *tf_Name;
	int tf_ComponentCount;
	int tf_MethodIndex;
	parsedString tf_Components[MAX_TEST_FIELD_COMPONENTS];
};

static
int fieldComponentHandler(struct testField *tf)
{
	int retval = 1;

	tf->tf_ComponentCount += 1;
	tf->tf_Components[tf->tf_ComponentCount] = tf->tf_Components[0];
	if( tf->tf_Components[0].len == 0 )
		tf->tf_MethodIndex = tf->tf_ComponentCount + 1;
	return( retval );
}

int parseTestField(struct testField *tf)
{
	return parseString(
		tf->tf_Name,
		SPO_NotEmpty,
		  SPO_Do,
		    SPO_String, &tf->tf_Components[0],
		    SPO_While, "_", "",
		    SPO_Handle, fieldComponentHandler, tf,
		    SPO_End,
		  SPO_End,
		SPO_End);
}

int field2values(jvalue *dst, parsed_signature_t *ps, struct testField *tf)
{
	int lpc, retval = 1;

	for( lpc = 1; (lpc <= ps->nargs) && retval; lpc++ )
	{
		parsedString *arg;
		char *str;

		arg = &tf->tf_Components[lpc + 1];
		switch( ps->signature->data[ps->ret_and_args[lpc]] )
		{
		case 'Z':
			dst[lpc - 1].z = !cmpPStrStr(arg, "true");
			break;
		case 'B':
			dst[lpc - 1].b = (char)strtol(arg->data, 0, 0);
			break;
		case 'C':
			dst[lpc - 1].c = arg->data[0];
			break;
		case 'S':
			dst[lpc - 1].s = (short)strtol(arg->data, 0, 0);
			break;
		case 'I':
			dst[lpc - 1].i = strtoul(arg->data, 0, 0);
			break;
		case 'J':
			dst[lpc - 1].j = (long long)strtouq(arg->data, 0, 0);
			break;
		case 'D':
		case 'F':
			if( (str = promoteParsedString(arg)) )
			{
				double value;
				char *sep;
				
				if( (sep = strchr(str, 'd')) )
				{
					*sep = '.';
				}
				value = strtod(str, 0);
				if( ps->signature->data[ps->ret_and_args[lpc]]
				    == 'D' )
				{
					dst[lpc - 1].d = value;
				}
				else
				{
					dst[lpc - 1].f = (float)value;
				}
				gc_free(str);
			}
			else
			{
				retval = 0;
			}
			break;
		default:
			assert(0);
			break;
		}
	}
	return( retval );
}

int testMethod(Hjava_lang_Class *cl, Field *field)
{
	struct testField tf;
	errorInfo einfo;
	int retval = 0;

	tf.tf_Name = (char *)field->name->data;
	tf.tf_ComponentCount = 0;
	if( parseTestField(&tf) )
	{
		Utf8Const *utf;
		Method *meth;
		
		utf = utf8ConstNew(tf.tf_Components[tf.tf_MethodIndex].data,
				   -1);
		meth = findMethodNoSig(cl, utf);
		if( translate(meth, &einfo) )
		{
			jvalue args[MAX_TEST_FIELD_COMPONENTS];
			void *methblock;
			jvalue rc;

			field2values(args, METHOD_PSIG(meth), &tf);
			methblock = GC_getObjectBase(main_collector,
						     METHOD_NATIVECODE(meth));
			callMethodA(meth,
				    METHOD_NATIVECODE(meth),
				    0,
				    args,
				    &rc,
				    0);
			if( !memcmp(&rc, field->info.addr, field->bsize) )
			{
				kaffe_dprintf("Success %08x\n", rc.i);
				retval = 1;
			}
			else
			{
				kaffe_dprintf("Failure %08x %f\n", rc.i, rc.d);
			}
		}
		else
		{
			assert(0);
		}
	}
	return( retval );
}

int internal_test(parsedString *ps)
{
	char *class_name;
	int retval = 0;

	if( (class_name = promoteParsedString(ps)) )
	{
		Hjava_lang_Class *cl;
		errorInfo einfo;
		int lpc;
		
		retval = 1;
		loadStaticClass(&cl, class_name);
		processClass(cl, CSTATE_COMPLETE, &einfo);
		kaffe_dprintf("class: %s\n", cl->name->data);
		for( lpc = 0; (lpc < CLASS_NSFIELDS(cl)) && retval; lpc++ )
		{
			Field *field;
			
			field = &CLASS_SFIELDS(cl)[lpc];
			kaffe_dprintf("  field: %s = 0x%08x\n",
				      field->name->data,
				      ((int *)field->info.addr)[0]);
			if( !strncmp("test_", field->name->data, 5) )
			{
				retval = testMethod(cl, field);
			}
		}
		gc_free(class_name);
	}
	return( retval );
}

#endif
