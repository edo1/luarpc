#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#ifdef __MINGW32__
void *alloca(size_t);
#else
#include <alloca.h>
#endif

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include "type.h"

enum {
  STATE_CLOSED=0,
  STATE_OPEN,
  STATE_SERVER,
  STATE_CLIENT
};

struct udp_userdata {
  int state;
  int fd;
  int maxpacketsize; // XXX allow to change in runtime?
  struct sockaddr_in sa;
  socklen_t sa_l;
};

static int create_udp_transport( lua_State *L )
{
  struct udp_userdata *ud = ( struct udp_userdata * )lua_newuserdata( L, sizeof( struct udp_userdata ) );
  luaL_getmetatable( L, "rpc_transport.udp" );
  lua_setmetatable( L, -2 );
  ud->state=STATE_CLOSED;
  ud->maxpacketsize=64*1024;
  return 1;
}

static void do_udp_close(struct udp_userdata *ud)
{
  if (ud->state != STATE_CLOSED) {
    close(ud->fd);
    ud->state = STATE_CLOSED;
  }
}

static int udp_listen (lua_State *L)
{
  struct udp_userdata *ud;
  int p;
  struct sockaddr_in sa;
    
  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");
  p=lua_tointeger(L, 2);
  luaL_argcheck(L, p, 2, "integer port expected");

  ud->fd = socket(AF_INET, SOCK_DGRAM, 0);
  ud->state = STATE_OPEN;
  bzero(&(sa), sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(p);
  
  if (bind(ud->fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
    // XXX - add error handle
    do_udp_close(ud);
    return 0;
  }
  ud->state=STATE_SERVER;
  return 0;
}

static int udp_connect (lua_State *L)
{
  struct udp_userdata *ud;
  struct hostent *host;
  int p;
  struct sockaddr_in sa;

  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");

  host = gethostbyname (lua_tostring (L,2));
  if (!host) {
//    deal_with_error (L,0,"could not resolve internet address");
    lua_pushnil (L);
    return 1;
  }

  if (host->h_addrtype != AF_INET || host->h_length != sizeof(struct in_addr)) {
//    deal_with_error (L,0,"not an internet IPv4 address");
    lua_pushnil (L);
    return 1;
  }

  p=lua_tointeger(L, 3);
  luaL_argcheck(L, p, 3, "integer port expected");

  ud->fd = socket(AF_INET, SOCK_DGRAM, 0);
  ud->state = STATE_OPEN;

  bzero(&(sa), sizeof(sa));
  sa.sin_family = AF_INET;
  memcpy( &(sa.sin_addr), host->h_addr_list[0], sizeof(struct in_addr));
  sa.sin_port = htons(p);
  
  if (connect(ud->fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
    // XXX - add error handle
    do_udp_close(ud);
    return 0;
  }
  ud->state=STATE_CLIENT;
  return 0;
}

static int udp_close (lua_State *L)
{
  struct udp_userdata *ud;
  
  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");

  do_udp_close(ud);
  return 0;
}

static int udp_is_server (lua_State *L)
{
  struct udp_userdata *ud;
  
  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");

  lua_pushboolean(L,ud->state==STATE_SERVER);
  return 1;
}

static int udp_is_stream (lua_State *L)
{
  lua_pushboolean(L, 0);
  return 1;
}

static int udp_read(lua_State *L)
{
  struct udp_userdata *ud;
  char *buf;
  int l;

  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");

  buf=alloca(ud->maxpacketsize);

  switch (ud->state) {
    case STATE_SERVER:
      ud->sa_l = sizeof(ud->sa);
      l = recvfrom(ud->fd, buf, ud->maxpacketsize, 0, (struct sockaddr *) &(ud->sa), &(ud->sa_l));
      break;
    case STATE_CLIENT:
      l = recv(ud->fd, buf, ud->maxpacketsize, 0);
    break;
    default:
      l = -1;
  }

  if (l>0) {
    lua_pushlstring(L,buf,l);
    return 1;
  }

  return 0;
}

static int udp_write(lua_State *L)
{
  struct udp_userdata *ud;
  const char *buf;
  size_t l;

  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");
  
  buf=lua_tolstring(L, 2, &l);
  luaL_argcheck(L, l, 2, "string expected");

  switch (ud->state) {
    case STATE_SERVER:
      sendto(ud->fd, buf, l, 0, (struct sockaddr *) &(ud->sa), ud->sa_l);
      break;
    case STATE_CLIENT:
      send(ud->fd, buf, l, 0);
      break;
  }
  return 0;
}

static int udp_getfd(lua_State *L)
{
  struct udp_userdata *ud;

  ud = ( struct udp_userdata * )luaL_checkudata(L, 1, "rpc_transport.udp");
  luaL_argcheck(L, ud, 1, "'rpc_transport.udp' userdata expected");

  switch (ud->state) {
    case STATE_SERVER:
    case STATE_CLIENT:
    case STATE_OPEN:
      lua_pushnumber(L, ud->fd);
      break;
    default:
      lua_pushnil(L);
      break;
  }
  return 1;
}


static const luaL_reg transport_map[] =
{
  { "udp", create_udp_transport },
  { NULL, NULL }
};

static const luaL_reg transport_udp_map[] =
{
  { "listen", udp_listen },
  { "connect", udp_connect },
  { "close", udp_close },
// interface for rpc
  { "read", udp_read },
  { "write", udp_write },
  { "is_server", udp_is_server },
  { "is_stream", udp_is_stream },
  { "getfd", udp_getfd },
  { "__gc", udp_close },
  { NULL, NULL }
};

LUALIB_API int luaopen_udp(lua_State *L)
{
  luaL_register( L, "rpc_transport", transport_map );
  luaL_newmetatable(L, "rpc_transport.udp");
  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);  /* pushes the metatable */
  lua_settable(L, -3);  /* metatable.__index = metatable */
  luaL_register( L, NULL, transport_udp_map);

  return 1;
}
