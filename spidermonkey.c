#define JS_C_STRINGS_ARE_UTF8

#include <ruby.h>

//#define DEBUG

#if defined NEED_MOZJS_PREFIX
#  include <mozjs/jsapi.h>
#  include <mozjs/jshash.h>
#  include <mozjs/jsobj.h>
#elif defined NEED_SMJS_PREFIX
#  include <smjs/jsapi.h>
#  include <smjs/jshash.h>
#  include <smjs/jsobj.h>
#else
#  include <jsapi.h>
#  include <jshash.h>
#  include <jsobj.h>
#endif

#define BRANCH_GC 0x1000

// Default stack size
#define JS_STACK_CHUNK_SIZE    16384

#define JS_RUNTIME_MAXBYTES   0x4000000L

#define RBSMJS_DEFAULT_CONTEXT "@@defaultContext"
#define RBSMJS_VALUES_CONTEXT "@context"
#define RBSMJS_CONTEXT_GLOBAL "global"
#define RBSMJS_CONTEXT_BINDINGS "bindings"
#define RBSMJS_RUBY_TO_JS_MAP "$mozjs_mapping"

#define RBSM_CONVERT_SHALLOW 1
#define RBSM_CONVERT_DEEP    0


#define ATTEMPT_GC(cx) do { JS_MaybeGC(cx); } while (0)


#ifdef WIN32
# ifdef DEBUG
#  define trace printf("\n"); printf
# else
#  define trace(msg) 
# endif
#elif defined DEBUG
# define trace(format, ...) do { printf(format, ## __VA_ARGS__); printf("\n"); } while (0)
#else
# define trace(format, ...) 
#endif


static const char*
rbsm_GetValueStringBytes( JSContext* cx, jsval value ){
  JSString* str;
  const char* bytes;
  JS_AddNamedRoot( cx, &value, "rbsm_GetValueStringBytes" );
  str = JS_ValueToString( cx, value );
  if( !str ){
    JS_RemoveRoot( cx, &value );
    return NULL;
  }
  trace("GetValueStringBytes(%x): %d bytes", value, JS_GetStringLength( str ));
  bytes = JS_GetStringBytes( str );
  JS_RemoveRoot( cx, &value );
  return bytes;
}

#define ZERO_ARITY_METHOD_IS_PROPERTY

VALUE rb_cDate;

VALUE eJSError;
VALUE eJSConvertError;
VALUE eJSEvalError;
VALUE cJSValue;
VALUE cJSContext;
VALUE cJSFunction;
VALUE cSMJS;
JSRuntime* gSMJS_runtime;

#ifdef BRANCH_GC
unsigned long gSMJS_counter;
#endif

#ifdef DEBUG
int alloc_count_js2rb;
int alloc_count_rb2js;
#endif

// RubyObject/RubyFunction Properties
typedef struct{
  VALUE rbobj;
  jsval jsv;
}sSMJS_Class;

// RubyException Error
typedef struct{
  int status;
  jsval erval;
  VALUE errinfo;
}sSMJS_Error;

// SpiderMonkey::Context -- Structure containing instance data
typedef struct{
  JSContext* cx;
  jsval last_exception; // Last JS Exception
  char last_message[BUFSIZ]; // Last Error Message
  JSObject* store; // "JS Store for the pair-GC"
  JSHashTable* id2rbval; // JS-VALUE for Ruby-Object#__id__
}sSMJS_Context;

// SpiderMonkey::Value -- Structure containing instance data
typedef struct{
  jsval value;
  sSMJS_Context* cs;
}sSMJS_Value;

// Data required for yield methods
typedef struct{
  JSContext* cx;
  JSObject* obj;
  jsint id;
  jsval val;
  jsval key;
  JSIdArray* ida;
  int i;
  void* data;
  char* keystr;
}sSMJS_Enumdata;

typedef void(* RBSMJS_YIELD)( sSMJS_Enumdata* enm );
typedef VALUE(* RBSMJS_Convert)( JSContext* cx, jsval val );

static VALUE RBSMContext_FROM_JsContext( JSContext* cx );
#ifdef BRANCH_GC
static JSBool rbsm_branch_callback( JSContext* cx, JSScript* script );
#endif
static JSBool rbsm_class_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static JSBool rbsm_class_get_temp_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static JSBool rbsm_class_set_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static JSBool rbsm_resolve( JSContext* cx, JSObject *obj, jsval id, uintN flags, JSObject **objp );
static int rbsm_check_ruby_property(JSContext* cx, JSObject* obj, jsval id);
static JSBool rbsm_error_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static void rbsm_class_finalize ( JSContext* cx, JSObject* obj );
static JSBool rb_smjs_value_object_callback( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval );
static JSBool rb_smjs_value_function_callback( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval );
static JSObject* rbsm_proc_to_function( JSContext* cx, VALUE proc );
static void rbsm_rberror_finalize ( JSContext* cx, JSObject* obj );
static JSObject* rbsm_ruby_to_jsobject( JSContext* cx, VALUE obj );
static JSBool rbsm_rubyexception_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval );
static JSBool rbsm_class_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval );
static JSBool rbsm_class_no_such_method( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval );
static VALUE rb_smjs_convertvalue( JSContext* cx, jsval value );
static VALUE rb_smjs_convert_prim( JSContext* cx, jsval value );
static JSBool rbsm_rubystring_to_jsval( JSContext* cx, VALUE rval, jsval* jval );
static JSBool rbsm_get_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj );
static VALUE rbsm_evalerror_new( VALUE context, VALUE erval );
static VALUE rbsm_evalerror_new_jsval( VALUE context, jsval jsvalerror );
static VALUE rb_smjs_value_new_jsval( VALUE context, jsval value );
static VALUE rb_smjs_value_get_prototype( VALUE self );
static void* rbsm_each( JSContext* cx, jsval value, RBSMJS_YIELD yield, void* data );
static JSObjectOps* rbsm_class_get_object_ops( JSContext* cx, JSClass* clasp );
static void rb_smjs_context_errorhandle( JSContext* cx, const char* message, JSErrorReport* report );
static VALUE rb_smjs_context_flush( VALUE self );
static VALUE rb_smjs_value_call_with_this(int argc, VALUE* rargv, VALUE self);

typedef enum RBSMErrNum{
#define MSG_DEF( name, number, count, exception, format ) \
    name = number,
#include "rbsm.msg"
#undef MSG_DEF
    RBSMErr_Limit
#undef MSGDEF
} RBSMErrNum;

JSErrorFormatString rbsm_ErrorFormatString[RBSMErr_Limit] = {
#define MSG_DEF( name, number, count, exception, format ) { format, count },
#include "rbsm.msg"
#undef MSG_DEF
  {0, 0}
};

#ifndef JSCLASS_GLOBAL_FLAGS
#define JSCLASS_GLOBAL_FLAGS 0
#endif

static JSClass global_class = {
  "global", JSCLASS_NEW_RESOLVE | JSCLASS_GLOBAL_FLAGS,
  JS_PropertyStub,  JS_PropertyStub,
  JS_PropertyStub,  JS_PropertyStub,
  JS_EnumerateStub, JS_ResolveStub,
  JS_ConvertStub,   JS_FinalizeStub,
JSCLASS_NO_OPTIONAL_MEMBERS     
};

static JSClass JSRubyObjectClass = {
  "RubyObject", JSCLASS_HAS_PRIVATE | JSCLASS_NEW_RESOLVE,
  /*addp*/JS_PropertyStub,  /*delpr*/JS_PropertyStub,
  /*getp*/rbsm_class_get_property,   /*setp*/rbsm_class_set_property,
  JS_EnumerateStub, (JSResolveOp)rbsm_resolve, JS_ConvertStub, rbsm_class_finalize,
  /* getObjectOps */NULL, /*checkAccess */NULL, /*call*/rb_smjs_value_object_callback,
  /* construct*/NULL, /* xdrObject*/NULL, /* hasInstance*/NULL, /* mark*/NULL,
  /* spare*/0
};

static JSClass JSRubyFunctionClass = {
  "RubyFunction", JSCLASS_HAS_PRIVATE,
  /*addp*/JS_PropertyStub,  /*delpr*/JS_PropertyStub,
  /*getp*/rbsm_class_get_property,   /*setp*/JS_PropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, rbsm_class_finalize,
  /* getObjectOps */rbsm_class_get_object_ops, /*checkAccess */NULL, /*call*/rb_smjs_value_function_callback,
  /* construct*/NULL, /* xdrObject*/NULL, /* hasInstance*/NULL, /* mark*/NULL,
  /* spare*/0
};
static JSFunctionSpec JSRubyObjectFunctions[] = {
  {"__noSuchMethod__", rbsm_class_no_such_method, 2},
  {"toString", rbsm_class_to_string, 0},
  {0}
};
static JSFunctionSpec JSRubyExceptionFunctions[] = {
  {"toString", rbsm_rubyexception_to_string, 0},
  {0}
};

static JSClass JSRubyExceptionClass = {
  "RubyException", JSCLASS_HAS_PRIVATE,
  /*addp*/JS_PropertyStub,  /*delpr*/JS_PropertyStub,
  /*getp*/rbsm_error_get_property,   /*setp*/JS_PropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, rbsm_rberror_finalize,
  JSCLASS_NO_OPTIONAL_MEMBERS
};

static JSObjectOps rbsm_FunctionOps;

// convert -------------------------------------------------------
// Convert a JS object to a Ruby String
static VALUE 
rb_smjs_to_s( JSContext* cx, jsval value ){
  VALUE out;
  JSString* str;
  JS_AddNamedRoot( cx, &value, "rb_smjs_to_s:value" );
  str = JS_ValueToString( cx, value );
  JS_RemoveRoot( cx, &value );
  if( !str ){
    rb_raise( eJSConvertError, "can't convert to string" );
  }
  out = rb_str_new( JS_GetStringBytes( str ), JS_GetStringLength( str ) );
  return out;
}

// Convert a JS object to a Ruby Boolean
static VALUE 
rb_smjs_to_bool( JSContext* cx, jsval value ){
  JSBool bp;
  JS_AddNamedRoot( cx, &value, "rb_smjs_to_bool" );
  if( !JS_ValueToBoolean( cx, value, &bp ) ){
    JS_RemoveRoot( cx, &value );
    rb_raise( eJSConvertError, "can't convert to boolean" );
  }
  JS_RemoveRoot( cx, &value );
  return bp ? Qtrue : Qfalse;
}

// Convert a JS object to a Ruby Array
static VALUE 
rb_smjs_to_a( JSContext* cx, jsval value, int shallow ){
  VALUE ary;
  VALUE r;
  if( JSVAL_IS_OBJECT( value ) ){
    JSObject* jo = JSVAL_TO_OBJECT( value );
    if( JSVAL_IS_NULL( value ) ) return rb_ary_new( );
    JS_AddNamedRoot( cx, &jo, "rb_smjs_to_a" );
    if( JS_IsArrayObject( cx, jo ) ){
      jsuint length;
      if( JS_HasArrayLength( cx, jo, &length ) ){
        jsuint i;
        trace("Converting JS array to Ruby (%d elements)", length);
        ary = rb_ary_new2( length );
        for( i = 0 ; i < length ; i++ ){
          jsval v;
          if( JS_GetElement( cx, jo, i, &v ) ){
            VALUE va, insp;
            if( shallow ){
              va = rb_smjs_convert_prim( cx, v );
            }else{
              va = rb_smjs_convertvalue( cx, v ); 
            }
#ifdef DEBUG
            insp = rb_inspect(va);
            trace(" element[%d]: %s", i, StringValuePtr(insp));
#endif
            rb_ary_store( ary, i, va );
          }else{
            JS_RemoveRoot( cx, &jo );
            rb_raise( eJSConvertError, "Fail convert to array[]" );
          }
        }
        JS_RemoveRoot( cx, &jo );
        return ary;
      }
    }
    JS_RemoveRoot( cx, &jo );
  }
  r = rb_smjs_convertvalue( cx, value );
  return rb_funcall( r, rb_intern( "to_a" ), 0 );
}
typedef struct{
  VALUE value;
  int shallow;
}sRBSM_toHash;

static void
rb_smjs_to_h_yield( sSMJS_Enumdata* enm ){
  sRBSM_toHash* dat;
  VALUE val;
  dat = (sRBSM_toHash*)(enm->data);
  if( dat->shallow ){
    val = rb_smjs_convertvalue( enm->cx, enm->val );
  }else{
    val = rb_smjs_convert_prim( enm->cx, enm->val );
  }
  rb_hash_aset( dat->value, rb_smjs_convertvalue( enm->cx, enm->key ), val );
}

static VALUE 
rb_smjs_to_h( JSContext* cx, jsval value, int shallow ){
  sRBSM_toHash edat;
  VALUE ret = rb_hash_new( );
  if( !JSVAL_IS_OBJECT( value )  ){
    rb_raise( eJSConvertError, "Value is not an object" );
  }else{
    if( JSVAL_IS_NULL( value ) ){
      return ret;
    }
  }
  edat.shallow = shallow;
  edat.value = ret;
  rbsm_each( cx, value, rb_smjs_to_h_yield, (void*)&edat );
  return edat.value;
}

static VALUE 
rb_smjs_to_i( JSContext* cx, jsval value ){
  jsint ip;
  JS_AddNamedRoot( cx, &value, "rb_smjs_to_i" );
  if( JS_ValueToInt32( cx, value, &ip ) ){
    JS_RemoveRoot( cx, &value );
    return INT2NUM( ip );
  }else{
    JS_RemoveRoot( cx, &value );
    rb_raise( eJSConvertError, "Failed to convert value to integer" );
  }
}

static VALUE 
rb_smjs_to_f( JSContext* cx, jsval value ){
  jsdouble d;
  JS_AddNamedRoot( cx, &value, "rb_smjs_to_f" );
  if( JS_ValueToNumber( cx, value, &d ) ){
    JS_RemoveRoot( cx, &value );
    return rb_float_new( d );
  }else{
    JS_RemoveRoot( cx, &value );
    rb_raise( eJSConvertError, "Failed to convert value to float" );
  }
}

static VALUE 
rb_smjs_to_num( JSContext* cx, jsval value ){
  if( JSVAL_IS_INT( value ) ){
    return rb_smjs_to_i( cx, value );
  }else if( JSVAL_IS_DOUBLE( value ) ){
    return rb_smjs_to_f( cx, value );
  }else{
    rb_raise( eJSConvertError, "Value is not a number" );
  }
}

static VALUE 
rb_smjs_convertvalue( JSContext* cx, jsval value ){
  JSType t;

  JS_AddNamedRoot( cx, &value, "rb_smjs_convertvalue" );
  t = JS_TypeOfValue( cx, value );
  JS_RemoveRoot( cx, &value );

  switch( t ){
  case JSTYPE_VOID: return Qnil;
  case JSTYPE_STRING:  return rb_smjs_to_s( cx, value );
  case JSTYPE_BOOLEAN: return rb_smjs_to_bool( cx, value );
  case JSTYPE_OBJECT:
    if( JSVAL_IS_NULL( value ) ) return Qnil;
    {
      JSObject* jo = JSVAL_TO_OBJECT( value );
      sSMJS_Class* so;

      JS_AddNamedRoot( cx, &jo, "rb_smjs_convertvalue" );
      so = JS_GetInstancePrivate( cx, jo, &JSRubyObjectClass, NULL );
      if( so ){
        JS_RemoveRoot( cx, &jo );
        return so->rbobj;
      }
      if( JS_IsArrayObject( cx, JSVAL_TO_OBJECT( value ) ) ){
        JS_RemoveRoot( cx, &jo );
        return rb_smjs_to_a( cx, value, RBSM_CONVERT_DEEP );
      }
      JS_RemoveRoot( cx, &jo );
      return rb_smjs_to_h( cx, value, RBSM_CONVERT_DEEP );
    }
  case JSTYPE_NUMBER:
    return rb_smjs_to_num( cx, value );
  case JSTYPE_FUNCTION:
    rb_raise ( eJSConvertError, "Unsupported: cannot convert JavaScript function to Ruby" ); break;
  default:
    rb_raise ( eJSConvertError, "Unsupported object type" );
    break;
  }
}

// Ruby String to JavaScript String
static JSBool
rbsm_rubystring_to_jsval( JSContext* cx, VALUE rval, jsval* jval ){
  JSString* jsstr;
  if( (jsstr = JS_NewStringCopyZ( cx, StringValuePtr( rval ) ) ) ){
    *jval = STRING_TO_JSVAL( jsstr );
    return JS_TRUE;
  }
  return JS_FALSE;
}

// Ruby Object to JS Object
static JSBool
rb_smjs_ruby_to_js( JSContext* cx, VALUE rval, jsval* jval ){
  if( rb_obj_is_instance_of( rval, cJSValue ) ){
    sSMJS_Value* sv;
    Data_Get_Struct( rval, sSMJS_Value, sv ); 
    *jval = sv->value;
    return JS_TRUE;
  }
  switch( TYPE( rval ) ){
  case T_STRING: return rbsm_rubystring_to_jsval( cx, rval, jval );
  case T_FIXNUM:
    if( NUM2LONG( rval ) >= INT_MIN && NUM2LONG( rval ) <= INT_MAX && INT_FITS_IN_JSVAL( NUM2INT( rval ) ) ){
      *jval = INT_TO_JSVAL( NUM2INT( rval ) );
      break;
    }
  case T_FLOAT: 
  case T_BIGNUM: return JS_NewDoubleValue( cx, NUM2DBL( rval ), jval );
  case T_TRUE:  *jval = JSVAL_TRUE; break;
  case T_FALSE: *jval = JSVAL_FALSE; break;
  case T_NIL:   *jval = JSVAL_NULL; break;
  default:
    {
      JSObject* jo = rbsm_ruby_to_jsobject( cx, rval );
      if( ! jo ) return JS_FALSE;
      *jval = OBJECT_TO_JSVAL( jo );
    }
  }
  return JS_TRUE;
}

static VALUE 
rb_smjs_convert_prim( JSContext* cx, jsval value ){
  JSObject* jo;
  sSMJS_Class* so;
  JSType t;
  VALUE context;
  JS_AddNamedRoot( cx, &value, "rb_smjs_convert_prim" );
  t = JS_TypeOfValue( cx, value );
  JS_RemoveRoot( cx, &value );
  switch( t ){
  case JSTYPE_VOID:    return Qnil;
  case JSTYPE_STRING:  return rb_smjs_to_s( cx, value );
  case JSTYPE_BOOLEAN: return rb_smjs_to_bool( cx, value );
  case JSTYPE_NUMBER:  return rb_smjs_to_num( cx, value );
  case JSTYPE_OBJECT:
    if( JSVAL_IS_NULL( value ) ) return Qnil;
    jo = JSVAL_TO_OBJECT( value );
    JS_AddNamedRoot( cx, &jo, "rb_smjs_convert_prim" );
    so = JS_GetInstancePrivate( cx, jo, &JSRubyObjectClass, NULL );
    if( so ){
      JS_RemoveRoot( cx, &jo );
      return so->rbobj;
    }
    if( JS_IsArrayObject( cx, jo ) ){
      JS_RemoveRoot( cx, &jo );
      return rb_smjs_to_a( cx, value, RBSM_CONVERT_SHALLOW );
    }
    JS_RemoveRoot( cx, &jo );
  default:
    context = RBSMContext_FROM_JsContext( cx );
    return rb_smjs_value_new_jsval( context, value );
  }
}

// etc ------------------------------------------------------------------

static VALUE 
RBSMContext_FROM_JsContext( JSContext* cx ){
  return (VALUE)JS_GetContextPrivate( cx );
}

static VALUE
rb_smjs_context_get_global( VALUE self ){
  return rb_iv_get( self, RBSMJS_CONTEXT_GLOBAL );
}

// Convert a JavaScript Exception to Ruby Exception
static void
rb_smjs_raise_ruby( JSContext* cx ){
  sSMJS_Error* se;
  JSObject* jo;
  jsval jsvalerror = 0;
  VALUE context = RBSMContext_FROM_JsContext( cx );
  VALUE self;

  ATTEMPT_GC( cx );

  if( !(JS_IsExceptionPending( cx ) && JS_GetPendingException( cx, &jsvalerror ) ) ){
    sSMJS_Context* cs;
    char tmpmsg[BUFSIZ];

    Data_Get_Struct( context, sSMJS_Context, cs );
    jsvalerror = cs->last_exception;

    if( !jsvalerror ){
      rb_raise( eJSError, "No exception for error: %s", cs->last_message );
    }

    cs->last_exception = 0;
    strncpy( tmpmsg, cs->last_message, BUFSIZ );
    sprintf( cs->last_message, "Exception already passed to Ruby: %s", tmpmsg );
  }

  JS_AddNamedRoot( cx, &jsvalerror, "rb_smjs_raise_ruby" );

  JS_ClearPendingException( cx );

  if( JSVAL_IS_OBJECT( jsvalerror ) &&
    ( jo = JSVAL_TO_OBJECT( jsvalerror ) ) ) {

    // If it was originally a Ruby exception, we continue that.
    se = JS_GetInstancePrivate( cx, jo, &JSRubyExceptionClass, NULL );
    if( se ){
      int st = se->status;
      JS_RemoveRoot( cx, &jsvalerror );
      se->status = 0;
      rb_jump_tag( st );
    }
  }

  // If the exception originated with JavaScript, we build and throw an
  // EvalError to Ruby.
  self = rbsm_evalerror_new_jsval( context, jsvalerror );
  
  JS_RemoveRoot( cx, &jsvalerror );
  rb_exc_raise( self );
}

// Throw a Ruby exception as a JavaScript exception.
static JSBool
rb_smjs_raise_js( JSContext* cx, int status ){
  JSObject* jo;
  sSMJS_Error* se;
  sSMJS_Value* sv;
  jsval stack_string;
  VALUE rb_g;
  VALUE context;
  VALUE rb_e = rb_gv_get( "$!" );
  sSMJS_Context* cs;

  ATTEMPT_GC( cx );

  context = RBSMContext_FROM_JsContext( cx );
  Data_Get_Struct( context, sSMJS_Context, cs );

  trace("rb_smjs_raise_js(cx=%x, status=%x)", cx, status);

  jo = JS_NewObject( cx, &JSRubyExceptionClass, NULL, NULL );
  if( ! jo ) return JS_FALSE;

  JS_AddNamedRoot( cx, &jo, "rb_smjs_raise_js" );
  JS_DefineFunctions( cx, jo, JSRubyExceptionFunctions );

  rb_g = rb_iv_get( context, RBSMJS_CONTEXT_GLOBAL );
  Data_Get_Struct( rb_g, sSMJS_Value, sv );

  if( JS_CallFunctionName( cx, JSVAL_TO_OBJECT( sv->value ), "__getStack__", 0, NULL, &stack_string ) ){
    VALUE rb_stack, stack_list;
    JS_AddNamedRoot( cx, &stack_string, "rb_smjs_raise_js:stack_string" );
    rb_stack = rb_smjs_to_s( cx, stack_string );

    stack_list = rb_iv_get( rb_e, "@all_stacks" );
    if( !RTEST( stack_list ) ){
      stack_list = rb_ary_new();
      rb_iv_set( rb_e, "@all_stacks", stack_list );
    }

    JS_SetProperty( cx, jo, "stack", &stack_string );

    {
      VALUE new_entry = rb_ary_new();
      rb_ary_push( new_entry, ID2SYM( rb_intern( "js" ) ) );
      rb_ary_push( new_entry, rb_stack );

      rb_ary_push( stack_list, new_entry );
    }

    {
      VALUE new_entry = rb_ary_new();
      rb_ary_push( new_entry, ID2SYM( rb_intern( "ruby" ) ) );
      rb_ary_push( new_entry, rb_funcall( Qnil, rb_intern( "caller" ), 0) );

      rb_ary_push( stack_list, new_entry );
    }

    JS_RemoveRoot( cx, &stack_string );
  } else {
    trace("Failure calling __getStack__!");
  }

  se = JS_malloc( cx, sizeof( sSMJS_Error ) );
  se->status = status;
  se->errinfo = rb_obj_dup( rb_e );
  JS_SetPrivate( cx, jo, (void*)se );

  cs->last_exception = OBJECT_TO_JSVAL( jo );
  JS_SetPendingException( cx, cs->last_exception );

  JS_RemoveRoot( cx, &jo );

  return JS_FALSE;
}

static const JSErrorFormatString*
rbsm_GetErrorMessage( void* userRef, const char* locale, const uintN errorNumber ){
  return &rbsm_ErrorFormatString[errorNumber];
}

// Convert a Ruby Exception to a JS String
static JSBool 
rbsm_rubyexception_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval ){
  sSMJS_Error* se;
  VALUE msg;
  se = JS_GetInstancePrivate( cx, obj, &JSRubyExceptionClass, NULL );
  if( !se ){ 
    // TODO: The function name should be the Object name
    JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyException", "toString", "Object" );
    return JS_FALSE;
  }
  msg = rb_funcall( se->errinfo, rb_intern( "to_s" ), 0 );
  rbsm_rubystring_to_jsval( cx, msg, rval );
  return JS_TRUE;
}

// jsval -> RubyObject Hash
static JSHashNumber
jsid2hash( const void* key ){
  return (JSHashNumber)key;
}
static intN 
jsidCompare( const void* v1, const void* v2 ){
  return (jsid)v1 == (jsid)v2;
}

static intN 
rbobjCompare( const void* v1, const void* v2 ){
  return (VALUE)v1 == (VALUE)v2;
}

static void
rbsm_set_jsval_to_rbval( sSMJS_Context* cs, VALUE self, jsval value ){
  char pname[10];
  JSHashEntry* x = JS_HashTableAdd( cs->id2rbval, (const void*)value, (void*)rb_obj_id( self ) );
  if( !x ) rb_raise( eJSError, "fail to set rbval to HashTable" );

  // Setting value to the one for JS-GC measure store
  sprintf( pname, "%x", (int)value );
  JS_SetProperty( cs->cx, cs->store, pname, &value );
}

static VALUE
rbsm_get_jsval_to_rbval( sSMJS_Context* cs, jsval value ){
  VALUE rid = (VALUE)JS_HashTableLookup( cs->id2rbval, (const void*)value );
  return rb_funcall( rb_const_get( rb_cObject, rb_intern( "ObjectSpace" ) ), rb_intern( "_id2ref" ), 1, rid );
}

static JSBool
rbsm_lookup_jsval_to_rbval( sSMJS_Context* cs, jsid value ){
  return NULL != JS_HashTableLookup( cs->id2rbval, (const void*)value );
}

static JSBool
rbsm_remove_jsval_to_rbval( sSMJS_Context* cs, jsval value ){
  char pname[10];
  
  sprintf( pname, "%x", (int)value );
  JS_DeleteProperty( cs->cx, cs->store, pname );
  return JS_HashTableRemove( cs->id2rbval, (const void*)value );
}

static JSContext* 
RBSMContext_TO_JsContext( VALUE context ){
  sSMJS_Context* cs;
  
  Data_Get_Struct( context, sSMJS_Context, cs );
  return cs->cx;
}

#ifdef BRANCH_GC
static JSBool
rbsm_branch_callback( JSContext* cx, JSScript* script ){
  if( ++gSMJS_counter % BRANCH_GC == 0 )
    JS_MaybeGC( cx );
  return JS_TRUE;
}
#endif

static jsval 
rb_smjs_evalscript( sSMJS_Context* cs, JSObject* obj, int argc, VALUE* argv ){
  char* source;
  char* filename;
  int lineno;
  jsval value;
  JSBool ok;
  VALUE code;
  VALUE file;
  VALUE line;

  rb_scan_args( argc, argv, "12", &code, &file, &line );

  SafeStringValue( code );
  source = StringValuePtr( code );

  if (NIL_P(file)) {
    filename = NULL;
  } else {
    SafeStringValue( file );
    filename = StringValuePtr( file );
  }

  if (NIL_P(line))
    lineno = 1;
  else
    lineno = NUM2INT( line );

  //cs->last_exception = 0;
  ok = JS_EvaluateScript( cs->cx, obj, source, strlen( source ), filename, lineno, &value );
  if( !ok ) rb_smjs_raise_ruby( cs->cx );

  JS_AddNamedRoot( cs->cx, &value, "rb_smjs_evalscript" );
  ATTEMPT_GC( cs->cx );
  JS_RemoveRoot( cs->cx, &value );

  return value;
}

static jsval 
rb_smjs_value_evalscript( int argc, VALUE* argv, VALUE self ){
  sSMJS_Value* sv;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return rb_smjs_evalscript( sv->cs, JSVAL_TO_OBJECT( sv->value ), argc, argv );
}

static void
rbsm_each_raise( sSMJS_Enumdata* enm, const char* msg ){
  JS_DestroyIdArray( enm->cx, enm->ida );
  rb_raise( eJSConvertError, "%s", msg );
}

static void*
rbsm_each( JSContext* cx, jsval value, RBSMJS_YIELD yield, void* data ){
  sSMJS_Enumdata enmdata;
  sSMJS_Enumdata* enm;
  JSObject* jo;
  
  if( JSVAL_IS_PRIMITIVE( value ) ){
    if( JSVAL_IS_STRING( value ) ){
      return data;
      //rb_raise ( eJSConvertError, "can't enumerate" );
    }else{
      return data;
    }
  }

  enm = &enmdata;
  enm->data = data;
  enm->cx = cx;
  enm->ida = NULL;
  enm->obj = JSVAL_TO_OBJECT( value );

  JS_AddNamedRoot( cx, &(enm->obj), "rbsm_each" );

  enm->ida = JS_Enumerate( enm->cx, enm->obj );

  if( !enm->ida ){
    JS_RemoveRoot( cx, &(enm->obj) );
    rb_raise( eJSConvertError, "Unable to enumerate" );
  }

  for( enm->i = 0; enm->i < enm->ida->length; enm->i++ ){
    enm->id = enm->ida->vector[enm->i];
    if( JS_IdToValue( enm->cx, enm->id, &enm->key ) ){
      //enm->keystr = JS_GetStringBytes( JS_ValueToString( cx, enm->key ) );
      //if( JS_GetProperty( enm->cx, enm->obj, enm->keystr, &enm->val ) ){
      if( OBJ_GET_PROPERTY( enm->cx, enm->obj, enm->id, &enm->val ) ){
        yield( enm );
      }else{
        JS_RemoveRoot( cx, &(enm->obj) );
        rbsm_each_raise( enm, "Unable to get property" );
      }
    }else{
      JS_RemoveRoot( cx, &(enm->obj) );
      rbsm_each_raise( enm, "Unable to get key name" );
    }
  }

  JS_DestroyIdArray( enm->cx, enm->ida );
  jo = JS_GetPrototype( enm->cx, enm->obj );
  JS_RemoveRoot( cx, &(enm->obj) );
  if( jo ){
    return rbsm_each( enm->cx, OBJECT_TO_JSVAL( jo ), yield, enm->data );
  }
  return enm->data;
}

static VALUE
rbsm_value_each( VALUE self, RBSMJS_YIELD yield, void* data ){
  sSMJS_Value* sv;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return (VALUE)rbsm_each( sv->cs->cx, sv->value, yield, data );
}

static void
rb_smjs_value_each_yield( sSMJS_Enumdata* enm ){
  JS_AddNamedRoot( enm->cx, &(enm->val), "rb_smjs_value_each_yield:enm->val" );
  rb_yield( rb_smjs_convert_prim( enm->cx, enm->val ) );
  JS_RemoveRoot( enm->cx, &(enm->val) );
}

static void
rb_smjs_value_each_with_index_yield( sSMJS_Enumdata* enm ){
  JS_AddNamedRoot( enm->cx, &(enm->key), "rb_smjs_value_each_with_index_yield:enm->key" );
  JS_AddNamedRoot( enm->cx, &(enm->val), "rb_smjs_value_each_with_index_yield:enm->val" );
  rb_yield_values( 2, rb_smjs_convertvalue( enm->cx,  enm->key ), rb_smjs_convert_prim(  enm->cx,  enm->val ) );
  JS_RemoveRoot( enm->cx, &(enm->val) );
  JS_RemoveRoot( enm->cx, &(enm->key) );
}

// Calls a Proc from inside protect; args is a Ruby array with the Proc
// appended.
static VALUE
rb_smjs_ruby_proc_caller( VALUE args ){
  // The proc to execute
  VALUE proc;
  proc = rb_ary_pop( args );
  return rb_apply( proc, rb_intern( "call" ), args );
}

static VALUE
rb_smjs_ruby_proc_caller3( VALUE proc ){
  return rb_funcall( proc, rb_intern( "call" ), 0 );
}

static VALUE
rb_smjs_ruby_string_caller( VALUE obj ){
  return rb_funcall( obj, rb_intern( "to_s" ), 0 );
}

// used by Object#[] below
static VALUE
rb_smjs_ruby_box_caller( VALUE args ){
  VALUE obj;
  obj = rb_ary_pop( args );
  return rb_apply( obj, rb_intern( "[]" ), args );
}

static VALUE
rb_smjs_ruby_missing_caller( VALUE args ){
  VALUE obj;
  obj = rb_ary_pop( args );
  return rb_apply( obj, rb_intern( "method_missing" ), args );
}

#ifdef ZERO_ARITY_METHOD_IS_PROPERTY
struct{
  char* keyname;
  jsval val;
}g_last0arity;
#endif

// Ruby Object#[] called in JS as JSObject()
static JSBool
rb_smjs_value_object_callback( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval ){
  JSObject* fobj;
  VALUE rargs, res;
  uintN i;
  int status;
  sSMJS_Class* so;
  
  fobj = JSVAL_TO_OBJECT( argv[-2] );

  // If we are trying to invoke this object as a zero-param method, and
  // it was just retrieved as a 0-arity "method as property", we
  // silently return the object itself; the likihood that we actually
  // meant to invoke :[] with no arguments is very low.
  if (argc == 0 && OBJECT_TO_JSVAL(fobj) == g_last0arity.val) {
    *rval = g_last0arity.val;
    return JS_TRUE;
  }

  so = (sSMJS_Class*)JS_GetPrivate( cx, fobj );
  
  // Argument in SpiderMonkey::Value
  rargs = rb_ary_new2( argc + 1 );
  for( i = 0 ; i < argc ; i++ )
    rb_ary_store( rargs, i, rb_smjs_convert_prim( cx, argv[i] ) );
  rb_ary_store( rargs, i, so->rbobj );
  
  // Execute the proc
  res = rb_protect( rb_smjs_ruby_box_caller, rargs, &status );
  
  // Check the Ruby function execution result; if an exception was
  // thrown, we raise a corresponding error in JavaScript.
  if( status != 0 )
    return rb_smjs_raise_js( cx, status );
  
  return rb_smjs_ruby_to_js( cx, res, rval );
}

// Ruby method called from JavaScript
static JSBool
rb_smjs_value_function_callback( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval ){
  JSObject* fobj;
  VALUE rargs, res;
  uintN i;
  int status;
  sSMJS_Class* so;
  
  fobj = JSVAL_TO_OBJECT( argv[-2] );
  so = (sSMJS_Class*)JS_GetPrivate( cx, fobj );
  
  rargs = rb_ary_new2( argc + 1 );
  for( i = 0 ; i < argc ; i++ )
    rb_ary_store( rargs, i, rb_smjs_convert_prim( cx, argv[i] ) );
  rb_ary_store( rargs, i, so->rbobj );
  
  // Run proc
  res = rb_protect( rb_smjs_ruby_proc_caller, rargs, &status );
  
  // If a Ruby exception was thrown, raise a JS exception
  if( status != 0 )
    return rb_smjs_raise_js( cx, status );
  
  return rb_smjs_ruby_to_js( cx, res, rval );
}

static VALUE
rb_smjs_ruby_proc_caller2( VALUE args ){
  ID mname;
  VALUE obj;
  
  obj = rb_ary_pop( args );
  mname = rb_ary_pop( args );
  return rb_apply( obj, mname, args );
}

static JSBool
rbsm_class_no_such_method( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval ){
  const char* keyname;
  sSMJS_Class* so;

  VALUE rargs, res;
  int status;

  keyname = rbsm_GetValueStringBytes( cx, argv[0] );
  if( !keyname ) return JS_FALSE;

  trace("_noSuchMethod__(cx=%x, keyname=%s)", cx, keyname );
  so = JS_GetInstancePrivate( cx, JSVAL_TO_OBJECT( argv[-2] ), &JSRubyObjectClass, NULL );
  if( !so ){
    so = JS_GetInstancePrivate( cx, thisobj, &JSRubyObjectClass, NULL );
    if( !so ){
      JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_NOT_FUNCTION, keyname );
      return JS_FALSE;
    }
  }
#ifdef ZERO_ARITY_METHOD_IS_PROPERTY
  if( strcmp( keyname, g_last0arity.keyname ) == 0 ){
    trace("  == g_last0arity.keyname" );
    *rval = g_last0arity.val;
    return JS_TRUE;
  }
  trace("  != g_last0arity.keyname [%s]", g_last0arity.keyname );
#endif

  if (argc != 2)
    return JS_FALSE;

  rargs = rb_smjs_convertvalue( cx, argv[1] );
  rb_ary_unshift( rargs, ID2SYM( rb_intern( keyname ) ) );
  rb_ary_push( rargs, so->rbobj );
  
  // Call method_missing
  res = rb_protect( rb_smjs_ruby_missing_caller, rargs, &status );
  
  // If an exception was thrown, raise a JS exception
  if( status != 0 )
    return rb_smjs_raise_js( cx, status );
  
  return rb_smjs_ruby_to_js( cx, res, rval );
}

static JSBool 
rbsm_class_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval ){
  sSMJS_Class* so;
  VALUE res;
  int status;

  so = JS_GetInstancePrivate( cx, JSVAL_TO_OBJECT( argv[-2] ), &JSRubyObjectClass, NULL );
  if( !so ){
    so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
    if( !so ){
      JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", "toString", "Object" );
      return JS_FALSE;
    }
  }

  res = rb_protect( rb_smjs_ruby_string_caller, so->rbobj, &status );
  
  // Check the Ruby function execution result; if an exception was
  // thrown, we raise a corresponding error in JavaScript.
  if( status != 0 )
    return rb_smjs_raise_js( cx, status );
  
  return rb_smjs_ruby_to_js( cx, res, rval );
}


static JSObjectOps*
rbsm_class_get_object_ops( JSContext* cx, JSClass* clasp ){
  return &rbsm_FunctionOps;
}

static JSBool
rbsm_error_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
  sSMJS_Error* se;
  JS_AddNamedRoot( cx, &id, "rbsm_error_get_property:id" );
  se = JS_GetInstancePrivate( cx, obj, &JSRubyExceptionClass, NULL );
  if( !se ){
    // TODO: Use the object's name as the function name
    const char* keyname = rbsm_GetValueStringBytes( cx, id );
    if( !keyname ) keyname = "?unknown?";
    JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
    JS_RemoveRoot( cx, &id );
    return JS_FALSE;
  }
  JS_RemoveRoot( cx, &id );
  return rbsm_get_ruby_property( cx, obj, id, vp, se->errinfo );
}

static void
rbsm_rberror_finalize( JSContext* cx, JSObject* obj ){
  sSMJS_Error* se;
  se = (sSMJS_Error*)JS_GetPrivate( cx, obj );
  if( se ){
    JS_free( cx, se );
  }
}

static sSMJS_Class* 
rbsm_wrap_class( JSContext* cx, VALUE rbobj ){
  sSMJS_Class* so;
  VALUE context;
  
  so = (sSMJS_Class*)malloc( sizeof( sSMJS_Class ) );
  so->rbobj = rbobj;
  context = RBSMContext_FROM_JsContext( cx );
  rb_hash_aset( rb_iv_get( context, RBSMJS_CONTEXT_BINDINGS ), rb_obj_id( rbobj ), rbobj );
  return so;
}

// Convert a Ruby Proc to a JS function
static JSObject*
rbsm_proc_to_function( JSContext* cx, VALUE proc ){
  JSObject* jo;
  sSMJS_Class* so;
  VALUE pname;
  
  so = rbsm_wrap_class( cx, proc );
  jo = JS_NewObject( cx, &JSRubyFunctionClass,  NULL, NULL ); 
  if( ! jo ) return NULL;

  JS_AddNamedRoot( cx, &jo, "rbsm_proc_to_function" );
#ifdef DEBUG
  pname = rb_inspect(proc);
  trace("rbsm_proc_to_function(cx=%x); %s; [count %d -> %d]", cx, StringValuePtr(pname), alloc_count_rb2js, ++alloc_count_rb2js);
#endif
  so->jsv = OBJECT_TO_JSVAL( jo );
  JS_SetPrivate( cx, jo, (void*)so );
  JS_RemoveRoot( cx, &jo );
  return jo;
}

static JSBool
rbsm_resolve( JSContext* cx, JSObject *obj, jsval id, uintN flags, JSObject **objp ) {
  const char *keyname;

  if(!rbsm_check_ruby_property(cx, obj, id)) {
    return JS_TRUE;
  }

  keyname = rbsm_GetValueStringBytes( cx, id );
  if( !keyname ) return JS_FALSE;

  JS_AddNamedRoot( cx, &id, "rbsm_resolve:id" );

  trace( "rbsm_resolve(cx=%x, keyname=%s)", cx, keyname );

  JS_DefineProperty(cx, obj, keyname, JSVAL_TRUE, 
    rbsm_class_get_temp_property, rbsm_class_set_property, JSPROP_ENUMERATE);

  JS_RemoveRoot( cx, &id );

  *objp = obj;
  return JS_TRUE;
}

static int
rbsm_check_ruby_property(JSContext* cx, JSObject* obj, jsval id) {     
  ID brackets   = rb_intern("[]");
  ID key_meth   = rb_intern("key?");
  ID array_like = rb_intern("array_like?");

  sSMJS_Class *so;
  VALUE rbobj;
  int keynumber;
  int result = 0;

  const char *keyname = rbsm_GetValueStringBytes( cx, id );
  if( !keyname ) return 0;

  JS_AddNamedRoot( cx, &id, "rbsm_check_ruby_property:id" );

  so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
  rbobj = so->rbobj;

  trace("rbsm_check_ruby_property(cx=%x, jo=%x, key=%s)", cx, obj, keyname);

  if(rb_respond_to(rbobj, brackets)) {
    if(JSVAL_IS_INT(id) &&
        (rb_obj_is_kind_of(rbobj, rb_cArray) || 
        (rb_respond_to(rbobj, array_like) && RTEST(rb_funcall(rbobj, array_like, 0))))) {
      result = 0;
    } else if(rb_respond_to(rbobj, key_meth)) {
      result = RTEST(rb_funcall(rbobj, key_meth, 1, rb_str_new2(keyname)));
    }
  }

  JS_RemoveRoot( cx, &id );
  return result;
}

static JSBool
rbsm_get_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj ){
  ID rid;
  VALUE method;
  int iarity;
  VALUE ret;

  ID brackets = rb_intern("[]");
  const char* keyname = rbsm_GetValueStringBytes( cx, id );
  if( !keyname ) return JS_FALSE;

  JS_AddNamedRoot( cx, &id, "rbsm_get_ruby_property:id" );

  if(rb_respond_to(rbobj, brackets)) {
    ID key_meth = rb_intern("key?");
    ID array_like = rb_intern("array_like?");

    if(JSVAL_IS_INT(id) &&
        (rb_obj_is_kind_of(rbobj, rb_cArray) || 
        (rb_respond_to(rbobj, array_like) && RTEST(rb_funcall(rbobj, array_like, 0))))) {
      int keynumber = JSVAL_TO_INT( id );
      // printf( "_get_property_( %d )", keynumber );
      return rb_smjs_ruby_to_js(cx, rb_funcall(rbobj, brackets, 1, INT2NUM(keynumber)), vp);
    } else if(rb_respond_to(rbobj, key_meth)) {
      if(rb_funcall(rbobj, key_meth, 1, rb_str_new2(keyname)) != Qfalse) {
        // printf( "_get_property_( %s )", keyname );
        return rb_smjs_ruby_to_js(cx, rb_funcall(rbobj, brackets, 1, rb_str_new2(keyname)), vp);
      }
    }
  }

  // FIXME: Every rb_funcall() below should be protected.

  trace( "_get_property_(cx=%x, jo=%x, keyname=%s)", cx, obj, keyname );
  rid = rb_intern( keyname );
  JS_RemoveRoot( cx, &id );

  // TODO: int rb_respond_to( VALUE obj, ID id )
  if( rb_is_const_id( rid ) && 
      rb_respond_to(rbobj, rb_intern("const_defined?")) &&
      rb_const_defined( rbobj, rid ) ){
    // The return value is a constant
    return rb_smjs_ruby_to_js( cx, rb_const_get( rbobj, rid ), vp );
  }else if( RTEST( rb_funcall( rbobj, rb_intern( "respond_to?" ), 1, ID2SYM( rid ) ) ) ){
    method = rb_funcall( rbobj, rb_intern( "method" ), 1, ID2SYM( rid ) );
#ifdef ZERO_ARITY_METHOD_IS_PROPERTY
    iarity = NUM2INT( rb_funcall( method, rb_intern( "arity" ), 0 ) );
    // If the arity is 0
    if( iarity == 0 /*|| iarity == -1*/ ){
      // Call the function and return it as the property
      JSBool success;
      int status;
      ret = rb_protect( rb_smjs_ruby_proc_caller3, method, &status );
      if( status != 0 )
        return rb_smjs_raise_js( cx, status );
      success = rb_smjs_ruby_to_js( cx, ret, vp );
      g_last0arity.keyname = rb_id2name( rid );
      g_last0arity.val = *vp;
      return success;
    } else {
      *vp = OBJECT_TO_JSVAL( rbsm_proc_to_function( cx, method ) );
      return JS_TRUE;
    }
#else
    *vp = OBJECT_TO_JSVAL( rbsm_proc_to_function( cx, method ) );
    return JS_TRUE;
#endif
  }

  // We don't know this property, so we leave *vp untouched.
  return JS_TRUE;
}

static JSBool
rbsm_set_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj ){
  VALUE pkeyname;
  char keyname[BUFSIZ];
  ID rid;
  int status;
  VALUE vals;
  
  ID brackets = rb_intern("[]=");

  JS_AddNamedRoot( cx, &id, "rbsm_set_ruby_property" );
  
  if(JSVAL_IS_INT(id))
    pkeyname = INT2NUM(JSVAL_TO_INT(id));
  else
    pkeyname = rb_str_new2(rbsm_GetValueStringBytes( cx, id ));

  if(rb_respond_to(rbobj, brackets)) {
    rid = rb_intern("[]=");
    vals = rb_ary_new2(4);
    // key
    rb_ary_push(vals, pkeyname);
    // thing being assigned
    rb_ary_push(vals, rb_smjs_convert_prim( cx, vp[0] ));
  }  else {
    // call foo=
    sprintf( keyname, "%s=", rbsm_GetValueStringBytes( cx, id ) );
    rid = rb_intern( keyname );
    vals = rb_ary_new2( 3 );
    // thing being assigned
    rb_ary_push( vals, rb_smjs_convert_prim( cx, vp[0] ) );
  }

  JS_RemoveRoot( cx, &id );

  // method ([]= or foo=)
  rb_ary_push(vals, rid);
  // self
  rb_ary_push(vals, rbobj);

  // Call method(val) inside begin/rescue
  rb_protect( rb_smjs_ruby_proc_caller2, vals, &status );
  if(status != 0)
    return rb_smjs_raise_js(cx, status);
  return JS_TRUE;
}

static JSBool
rbsm_class_get_temp_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
  sSMJS_Class* so;
  const char* keyname = rbsm_GetValueStringBytes( cx, id );
  if( !keyname ) return JS_FALSE;

  JS_AddNamedRoot( cx, &id, "rbsm_class_get_temp_property:id" );

  so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
  if( !so ){
    // TODO: Use the function name as the object name
    JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
    JS_RemoveRoot( cx, &id );
    return JS_FALSE;
  }

  JS_DeleteProperty(cx, obj, keyname);

  JS_RemoveRoot( cx, &id );
  return rbsm_get_ruby_property( cx, obj, id, vp, so->rbobj );
}

static JSBool
rbsm_class_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
  sSMJS_Class* so;

  JS_AddNamedRoot( cx, &id, "rbsm_class_get_property:id" );

  so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
  if( !so ){
    const char* keyname = rbsm_GetValueStringBytes( cx, id );
    if( !keyname ) keyname = "?unknown?";
    // TODO: Use the function name as the object name
    JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
    JS_RemoveRoot( cx, &id );
    return JS_FALSE;
  }
  JS_RemoveRoot( cx, &id );
  return rbsm_get_ruby_property( cx, obj, id, vp, so->rbobj );
}

static JSBool
rbsm_class_set_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
  sSMJS_Class* so;
  JS_AddNamedRoot( cx, &id, "rbsm_class_set_property:id" );
  so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
  if( !so ){
    const char* keyname = rbsm_GetValueStringBytes( cx, id );
    if( !keyname ) keyname = "?unknown?";
    // TODO: Use the function name as the object name
    JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
    JS_RemoveRoot( cx, &id );
    return JS_FALSE;
  }
  JS_RemoveRoot( cx, &id );
  return rbsm_set_ruby_property( cx, obj, id, vp, so->rbobj );
}

static JSObject*
rbsm_ruby_to_jsdate( JSContext* cx, VALUE msec ){
  JSObject* jsGlobal = JS_GetGlobalObject( cx );
  jsval msec_js, dateval;

  rb_smjs_ruby_to_js( cx, msec, &msec_js );

  JS_AddNamedRoot( cx, &msec_js, "rbsm_ruby_to_jsdate" );
  JS_CallFunctionName( cx, jsGlobal, "__newDate__", 1, &msec_js, &dateval );
  JS_RemoveRoot( cx, &msec_js );

  return JSVAL_TO_OBJECT( dateval );
}

inline static long rb_ary_size(VALUE ary) { return RARRAY(ary)->len; }

static JSObject*
rbsm_ruby_to_jsarray( JSContext* cx, VALUE obj ){
  long i;
  long len = rb_ary_size( obj );
  JSObject* jo;
  trace("Converting Ruby array to JS (%d elements)", len);
  jo = JS_NewArrayObject( cx, 0, NULL );
  if( ! jo ) return NULL;

  JS_AddNamedRoot( cx, &jo, "rbsm_ruby_to_jsarray" );
  for( i = 0; i < len; i++ ){
    jsval tmp;
#ifdef DEBUG
    VALUE insp = rb_inspect(rb_ary_entry(obj, i));
    trace(" element[%d]: %s", i, StringValuePtr(insp));
#endif
    rb_smjs_ruby_to_js( cx, rb_ary_entry( obj, i ), &tmp );
    JS_SetElement( cx, jo, i, &tmp );
  }
  JS_RemoveRoot( cx, &jo );
  return jo;
}

// Wrap Ruby object in JS object
static JSObject*
rbsm_ruby_to_jsobject( JSContext* cx, VALUE obj ){

  JSObject* jo;
  sSMJS_Value* sv;
  sSMJS_Class* so;
  
  // Check if we've already converted this object into a JS Object
  if(rb_gv_get(RBSMJS_RUBY_TO_JS_MAP) == Qnil) rb_gv_set(RBSMJS_RUBY_TO_JS_MAP, rb_hash_new());
  if(rb_funcall(rb_gv_get(RBSMJS_RUBY_TO_JS_MAP), rb_intern("key?"), 1, obj) == Qtrue) {
    jsval js = (jsval)FIX2INT(rb_hash_aref(rb_gv_get(RBSMJS_RUBY_TO_JS_MAP), obj));
    return JSVAL_TO_OBJECT(js);
  }

  ATTEMPT_GC( cx );
  
  if( rb_obj_is_kind_of( obj, cJSValue ) ){
    Data_Get_Struct( obj, sSMJS_Value, sv );
    return JSVAL_TO_OBJECT( sv->value );
  }
  if( rb_obj_is_kind_of( obj, rb_cTime ) ){
    VALUE msec;
    trace("creating Time");
    msec = rb_funcall( obj, rb_intern( "to_i" ), 0 );
    return rbsm_ruby_to_jsdate( cx, msec );
  }
  if( rb_obj_is_kind_of( obj, rb_cDate ) ){
    VALUE msec;
    trace("creating Date");
    msec = rb_funcall( rb_funcall( obj, rb_intern( "strftime" ), 1, rb_str_new2( "%s" ) ), rb_intern( "to_i" ), 0);
    return rbsm_ruby_to_jsdate( cx, msec );
  }
  if( rb_obj_is_kind_of( obj, rb_cArray ) ){
    return rbsm_ruby_to_jsarray( cx, obj );
  }
  so = rbsm_wrap_class( cx, obj );
  jo = JS_NewObject( cx, &JSRubyObjectClass, NULL, NULL ); 
  if( ! jo ) return NULL;

  JS_AddNamedRoot( cx, &jo, "rbsm_ruby_to_jsobject" );
  trace("rbsm_ruby_to_jsobject(cx=%x, obj=%x); [count %d -> %d]", cx, jo, alloc_count_rb2js, ++alloc_count_rb2js);
  so->jsv = OBJECT_TO_JSVAL( jo );
  JS_SetPrivate( cx, jo, (void*)so );
  JS_DefineFunctions( cx, jo, JSRubyObjectFunctions );
  
  rb_hash_aset(rb_gv_get(RBSMJS_RUBY_TO_JS_MAP), obj, INT2FIX((int)OBJECT_TO_JSVAL(jo)));
  JS_RemoveRoot( cx, &jo );
  
  return jo;
}

// RubyObjectClass destructor
static void
rbsm_class_finalize( JSContext* cx, JSObject* obj ){
  sSMJS_Class* so;
  
  so = (sSMJS_Class*)JS_GetPrivate( cx, obj );
  if( so ){
    trace("rbsm_class_finalize(cx=%x, obj=%x); [count %d -> %d]", cx, obj, alloc_count_rb2js, --alloc_count_rb2js);
    if( so->rbobj ){
      if( JS_GetContextPrivate( cx ) ){
        VALUE context = RBSMContext_FROM_JsContext( cx );
        VALUE bindings = rb_iv_get( context, RBSMJS_CONTEXT_BINDINGS );
        if( RTEST( bindings ) ){
          rb_hash_delete( rb_iv_get( context, RBSMJS_CONTEXT_BINDINGS ), rb_obj_id( so->rbobj ) );
        }
        rb_hash_delete( rb_gv_get(RBSMJS_RUBY_TO_JS_MAP), so->rbobj);
      }
    }
    if( so->jsv ){
      JS_RemoveRoot( cx, &(so->jsv) );
    }
    free( so );
  }
}

// SpiderMonkey::Value ---------------------------------------------------------------------

// SpiderMonkey::Value destructor
static void 
rb_smjs_value_free( sSMJS_Value* sv ){
  trace( "value_free(cx=%x, value=%x); [count %d -> %d]", sv->cs->cx, sv->value, alloc_count_js2rb, --alloc_count_js2rb );
  if( sv->value && sv->cs->store ){
    rbsm_remove_jsval_to_rbval( sv->cs, sv->value );
  }
  free( sv );
}

// Ruby cannot make a SpiderMonkey::Value
static VALUE
rb_smjs_value_initialize( VALUE self, VALUE context ){
  rb_raise( eJSError, "do not create SpiderMonkey::Value" );
  return Qnil;
}

// Actual constructor
static VALUE
rb_smjs_value_new_jsval( VALUE context, jsval value ){
  sSMJS_Context* cs;
  Data_Get_Struct( context, sSMJS_Context, cs );

  if( rbsm_lookup_jsval_to_rbval( cs, value ) ){
    // There's already an object in the JS -> RB map
    return rbsm_get_jsval_to_rbval( cs, value );
  }else{
    // There's no object in the JS -> RB map
    sSMJS_Value* sv;
    VALUE self;

    self = Data_Make_Struct( cJSValue, sSMJS_Value, 0, rb_smjs_value_free, sv );
    sv->cs = cs;
    sv->value = value;
    trace( "value_new(cx=%x, value=%x); [count %d -> %d]", sv->cs->cx, value, alloc_count_js2rb, ++alloc_count_js2rb );

    // A context for instance variables
    rb_iv_set( self, RBSMJS_VALUES_CONTEXT, context );

    rbsm_set_jsval_to_rbval( sv->cs, self, value );
    return self;
  }
}

static VALUE
rb_smjs_value_get_jsvalid( VALUE self ){
  sSMJS_Value* sv;
  jsid id;

  Data_Get_Struct( self, sSMJS_Value, sv );
  JS_ValueToId( sv->cs->cx, sv->value, &id );
  return INT2NUM( id );
}

static VALUE
rb_smjs_value_get_context( VALUE self ){
  return rb_iv_get( self, RBSMJS_VALUES_CONTEXT );
}

// Run code and return a SpiderMonkey::Value
static VALUE
rb_smjs_value_eval( int argc, VALUE* argv, VALUE self ){
  sSMJS_Value* sv;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return rb_smjs_convert_prim( sv->cs->cx, rb_smjs_evalscript( sv->cs, JSVAL_TO_OBJECT( sv->value ), argc, argv ) );
}

// Run code and return a SpiderMonkey::Value
static VALUE
rb_smjs_value_evalget( int argc, VALUE* argv, VALUE self ){
  return rb_smjs_value_new_jsval( rb_smjs_value_get_context( self ), rb_smjs_value_evalscript( argc, argv, self ) );
}

// Run code and return a Ruby object
static VALUE
rb_smjs_value_evaluate( int argc, VALUE* argv, VALUE self ){
  sSMJS_Value* sv;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return rb_smjs_convertvalue( sv->cs->cx, rb_smjs_evalscript( sv->cs, JSVAL_TO_OBJECT( sv->value ), argc, argv ) );
}
static VALUE
rb_smjs_value_each( VALUE self ){
  rbsm_value_each( self, rb_smjs_value_each_yield, NULL );
  return Qnil;
}
static VALUE
rb_smjs_value_each_with_index( VALUE self ){
  rbsm_value_each( self, rb_smjs_value_each_with_index_yield, NULL );
  return Qnil;
}

static VALUE
rb_smjs_value_get_parent( VALUE self ){
  sSMJS_Value* sv;
  JSObject* jo;

  Data_Get_Struct( self, sSMJS_Value, sv );
  jo = JS_GetParent( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ) );
  if( !jo ) return Qnil;
  return rb_smjs_convert_prim( sv->cs->cx, OBJECT_TO_JSVAL( jo ) );
}

static VALUE
rb_smjs_value_get_prototype( VALUE self ){
  sSMJS_Value* sv;
  JSObject* jo;

  Data_Get_Struct( self, sSMJS_Value, sv );
  jo = JS_GetPrototype( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ) );
  if( !jo ) return Qnil;
  return rb_smjs_convert_prim( sv->cs->cx, OBJECT_TO_JSVAL( jo ) );
}

static VALUE
rb_smjs_value_call( VALUE self ){
  sSMJS_Value* sv;
  Data_Get_Struct( self, sSMJS_Value, sv );
  if( !JS_ObjectIsFunction( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ) ) ){
    rb_raise( eJSEvalError, "object is not function" );
  }
  return Qnil;
}

static VALUE RBSM_CALL_CONTEXT_JSVALUE( RBSMJS_Convert f, VALUE v ){
  sSMJS_Value* sv;
  Data_Get_Struct( v, sSMJS_Value, sv );
  return f( sv->cs->cx, sv->value );
}

static VALUE
rb_smjs_value_to_ruby( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_convertvalue, self );
}

static VALUE 
rb_smjs_value_to_s( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_to_s, self );
}

static VALUE
rb_smjs_value_to_i( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_to_i, self );
}

static VALUE
rb_smjs_value_to_f( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_to_f, self );
}

static VALUE
rb_smjs_value_to_num( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_to_num, self );
}

static VALUE
rb_smjs_value_to_bool( VALUE self ){
  return RBSM_CALL_CONTEXT_JSVALUE( rb_smjs_to_bool, self );
}

static VALUE
rb_smjs_value_to_a( VALUE self, VALUE shallow ){
  sSMJS_Value* sv;
  int shal;

  shal = ( NIL_P( shallow ) ) ? 1 : 0;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return rb_smjs_to_a( sv->cs->cx, sv->value, shal );
}

static VALUE
rb_smjs_value_to_h( VALUE self, VALUE shallow ){
  sSMJS_Value* sv;
  int shal;

  shal = ( NIL_P( shallow ) )? 1 : 0;
  Data_Get_Struct( self, sSMJS_Value, sv );
  return rb_smjs_to_h( sv->cs->cx, sv->value, shal );
}

// typeof
static VALUE
rb_smjs_value_typeof( VALUE self ){
  sSMJS_Value* sv;
  const char*  name;
  JSType type;

  Data_Get_Struct( self, sSMJS_Value, sv );
  type = JS_TypeOfValue( sv->cs->cx, sv->value );
  name = JS_GetTypeName( sv->cs->cx, type );
  return rb_str_new2( name );
}

// Registers a Ruby function to JavaScript; takes the function name and
// a Proc as arguments.
static VALUE
rb_smjs_value_function( int argc, VALUE* argv, VALUE self ){
  char* cname;
  JSObject* jo;
  VALUE proc, name;
  jsval jname;
  sSMJS_Value* sv;

  // Analyze the given arguments
  rb_scan_args( argc, argv, "1&", &name, &proc );

  // Make sure it's actually a proc
  if( !RTEST( proc ) ) {
    rb_raise( rb_eArgError, "block required" );
  }

  Data_Get_Struct( self, sSMJS_Value, sv );
  cname = StringValuePtr( name );

  jo = rbsm_proc_to_function( sv->cs->cx, proc );
  JS_AddNamedRoot( sv->cs->cx, &jo, "rb_smjs_value_function" );

  if( rbsm_rubystring_to_jsval( sv->cs->cx, name, &jname ) ){
    JS_SetProperty( sv->cs->cx, jo, "name", &jname );
  }

  JS_DefineProperty( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), cname, OBJECT_TO_JSVAL( jo ), NULL, NULL, JSPROP_PERMANENT | JSPROP_READONLY );
  JS_RemoveRoot( sv->cs->cx, &jo );

  return proc;
}

// To set the property
static VALUE
rb_smjs_value_set_property( VALUE self, VALUE name, VALUE val ){
  sSMJS_Value* sv;
  jsval jval;
  char* pname;
  
  pname = StringValuePtr( name );
  Data_Get_Struct( self, sSMJS_Value, sv );
  rb_smjs_ruby_to_js( sv->cs->cx, val, &jval );
  JS_SetProperty( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), pname, &jval );
  return self;
}

static void
rb_smjs_value_get_properties_yield( sSMJS_Enumdata* enm ){
  rb_ary_store( (VALUE)enm->data, enm->i, rb_smjs_to_s( enm->cx, enm->key ) );

}
// Get a list of properties
static VALUE
rb_smjs_value_get_properties( VALUE self ){
  VALUE ret;
  
  ret = rb_ary_new( );
  rbsm_value_each( self, rb_smjs_value_get_properties_yield, (void*)ret );
  return ret;
}

// Get a single property
static VALUE
rb_smjs_value_get_property( VALUE self, VALUE name ){
  sSMJS_Value* sv;
  jsval jval;
  char* pname;

  pname = StringValuePtr( name );
  Data_Get_Struct( self, sSMJS_Value, sv );
  JS_GetProperty( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), pname, &jval );
  return rb_smjs_convert_prim( sv->cs->cx, jval );
}

static VALUE
rb_smjs_value_call_function( int argc, VALUE* rargv, VALUE self ){
  JSBool ok;
  sSMJS_Value* sv;
  jsval jval;
  char* pname;
  int i;
#ifdef WIN32
  jsval* jargv;
#else
  jsval jargv[argc - 1];
#endif
  
  Data_Get_Struct( self, sSMJS_Value, sv );

  if(argc == 0 && JS_TypeOfValue(sv->cs->cx, sv->value) == JSTYPE_FUNCTION) {
    pname = "call";
    argc = 1;
  } else if(argc == 0) {
    rb_raise(rb_eArgError, "You must pass in a function to call on the object");
  } else {
    pname = StringValuePtr( rargv[0] );
  }

  // Convert the arguments from Ruby to JavaScript values.

#ifdef WIN32
  jargv = JS_malloc( sv->cs->cx, sizeof( jsval*) * ( argc - 1 ) );
#endif

  for( i = 1 ; i < argc ; i++ ){
    rb_smjs_ruby_to_js( sv->cs->cx, rargv[i], &jargv[i - 1] );
  }

  // Execution
  ok = JS_CallFunctionName( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), pname, argc - 1, jargv, &jval );

#ifdef WIN32
  JS_free( sv->cs->cx, jargv );
#endif

  if( !ok ) rb_smjs_raise_ruby( sv->cs->cx );

  JS_AddNamedRoot( sv->cs->cx, &jval, "rb_smjs_value_call_function" );
  ATTEMPT_GC( sv->cs->cx );
  JS_RemoveRoot( sv->cs->cx, &jval );

  return rb_smjs_convert_prim( sv->cs->cx, jval );
}

// SpiderMonkey::EvalError ---------------------------------------------------------------------

static VALUE
rbsm_evalerror_new( VALUE context, VALUE erval ){
  VALUE self;

  self = rb_funcall( eJSEvalError, rb_intern( "new" ), 1, rb_funcall( erval, rb_intern( "to_s" ), 0 ) );
  rb_iv_set( self, "error", erval );
  return self;
}

static VALUE
rbsm_evalerror_new_jsval( VALUE context, jsval jsvalerror ){
  VALUE erval;
  erval = rb_smjs_value_new_jsval( context, jsvalerror );
  return rbsm_evalerror_new( context, erval );
}

static VALUE
rbsm_evalerror_message( VALUE self ){
  return rb_funcall( rb_iv_get( self, "error" ), rb_intern( "to_s" ), 0 );
}

static VALUE
rbsm_evalerror_lineno( VALUE self ){
  sSMJS_Value* sv;
  VALUE erval;

  erval = rb_iv_get( self, "error" );
  Data_Get_Struct( erval, sSMJS_Value, sv ); 
  return INT2NUM( JS_ErrorFromException( sv->cs->cx, sv->value )->lineno );
}

static VALUE
rbsm_evalerror_error_number( VALUE self ){
  sSMJS_Value* sv;
  VALUE erval;

  erval = rb_iv_get( self, "error" );
  Data_Get_Struct( erval, sSMJS_Value, sv ); 
  return INT2NUM( JS_ErrorFromException( sv->cs->cx, sv->value )->errorNumber );
}

static VALUE
rbsm_evalerror_js_error( VALUE self ){
  sSMJS_Value* sv;
  VALUE erval;

  erval = rb_iv_get( self, "error" );
  Data_Get_Struct( erval, sSMJS_Value, sv ); 
  return rb_smjs_convert_prim( sv->cs->cx, sv->value );
}

// SpiderMonkey::Context -------------------------------------------------------------
static VALUE
rb_smjs_context_get_scope_chain( VALUE self ){
  JSContext* cx;
  JSObject* jo;

  cx = RBSMContext_TO_JsContext( self );
  jo = JS_GetScopeChain( cx );
  if( !jo )return Qnil;
  return rb_smjs_convert_prim( cx, OBJECT_TO_JSVAL( jo ) );
}

static void
rbsm_destroy_context( sSMJS_Context* cs ){
  if ( !cs || !cs->cx )
    return;

  trace( "Destroy: store: %x; id2rbval: %x", cs->store, cs->id2rbval );

  // Destruction of the JavaScript context
  JS_SetContextPrivate( cs->cx, 0 );
  JS_RemoveRoot( cs->cx, cs->store );
  cs->store = NULL;

  JS_HashTableDestroy( cs->id2rbval );
  JS_DestroyContext( cs->cx ); 
  cs->cx = NULL;
}

// Free the memory allocated to this context instance
static void
rb_smjs_context_free( sSMJS_Context* cs ){
  trace( "context_free(cx=%x)", cs->cx );
  rbsm_destroy_context( cs );

  // Release the structure's memory
  free( cs );
}

// Required memory is allocated, and a function is registered to handle
// the release of the allocated memory.
static VALUE 
rb_smjs_context_alloc( VALUE self ){
  sSMJS_Context* cs;
  // Allocate the structure's memory
  cs = ALLOC( sSMJS_Context );
  cs->cx = 0;
  // Allocate the required memory, and register our release handler.
  return Data_Wrap_Struct( self, 0, rb_smjs_context_free, cs );
}

// Error Handling
static void
rb_smjs_context_errorhandle( JSContext* cx, const char* message, JSErrorReport* report ){
  sSMJS_Context* cs;

  VALUE context = RBSMContext_FROM_JsContext( cx );
  Data_Get_Struct( context, sSMJS_Context, cs );
  strncpy( cs->last_message, message, BUFSIZ );
  JS_GetPendingException( cx, &cs->last_exception );
  trace( "SpiderMonkey Error: %s [exception: %x]", cs->last_message, cs->last_exception );
}

// Initialize Context
static VALUE
rb_smjs_context_initialize( int argc, VALUE* argv, VALUE self ){
  sSMJS_Context* cs;
  int stacksize;
  int options;
  Data_Get_Struct( self, sSMJS_Context, cs );

  strncpy( cs->last_message, "<<NO MESSAGE>>", BUFSIZ );
  cs->last_exception = 0;

  // Create context
  if( argc == 0 ){
    stacksize = JS_STACK_CHUNK_SIZE;
  }else{
    stacksize = NUM2INT( argv[0] );
  }
  cs->cx = JS_NewContext( gSMJS_runtime, stacksize );
  if( !cs->cx )
    rb_raise( eJSError, "Failed to create context" );

#ifdef BRANCH_GC
  JS_SetBranchCallback( cs->cx, rbsm_branch_callback );
#endif

  JS_SetOptions( cs->cx, JS_GetOptions( cs->cx )
#ifdef JSOPTION_DONT_REPORT_UNCAUGHT
    | JSOPTION_DONT_REPORT_UNCAUGHT
#endif
#ifdef JSOPTION_VAROBJFIX
    | JSOPTION_VAROBJFIX
#endif
#ifdef JSOPTION_XML
    | JSOPTION_XML
#endif
  );

  JS_SetContextPrivate( cs->cx, (void*)self );
  
  cs->store = JS_NewObject( cs->cx, NULL, 0, 0 );
  if( !cs->store )
    rb_raise( eJSError, "Failed to create object store" );
  JS_AddNamedRoot( cs->cx, &(cs->store), "rbsm-store" );
  cs->id2rbval = JS_NewHashTable( 0, jsid2hash, jsidCompare, rbobjCompare, NULL, NULL );

  trace( "CREATED: store: %x; id2rbval: %x", cs->store, cs->id2rbval );

  rb_iv_set( self, RBSMJS_CONTEXT_BINDINGS, rb_hash_new( ) );

  // Register the error report handler
  JS_SetErrorReporter( cs->cx, rb_smjs_context_errorhandle );

  rb_smjs_context_flush( self );
  
  return Qnil;
}

static VALUE
rb_smjs_context_flush( VALUE self ){
  sSMJS_Context* cs;
  JSObject* jsGlobal;
  VALUE rbGlobal;
  char* str_getStack = "try { null.foo(); } catch(ex) { return ex.stack; }";
  char* str_newDate = "return new Date(arguments[0] * 1000);";
  Data_Get_Struct( self, sSMJS_Context, cs );

  // Initialize the global JavaScript object
  jsGlobal = JS_NewObject( cs->cx, &global_class, 0, 0 );
  if( ! jsGlobal )
    rb_raise( eJSError, "Failed to create global object" );
  JS_AddNamedRoot( cs->cx, &jsGlobal, "rb_smjs_context_flush" );
  if( !JS_InitStandardClasses( cs->cx, jsGlobal ) )
    rb_raise( eJSError, "Failed to initialize global object" );
  
  JS_CompileFunction( cs->cx, jsGlobal, "__getStack__", 0, NULL, str_getStack, strlen( str_getStack ), "spidermonkey.c:str_getStack", 1 );
  JS_CompileFunction( cs->cx, jsGlobal, "__newDate__", 0, NULL, str_newDate, strlen( str_newDate ), "spidermonkey.c:str_newDate", 1 );

  // Set the @global instance variable to hold the global object
  rbGlobal = rb_smjs_value_new_jsval( self, OBJECT_TO_JSVAL( jsGlobal ) );
  rb_iv_set( self, RBSMJS_CONTEXT_GLOBAL, rbGlobal );

  JS_GC( cs->cx );

  JS_RemoveRoot( cs->cx, &jsGlobal );

  return Qnil;
}

// Return the JavaScript version as a Ruby string.
static VALUE
rb_smjs_context_get_version( VALUE self ){
  return rb_str_new2( JS_VersionToString( JS_GetVersion( RBSMContext_TO_JsContext( self ) ) ) );
}

static VALUE
rb_smjs_context_shutdown( VALUE self ){
  sSMJS_Context* cs;
  Data_Get_Struct( self, sSMJS_Context, cs );
  rbsm_destroy_context( cs );
  return Qnil;
}

// Set the JavaScript version from a Ruby string value.
static VALUE
rb_smjs_context_set_version( VALUE self, VALUE sver ){
  JSVersion v;
  
  v = JS_StringToVersion( StringValuePtr( sver ) );
  if( v == JSVERSION_UNKNOWN )
    rb_raise( eJSError, "Unknown version" );
  JS_SetVersion( RBSMContext_TO_JsContext( self ), v );
  return self;
}

// Is the context running?
static VALUE
rb_smjs_context_is_running( VALUE self ){
  return JS_IsRunning( RBSMContext_TO_JsContext( self ) ) ? Qtrue : Qfalse;
}

// Run GC
static VALUE
rb_smjs_context_gc( VALUE self ){
  JS_GC( RBSMContext_TO_JsContext( self ) ); 
  return Qnil;
}

static VALUE
rb_smjs_context_is_constructing( VALUE self ){
  return JS_IsConstructing( RBSMContext_TO_JsContext( self ) ) ? Qtrue : Qfalse; 
}

static VALUE
rb_smjs_context_begin_request( VALUE self ){
#ifdef JS_THREADSAFE
  JS_BeginRequest( RBSMContext_TO_JsContext( self ) );
#endif
  return Qnil;
}

static VALUE
rb_smjs_context_suspend_request( VALUE self ){
#ifdef JS_THREADSAFE
  JS_SuspendRequest( RBSMContext_TO_JsContext( self ) );
#endif
  return Qnil;
}

static VALUE
rb_smjs_context_resume_request( VALUE self ){
#ifdef JS_THREADSAFE
  JS_ResumeRequest( RBSMContext_TO_JsContext( self ) );
#endif
  return Qnil;
}

static VALUE
rb_smjs_context_end_request( VALUE self ){
#ifdef JS_THREADSAFE
  JS_EndRequest( RBSMContext_TO_JsContext( self ) );
#endif
  return Qnil;
}

// Context and ability to eval
static VALUE
rb_smjs_context_delegate_global( int argc, VALUE* argv, VALUE self ){
  return rb_funcall3( rb_smjs_context_get_global( self ), SYM2ID( argv[0] ), argc - 1, argv + 1 );
}

// SpiderMonkey --------------------------------------------------------
// SpiderMonkey can eval
static VALUE
rbsm_spidermonkey_delegate_default_context( int argc, VALUE* argv, VALUE self ){
  return rb_funcall3( rb_iv_get( self, RBSMJS_DEFAULT_CONTEXT ), SYM2ID( argv[0] ), argc - 1, argv + 1 );
}

// thread safe?
static VALUE
rbsm_spidermonkey_is_threadsafe( VALUE self ){
#ifdef JS_THREADSAFE
  return Qtrue;
#else
  return Qfalse;
#endif
}
// Runtime -------------------------------------------------------------
static void 
smjs_runtime_release( ){
  JS_DestroyRuntime( gSMJS_runtime );
  JS_ShutDown( );
  trace( "runtime free" );
}

static void 
smjs_runtime_init( ){
  gSMJS_runtime = JS_NewRuntime( JS_RUNTIME_MAXBYTES );
  if( !gSMJS_runtime )
    rb_raise( eJSError, "Failed to create runtime" );
  atexit( (void (*)(void))smjs_runtime_release );
}

void Init_spidermonkey( ){
  rb_require("date");
  rb_cDate = rb_const_get( rb_cObject, rb_intern( "Date" ) );

  smjs_runtime_init( );
  memcpy( &rbsm_FunctionOps, &js_ObjectOps, sizeof( JSObjectOps ) );
  cSMJS = rb_define_class( "SpiderMonkey", rb_cObject );
  rb_define_const( cSMJS, "LIB_VERSION", rb_obj_freeze ( rb_str_new2( JS_GetImplementationVersion( ) ) ) ); 
  rb_define_singleton_method( cSMJS, "method_missing", rbsm_spidermonkey_delegate_default_context, -1 );
  rb_define_singleton_method( cSMJS, "threadsafe?", rbsm_spidermonkey_is_threadsafe, 0 );

  eJSError = rb_define_class_under( cSMJS, "Error", rb_eStandardError );
  eJSConvertError = rb_define_class_under( cSMJS, "ConvertError", eJSError );
  eJSEvalError = rb_define_class_under( cSMJS, "EvalError", eJSError );
  rb_define_method( eJSEvalError, "lineno", rbsm_evalerror_lineno, 0 );
  rb_define_method( eJSEvalError, "message", rbsm_evalerror_message, 0 );
  rb_define_method( eJSEvalError, "error_number", rbsm_evalerror_error_number, 0 );
  rb_define_method( eJSEvalError, "js_error", rbsm_evalerror_js_error, 0 );

  cJSContext = rb_define_class_under( cSMJS, "Context", rb_cObject );
  rb_define_alloc_func( cJSContext, rb_smjs_context_alloc );
  rb_define_private_method( cJSContext, "initialize", rb_smjs_context_initialize, -1 );
  rb_define_method( cJSContext, "method_missing", rb_smjs_context_delegate_global, -1 );
  rb_define_method( cJSContext, "running?", rb_smjs_context_is_running, 0 );
  rb_define_method( cJSContext, "shutdown", rb_smjs_context_shutdown, 0 );
  rb_define_method( cJSContext, "flush", rb_smjs_context_flush, 0 );
  rb_define_method( cJSContext, "version", rb_smjs_context_get_version, 0 );
  rb_define_method( cJSContext, "version=", rb_smjs_context_set_version, 1 );
  rb_define_method( cJSContext, "global", rb_smjs_context_get_global, 0 );
  rb_define_method( cJSContext, "gc", rb_smjs_context_gc, 0 );
  rb_define_method( cJSContext, "constructing?", rb_smjs_context_is_constructing, 0 );
  rb_define_method( cJSContext, "begin_request", rb_smjs_context_begin_request, 0 );
  rb_define_method( cJSContext, "suspend_request", rb_smjs_context_suspend_request, 0 );
  rb_define_method( cJSContext, "resume_request", rb_smjs_context_resume_request, 0 );
  rb_define_method( cJSContext, "end_request", rb_smjs_context_end_request, 0 );
  rb_define_method( cJSContext, "scope_chain", rb_smjs_context_get_scope_chain, 0 );

  cJSValue = rb_define_class_under( cSMJS, "Value", rb_cObject );
  rb_define_private_method( cJSValue, "initialize", rb_smjs_value_initialize, 0 );
  rb_define_method( cJSValue, "eval", rb_smjs_value_eval, -1 );
  rb_define_method( cJSValue, "evalget", rb_smjs_value_evalget, -1 );
  rb_define_method( cJSValue, "evaluate", rb_smjs_value_evaluate, -1 );
  rb_define_method( cJSValue, "to_ruby", rb_smjs_value_to_ruby, 0 );
  rb_define_method( cJSValue, "to_s", rb_smjs_value_to_s, 0 );
  rb_define_method( cJSValue, "to_a", rb_smjs_value_to_a, 0 );
  rb_define_method( cJSValue, "to_h", rb_smjs_value_to_h, 0 );
  rb_define_method( cJSValue, "to_ruby", rb_smjs_value_to_ruby, 0 );
  rb_define_method( cJSValue, "to_f", rb_smjs_value_to_f, 0 );
  rb_define_method( cJSValue, "to_i", rb_smjs_value_to_i, 0 );
  rb_define_method( cJSValue, "to_num", rb_smjs_value_to_num, 0 );
  rb_define_method( cJSValue, "to_bool", rb_smjs_value_to_bool, 0 );
  rb_define_method( cJSValue, "function", rb_smjs_value_function, -1 );
  rb_define_method( cJSValue, "call_function", rb_smjs_value_call_function, -1 );
  rb_define_method( cJSValue, "typeof", rb_smjs_value_typeof, 0 );
  rb_define_method( cJSValue, "set_property", rb_smjs_value_set_property, 2 );
  rb_define_method( cJSValue, "get_property", rb_smjs_value_get_property, 1 );
  rb_define_method( cJSValue, "get_properties", rb_smjs_value_get_properties, 0 );
  rb_define_method( cJSValue, "get_context", rb_smjs_value_get_context, 0 );
  rb_define_method( cJSValue, "get_jsvalid", rb_smjs_value_get_jsvalid, 0 );
  rb_define_method( cJSValue, "each", rb_smjs_value_each, 0 );
  rb_define_method( cJSValue, "each_with_index", rb_smjs_value_each_with_index, 0 );
  rb_define_method( cJSValue, "call", rb_smjs_value_call, 0 );
  rb_define_method( cJSValue, "get_parent", rb_smjs_value_get_parent, 0 );
  rb_define_method( cJSValue, "get_prototype", rb_smjs_value_get_prototype, 0 );
  //rb_include_module( cJSValue, rb_const_get( rb_cObject, rb_intern("Enumerable" ) ) );

  cJSFunction = rb_define_class_under( cSMJS, "Function", rb_cObject );
  rb_define_class_variable( cSMJS, RBSMJS_DEFAULT_CONTEXT, rb_funcall( cJSContext, rb_intern("new" ), 0 ) );
}

