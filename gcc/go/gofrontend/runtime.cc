// runtime.cc -- runtime functions called by generated code

// Copyright 2011 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "go-system.h"

#include <gmp.h>

#include "gogo.h"
#include "types.h"
#include "expressions.h"
#include "runtime.h"

// The frontend generates calls to various runtime functions.  They
// are implemented in libgo/runtime.  This is how the runtime
// functions are represented in the frontend.  Note that there is
// currently nothing which ensures that the compiler's understanding
// of the runtime function matches the actual implementation in
// libgo/runtime.

// Parameter and result types used by runtime functions.

enum Runtime_function_type
{
  // General indicator that value is not used.
  RFT_VOID,
  // Go type bool, C type _Bool.
  RFT_BOOL,
  // Go type *bool, C type _Bool*.
  RFT_BOOLPTR,
  // Go type int, C type int.
  RFT_INT,
  // Go type int64, C type int64_t.
  RFT_INT64,
  // Go type uint64, C type uint64_t.
  RFT_UINT64,
  // Go type uintptr, C type uintptr_t.
  RFT_UINTPTR,
  // Go type float64, C type double.
  RFT_FLOAT64,
  // Go type complex128, C type __complex double.
  RFT_COMPLEX128,
  // Go type string, C type struct __go_string.
  RFT_STRING,
  // Go type unsafe.Pointer, C type "void *".
  RFT_POINTER,
  // Go type []any, C type struct __go_open_array.
  RFT_SLICE,
  // Go type map[any]any, C type struct __go_map *.
  RFT_MAP,
  // Pointer to map iteration type.
  RFT_MAPITER,
  // Go type chan any, C type struct __go_channel *.
  RFT_CHAN,
  // Go type *chan any, C type struct __go_channel **.
  RFT_CHANPTR,
  // Go type non-empty interface, C type struct __go_interface.
  RFT_IFACE,
  // Go type interface{}, C type struct __go_empty_interface.
  RFT_EFACE,
  // Go type func(unsafe.Pointer), C type void (*) (void *).
  RFT_FUNC_PTR,
  // Pointer to Go type descriptor.
  RFT_TYPE,

  NUMBER_OF_RUNTIME_FUNCTION_TYPES
};

// The Type structures for the runtime function types.

static Type* runtime_function_types[NUMBER_OF_RUNTIME_FUNCTION_TYPES];

// Get the Type for a Runtime_function_type code.

static Type*
runtime_function_type(Runtime_function_type bft)
{
  gcc_assert(bft < NUMBER_OF_RUNTIME_FUNCTION_TYPES);
  if (runtime_function_types[bft] == NULL)
    {
      const source_location bloc = BUILTINS_LOCATION;
      Type* t;
      switch (bft)
	{
	default:
	case RFT_VOID:
	  gcc_unreachable();

	case RFT_BOOL:
	  t = Type::lookup_bool_type();
	  break;

	case RFT_BOOLPTR:
	  t = Type::make_pointer_type(Type::lookup_bool_type());
	  break;

	case RFT_INT:
	  t = Type::lookup_integer_type("int");
	  break;

	case RFT_INT64:
	  t = Type::lookup_integer_type("int64");
	  break;

	case RFT_UINT64:
	  t = Type::lookup_integer_type("uint64");
	  break;

	case RFT_UINTPTR:
	  t = Type::lookup_integer_type("uintptr");
	  break;

	case RFT_FLOAT64:
	  t = Type::lookup_float_type("float64");
	  break;

	case RFT_COMPLEX128:
	  t = Type::lookup_complex_type("complex128");
	  break;

	case RFT_STRING:
	  t = Type::lookup_string_type();
	  break;

	case RFT_POINTER:
	  t = Type::make_pointer_type(Type::make_void_type());
	  break;

	case RFT_SLICE:
	  t = Type::make_array_type(Type::make_void_type(), NULL);
	  break;

	case RFT_MAP:
	  t = Type::make_map_type(Type::make_void_type(),
				  Type::make_void_type(),
				  bloc);
	  break;

	case RFT_MAPITER:
	  t = Type::make_pointer_type(Runtime::map_iteration_type());
	  break;

	case RFT_CHAN:
	  t = Type::make_channel_type(true, true, Type::make_void_type());
	  break;

	case RFT_CHANPTR:
	  t = Type::make_pointer_type(runtime_function_type(RFT_CHAN));
	  break;

	case RFT_IFACE:
	  {
	    Typed_identifier_list* methods = new Typed_identifier_list();
	    Type* mtype = Type::make_function_type(NULL, NULL, NULL, bloc);
	    methods->push_back(Typed_identifier("x", mtype, bloc));
	    t = Type::make_interface_type(methods, bloc);
	  }
	  break;

	case RFT_EFACE:
	  t = Type::make_interface_type(NULL, bloc);
	  break;

	case RFT_FUNC_PTR:
	  {
	    Typed_identifier_list* param_types = new Typed_identifier_list();
	    Type* ptrtype = runtime_function_type(RFT_POINTER);
	    param_types->push_back(Typed_identifier("", ptrtype, bloc));
	    t = Type::make_function_type(NULL, param_types, NULL, bloc);
	  }
	  break;

	case RFT_TYPE:
	  t = Type::make_type_descriptor_ptr_type();
	  break;
	}

      runtime_function_types[bft] = t;
    }

  return runtime_function_types[bft];
}

// Convert an expression to the type to pass to a runtime function.

static Expression*
convert_to_runtime_function_type(Runtime_function_type bft, Expression* e,
				 source_location loc)
{
  switch (bft)
    {
    default:
    case RFT_VOID:
      gcc_unreachable();

    case RFT_BOOL:
    case RFT_BOOLPTR:
    case RFT_INT:
    case RFT_INT64:
    case RFT_UINT64:
    case RFT_UINTPTR:
    case RFT_FLOAT64:
    case RFT_COMPLEX128:
    case RFT_STRING:
    case RFT_POINTER:
    case RFT_MAPITER:
    case RFT_FUNC_PTR:
      {
	Type* t = runtime_function_type(bft);
	if (!Type::are_identical(t, e->type(), true, NULL))
	  e = Expression::make_cast(t, e, loc);
	return e;
      }

    case RFT_SLICE:
    case RFT_MAP:
    case RFT_CHAN:
    case RFT_CHANPTR:
    case RFT_IFACE:
    case RFT_EFACE:
      return Expression::make_unsafe_cast(runtime_function_type(bft), e, loc);

    case RFT_TYPE:
      gcc_assert(e->type() == Type::make_type_descriptor_ptr_type());
      return e;
    }
}

// Convert all the types used for runtime functions to the backend
// representation.

void
Runtime::convert_types(Gogo* gogo)
{
  for (int i = 0; i < static_cast<int>(NUMBER_OF_RUNTIME_FUNCTION_TYPES); ++i)
    {
      Type* t = runtime_function_types[i];
      if (t != NULL && t->named_type() != NULL)
	{
	  bool r = t->verify();
	  gcc_assert(r);
	  t->named_type()->convert(gogo);
	}
    }
}

// The type used to define a runtime function.

struct Runtime_function
{
  // Function name.
  const char* name;
  // Parameter types.  Never more than 6, as it happens.  RFT_VOID if
  // not used.
  Runtime_function_type parameter_types[6];
  // Result types.  Never more than 2, as it happens.  RFT_VOID if not
  // used.
  Runtime_function_type result_types[2];
};

static const Runtime_function runtime_functions[] =
{

#define DEF_GO_RUNTIME(CODE, NAME, PARAMS, RESULTS) { NAME, PARAMS, RESULTS } ,

#include "runtime.def"

#undef DEF_GO_RUNTIME

};

static Named_object*
runtime_function_declarations[Runtime::NUMBER_OF_FUNCTIONS];

// Get the declaration of a runtime function.

Named_object*
Runtime::runtime_declaration(Function code)
{
  gcc_assert(code < Runtime::NUMBER_OF_FUNCTIONS);
  if (runtime_function_declarations[code] == NULL)
    {
      const Runtime_function* pb = &runtime_functions[code];

      source_location bloc = BUILTINS_LOCATION;

      Typed_identifier_list* param_types = NULL;
      if (pb->parameter_types[0] != RFT_VOID)
	{
	  param_types = new Typed_identifier_list();
	  for (unsigned int i = 0;
	       i < (sizeof(pb->parameter_types)
		    / sizeof (pb->parameter_types[0]));
	       i++)
	    {
	      if (pb->parameter_types[i] == RFT_VOID)
		break;
	      Type* t = runtime_function_type(pb->parameter_types[i]);
	      param_types->push_back(Typed_identifier("", t, bloc));
	    }
	}

      Typed_identifier_list* result_types = NULL;
      if (pb->result_types[0] != RFT_VOID)
	{
	  result_types = new Typed_identifier_list();
	  for (unsigned int i = 0;
	       i < sizeof(pb->result_types) / sizeof(pb->result_types[0]);
	       i++)
	    {
	      if (pb->result_types[i] == RFT_VOID)
		break;
	      Type* t = runtime_function_type(pb->result_types[i]);
	      result_types->push_back(Typed_identifier("", t, bloc));
	    }
	}

      Function_type* fntype = Type::make_function_type(NULL, param_types,
						       result_types, bloc);
      const char* n = pb->name;
      const char* n1 = strchr(n, '.');
      if (n1 != NULL)
	n = n1 + 1;
      Named_object* no = Named_object::make_function_declaration(n, NULL,
								 fntype, bloc);
      no->func_declaration_value()->set_asm_name(pb->name);

      runtime_function_declarations[code] = no;
    }

  return runtime_function_declarations[code];
}

// Make a call to a runtime function.

Call_expression*
Runtime::make_call(Runtime::Function code, source_location loc,
		   int param_count, ...)
{
  gcc_assert(code < Runtime::NUMBER_OF_FUNCTIONS);

  const Runtime_function* pb = &runtime_functions[code];

  gcc_assert(static_cast<size_t>(param_count)
	     <= sizeof(pb->parameter_types) / sizeof(pb->parameter_types[0]));

  Named_object* no = runtime_declaration(code);
  Expression* func = Expression::make_func_reference(no, NULL, loc);

  Expression_list* args = new Expression_list();
  args->reserve(param_count);

  va_list ap;
  va_start(ap, param_count);
  for (int i = 0; i < param_count; ++i)
    {
      Expression* e = va_arg(ap, Expression*);
      Runtime_function_type rft = pb->parameter_types[i];
      args->push_back(convert_to_runtime_function_type(rft, e, loc));
    }
  va_end(ap);

  return Expression::make_call(func, args, false, loc);
}

// The type we use for a map iteration.  This is really a struct which
// is four pointers long.  This must match the runtime struct
// __go_hash_iter.

Type*
Runtime::map_iteration_type()
{
  const unsigned long map_iteration_size = 4;

  mpz_t ival;
  mpz_init_set_ui(ival, map_iteration_size);
  Expression* iexpr = Expression::make_integer(&ival, NULL, BUILTINS_LOCATION);
  mpz_clear(ival);

  return Type::make_array_type(runtime_function_type(RFT_POINTER), iexpr);
}

// Return the type used to pass a list of general channels to the
// select runtime function.

Type*
Runtime::chanptr_type()
{
  return runtime_function_type(RFT_CHANPTR);
}
