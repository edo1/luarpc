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

static void server_negotiate( Transport *tpt )
{
  struct exception e;
  char header[ 8 ];
  int x = 1;
  
  TRANSPORT_START_READING(tpt);
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
  TRANSPORT_START_WRITING(tpt);
  transport_write_string( tpt, header, sizeof( header ) );
  TRANSPORT_STOP(tpt);
}

#if 0
// global error default (no handler) 
static int global_error_handler = LUA_NOREF;
#endif

// **************************************************************************
// server side handle userdata objects. 

ServerHandle *server_handle_create( lua_State *L )
{
  ServerHandle *h = ( ServerHandle * )lua_newuserdata( L, sizeof( ServerHandle ) );
  luaL_getmetatable( L, "rpc.server_handle" );
  lua_setmetatable( L, -2 );

  h->link_errs = 0;

  transport_init( &h->ltpt );
  transport_init( &h->atpt );
  return h;
}

void server_handle_shutdown( ServerHandle *h )
{
  transport_close( &h->ltpt );
  transport_close( &h->atpt );
}

void server_handle_destroy( ServerHandle *h )
{
  server_handle_shutdown( h );
}



//****************************************************************************
// lua remote function server
//   read function call data and execute the function. this function empties the
//   stack on entry and exit. This sets a custom error handler to catch errors 
//   around the function call.

static void read_cmd_call( Transport *tpt, lua_State *L )
{
  int i, stackpos, good_function, nargs;
  u32 len;
  char *funcname;
  char *token = NULL;

  // read function name
  len = transport_read_u32( tpt ); /* function name string length */ 
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;
    
  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }
  stackpos = lua_gettop( L ) - 1;
  good_function = LUA_ISCALLABLE( L, -1 );

  // read number of arguments
  nargs = transport_read_u32( tpt );

  // read in each argument, leave it on the stack
  for ( i = 0; i < nargs; i ++ ) 
    read_variable( tpt, L );

  // call the function
  if( good_function )
  {
    int nret, error_code;
    error_code = lua_pcall( L, nargs, LUA_MULTRET, 0 );
    
    // handle errors

    TRANSPORT_START_WRITING(tpt);

    if ( error_code )
    {
      size_t len;
      const char *errmsg;
      errmsg = lua_tolstring (L, -1, &len);
      transport_write_u8( tpt, 1 );
      transport_write_u32( tpt, error_code );
      transport_write_u32( tpt, len );
      transport_write_string( tpt, errmsg, len );
    }
    else
    {
      // pass the return values back to the caller
      transport_write_u8( tpt, 0 );
      nret = lua_gettop( L ) - stackpos;
      transport_write_u32( tpt, nret );
      for ( i = 0; i < nret; i ++ )
        write_variable( tpt, L, stackpos + 1 + i );
    }
  }
  else
  {
    // bad function
    const char *msg = "undefined function: ";
    int errlen = strlen( msg ) + len;
    transport_write_u8( tpt, 1 );
    transport_write_u32( tpt, LUA_ERRRUN );
    transport_write_u32( tpt, errlen );
    transport_write_string( tpt, msg, strlen( msg ) );
    transport_write_string( tpt, funcname, len );
  }
  // empty the stack
  lua_settop ( L, 0 );
}


static void read_cmd_get( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;

  // read function name
  len = transport_read_u32( tpt ); // function name string length 
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;

  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  token = strtok( funcname, "." );
  lua_getglobal( L, token );
  token = strtok( NULL, "." );
  while( token != NULL )
  {
    lua_getfield( L, -1, token );
    lua_remove( L, -2 );
    token = strtok( NULL, "." );
  }

  TRANSPORT_START_WRITING(tpt);

  // return top value on stack
  write_variable( tpt, L, lua_gettop( L ) );

  // empty the stack
  lua_settop ( L, 0 );
}


static void read_cmd_newindex( Transport *tpt, lua_State *L )
{
  u32 len;
  char *funcname;
  char *token = NULL;

  // read function name
  len = transport_read_u32( tpt ); // function name string length
  funcname = ( char * )alloca( len + 1 );
  transport_read_string( tpt, funcname, len );
  funcname[ len ] = 0;

  // get function
  // @@@ perhaps handle more like variables instead of using a long string?
  // @@@ also strtok is not thread safe
  if( strlen( funcname ) > 0 )
  {
    token = strtok( funcname, "." );
    lua_getglobal( L, token );
    token = strtok( NULL, "." );
    while( token != NULL )
    {
      lua_getfield( L, -1, token );
      lua_remove( L, -2 );
      token = strtok( NULL, "." );
    }
    read_variable( tpt, L ); // key
    read_variable( tpt, L ); // value
    lua_settable( L, -3 ); // set key to value on indexed table
  }
  else
  {
    read_variable( tpt, L ); // key
    read_variable( tpt, L ); // value
    lua_setglobal( L, lua_tostring( L, -2 ) );
  }

  TRANSPORT_START_WRITING(tpt);

  // Write out 0 to indicate no error and that we're done
  transport_write_u8( tpt, 0 );
  
  // if ( error_code ) // Add some error handling later
  // {
  //   size_t len;
  //   const char *errmsg;
  //   errmsg = lua_tolstring (L, -1, &len);
  //   transport_write_u8( tpt, 1 );
  //   transport_write_u32( tpt, error_code );
  //   transport_write_u32( tpt, len );
  //   transport_write_string( tpt, errmsg, len );
  // }
  
  // empty the stack
  lua_settop ( L, 0 );
}


void rpc_dispatch_helper( lua_State *L, ServerHandle *handle )
{  
  struct exception e;

  Try 
  {
    // if accepting transport is open, read function calls
    if ( transport_is_open( &handle->atpt ) )
    {
      Try
      {
        TRANSPORT_START_READING(&handle->atpt);

        switch ( transport_read_u8( &handle->atpt ) )
        {
          case RPC_CMD_CALL:  // call function
#ifdef HELPER_WAIT
            transport_write_u8( &handle->atpt, RPC_READY );
#endif
            read_cmd_call( &handle->atpt, L );
            break;
          case RPC_CMD_GET: // get server-side variable for client
#ifdef HELPER_WAIT
            transport_write_u8( &handle->atpt, RPC_READY );
#endif
            read_cmd_get( &handle->atpt, L );
            break;
          case RPC_CMD_CON: //  allow client to renegotiate active connection
            server_negotiate( &handle->atpt );
            break;
          case RPC_CMD_NEWINDEX: // assign new variable on server
#ifdef HELPER_WAIT
            transport_write_u8( &handle->atpt, RPC_READY );
#endif
            read_cmd_newindex( &handle->atpt, L );
            break;
          default: // complain and throw exception if unknown command
#ifdef HELPER_WAIT
            transport_write_u8(&handle->atpt, RPC_UNSUPPORTED_CMD );
#endif
            e.type = nonfatal;
            e.errnum = ERR_COMMAND;
            Throw( e );
        }
        
        handle->link_errs = 0;

        TRANSPORT_STOP(&handle->atpt);
      }
      Catch( e )
      {
        switch( e.type )
        {
          case fatal: // shutdown will initiate after throw
            Throw( e );
            
          case nonfatal:
            handle->link_errs++;
            if ( handle->link_errs > MAX_LINK_ERRS )
            {
              handle->link_errs = 0;
              Throw( e ); // remote connection will be closed
            }
            break;
            
          default: 
            Throw( e );
        }
      }
    }
    else
    {
      // if accepting transport is not open, accept a new connection from the
      // listening transport
      transport_accept( &handle->ltpt, &handle->atpt );
      
      TRANSPORT_START_READING(&handle->atpt);
      switch ( transport_read_u8( &handle->atpt ) )
      {
        case RPC_CMD_CON:
          server_negotiate( &handle->atpt );
          break;
        default: // connection must be established to issue any other commands
          e.type = nonfatal;
          e.errnum = ERR_COMMAND;
          Throw( e ); // remote connection will be closed
      }
    }
  }
  Catch( e )
  {
    switch( e.type )
    {
      case fatal:
        server_handle_shutdown( handle );
        deal_with_error( L, 0, errorString( e.errnum ) );
        break;
        
      case nonfatal:
        transport_close( &handle->atpt );
        break;
        
      default:
        Throw( e );
    }
  }
}


// rpc_dispatch( server_handle )
int rpc_dispatch( lua_State *L )
{
  ServerHandle *handle;
  check_num_args( L, 1 );
  
  handle = ( ServerHandle * )luaL_checkudata(L, 1, "rpc.server_handle");
  luaL_argcheck(L, handle, 1, "server handle expected");

  handle = ( ServerHandle * )lua_touserdata( L, 1 );

  rpc_dispatch_helper( L, handle );
  return 0;
}

#endif
