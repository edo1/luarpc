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
// lua utilities

// replacement for lua_error
void my_lua_error( lua_State *L, const char *errmsg )
{
  lua_pushstring( L, errmsg );
  lua_error( L );
}

int check_num_args( lua_State *L, int desired_n )
{
  int n = lua_gettop( L );   // number of arguments on stack
  if ( n != desired_n )
  {
    return luaL_error( L, "must have %d arg%c", desired_n,
       ( desired_n == 1 ) ? '\0' : 's' );
  }
  return n;
}

int ismetatable_type( lua_State *L, int ud, const char *tname )
{
  if( lua_getmetatable( L, ud ) ) {  // does it have a metatable?
    lua_getfield( L, LUA_REGISTRYINDEX, tname );  // get correct metatable
    if( lua_rawequal( L, -1, -2 ) ) {  // does it have the correct mt?
      lua_pop( L, 2 );  // remove both metatables
      return 1;
    }
  }
  return 0;
}

