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



static int generic_catch_handler(lua_State *L, Handle *handle, struct exception e )
{
  deal_with_error( L, handle, errorString( e.errnum ) );
  switch( e.type )
  {
    case nonfatal:
      lua_pushnil( L );
      return 1;
      break;
    case fatal:
      transport_close( &handle->tpt );
      break;
    default: lua_assert( 0 );
  }
  return 0;
}

// **************************************************************************
// client side handle and handle helper userdata objects.
//
//  a handle userdata (handle to a RPC server) is a pointer to a Handle object.
//  a helper userdata is a pointer to a Helper object.
//
//  helpers let us make expressions like:
//     handle.funcname (a,b,c)
//  "handle.funcname" returns the helper object, which calls the remote
//  function.

// handle a client or server side error. NOTE: this function may or may not
// return. the handle `h' may be 0.
void deal_with_error(lua_State *L, Handle *h, const char *error_string)
{ 
  if( global_error_handler !=  LUA_NOREF )
  {
    lua_getref( L, global_error_handler );
    lua_pushstring( L, error_string );
    lua_pcall( L, 1, 0, 0 );
  }
  else
    luaL_error( L, error_string );
}

Handle *handle_create( lua_State *L )
{
  Handle *h = ( Handle * )lua_newuserdata( L, sizeof( Handle ) );
  luaL_getmetatable( L, "rpc.handle" );
  lua_setmetatable( L, -2 );
  h->error_handler = LUA_NOREF;
  h->async = 0;
  h->read_reply_count = 0;
  return h;
}

static Helper *helper_create( lua_State *L, Handle *handle, const char *funcname )
{
  Helper *h = ( Helper * )lua_newuserdata( L, sizeof( Helper ) );
  luaL_getmetatable( L, "rpc.helper" );
  lua_setmetatable( L, -2 );
  
  lua_pushvalue( L, 1 ); // push parent handle
  h->pref = luaL_ref( L, LUA_REGISTRYINDEX ); // put ref into struct
  h->handle = handle;
  h->parent = NULL;
  h->nparents = 0;
  strncpy( h->funcname, funcname, NUM_FUNCNAME_CHARS );
  return h;
}


// indexing a handle returns a helper 
static int handle_index (lua_State *L)
{
  const char *s;
  
  check_num_args( L, 2 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.handle" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index a handle with a non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, errorString( ERR_LONGFNAME ) );
    
  helper_create( L, ( Handle * )lua_touserdata( L, 1 ), s );

  // return the helper object 
  return 1;
}

static int helper_newindex( lua_State *L );

// indexing a handle returns a helper
static int handle_newindex( lua_State *L )
{
  const char *s;

  check_num_args( L, 3 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.handle" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index handle with a non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, errorString( ERR_LONGFNAME ) );
  
  helper_create( L, ( Handle * )lua_touserdata( L, 1 ), "" );
  lua_replace(L, 1);

  helper_newindex( L );

  return 0;
}

// replays series of indexes to remote side as a string
void helper_remote_index( Helper *helper )
{
  int i, len;
  Helper **hstack;
  Transport *tpt = &helper->handle->tpt;
  
  // get length of name & make stack of helpers
  len = strlen( helper->funcname );
  if( helper->nparents > 0 ) // If helper has parents, build string to remote index
  {
    hstack = ( Helper ** )alloca( sizeof( Helper * ) * helper->nparents );
    hstack[ helper->nparents - 1 ] = helper->parent;
    len += strlen( hstack[ helper->nparents - 1 ]->funcname ) + 1;
  
    for( i = helper->nparents - 1 ; i > 0 ; i -- )
    {
      hstack[ i - 1 ] = hstack[ i ]->parent;
      len += strlen( hstack[ i ]->funcname ) + 1;
    }
	
	  transport_write_u32( tpt, len );

    // replay helper key names      
    for( i = 0 ; i < helper->nparents ; i ++ )
    {
     transport_write_string( tpt, hstack[ i ]->funcname, strlen( hstack[ i ]->funcname ) );
     transport_write_string( tpt, ".", 1 ); 
    }
  }
  else // If helper has no parents, just use length of global
	  transport_write_u32( tpt, len );

  transport_write_string( tpt, helper->funcname, strlen( helper->funcname ) );
}

#ifdef HELPER_WAIT
static void helper_wait_ready( Transport *tpt, u8 cmd )
{
  struct exception e;
  u8 cmdresp;

  transport_write_u8( tpt, cmd );
  cmdresp = transport_read_u8( tpt );
  if( cmdresp != RPC_READY )
  {
    e.errnum = ERR_PROTOCOL;
    e.type = nonfatal;
    Throw( e );
  }

}
#else
#define helper_wait_ready transport_write_u8
#endif

static int helper_get( lua_State *L, Helper *helper )
{
  struct exception e;
  int freturn = 0;
  Transport *tpt = &helper->handle->tpt;
  
  Try
  {
    TRANSPORT_START_WRITING(tpt);
    helper_wait_ready( tpt, RPC_CMD_GET );
    helper_remote_index( helper );
    
    TRANSPORT_START_READING(tpt);
    read_variable( tpt, L );
    TRANSPORT_STOP(tpt);

    freturn = 1;
  }
  Catch( e )
  {
    freturn = generic_catch_handler( L, helper->handle, e );
  }
  return freturn;
}


// static int helper_async( lua_State *L )
// {
//     /* first read out any pending return values for old async calls */
//     for (; h->handle->read_reply_count > 0; h->handle->read_reply_count--) {
//       ret_code = transport_read_u8 (tpt);   /* return code */
//       if( ret_code == 0 )
//       {
//         /* read return arguments, ignore everything we read */
//         nret = transport_read_u32( tpt );
//       
//         for (i=0; i < ( ( int ) nret ); i++)
//           read_variable (tpt,L);
//       
//         lua_pop (L,nret);
//       }
//       else
//       {
//         /* read error and handle it */
//         u32 code = transport_read_u32( tpt );
//         u32 len = transport_read_u32( tpt );
//         char *err_string = ( char * )alloca( len + 1 );
//         transport_read_string( tpt, err_string, len );
//         err_string[ len ] = 0;
// 
//         deal_with_error( L, h->handle, err_string );
//         freturn = 0;
//       }
//     }
// }




static int helper_call (lua_State *L)
{
  struct exception e;
  int freturn = 0;
  Helper *h;
  Transport *tpt;
  
  h = ( Helper * )luaL_checkudata(L, 1, "rpc.helper");
  luaL_argcheck(L, h, 1, "helper expected");
  
  tpt = &h->handle->tpt;
  
  // capture special calls, otherwise execute normal remote call
  if( strcmp("get", h->funcname ) == 0 )
  {
    helper_get( L, h->parent );
    freturn = 1;
  }
  else
  {
    Try
    {
      int i,n;
      u32 nret,ret_code;

     TRANSPORT_START_WRITING(tpt);
      // write function name
      helper_wait_ready( tpt, RPC_CMD_CALL );
      helper_remote_index( h );


      // write number of arguments
      n = lua_gettop( L );
      transport_write_u32( tpt, n - 1 );
    
      // write each argument
      for( i = 2; i <= n; i ++ )
        write_variable( tpt, L, i );

      /* if we're in async mode, we're done */
      /*if ( h->handle->async )
      {
        h->handle->read_reply_count++;
        freturn = 0;
      }*/

      TRANSPORT_START_READING(tpt);
      // read return code
      ret_code = transport_read_u8( tpt );

      if ( ret_code == 0 )
      {
        // read return arguments
        nret = transport_read_u32( tpt );
      
        for ( i = 0; i < ( ( int ) nret ); i ++ )
          read_variable( tpt, L );
      
        freturn = ( int )nret;
      }
      else
      {
        // read error and handle it
        transport_read_u32( tpt ); // read code (not being used here)
        u32 len = transport_read_u32( tpt );
        char *err_string = ( char * )alloca( len + 1 );
        transport_read_string( tpt, err_string, len );
        err_string[ len ] = 0;

        deal_with_error( L, h->handle, err_string );
        freturn = 0;
      }

      TRANSPORT_STOP(tpt);

    }
    Catch( e )
    {
      freturn = generic_catch_handler( L, h->handle, e );
    }
  }
  return freturn;
}

// __newindex even on helper, 
static int helper_newindex( lua_State *L )
{
  struct exception e;
  int freturn = 0;
  int ret_code;
  Helper *h;
  Transport *tpt;
  
  h = ( Helper * )luaL_checkudata(L, -3, "rpc.helper");
  luaL_argcheck(L, h, -3, "helper expected");
  
  luaL_checktype(L, -2, LUA_TSTRING );
  
  tpt = &h->handle->tpt;
  
  Try
  {  
    // index destination on remote side
    TRANSPORT_START_WRITING(tpt);
    helper_wait_ready( tpt, RPC_CMD_NEWINDEX );
    helper_remote_index( h );

    write_variable( tpt, L, lua_gettop( L ) - 1 );
    write_variable( tpt, L, lua_gettop( L ) );

    TRANSPORT_START_READING(tpt);
    ret_code = transport_read_u8( tpt );
    if( ret_code != 0 )
    {
      // read error and handle it
      transport_read_u32( tpt ); // Read code (not using here)
      u32 len = transport_read_u32( tpt );
      char *err_string = ( char * )alloca( len + 1 );
      transport_read_string( tpt, err_string, len );
      err_string[ len ] = 0;

      deal_with_error( L, h->handle, err_string );
    }

    TRANSPORT_STOP(tpt);

    freturn = 0;
  }
  Catch( e )
  {
    freturn = generic_catch_handler( L, h->handle, e );
  }
  return freturn;
}


static Helper *helper_append( lua_State *L, Helper *helper, const char *funcname )
{
  Helper *h = ( Helper * )lua_newuserdata( L, sizeof( Helper ) );
  luaL_getmetatable( L, "rpc.helper" );
  lua_setmetatable( L, -2 );
  
  lua_pushvalue( L, 1 ); // push parent
  h->pref = luaL_ref( L, LUA_REGISTRYINDEX ); // put ref into struct
  h->handle = helper->handle;
  h->parent = helper;
  h->nparents = helper->nparents + 1;
  strncpy ( h->funcname, funcname, NUM_FUNCNAME_CHARS );
  return h;
}

// indexing a helper returns a helper 
static int helper_index( lua_State *L )
{
  const char *s;

  check_num_args( L, 2 );
  MYASSERT( lua_isuserdata( L, 1 ) && ismetatable_type( L, 1, "rpc.helper" ) );

  if( lua_type( L, 2 ) != LUA_TSTRING )
    return luaL_error( L, "can't index handle with non-string" );
  s = lua_tostring( L, 2 );
  if ( strlen( s ) > NUM_FUNCNAME_CHARS - 1 )
    return luaL_error( L, errorString( ERR_LONGFNAME ) );
  
  helper_append( L, ( Helper * )lua_touserdata( L, 1 ), s );

  return 1;
}

static int helper_close (lua_State *L)
{
  Helper *h = ( Helper * )luaL_checkudata(L, 1, "rpc.helper");
  luaL_argcheck(L, h, 1, "helper expected");
  
  luaL_unref(L, LUA_REGISTRYINDEX, h->pref);
  h->pref = LUA_REFNIL;
  return 0;
}


#ifndef LUARPC_STANDALONE

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE rpc_handle[] =
{
  { LSTRKEY( "__index" ), LFUNCVAL( handle_index ) },
  { LSTRKEY( "__newindex"), LFUNCVAL( handle_newindex )},
  { LNILKEY, LNILVAL }
};

const LUA_REG_TYPE rpc_helper[] =
{
  { LSTRKEY( "__call" ), LFUNCVAL( helper_call ) },
  { LSTRKEY( "__index" ), LFUNCVAL( helper_index ) },
  { LSTRKEY( "__newindex" ), LFUNCVAL( helper_newindex ) },
  { LSTRKEY( "__gc" ), LFUNCVAL( helper_close ) },
  { LNILKEY, LNILVAL }
};

void register_client(lua_State *L)
{
#if LUA_OPTIMIZE_MEMORY > 0
  luaL_rometatable(L, "rpc.helper", (void*)rpc_helper);
  luaL_rometatable(L, "rpc.handle", (void*)rpc_handle);
#else
  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
#endif
}

#else

static const luaL_reg rpc_handle[] =
{
  { "__index", handle_index },
  { "__newindex", handle_newindex },
  { NULL, NULL }
};

static const luaL_reg rpc_helper[] =
{
  { "__call", helper_call },
  { "__index", helper_index },
  { "__newindex", helper_newindex },
  { "__gc", helper_close },
  { NULL, NULL }
};

void register_client(lua_State *L)
{
  luaL_newmetatable( L, "rpc.helper" );
  luaL_register( L, NULL, rpc_helper );
  
  luaL_newmetatable( L, "rpc.handle" );
  luaL_register( L, NULL, rpc_handle );
}

#endif

#endif
