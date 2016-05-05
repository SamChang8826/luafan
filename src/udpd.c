
#include "utlua.h"

#define LUA_UDPD_CONNECTION_TYPE "UDPD_CONNECTION_TYPE"
#define LUA_UDPD_DEST_TYPE "LUA_UDPD_DEST_TYPE"

typedef struct {
  struct event reconnect_clock;

  lua_State *L;
  int onReadRef;
  int onSendReadyRef;

  char *host;
  char *bind_host;
  int port;
  int bind_port;
  int socket_fd;
  struct sockaddr addr;
  socklen_t addrlen;

  struct event *read_ev;
  struct event *write_ev;
} Conn;

typedef struct {
  struct sockaddr_in si_client;
  socklen_t client_len;
} Dest;

LUA_API int lua_udpd_conn_gc(lua_State *L) {
  Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
  if (conn->bind_host) {
    free(conn->bind_host);
  }
  if (conn->host) {
    free(conn->host);
  }

  if (conn->onReadRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, conn->onReadRef);
  }

  if (conn->onSendReadyRef != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, conn->onSendReadyRef);
    conn->onSendReadyRef = LUA_NOREF;
  }

  if (conn->read_ev) {
    event_del(conn->read_ev);
    conn->read_ev = NULL;
  }

  if (conn->write_ev) {
    event_del(conn->write_ev);
    conn->write_ev = NULL;
  }

  return 0;
}

#define BUFLEN 1024

static void udpd_writecb(evutil_socket_t fd, short what, void *arg) {
  Conn *conn = (Conn *)arg;

  if (conn->onSendReadyRef != LUA_NOREF) {
    lua_State *co = utlua_newthread(conn->L);
    lua_pop(conn->L, 1);

    lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onSendReadyRef);
    utlua_resume(co, conn->L, 0);
  }
}

static void udpd_readcb(evutil_socket_t fd, short what, void *arg) {
  Conn *conn = (Conn *)arg;

  struct sockaddr_in si_client;
  socklen_t client_len = sizeof(si_client);

  char buf[BUFLEN];
  ssize_t len = recvfrom(conn->socket_fd, buf, BUFLEN, 0,
                         (struct sockaddr *)&si_client, &client_len);
  if (len >= 0) {
    if (conn->onReadRef != LUA_NOREF) {
      lua_State *co = utlua_newthread(conn->L);
      lua_pop(conn->L, 1);

      lua_rawgeti(co, LUA_REGISTRYINDEX, conn->onReadRef);
      lua_pushlstring(co, (const char *)buf, len);

      Dest *dest = lua_newuserdata(co, sizeof(Dest));
      luaL_getmetatable(co, LUA_UDPD_DEST_TYPE);
      lua_setmetatable(co, -2);

      memcpy(&dest->si_client, &si_client, sizeof(si_client));
      dest->client_len = client_len;

      utlua_resume(co, conn->L, 2);
    }
  }
}

static int setnonblock(int fd) {
  int flags;

  flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return flags;
  flags |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) < 0)
    return -1;

  return 0;
}

static int luaudpd_reconnect(Conn *conn, lua_State *L) {
  if (conn->socket_fd) {
    return 1;
  }

  {
    char portbuf[6];
    evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->port);

    struct evutil_addrinfo hints = {0};
    struct evutil_addrinfo *answer = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
    int err = evutil_getaddrinfo(conn->host, portbuf, &hints, &answer);
    if (err < 0) {
      luaL_error(L, "invaild address %s:%d", conn->host, conn->port);
    }

    struct sockaddr *addr = answer->ai_addr;
    memcpy(&conn->addr, addr, sizeof(struct sockaddr));
    conn->addrlen = answer->ai_addrlen;
  }

  int socket_fd = 0;
  if ((socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
    return 0;
  }

  int value = 1;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) ==
      -1) {
    return 0;
  }

  if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) ==
      -1) {
    return 0;
  }

  if (conn->bind_port) {
    if (conn->bind_host) {
      char portbuf[6];
      evutil_snprintf(portbuf, sizeof(portbuf), "%d", conn->bind_port);

      struct evutil_addrinfo hints = {0};
      struct evutil_addrinfo *answer = NULL;
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      hints.ai_protocol = IPPROTO_UDP;
      hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
      int err = evutil_getaddrinfo(conn->bind_host, portbuf, &hints, &answer);
      if (err < 0) {
        luaL_error(L, "invaild address %s:%d", conn->bind_host,
                   conn->bind_port);
      }

      struct sockaddr *addr = answer->ai_addr;

      if (bind(socket_fd, (const struct sockaddr *)&addr, answer->ai_addrlen) ==
          -1) {
        return 0;
      }
    } else {
      struct sockaddr_in addr;
      memset((char *)&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(conn->bind_port);
      addr.sin_addr.s_addr = htonl(INADDR_ANY);

      if (bind(socket_fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
        return 0;
      }
    }
  }

  if (setnonblock(socket_fd) < 0) {
    return 0;
  }

  if (conn->onSendReadyRef != LUA_NOREF) {
    conn->write_ev =
        event_new(event_mgr_base(), socket_fd, EV_WRITE, udpd_writecb, conn);
  } else {
    conn->write_ev = NULL;
  }

  if (conn->onReadRef != LUA_NOREF) {
    conn->read_ev = event_new(event_mgr_base(), socket_fd, EV_PERSIST | EV_READ,
                              udpd_readcb, conn);
    event_add(conn->read_ev, NULL);
  } else {
    conn->read_ev = NULL;
  }

  conn->socket_fd = socket_fd;

  return 1;
}

LUA_API int udpd_new(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);

  Conn *conn = lua_newuserdata(L, sizeof(Conn));
  conn->socket_fd = 0;

  lua_getfield(L, 1, "onread");
  if (lua_isfunction(L, -1)) {
    conn->onReadRef = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    conn->onReadRef = LUA_NOREF;
    lua_pop(L, 1);
  }

  lua_getfield(L, 1, "onsendready");
  if (lua_isfunction(L, -1)) {
    conn->onSendReadyRef = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    conn->onSendReadyRef = LUA_NOREF;
    lua_pop(L, 1);
  }

  lua_getfield(L, 1, "host");
  const char *host = lua_tostring(L, -1);
  if (host) {
    conn->host = strdup(host);
  } else {
    conn->host = NULL;
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "port");
  int port = (int)lua_tointeger(L, -1);
  conn->port = port;
  lua_pop(L, 1);

  lua_getfield(L, 1, "bind_host");
  const char *bind_host = luaL_optstring(L, -1, NULL);
  if (bind_host) {
    conn->bind_host = strdup(bind_host);
  } else {
    conn->bind_host = NULL;
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "bind_port");
  int bind_port = (int)luaL_optinteger(L, -1, 0);
  conn->bind_port = bind_port;
  lua_pop(L, 1);

  luaL_getmetatable(L, LUA_UDPD_CONNECTION_TYPE);
  lua_setmetatable(L, -2);

  conn->L = utlua_mainthread(L);

  if (conn->bind_port) {
    luaudpd_reconnect(conn, L);
  }

  return 1;
}

static const luaL_Reg udpdlib[] = {{"new", udpd_new}, {NULL, NULL}};

LUA_API int udpd_conn_send(lua_State *L) {
  Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);
  size_t len = 0;
  const char *data = luaL_checklstring(L, 2, &len);

  luaudpd_reconnect(conn, L);

  if (data && len > 0 && conn->socket_fd) {
    if (lua_gettop(L) > 2) {
      Dest *dest = lua_touserdata(L, 3);
      sendto(conn->socket_fd, data, len, 0, (struct sockaddr *)&dest->si_client,
             dest->client_len);
    } else {
      sendto(conn->socket_fd, data, len, 0, &conn->addr, conn->addrlen);
    }
  }
  return 0;
}

LUA_API int udpd_conn_send_request(lua_State *L) {
  Conn *conn = luaL_checkudata(L, 1, LUA_UDPD_CONNECTION_TYPE);

  if (conn->write_ev) {
    event_add(conn->write_ev, NULL);
  } else {
    if (conn->onSendReadyRef == LUA_NOREF) {
      luaL_error(L, "onsendready not defined.");
    }
    luaL_error(L, "not writable.");
  }

  return 0;
}

LUA_API int udpd_dest_host(lua_State *L) {
  Dest *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
  char buf[20];
  const char *out =
      inet_ntop(AF_INET, (void *)&dest->si_client.sin_addr, buf, 20);
  if (out) {
    lua_pushstring(L, out);
    return 1;
  }

  return 0;
}

LUA_API int udpd_dest_port(lua_State *L) {
  Dest *dest = luaL_checkudata(L, 1, LUA_UDPD_DEST_TYPE);
  lua_pushinteger(L, ntohs(dest->si_client.sin_port));
  return 1;
}

LUA_API int luaopen_fan_udpd(lua_State *L) {
  luaL_newmetatable(L, LUA_UDPD_CONNECTION_TYPE);

  lua_pushcfunction(L, &udpd_conn_send);
  lua_setfield(L, -2, "send");

  lua_pushcfunction(L, &udpd_conn_send_request);
  lua_setfield(L, -2, "send_req");

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pushstring(L, "__gc");
  lua_pushcfunction(L, &lua_udpd_conn_gc);
  lua_rawset(L, -3);
  lua_pop(L, 1);

  luaL_newmetatable(L, LUA_UDPD_DEST_TYPE);
  lua_pushcfunction(L, &udpd_dest_host);
  lua_setfield(L, -2, "getHost");

  lua_pushcfunction(L, &udpd_dest_port);
  lua_setfield(L, -2, "getPort");

  lua_pushstring(L, "__index");
  lua_pushvalue(L, -2);
  lua_rawset(L, -3);

  lua_pop(L, 1);

  lua_newtable(L);
  luaL_register(L, "udpd", udpdlib);

  return 1;
}
