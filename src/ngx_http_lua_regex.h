#ifndef NGX_HTTP_LUA_REGEX_H
#define NGX_HTTP_LUA_REGEX_H


#include "ngx_http_lua_common.h"


#if (NGX_PCRE)
int ngx_http_lua_ngx_re_match(lua_State *L);
int ngx_http_lua_ngx_re_gmatch(lua_State *L);
#endif


#endif /* NGX_HTTP_LUA_REGEX_H */
