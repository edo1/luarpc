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


#ifdef BUILD_RPC

// Support for Compiling with & without rotables 
#ifdef LUA_OPTIMIZE_MEMORY
#define LUA_ISCALLABLE( state, idx ) ( lua_isfunction( state, idx ) || lua_islightfunction( state, idx ) )
#else
#define LUA_ISCALLABLE( state, idx ) lua_isfunction( state, idx )
#endif

// Prototypes for Local Functions  
LUALIB_API int luaopen_rpc( lua_State *L );
Handle *handle_create( lua_State *L );


struct exception_context the_exception_context[ 1 ];

#if 0

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
#endif

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


// **************************************************************************
// rpc utilities

// functions for sending and receving headers 
static void client_negotiate( Transport *tpt )
{
  struct exception e;
  char header[ 8 ];
  int x = 1;

  TRANSPORT_START_WRITING(tpt);
  // default client configuration
  tpt->loc_little = ( char )*( char * )&x;
  tpt->lnum_bytes = ( char )sizeof( lua_Number );
  tpt->loc_intnum = ( char )( ( ( lua_Number )0.5 ) == 0 );

  // write the protocol header 
  header[0] = 'L';
  header[1] = 'R';
  header[2] = 'P';
  header[3] = 'C';
  header[4] = RPC_PROTOCOL_VERSION;
  header[5] = tpt->loc_little;
  header[6] = tpt->lnum_bytes;
  header[7] = tpt->loc_intnum;
  transport_write_string( tpt, header, sizeof( header ) );
  
  
  TRANSPORT_START_READING(tpt);
  // read server's response
  transport_read_string( tpt, header, sizeof( header ) );
  if( header[0] != 'L' ||
      header[1] != 'R' ||
      header[2] != 'P' ||
      header[3] != 'C' ||
      header[4] != RPC_PROTOCOL_VERSION )
  {
    e.errnum = ERR_HEADER;
    e.type = nonfatal;
    Throw( e );
  }
  
  TRANSPORT_STOP(tpt);

  // write configuration from response
  tpt->net_little = header[5];
  tpt->lnum_bytes = header[6];
  tpt->net_intnum = header[7];
}

#if 0
static void server_negotiate( Transport *tpt )
{
  struct exception e;
  char header[ 8 ];
  int x = 1;
  
  // default sever configuration
  tpt->net_little = tpt->loc_little = ( char )*( char * )&x;
  tpt->lnum_bytes = ( char )sizeof( lua_Number );
  tpt->net_intnum = tpt->loc_intnum = ( char )( ( ( lua_Number )0.5 ) == 0 );
  
  // read and check header from client
  transport_read_string( tpt, header, sizeof( header ) );
  if( header[0] != 'L' ||
      header[1] != 'R' ||
      header[2] != 'P' ||
      header[3] != 'C' ||
      header[4] != RPC_PROTOCOL_VERSION )
  {
    e.errnum = ERR_HEADER;
    e.type = nonfatal;
    Throw( e );
  }
  
  // check if endianness differs, if so use big endian order  
  if( header[ 5 ] != tpt->loc_little )
    header[ 5 ] = tpt->net_little = 0;
    
  // set number precision to lowest common denominator 
  if( header[ 6 ] > tpt->lnum_bytes )
    header[ 6 ] = tpt->lnum_bytes;
  if( header[ 6 ] < tpt->lnum_bytes )
    tpt->lnum_bytes = header[ 6 ];
  
  // if lua_Number is integer on either side, use integer 
  if( header[ 7 ] != tpt->loc_intnum )
    header[ 7 ] = tpt->net_intnum = 1;
  
  // send reconciled configuration to client
  transport_write_string( tpt, header, sizeof( header ) );
}
#endif

// global error default (no handler) 
static int global_error_handler = LUA_NOREF;

// **************************************************************************
// remote function calling (client side)

// rpc_connect (ip_address, port)
//      returns a handle to the new connection, or nil if there was an error.
//      if there is an RPC error function defined, it will be called on error.

static int rpc_connect( lua_State *L )
{
  struct exception e;
  Handle *handle = 0;
  
  Try
  {
    handle = handle_create ( L );
    transport_open_connection( L, handle );

    TRANSPORT_START_WRITING(&handle->tpt);
    transport_write_u8( &handle->tpt, RPC_CMD_CON );
    client_negotiate( &handle->tpt );
  }
  Catch( e )
  {     
    deal_with_error( L, 0, errorString( e.errnum ) );
    lua_pushnil( L );
  }
  return 1;
}


// rpc_close( handle )
//     this closes the transport, but does not free the handle object. that's
//     because the handle will still be in the user's name space and might be
//     referred to again. we'll let garbage collection free the object.
//     it's a lua runtime error to refer to a transport after it has been closed.


static int rpc_close( lua_State *L )
{
  check_num_args( L, 1 );

  if( lua_isuserdata( L, 1 ) )
  {
    if( ismetatable_type( L, 1, "rpc.handle" ) )
    {
      Handle *handle = ( Handle * )lua_touserdata( L, 1 );
      transport_close( &handle->tpt );
      return 0;
    }
    if( ismetatable_type( L, 1, "rpc.server_handle" ) )
    {
      ServerHandle *handle = ( ServerHandle * )lua_touserdata( L, 1 );
      server_handle_shutdown( handle );
      return 0;
    }
  }

  return luaL_error(L,"arg must be handle");
}


static ServerHandle *rpc_listen_helper( lua_State *L )
{
  struct exception e;
  ServerHandle *handle = 0;

  Try
  {
    // make server handle 
    handle = server_handle_create( L );

    // make listening transport 
    transport_open_listener( L, handle );
  }
  Catch( e )
  {
    if( handle )
      server_handle_destroy( handle );
    
    deal_with_error( L, 0, errorString( e.errnum ) );
    return 0;
  }
  return handle;
}

// rpc_listen( transport_indentifier ) --> server_handle
//    transport_identifier defines where to listen, identifier type is subject to transport implementation
static int rpc_listen( lua_State *L )
{
  ServerHandle *handle;

  handle = rpc_listen_helper( L );
  if ( handle == 0 )
    return luaL_error( L, "bad handle" );
    
  return 1;
}


// rpc_peek( server_handle ) --> 0 or 1 
static int rpc_peek( lua_State *L )
{
  ServerHandle *handle;

  check_num_args( L, 1 );
  if ( !( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.server_handle" ) ) )
    return luaL_error( L, "arg must be server handle" );

  handle = ( ServerHandle * )lua_touserdata( L, 1 );

  // if accepting transport is open, see if there is any data to read
  if ( transport_is_open( &handle->atpt ) )
  {
    if ( transport_readable( &handle->atpt ) )
      lua_pushnumber( L, 1 );
    else 
      lua_pushnil( L );
      
    return 1;
  }

  // otherwise, see if there is a new connection on the listening transport
  if ( transport_is_open( &handle->ltpt ) )
  {
    if ( transport_readable( &handle->ltpt ) )
      lua_pushnumber ( L, 1 );
    else
      lua_pushnil( L );
      
    return 1;
  }

  lua_pushnumber( L, 0 );
  return 1;
}


// rpc_server( transport_identifier )
static int rpc_server( lua_State *L )
{
  int shref;
  ServerHandle *handle = rpc_listen_helper( L );
  
  // Anchor handle in the registry
  //   This is needed because garbage collection can steal our handle, 
  //   which isn't otherwise referenced
  //
  //   @@@ this should be replaced when we create a system for multiple 
  //   @@@ connections. such a mechanism would likely likely create a
  //   @@@ table for multiple connections that we could service in an event loop 
  
  shref = luaL_ref( L, LUA_REGISTRYINDEX );
  lua_rawgeti(L, LUA_REGISTRYINDEX, shref );
  
  while ( transport_is_open( &handle->ltpt ) )
    rpc_dispatch_helper( L, handle );
    
  luaL_unref( L, LUA_REGISTRYINDEX, shref );
  server_handle_destroy( handle );
  return 0;
}

// **************************************************************************
// more error handling stuff 

// rpc_on_error( [ handle, ] error_handler )
static int rpc_on_error( lua_State *L )
{
  check_num_args( L, 1 );

  if( global_error_handler !=  LUA_NOREF )
    lua_unref (L,global_error_handler);
  
  global_error_handler = LUA_NOREF;

  if ( LUA_ISCALLABLE( L, 1 ) )
    global_error_handler = lua_ref( L, 1 );
  else if ( lua_isnil( L, 1 ) )
    { ;; }
  else
    return luaL_error( L, "bad args" );

  // @@@ add option for handle 
  // Handle *h = (Handle*) lua_touserdata (L,1); 
  // if (lua_isuserdata (L,1) && ismetatable_type(L, 1, "rpc.handle")); 

  return 0;
}

// **************************************************************************
// register RPC functions 

#ifndef LUARPC_STANDALONE

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE rpc_server_handle[] =
{
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_map[] =
{
  {  LSTRKEY( "connect" ), LFUNCVAL( rpc_connect ) },
  {  LSTRKEY( "close" ), LFUNCVAL( rpc_close ) },
  {  LSTRKEY( "server" ), LFUNCVAL( rpc_server ) },
  {  LSTRKEY( "on_error" ), LFUNCVAL( rpc_on_error ) },
  {  LSTRKEY( "listen" ), LFUNCVAL( rpc_listen ) },
  {  LSTRKEY( "peek" ), LFUNCVAL( rpc_peek ) },
  {  LSTRKEY( "dispatch" ), LFUNCVAL( rpc_dispatch ) },
//  {  LSTRKEY( "rpc_async" ), LFUNCVAL( rpc_async ) },
#if LUA_OPTIMIZE_MEMORY > 0
// {  LSTRKEY("mode"), LSTRVAL( LUARPC_MODE ) }, 
#endif // #if LUA_OPTIMIZE_MEMORY > 0
  { LNILKEY, LNILVAL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
#if LUA_OPTIMIZE_MEMORY > 0
  register_client(L);

  luaL_rometatable(L, "rpc.server_handle", (void*)rpc_server_handle);
#else
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring( L, LUARPC_MODE );
  lua_setfield(L, -2, "mode");

  register_client(L);

  luaL_newmetatable( L, "rpc.server_handle" );
#endif
  return 1;
}

#else

static const luaL_reg rpc_server_handle[] =
{
  { NULL, NULL }
};

static const luaL_reg rpc_map[] =
{
  { "connect", rpc_connect },
  { "close", rpc_close },
  { "server", rpc_server },
  { "on_error", rpc_on_error },
  { "listen", rpc_listen },
  { "peek", rpc_peek },
  { "dispatch", rpc_dispatch },
//  { "rpc_async", rpc_async },
  { NULL, NULL }
};


LUALIB_API int luaopen_rpc(lua_State *L)
{
  luaL_register( L, "rpc", rpc_map );
  lua_pushstring(L, LUARPC_MODE);
  lua_setfield(L, -2, "mode");
  register_client(L);
  luaL_newmetatable( L, "rpc.server_handle" );

  return 1;
}

#endif

#endif
