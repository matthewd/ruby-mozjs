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

// デフォルトのスタックサイズ : Default stack size
#define JS_STACK_CHUNK_SIZE    16384

// SETTING THIS TOO LOW RESULTS IN SEGFAULTS!
// More specifically, having the runtime actually reach its maximum
// memory allocation appears to cause segfaults (JS_NewObject returns a
// bad pointer, then JS_DefineFunction segfaults). It is thus quite
// important to call JS_GC frequently... we call JS_MaybeGC before
// evaluating a script to reduce the problem.
#define JS_RUNTIME_MAXBYTES   0x3000000L

#define RBSMJS_DEFAULT_CONTEXT "@@defaultContext"
#define RBSMJS_VALUES_CONTEXT "@context"
#define RBSMJS_CONTEXT_GLOBAL "global"
#define RBSMJS_CONTEXT_BINDINGS "bindings"

#define RBSM_CONVERT_SHALLOW 1
#define RBSM_CONVERT_DEEP    0


#ifdef WIN32
# ifdef DEBUG
#  define trace printf("\n"); printf
# else
#  define trace(msg) 
# endif
#elif DEBUG
# define trace(format, ...) do { printf(format, ## __VA_ARGS__); printf("\n"); } while (0)
#else
# define trace(format, ...) 
#endif


#define ZERO_ARITY_METHOD_IS_PROPERTY

VALUE eJSError;
VALUE eJSConvertError;
VALUE eJSEvalError;
VALUE cJSValue;
VALUE cJSContext;
VALUE cJSFunction;
VALUE cSMJS;
JSRuntime* gSMJS_runtime;

#ifdef DEBUG
int alloc_count_js2rb;
int alloc_count_rb2js;
#endif

// RubyObject/RubyFunction が持つ情報 : Properties
typedef struct{
	VALUE rbobj;
	jsval jsv;
}sSMJS_Class;

// RubyException エラー : Error
typedef struct{
	int status;
	jsval erval;
	VALUE errinfo;
}sSMJS_Error;

// SpiderMonkey::Context -- インスタンスが持つ情報を格納した構造体
//                       -- Structure containing instance data
typedef struct{
	JSContext* cx;
	jsval last_exception; // 最後のJS例外 : Last JS Exception
	char last_message[BUFSIZ]; // 最後のエラーメッセージ : Last Error Message
	JSObject* store; // 対JS-GC用store : "For opposite JS-GC store"
	JSHashTable* id2rbval; // JS-VALUE値とRuby-Object#__id__の対 : "JS-VALUE value and Ruby-Object#__id__[no] opposite"
}sSMJS_Context;

// SpiderMonkey::Value -- インスタンスが持つ情報を格納した構造体 
//                     -- Structure containing instance data
typedef struct{
	jsval value;
	sSMJS_Context* cs;
}sSMJS_Value;

// each 用に必要な情報を持つ構造体 
// "The structure which has the information which is necessary for business"
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
static JSBool rbsm_class_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static JSBool rbsm_class_set_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static JSBool rbsm_error_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp );
static void rbsm_class_finalize ( JSContext* cx, JSObject* obj );
static JSBool rb_smjs_value_object_callback( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval );
static JSBool rb_smjs_value_function_callback( JSContext* cx, JSObject* thisobj, uintN argc, jsval* argv, jsval* rval );
static JSObject* rbsm_proc_to_function( JSContext* cx, VALUE proc );
static void rbsm_rberror_finalize ( JSContext* cx, JSObject* obj );
static JSObject* rbsm_ruby_to_jsobject( JSContext* cx, VALUE obj );
static JSBool rbsm_rubyexception_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval );
static VALUE rb_smjs_convertvalue( JSContext* cx, jsval value );
static VALUE rb_smjs_convert_prim( JSContext* cx, jsval value );
static JSBool rbsm_rubystring_to_jsval( JSContext* cx, VALUE rval, jsval* jval );
static JSBool rbsm_get_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj );
static VALUE rbsm_evalerror_new_jsval( VALUE context, jsval jsvalerror );
static VALUE rb_smjs_value_new_jsval( VALUE context, jsval value );
static VALUE rb_smjs_value_get_prototype( VALUE self );
static void* rbsm_each( JSContext* cx, jsval value, RBSMJS_YIELD yield, void* data );
static JSObjectOps* rbsm_class_get_object_ops( JSContext* cx, JSClass* clasp );
static void rb_smjs_context_errorhandle( JSContext* cx, const char* message, JSErrorReport* report );

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

static JSClass global_class = {
	"global", JSCLASS_NEW_RESOLVE,
	JS_PropertyStub,  JS_PropertyStub,
	JS_PropertyStub,  JS_PropertyStub,
	JS_EnumerateStub, JS_ResolveStub,
	JS_ConvertStub,   JS_FinalizeStub,
JSCLASS_NO_OPTIONAL_MEMBERS     
};

static JSClass JSRubyObjectClass = {
	"RubyObject", JSCLASS_HAS_PRIVATE,
	/*addp*/JS_PropertyStub,  /*delpr*/JS_PropertyStub,
	/*getp*/rbsm_class_get_property,   /*setp*/rbsm_class_set_property,
	JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, rbsm_class_finalize,
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
/*
=begin
 JS変数をRubyのStringオブジェクトに変更
 Convert a JavaScript variable to a Ruby String
=end
*/
static VALUE 
rb_smjs_to_s( JSContext* cx, jsval value ){
	JSString* str = JS_ValueToString( cx, value );
	return rb_str_new( JS_GetStringBytes( str ), JS_GetStringLength( str ) );
}

static VALUE 
rb_smjs_to_bool( JSContext* cx, jsval value ){
	JSBool bp;
	if( !JS_ValueToBoolean( cx, value, &bp ) ){
		rb_raise( eJSConvertError, "can't convert to boolean" );
	}
	return bp ? Qtrue : Qfalse;
}

static VALUE 
rb_smjs_to_a( JSContext* cx, jsval value, int shallow ){
	VALUE ary;
	VALUE r;
	if( JSVAL_IS_OBJECT( value ) ){
		JSObject* jo = JSVAL_TO_OBJECT( value );
		if( JSVAL_IS_NULL( value ) ) return rb_ary_new( );
		if( JS_IsArrayObject( cx, jo ) ){
			jsuint length;
			if( JS_HasArrayLength( cx, jo, &length ) ){
				jsuint i;
				ary = rb_ary_new2( length );
				for( i = 0 ; i < length ; i++ ){
					jsval v;
					if( JS_GetElement( cx, jo, i, &v ) ){
						VALUE va;
						if( shallow ){
							va = rb_smjs_convert_prim( cx, v );
						}else{
							va = rb_smjs_convertvalue( cx, v ); 
						}
						rb_ary_store( ary, i, va );
					}else{
						rb_raise( eJSConvertError, "Fail convert to array[]" );
					}
				}
				return ary;
			}
		}
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
		rb_raise( eJSConvertError, "can't convert to hash from no object" );
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
	if( JS_ValueToInt32( cx, value, &ip ) ){
		return INT2NUM( ip );
	}else{
		rb_raise( eJSConvertError, "can't convert object to i" );
	}
}

static VALUE 
rb_smjs_to_f( JSContext* cx, jsval value ){
	jsdouble d;
	if( JS_ValueToNumber( cx, value, &d ) ){
		return rb_float_new( d );
	}else{
		rb_raise( eJSConvertError, "can't convert object to float" );
	}
}

static VALUE 
rb_smjs_to_num( JSContext* cx, jsval value ){
	if( JSVAL_IS_INT( value ) ){
		return rb_smjs_to_i( cx, value );
	}else if( JSVAL_IS_DOUBLE( value ) ){
		return rb_smjs_to_f( cx, value );
	}else{
		rb_raise( eJSConvertError, "can't convert to num" );
	}
}

static VALUE 
rb_smjs_convertvalue( JSContext* cx, jsval value ){
	JSObject* jo;
	sSMJS_Class* so;
	JSType t = JS_TypeOfValue( cx, value );
	switch( t ){
	case JSTYPE_VOID: return Qnil;
	case JSTYPE_STRING:  return rb_smjs_to_s( cx, value );
	case JSTYPE_BOOLEAN: return rb_smjs_to_bool( cx, value );
	case JSTYPE_OBJECT:
		if( JSVAL_IS_NULL( value ) ) return Qnil;
		jo = JSVAL_TO_OBJECT( value );
		so = JS_GetInstancePrivate( cx, jo, &JSRubyObjectClass, NULL );
		if( so ){
			return so->rbobj;
		}
		if( JS_IsArrayObject( cx, JSVAL_TO_OBJECT( value ) ) )
			return rb_smjs_to_a( cx, value, RBSM_CONVERT_DEEP );
		return rb_smjs_to_h( cx, value, RBSM_CONVERT_DEEP );
	case JSTYPE_NUMBER:
		return rb_smjs_to_num( cx, value );
	case JSTYPE_FUNCTION:
		rb_raise ( eJSConvertError, "function no support [%s]", JS_GetStringBytes(JS_ValueToString(cx, value)) ); break;
	default:
		rb_raise ( eJSConvertError, "object not supported type" );
		break;
	}
}

// Rubyの文字列を jsvalへ変換 
// Convert a Ruby string to a JavaScript value.
static JSBool
rbsm_rubystring_to_jsval( JSContext* cx, VALUE rval, jsval* jval ){
	JSString* jsstr;
	if( (jsstr = JS_NewStringCopyZ( cx, StringValuePtr( rval ) ) ) ){
		*jval = STRING_TO_JSVAL( jsstr );
		return JS_TRUE;
	}
	return JS_FALSE;
}

// Rubyの変数VALUE rval を jsval jval へ変換 
// Convert a Ruby value to a JavaScript value.
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
	case T_FIXNUM: //return INT_TO_JSVAL( NUM2INT( rval ) ); 
	case T_FLOAT: //return INT_TO_JSVAL( NUM2INT( rval ) ); 
	case T_BIGNUM: return JS_NewDoubleValue( cx, NUM2DBL( rval ), jval );
	case T_TRUE:  *jval = JSVAL_TRUE; break;
	case T_FALSE: *jval = JSVAL_FALSE; break;
	case T_NIL:   *jval = JSVAL_NULL; break;
	default:
		*jval = OBJECT_TO_JSVAL( rbsm_ruby_to_jsobject( cx, rval ) );
		//rb_raise( rb_eTypeError, "cant convert type" );
	}
	return JS_TRUE;
}

// 適度コンバートする 適度=プリミティブ値、nullのみ 
// Convert a JavaScript value to a Ruby value.
static VALUE 
rb_smjs_convert_prim( JSContext* cx, jsval value ){
	JSType t = JS_TypeOfValue( cx, value );
	JSObject* jo;
	sSMJS_Class* so;
	VALUE context;
	switch( t ){
	case JSTYPE_VOID:    return Qnil;
	case JSTYPE_STRING:  return rb_smjs_to_s( cx, value );
	case JSTYPE_BOOLEAN: return rb_smjs_to_bool( cx, value );
	case JSTYPE_NUMBER:  return rb_smjs_to_num( cx, value );
	case JSTYPE_OBJECT:
		if( JSVAL_IS_NULL( value ) ) return Qnil;
		jo = JSVAL_TO_OBJECT( value );
		so = JS_GetInstancePrivate( cx, jo, &JSRubyObjectClass, NULL );
		if( so ){
			return so->rbobj;
		}
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

// Ruby例外をJS例外として投げる 
// Throw a Ruby exception as a JavaScript exception.
static JSBool
rb_smjs_raise_js( JSContext* cx, int status ){
	sSMJS_Error* se;
	JSObject* jo;

	VALUE context = RBSMContext_FROM_JsContext( cx );
	sSMJS_Context* cs;
	Data_Get_Struct( context, sSMJS_Context, cs );

	se = JS_malloc( cx, sizeof( sSMJS_Error ) );
	se->status = status;
	se->errinfo = rb_obj_dup( rb_gv_get( "$!" ) );
	jo = JS_NewObject( cx, &JSRubyExceptionClass, NULL, NULL );
	JS_SetPendingException( cx, OBJECT_TO_JSVAL( jo ) );
	JS_DefineFunctions( cx, jo, JSRubyExceptionFunctions );
	JS_SetPrivate( cx, jo, (void*)se );
	cs->last_exception = OBJECT_TO_JSVAL( jo );

	return JS_FALSE;
}

// JS例外をRuby例外として投げる 
// Throw a JavaScript exception as a Ruby exception.
static void
rb_smjs_raise_ruby( JSContext* cx ){
	sSMJS_Error* se;
	JSObject* jo;
	jsval jsvalerror = 0;
	VALUE context = RBSMContext_FROM_JsContext( cx );
	VALUE self;

	if( !(JS_IsExceptionPending( cx ) && JS_GetPendingException( cx, &jsvalerror ) ) ){
		sSMJS_Context* cs;
		char tmpmsg[BUFSIZ];

		Data_Get_Struct( context, sSMJS_Context, cs );
		jsvalerror = cs->last_exception;

		if( !jsvalerror ){
			rb_raise( eJSError, "invalid: %s", cs->last_message );
		}

		cs->last_exception = 0;
		strncpy( tmpmsg, cs->last_message, BUFSIZ );
		sprintf( cs->last_message, "Rubified: %s", tmpmsg );
	}

	JS_ClearPendingException( cx );

	if( !JSVAL_IS_OBJECT( jsvalerror ) )
		rb_raise( eJSError, "invalid2: jsvalerror is not an object" );

	jo = JSVAL_TO_OBJECT( jsvalerror );
	if( !jo )
		rb_raise( eJSError, "invalid3:" );

	// 元がRuby例外ならそれを継続 
	// If it was originally a Ruby exception, we continue that.
	se = JS_GetInstancePrivate( cx, jo, &JSRubyExceptionClass, NULL );
	if( se ){
		int st = se->status;
		se->status = 0;
		rb_jump_tag( st );
	}
	// 元がJS例外なら EvalError を作成してRubyに投げる 
	// If the exception originated with JavaScript, we build and throw an
	// EvalError to Ruby.
	self = rbsm_evalerror_new_jsval( context, jsvalerror );
	
	rb_exc_raise( self );
}

static const JSErrorFormatString*
rbsm_GetErrorMessage( void* userRef, const char* locale, const uintN errorNumber ){
	return &rbsm_ErrorFormatString[errorNumber];
}

// JS例外として投げられたRuby例外の文字化 
// String conversion of a Ruby exception that has been thrown as a
// JavaScript exception.
static JSBool 
rbsm_rubyexception_to_string( JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval ){
	sSMJS_Error* se;
	VALUE msg;
	se = JS_GetInstancePrivate( cx, obj, &JSRubyExceptionClass, NULL );
	if( !se ){ 
		// TODO: 正しい関数名、オブジェクト名を出す 
		// TODO: Output the correct object and function names
		JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyException", "toString", "Object" );
		return JS_FALSE;
	}
	msg = rb_funcall( se->errinfo, rb_intern( "to_s" ), 0 );
	rbsm_rubystring_to_jsval( cx, msg, rval );
	return JS_TRUE;
}

// jsval -> RubyObject のハッシュテーブル : Hash Table
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
	// JS-GC対策用storeに値を設定 : Setting value to the one for JS-GC measure store
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
	JS_MaybeGC( cs->cx );
	ok = JS_EvaluateScript( cs->cx, obj, source, strlen( source ), filename, lineno, &value );
	if( !ok ) rb_smjs_raise_ruby( cs->cx );
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
	
	enm = &enmdata;
	enm->data = data;
	enm->cx = cx;
	enm->ida = NULL;
	enm->obj = JSVAL_TO_OBJECT( value );
	if( JSVAL_IS_PRIMITIVE( value ) ){
		if( JSVAL_IS_STRING( value ) ){
			return enm->data;
			//rb_raise ( eJSConvertError, "can't enumerate" );
		}else{
			return enm->data;
		}
	}else{
		enm->ida = JS_Enumerate( enm->cx, enm->obj );
	}
	if( !enm->ida ){
		rb_raise( eJSConvertError, "can't enumerate" );
	}

	for( enm->i = 0; enm->i < enm->ida->length; enm->i++ ){
		enm->id = enm->ida->vector[enm->i];
		if( JS_IdToValue( enm->cx, enm->id, &enm->key ) ){
			//enm->keystr = JS_GetStringBytes( JS_ValueToString( cx, enm->key ) );
			//if( JS_GetProperty( enm->cx, enm->obj, enm->keystr, &enm->val ) ){
			if( OBJ_GET_PROPERTY( enm->cx, enm->obj, enm->id, &enm->val ) ){
				yield( enm );
			}else{
				rbsm_each_raise( enm, "can't get property" );
			}
		}else{
			rbsm_each_raise( enm, "can't get key name" );
		}
	}

	JS_DestroyIdArray( enm->cx, enm->ida );
	jo = JS_GetPrototype( enm->cx, enm->obj );
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
	rb_yield( rb_smjs_convert_prim( enm->cx, enm->val ) );
}

static void
rb_smjs_value_each_with_index_yield( sSMJS_Enumdata* enm ){
	rb_yield_values( 2, rb_smjs_convertvalue( enm->cx,  enm->key ), rb_smjs_convert_prim(  enm->cx,  enm->val ) );
}

// protect 内で proc を呼ぶ。引数 args は rb_ary で、最後に proc が入っている 
// Calls a Proc from inside protect; args is a Ruby array with the Proc
// appended.
static VALUE
rb_smjs_ruby_proc_caller( VALUE args ){
	// Procを実行 : The Proc to execute
	VALUE proc;
	proc = rb_ary_pop( args );
	return rb_apply( proc, rb_intern( "call" ), args );
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

// Ruby Object#[]
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
	
	// 引数をSpiderMonkey::Valueに : Argument in SpiderMonkey::Value
	rargs = rb_ary_new2( argc + 1 );
	for( i = 0 ; i < argc ; i++ )
		rb_ary_store( rargs, i, rb_smjs_convert_prim( cx, argv[i] ) );
	rb_ary_store( rargs, i, so->rbobj );
	
	// proc を実行 : The Proc to execute
	res = rb_protect( rb_smjs_ruby_box_caller, rargs, &status );
	
	// Ruby関数実行結果、例外が投げられた場合はJS例外にラップして投げる 
	// Check the Ruby function execution result; if an exception was
	// thrown, we raise a corresponding error in JavaScript.
	if( status != 0 )
		return rb_smjs_raise_js( cx, status );
	
	return rb_smjs_ruby_to_js( cx, res, rval );
}

// Ruby proc/method がJavaScriptから呼ばれた
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
	
	// 引数をSpiderMonkey::Valueに : Argument in SpiderMonkey::Value
	rargs = rb_ary_new2( argc + 1 );
	for( i = 0 ; i < argc ; i++ )
		rb_ary_store( rargs, i, rb_smjs_convert_prim( cx, argv[i] ) );
	rb_ary_store( rargs, i, so->rbobj );
	
	// proc を実行 : The Proc to execute
	res = rb_protect( rb_smjs_ruby_proc_caller, rargs, &status );
	
	// Ruby関数実行結果、例外が投げられた場合はJS例外にラップして投げる 
	// Check the Ruby function execution result; if an exception was
	// thrown, we raise a corresponding error in JavaScript.
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
	char* keyname;
	sSMJS_Class* so;

	VALUE rargs, res;
	int status;

	keyname = JS_GetStringBytes( JS_ValueToString( cx, argv[0] ) );
	trace("_noSuchMethod__( %s )", keyname );
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

	// 引数をSpiderMonkey::Valueに : Argument in SpiderMonkey::Value
	rargs = rb_smjs_convertvalue( cx, argv[1] );
	rb_ary_unshift( rargs, ID2SYM( rb_intern( keyname ) ) );
	rb_ary_push( rargs, so->rbobj );
	
	// proc を実行 : The Proc to execute
	res = rb_protect( rb_smjs_ruby_missing_caller, rargs, &status );
	
	// Ruby関数実行結果、例外が投げられた場合はJS例外にラップして投げる 
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
	se = JS_GetInstancePrivate( cx, obj, &JSRubyExceptionClass, NULL );
	if( !se ){
		// TODO: 正しい関数名、オブジェクト名を出す 
		// TODO: Output the correct object and function names
		char* keyname = JS_GetStringBytes( JS_ValueToString( cx, id ) );
		JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
		return JS_FALSE;
	}
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

// Ruby のProcを JSのfunctionとしてラップして返す 
// Convert a Ruby Proc to a JavaScript function
static JSObject*
rbsm_proc_to_function( JSContext* cx, VALUE proc ){
	JSObject* jo;
	sSMJS_Class* so;
	
	so = rbsm_wrap_class( cx, proc );
	jo = JS_NewObject( cx, &JSRubyFunctionClass,  NULL, NULL ); 
	trace("rbsm_proc_to_function(obj=%x); [count %d -> %d]", jo, alloc_count_rb2js, ++alloc_count_rb2js);
	so->jsv = OBJECT_TO_JSVAL( jo );
	JS_SetPrivate( cx, jo, (void*)so );
	return jo;
}

static JSBool
rbsm_get_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj ){
	char* keyname;
	ID rid;
	VALUE method;
	int iarity;
	VALUE ret;
	keyname = JS_GetStringBytes( JS_ValueToString( cx, id ) );

	trace( "_get_property_( %s )", keyname );
	rid = rb_intern( keyname );
	// TODO: int rb_respond_to( VALUE obj, ID id )
	if( rb_is_const_id( rid ) && 
			rb_funcall( rbobj, rb_intern( "respond_to?" ), 1, ID2SYM( rb_intern( "constants" ) ) ) &&
			rb_const_defined( rbobj, rid ) ){
		// 定数はその値を返す 
		// Constant returns the value
		return rb_smjs_ruby_to_js( cx, rb_const_get( rbobj, rid ), vp );
	}else if( rb_funcall( rbobj, rb_intern( "respond_to?" ), 1, ID2SYM( rid ) ) ){
		method = rb_funcall( rbobj, rb_intern( "method" ), 1, ID2SYM( rid ) );
#ifdef ZERO_ARITY_METHOD_IS_PROPERTY
		// メソッドの引数の数を調べる 
		// We check the number of arguments the method takes
		iarity = NUM2INT( rb_funcall( method, rb_intern( "arity" ), 0 ) );
		if( iarity == 0 /*|| iarity == -1*/ ){
			JSBool success;
			// 引数0のメソッドはプロパティーとして値を取得 
			// A method with 0 arguments directly returns the value, acting as a property
			ret = rb_funcall( method, rb_intern( "call" ), 0 );
			//ret = rb_funcall( rbobj, rid, 0 );
			success = rb_smjs_ruby_to_js( cx, ret, vp );
			g_last0arity.keyname = keyname == "ID" ? "id" : keyname;
			g_last0arity.val = *vp;
			return success;
		}else{
			// 引数0以上、あるいは可変引数のメソッドはfunctionオブジェクトにして返す 
			// Methods with more than zero arguments (or a variable number)
			// result in a JavaScript function object.
			*vp = OBJECT_TO_JSVAL( rbsm_proc_to_function( cx, method ) );
		}
#else
		*vp = OBJECT_TO_JSVAL( rbsm_proc_to_function( cx, method ) );
#endif
	}
	return JS_TRUE;
}

static JSBool
rbsm_set_ruby_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp, VALUE rbobj ){
	char* pkeyname;
	char keyname[BUFSIZ];
	ID rid;
	int status;
	VALUE vals = rb_ary_new2( 3 );
	
	pkeyname = JS_GetStringBytes( JS_ValueToString( cx, id ) );
	sprintf( keyname, "%s=", pkeyname );
	rid = rb_intern( keyname );
	// 引数をSpiderMonkey::Valueに : Argument in SpiderMonkey::Value
	rb_ary_push( vals, rb_smjs_convert_prim( cx, vp[0] ) );
	rb_ary_push( vals, rid );
	rb_ary_push( vals, rbobj );
	// proc を実行 : The Proc to execute
	rb_protect( rb_smjs_ruby_proc_caller2, vals, &status );
	if( status != 0 )
		return rb_smjs_raise_js( cx, status );
	return JS_TRUE;
}

static JSBool
rbsm_class_get_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
	sSMJS_Class* so;

	so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
	if( !so ){
		char* keyname = JS_GetStringBytes( JS_ValueToString( cx, id ) );
		// TODO: 正しい関数名、オブジェクト名を出す 
		// TODO: Output the correct object and function names
		JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
		return JS_FALSE;
	}
	return rbsm_get_ruby_property( cx, obj, id, vp, so->rbobj );
}

static JSBool
rbsm_class_set_property( JSContext* cx, JSObject* obj, jsval id, jsval* vp ){
	sSMJS_Class* so = JS_GetInstancePrivate( cx, obj, &JSRubyObjectClass, NULL );
	if( !so ){
		char* keyname = JS_GetStringBytes( JS_ValueToString( cx, id ) );
		// TODO: 正しい関数名、オブジェクト名を出す 
		// TODO: Output the correct object and function names
		JS_ReportErrorNumber( cx, rbsm_GetErrorMessage, NULL, RBSMMSG_INCOMPATIBLE_PROTO, "RubyObject", keyname, "Object" );
		return JS_FALSE;
	}
	return rbsm_set_ruby_property( cx, obj, id, vp, so->rbobj );
}

// rubyobj obj を javascript で使えるようにラップする 
// To access a Ruby object from JavaScript, we wrap it.
static JSObject*
rbsm_ruby_to_jsobject( JSContext* cx, VALUE obj ){
	JSObject* jo;
	sSMJS_Value* sv;
	sSMJS_Class* so;
	
	if( rb_obj_is_kind_of( obj, cJSValue ) ){
		Data_Get_Struct( obj, sSMJS_Value, sv );
		return JSVAL_TO_OBJECT( sv->value );
	}
	so = rbsm_wrap_class( cx, obj );
	jo = JS_NewObject( cx, &JSRubyObjectClass, NULL, NULL ); 
	trace("rbsm_ruby_to_jsobject(obj=%x); [count %d -> %d]", jo, alloc_count_rb2js, ++alloc_count_rb2js);
	so->jsv = OBJECT_TO_JSVAL( jo );
	JS_SetPrivate( cx, jo, (void*)so );
	JS_DefineFunction( cx, jo, "__noSuchMethod__", rbsm_class_no_such_method, 2, 0 );
	return jo;
}

// RubyObjectClass のデストラクタ : RubyObjectClass Destructor
static void
rbsm_class_finalize( JSContext* cx, JSObject* obj ){
	sSMJS_Class* so;
	
	so = (sSMJS_Class*)JS_GetPrivate( cx, obj );
	if( so ){
		trace("rbsm_class_finalize(obj=%x); [count %d -> %d]", obj, alloc_count_rb2js, --alloc_count_rb2js);
		if( so->rbobj ){
			if( JS_GetContextPrivate( cx ) ){
				VALUE context = RBSMContext_FROM_JsContext( cx );
				VALUE bindings = rb_iv_get( context, RBSMJS_CONTEXT_BINDINGS );
				if( RTEST( bindings ) ){
					rb_hash_delete( rb_iv_get( context, RBSMJS_CONTEXT_BINDINGS ), rb_obj_id( so->rbobj ) );
				}
			}
		}
		if( so->jsv ){
			JS_RemoveRoot( cx, &(so->jsv) );
		}
		free( so );
	}
}

// SpiderMonkey::Value ---------------------------------------------------------------------

// SpiderMonkey::Valueのデストラクタ : SpiderMonkey::Value Destructor
static void 
rb_smjs_value_free( sSMJS_Value* sv ){
	trace( "value_free(cx=%x, value=%x); [count %d -> %d]", sv->cs->cx, sv->value, alloc_count_js2rb, --alloc_count_js2rb );
	if( sv->value && sv->cs->store ){
		rbsm_remove_jsval_to_rbval( sv->cs, sv->value );
	}
	free( sv );
}

// Rubyレベルで作成はできないように 
// SpiderMonkey::Value can't be created directly from within Ruby.
static VALUE
rb_smjs_value_initialize( VALUE self, VALUE context ){
	rb_raise( eJSError, "do not create SpiderMonkey::Value" );
	return Qnil;
}

// Valueの実際のコンストラクタ 
// The actual constructor of SpiderMonkey::Value.
static VALUE
rb_smjs_value_new_jsval( VALUE context, jsval value ){
	sSMJS_Context* cs;
	Data_Get_Struct( context, sSMJS_Context, cs );

	if( rbsm_lookup_jsval_to_rbval( cs, value ) ){
		// 過去に同じ値でRubyオブジェクトを作成していた場合、そのオブジェクトを取り出す 
		// When a past Ruby object was created at the same value, we
		// discard our half-constructed object, and use that instead.
		return rbsm_get_jsval_to_rbval( cs, value );
	}else{
		sSMJS_Value* sv;
		VALUE self;

		// 領域の確保 : "Guarantee of territory?"
		self = Data_Make_Struct( cJSValue, sSMJS_Value, 0, rb_smjs_value_free, sv );
		sv->cs = cs;
		sv->value = value;
		trace( "value_new(cx=%x, value=%x); [count %d -> %d]", sv->cs->cx, value, alloc_count_js2rb, ++alloc_count_js2rb );

		// context をインスタンス変数として持つ 
		// It has the context as an instance variable.
		rb_iv_set( self, RBSMJS_VALUES_CONTEXT, context );

		// 過去に作成していなかった場合 
		// If not, we store this object, and then use it.
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

// code を実行し、SpiderMonkey::Valueオブジェクトを返す 
// Execute code; a SpiderMonkey::Value is returned.
static VALUE
rb_smjs_value_eval( int argc, VALUE* argv, VALUE self ){
	sSMJS_Value* sv;
	Data_Get_Struct( self, sSMJS_Value, sv );
	return rb_smjs_convert_prim( sv->cs->cx, rb_smjs_evalscript( sv->cs, JSVAL_TO_OBJECT( sv->value ), argc, argv ) );
}

// code を実行し、SpiderMonkey::Valueオブジェクトを返す 
// Execute code; a SpiderMonkey::Value is returned.
static VALUE
rb_smjs_value_evalget( int argc, VALUE* argv, VALUE self ){
	return rb_smjs_value_new_jsval( rb_smjs_value_get_context( self ), rb_smjs_value_evalscript( argc, argv, self ) );
}

// code を実行し、なるべく近いRuby のオブジェクトを返す 
// Execute code; where possible, the nearest Ruby object is returned.
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

// typeof 文字列で返す 
// Returns the JavaScript typeof this SpiderMonkey::Value as a Ruby string.
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
// Ruby関数をJavaScriptに登録する 
// Registers a Ruby function to JavaScript; takes the function name and
// a Proc as arguments.
static VALUE
rb_smjs_value_function( int argc, VALUE* argv, VALUE self ){
	char* cname;
	JSObject* jo;
	VALUE proc, name;
	sSMJS_Value* sv;

	// 引数の解析 : Analyse the given arguments
	rb_scan_args( argc, argv, "1&", &name, &proc );
	Data_Get_Struct( self, sSMJS_Value, sv );
	cname = StringValuePtr( name );

	jo = rbsm_proc_to_function( sv->cs->cx, proc );
	JS_DefineProperty( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), cname, OBJECT_TO_JSVAL( jo ), NULL, NULL, JSPROP_PERMANENT | JSPROP_READONLY );

	return proc;
}

// プロパティーを設定する 
// Set a property
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
// プロパティー一覧を取り出す 
// Return an array listing the properties of the JavaScript object.
static VALUE
rb_smjs_value_get_properties( VALUE self ){
	VALUE ret;
	
	ret = rb_ary_new( );
	rbsm_value_each( self, rb_smjs_value_get_properties_yield, (void*)ret );
	return ret;
}

// プロパティーを取り出す 
// Return the value for a specified property.
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

// JavaScript関数を呼び出す 
// Call a JavaScript function.
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
	
	pname = StringValuePtr( rargv[0] );
	Data_Get_Struct( self, sSMJS_Value, sv );

	// 引数をJavaScriptの変数に 
	// Convert the arguments from Ruby to JavaScript values.
	//jsval* jargv = _alloca( sizeof( jsval*) * ( argc - 1 ) );
#ifdef WIN32
	jargv = JS_malloc( sv->cs->cx, sizeof( jsval*) * ( argc - 1 ) );
#endif
	for( i = 1 ; i < argc ; i++ ){
		rb_smjs_ruby_to_js( sv->cs->cx, rargv[i], &jargv[i - 1] );
	}

	// 実行 : Execution
	//sv->cs->last_exception = 0;
	ok = JS_CallFunctionName( sv->cs->cx, JSVAL_TO_OBJECT( sv->value ), pname, argc - 1, jargv, &jval );

#ifdef WIN32
	JS_free( sv->cs->cx, jargv );
#endif

	if( !ok ) rb_smjs_raise_ruby( sv->cs->cx );

	return rb_smjs_convert_prim( sv->cs->cx, jval );
}

// SpiderMonkey::EvalError ---------------------------------------------------------------------

static VALUE
rbsm_evalerror_new_jsval( VALUE context, jsval jsvalerror ){
	VALUE self;
	VALUE erval;

	self = rb_funcall( eJSEvalError, rb_intern( "new" ), 1, context );
	erval = rb_smjs_value_new_jsval( context, jsvalerror );
	rb_iv_set( self, "error", erval );
	return self;
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
rb_smjs_context_get_global( VALUE self ){
	return rb_iv_get( self, RBSMJS_CONTEXT_GLOBAL );
}

static VALUE
rb_smjs_context_get_scope_chain( VALUE self ){
	JSContext* cx;
	JSObject* jo;

	cx = RBSMContext_TO_JsContext( self );
	jo = JS_GetScopeChain( cx );
	if( !jo )return Qnil;
	return rb_smjs_convert_prim( cx, OBJECT_TO_JSVAL( jo ) );
}

// Contextインスタンスに関連づけたメモリを解放する 
// Free the memory allocated to this context instance.
static void
rb_smjs_context_free( sSMJS_Context* cs ){
	trace( "context_free" );
	if( cs->cx ){
		//JSコンテキストの破棄 
		// Destruction of the JavaScript context
		JS_SetContextPrivate( cs->cx, 0 );
		JS_RemoveRoot( cs->cx, &(cs->store) );
		cs->store = NULL;
		JS_HashTableDestroy( cs->id2rbval );
		JS_DestroyContext( cs->cx ); 
		//構造体のメモリを解放 
		// Release the structure's memory
	}
	free( cs );
}

// 必要なメモリを確保し、Contextインスタンスにメモリを関連づけ、メモリ解放用の関数を登録する 
// Required memory is allocated, and a function is registered to handle
// the release of the allocated memory.
static VALUE 
rb_smjs_context_alloc( VALUE self ){
	sSMJS_Context* cs;
	// 構造体のメモリを確保する 
	// Allocate the structure's memory
	cs = ALLOC( sSMJS_Context );
	cs->cx = 0;
	// 確保したメモリと、その解放用関数をRubyオブジェクトに関連づけて返す 
	// Allocate the required memory, and register our release handler.
	return Data_Wrap_Struct( self, 0, rb_smjs_context_free, cs );
}

//エラーレポート 
// Handle an error report
static void
rb_smjs_context_errorhandle( JSContext* cx, const char* message, JSErrorReport* report ){
	sSMJS_Context* cs;

	if( JSREPORT_IS_EXCEPTION( report->flags ) ){
		// 例外をContext毎に記録する 
		// The exception is recorded in our context
		VALUE context = RBSMContext_FROM_JsContext( cx );
		Data_Get_Struct( context, sSMJS_Context, cs );
		strncpy( cs->last_message, message, BUFSIZ );
		trace( "error occer (%s)", message );
		JS_GetPendingException( cx, &cs->last_exception );
		trace( "get pending excepiton(%x)", cs->last_exception );
	}
}

// コンテキストの初期化 
// Initialize the Context
static VALUE
rb_smjs_context_initialize( int argc, VALUE* argv, VALUE self ){
	sSMJS_Context* cs;
	JSObject* jsGlobal;
	VALUE rbGlobal;
	int stacksize;
	Data_Get_Struct( self, sSMJS_Context, cs );

	strncpy( cs->last_message, "<<NO MESSAGE>>", BUFSIZ );
	cs->last_exception = 0;

	// コンテキストの作成 
	// Create the underlying JavaScript context
	if( argc == 0 ){
		stacksize = JS_STACK_CHUNK_SIZE;
	}else{
		stacksize = NUM2INT( argv[0] );
	}
	cs->cx = JS_NewContext( gSMJS_runtime, stacksize );
	if( !cs->cx )
		rb_raise( eJSError, "can't create JavaScript context" );

#ifdef JSOPTION_DONT_REPORT_UNCAUGHT
	if( JS_GetOptions( cs->cx ) & JSOPTION_DONT_REPORT_UNCAUGHT == 0 )
		JS_ToggleOptions( cs->cx, JSOPTION_DONT_REPORT_UNCAUGHT );
#endif

	JS_SetContextPrivate( cs->cx, (void*)self );
	
	// ガベレージコレクタのためのマーカー・ハッシュ 
	cs->store = JS_NewObject( cs->cx, NULL, 0, 0 );
	if( !cs->store )
		rb_raise( eJSError, "fail for create object store" );
	JS_AddNamedRoot( cs->cx, &(cs->store), "rbsm-store" );
	cs->id2rbval = JS_NewHashTable( 0, jsid2hash, jsidCompare, rbobjCompare, NULL, NULL );

	// グローバルオブジェクト初期化 
	// Initialize the global JavaScript object
	jsGlobal = JS_NewObject( cs->cx, &global_class, 0, 0 );
	if( !JS_InitStandardClasses( cs->cx, jsGlobal ) )
		rb_raise( eJSError, "can't initialize JavaScript global object" );
	// @global に初期化したグローバルオブジェクトをセット 
	// Set the @global instance variable to hold the global object
	rbGlobal = rb_smjs_value_new_jsval( self, OBJECT_TO_JSVAL( jsGlobal ) );
	rb_iv_set( self, RBSMJS_CONTEXT_GLOBAL, rbGlobal );

	rb_iv_set( self, RBSMJS_CONTEXT_BINDINGS, rb_hash_new( ) );

	// エラーレポーター登録 
	// Register the error report handler
	JS_SetErrorReporter( cs->cx, rb_smjs_context_errorhandle );
	
	return Qnil;
}

// バージョン文字列の取得 
// Return the JavaScript version as a Ruby string.
static VALUE
rb_smjs_context_get_version( VALUE self ){
	return rb_str_new2( JS_VersionToString( JS_GetVersion( RBSMContext_TO_JsContext( self ) ) ) );
}

// バージョン文字列の設定 
// Set the JavaScript version from a Ruby string value.
static VALUE
rb_smjs_context_set_version( VALUE self, VALUE sver ){
	JSVersion v;
	
	v = JS_StringToVersion( StringValuePtr( sver ) );
	if( v == JSVERSION_UNKNOWN )
		rb_raise( eJSError, "unknown version" );
	JS_SetVersion( RBSMContext_TO_JsContext( self ), v );
	return self;
}

// コンテキストが実行中かどうか 
// Determines whether this context is currently executing.
static VALUE
rb_smjs_context_is_running( VALUE self ){
	return JS_IsRunning( RBSMContext_TO_JsContext( self ) ) ? Qtrue : Qfalse;
}

// ガベレージ・コレクションを実行する 
// Invoke the JavaScript GC.
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

// Context に対して eval等ができるように
// We make eval (etc) available on the Context, by forwarding to the
// underlying global object.
static VALUE
rb_smjs_context_delegate_global( int argc, VALUE* argv, VALUE self ){
	return rb_funcall3( rb_smjs_context_get_global( self ), SYM2ID( argv[0] ), argc - 1, argv + 1 );
}

// SpiderMonkey --------------------------------------------------------
// SpiderMonkey に対してeval等ができるように 
// We make eval (etc) available on the global SpiderMonkey constant, by
// forwarding to the "default" Context.
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
		rb_raise( eJSError, "can't create JavaScript runtime" );
	atexit( (void (*)(void))smjs_runtime_release );
}

void Init_spidermonkey( ){
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

