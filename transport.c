/*****************************************************************************
* Lua-RPC library, Copyright (C) 2001 Russell L. Smith. All rights reserved. *
*   Email: russ@q12.org   Web: www.q12.org                                   *
* For documentation, see http://www.q12.org/lua. For the license agreement,  *
* see the file LICENSE that comes with this distribution.                    *
*****************************************************************************/

// Modifications by James Snyder - jbsnyder@fanplastic.org
//  - more generic backend interface to accomodate different link types
//  - integration with eLua (including support for rotables)
//  - extensions of remote global table as local table metaphor
//    - methods to allow remote assignment, getting remote values
//    - accessing and calling types nested multiple levels deep on tables now works
//  - port from Lua 4.x to 5.x

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef __MINGW32__
void *alloca(size_t);
#else
#include <alloca.h>
#endif

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#if !defined( LUA_CROSS_COMPILER ) && !defined( LUARPC_STANDALONE )
#include "platform.h"
#endif

#include "platform_conf.h"

#ifdef LUA_OPTIMIZE_MEMORY
#include "lrotable.h"
#endif

#include "luarpc_rpc.h"


//#ifdef BUILD_RPC

// Support for Compiling with & without rotables 
#ifdef LUA_OPTIMIZE_MEMORY
#define LUA_ISCALLABLE( state, idx ) ( lua_isfunction( state, idx ) || lua_islightfunction( state, idx ) )
#else
#define LUA_ISCALLABLE( state, idx ) lua_isfunction( state, idx )
#endif

#if 0

// Prototypes for Local Functions  
LUALIB_API int luaopen_rpc( lua_State *L );
Handle *handle_create( lua_State *L );


struct exception_context the_exception_context[ 1 ];

static void errorMessage( const char *msg, va_list ap )
{
  fflush( stdout );
  fflush( stderr );
  fprintf( stderr,"\nError: " );
  vfprintf( stderr,msg,ap );
  fprintf( stderr,"\n\n" );
  fflush( stderr );
}

DOGCC(static void panic( const char *msg, ... )
      __attribute__ ((noreturn,unused));)
static void panic (const char *msg, ...)
{
  va_list ap;
  va_start (ap,msg);
  errorMessage (msg,ap);
  exit (1);
}


DOGCC(static void rpcdebug( const char *msg, ... )
      __attribute__ ((noreturn,unused));)
static void rpcdebug (const char *msg, ...)
{
  va_list ap;
  va_start (ap,msg);
  errorMessage (msg,ap);
  abort();
}
#endif
// Lua Types
enum {
  RPC_NIL=0,
  RPC_NUMBER,
  RPC_BOOLEAN,
  RPC_STRING,
  RPC_TABLE,
  RPC_TABLE_END,
  RPC_FUNCTION,
  RPC_FUNCTION_END,
  RPC_REMOTE
};
#if 0
// RPC Commands
enum
{
  RPC_CMD_CALL = 1,
  RPC_CMD_GET,
  RPC_CMD_CON,
  RPC_CMD_NEWINDEX
};

// RPC Status Codes
enum
{
  RPC_READY = 64,
  RPC_UNSUPPORTED_CMD,
  RPC_DONE
};

enum { RPC_PROTOCOL_VERSION = 3 };


// return a string representation of an error number 

static const char * errorString( int n )
{
  switch (n) {
    case ERR_EOF: return "connection closed unexpectedly";
    case ERR_CLOSED: return "operation requested on closed transport";
    case ERR_PROTOCOL: return "error in the received protocol";
    case ERR_COMMAND: return "undefined command";
    case ERR_NODATA: return "no data received when attempting to read";
    case ERR_HEADER: return "header exchanged failed";
    case ERR_LONGFNAME: return "function name too long";
    default: return transport_strerror( n );
  }
}

#endif

// **************************************************************************
// transport layer generics

// read arbitrary length from the transport into a string buffer. 
void transport_read_string( Transport *tpt, const char *buffer, int length )
{
  struct exception e;
  TRANSPORT_VERIFY_READ;
  transport_read_buffer( tpt, ( u8 * )buffer, length );
}


// write arbitrary length string buffer to the transport 
void transport_write_string( Transport *tpt, const char *buffer, int length )
{
  struct exception e;
  TRANSPORT_VERIFY_WRITE;
  transport_write_buffer( tpt, ( u8 * )buffer, length );
}


// read a u8 from the transport 
u8 transport_read_u8( Transport *tpt )
{
  u8 b;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_READ;
  transport_read_buffer( tpt, &b, 1 );
  return b;
}


// write a u8 to the transport 
void transport_write_u8( Transport *tpt, u8 x )
{
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_WRITE;
  transport_write_buffer( tpt, &x, 1 );
}

static void swap_bytes( uint8_t *number, size_t numbersize )
{
  int i;
  for ( i = 0 ; i < numbersize / 2 ; i ++ )
  {
    uint8_t temp = number[ i ];
    number[ i ] = number[ numbersize - 1 - i ];
    number[ numbersize - 1 - i ] = temp;
  }
}

union u32_bytes {
  uint32_t i;
  uint8_t  b[ 4 ];
};

// read a u32 from the transport 
u32 transport_read_u32( Transport *tpt )
{
  union u32_bytes ub;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_READ;
  transport_read_buffer ( tpt, ub.b, 4 );
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )ub.b, 4 );
  return ub.i;
}


// write a u32 to the transport 
void transport_write_u32( Transport *tpt, u32 x )
{
  union u32_bytes ub;
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_WRITE;
  ub.i = ( uint32_t )x;
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )ub.b, 4 );
  transport_write_buffer( tpt, ub.b, 4 );
}

// read a lua number from the transport 
lua_Number transport_read_number( Transport *tpt )
{
  lua_Number x;
  u8 b[ tpt->lnum_bytes ];
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_READ;
  transport_read_buffer ( tpt, b, tpt->lnum_bytes );
  
  if( tpt->net_little != tpt->loc_little )
    swap_bytes( ( uint8_t * )b, tpt->lnum_bytes );
  
  if( tpt->net_intnum != tpt->loc_intnum ) // if we differ on num types, use int
  {
    switch( tpt->lnum_bytes ) // read integer types
    {
      case 1: {
        int8_t y = *( int8_t * )b;
        x = ( lua_Number )y;
      } break;
       case 2: {
        int16_t y = *( int16_t * )b;
        x = ( lua_Number )y;
      } break;
      case 4: {
        int32_t y = *( int32_t * )b;
        x = ( lua_Number )y;
      } break;
      case 8: {
        int64_t y = *( int64_t * )b;
        x = ( lua_Number )y;
      } break;
      default: lua_assert( 0 );
    }
  }
  else
    x = ( lua_Number ) *( lua_Number * )b; // if types match, use native type
    
  return x;
}


// write a lua number to the transport 
void transport_write_number( Transport *tpt, lua_Number x )
{
  struct exception e;
  TRANSPORT_VERIFY_OPEN;
  TRANSPORT_VERIFY_WRITE;
   
  if( tpt->net_intnum )
  {
    switch( tpt->lnum_bytes )
    {
      case 1: {
        int8_t y = ( int8_t )x;
        transport_write_buffer( tpt, ( u8 * )&y, 1 );
      } break;
      case 2: {
        int16_t y = ( int16_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 2 );
        transport_write_buffer( tpt, ( u8 * )&y, 2 );
      } break;
      case 4: {
        int32_t y = ( int32_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 4 );
        transport_write_buffer( tpt,( u8 * )&y, 4 );
      } break;
      case 8: {
        int64_t y = ( int64_t )x;
        if( tpt->net_little != tpt->loc_little )
          swap_bytes( ( uint8_t * )&y, 8 );
        transport_write_buffer( tpt, ( u8 * )&y, 8 );
      } break;
      default: lua_assert(0);
    }
  }
  else
  {
    if( tpt->net_little != tpt->loc_little )
       swap_bytes( ( uint8_t * )&x, 8 );
    transport_write_buffer( tpt, ( u8 * )&x, 8 );
  }
}


/****************************************************************************/
// read and write lua variables to a transport.
//   these functions do little error handling of their own, but they call transport
//   functions which may throw exceptions, so calls to these functions must be
//   wrapped in a Try block.

//static void write_variable( Transport *tpt, lua_State *L, int var_index );
//static int read_variable( Transport *tpt, lua_State *L );

// write a table at the given index in the stack. the index must be absolute
// (i.e. positive).
// @@@ circular table references will cause stack overflow!
static void write_table( Transport *tpt, lua_State *L, int table_index )
{
  lua_pushnil( L );  // push first key
  while ( lua_next( L, table_index ) ) 
  {
    // next key and value were pushed on the stack 
    write_variable( tpt, L, lua_gettop( L ) - 1 );
    write_variable( tpt, L, lua_gettop( L ) );
    
    // remove value, keep key for next iteration 
    lua_pop( L, 1 );
  }
}

static int writer( lua_State *L, const void* b, size_t size, void* B ) {
  (void)L;
  luaL_addlstring((luaL_Buffer*) B, (const char *)b, size);
  return 0;
}

#if defined( LUA_CROSS_COMPILER )  && !defined( LUARPC_STANDALONE )
#include "lundump.h"
#include "ldo.h"

// Dump bytecode representation of function onto stack and send. This
// implementation uses eLua's crosscompile dump to match match the
// bytecode representation to the client/server negotiated format.
static void write_function( Transport *tpt, lua_State *L, int var_index )
{
  TValue *o;
  luaL_Buffer b;
  DumpTargetInfo target;
  
  target.little_endian=tpt->net_little;
  target.sizeof_int=sizeof(int);
  target.sizeof_strsize_t=sizeof(strsize_t);
  target.sizeof_lua_Number=tpt->lnum_bytes;
  target.lua_Number_integral=tpt->net_intnum;
  target.is_arm_fpa=0;
  
  // push function onto stack, serialize to string 
  lua_pushvalue( L, var_index );
  luaL_buffinit( L, &b );
  lua_lock(L);
  o = L->top - 1;
  luaU_dump_crosscompile(L,clvalue(o)->l.p,writer,&b,0,target);
  lua_unlock(L);
  
  // put string representation on stack and send it
  luaL_pushresult( &b );
  write_variable( tpt, L, lua_gettop( L ) );
  
  // Remove function & dumped string from stack
  lua_pop( L, 2 );
}
#else
static void write_function( Transport *tpt, lua_State *L, int var_index )
{
  luaL_Buffer b;
  
  // push function onto stack, serialize to string 
  lua_pushvalue( L, var_index );
  luaL_buffinit( L, &b );
  lua_dump(L, writer, &b);
  
  // put string representation on stack and send it
  luaL_pushresult( &b );
  write_variable( tpt, L, lua_gettop( L ) );
  
  // Remove function & dumped string from stack
  lua_pop( L, 2 );
}
#endif

// write a variable at the given index in the stack. the index must be absolute
// (i.e. positive).

void write_variable( Transport *tpt, lua_State *L, int var_index )
{
//  int stack_at_start = lua_gettop( L );
  
  switch( lua_type( L, var_index ) )
  {
    case LUA_TNUMBER:
      transport_write_u8( tpt, RPC_NUMBER );
      transport_write_number( tpt, lua_tonumber( L, var_index ) );
      break;

    case LUA_TSTRING:
    {
      const char *s;
      u32 len;
      transport_write_u8( tpt, RPC_STRING );
      s = lua_tostring( L, var_index );
      len = lua_strlen( L, var_index );
      transport_write_u32( tpt, len );
      transport_write_string( tpt, s, len );
      break;
    }

    case LUA_TTABLE:
      transport_write_u8( tpt, RPC_TABLE );
      write_table( tpt, L, var_index );
      transport_write_u8( tpt, RPC_TABLE_END );
      break;

    case LUA_TNIL:
      transport_write_u8( tpt, RPC_NIL );
      break;

    case LUA_TBOOLEAN:
      transport_write_u8( tpt,RPC_BOOLEAN );
      transport_write_u8( tpt, ( u8 )lua_toboolean( L, var_index ) );
      break;

    case LUA_TFUNCTION:
      transport_write_u8( tpt, RPC_FUNCTION );
      write_function( tpt, L, var_index );
      transport_write_u8( tpt, RPC_FUNCTION_END );
      break;

    case LUA_TUSERDATA:
      if( lua_isuserdata( L, var_index ) && ismetatable_type( L, var_index, "rpc.helper" ) )
      {
        transport_write_u8( tpt, RPC_REMOTE );
        helper_remote_index( ( Helper * )lua_touserdata( L, var_index ) );        
      } else
        luaL_error( L, "userdata transmission unsupported" );
      break;

    case LUA_TTHREAD:
      luaL_error( L, "thread transmission unsupported" );
      break;

    case LUA_TLIGHTUSERDATA:
      luaL_error( L, "light userdata transmission unsupported" );
      break;
  }
  MYASSERT( lua_gettop( L ) == stack_at_start );
}


// read a table and push in onto the stack 
static void read_table( Transport *tpt, lua_State *L )
{
  int table_index;
  lua_newtable( L );
  table_index = lua_gettop( L );
  for ( ;; ) 
  {
    if( !read_variable( tpt, L ) )
      return;
    read_variable( tpt, L );
    lua_rawset( L, table_index );
  }
}

// read function and load
static void read_function( Transport *tpt, lua_State *L )
{
  const char *b;
  size_t len;
  
  for( ;; )
  {
    if( !read_variable( tpt, L ) )
      return;

    b = luaL_checklstring( L, -1, &len );
    luaL_loadbuffer( L, b, len, b );
    lua_insert( L, -2 );
    lua_pop( L, 1 );
  }
}

static void read_index( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;
  
  len = transport_read_u32( tpt ); // variable name length
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;
  
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }
}


// read a variable and push in onto the stack. this returns 1 if a "normal"
// variable was read, or 0 if an end-table or end-function marker was read (in which case
// nothing is pushed onto the stack).
int read_variable( Transport *tpt, lua_State *L )
{
  struct exception e;
  u8 type = transport_read_u8( tpt );

  switch( type )
  {
    case RPC_NIL:
      lua_pushnil( L );
      break;

    case RPC_BOOLEAN:
      lua_pushboolean( L, transport_read_u8( tpt ) );
      break;

    case RPC_NUMBER:
      lua_pushnumber( L, transport_read_number( tpt ) );
      break;

    case RPC_STRING:
    {
      u32 len = transport_read_u32( tpt );
      char *s = ( char * )alloca( len + 1 );
      transport_read_string( tpt, s, len );
      s[ len ] = 0;
      lua_pushlstring( L, s, len );
      break;
    }

    case RPC_TABLE:
      read_table( tpt, L );
      break;

    case RPC_TABLE_END:
      return 0;

    case RPC_FUNCTION:
      read_function( tpt, L );
      break;
    
    case RPC_FUNCTION_END:
      return 0;

    case RPC_REMOTE:
      read_index( tpt, L );
      break;

    default:
      e.errnum = type;
      e.type = fatal;
      Throw( e );
  }
  return 1;
}

void transport_set_mode(Transport *tpt, int mode)
{
printf("transport switched to mode %d\n",mode);
    tpt->mode=mode;
}
