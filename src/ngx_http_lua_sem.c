
/*
 * Copyright (C) Frank Tian (0x5446)
 */


#ifndef DDEBUG
#define DDEBUG 1
#endif
#include "ddebug.h"


#include "ngx_http_lua_util.h"
#include "ngx_http_lua_sem.h"
#include "ngx_http_lua_contentby.h"

#define SEM_TIMEOUT 0x00
#define SEM_WOKENUP 0x01

typedef struct sem_s {
	char *sem_key;
	void *data;
	unsigned char sem_stat;
} sem_t;

static void ngx_http_lua_sem_map_create(lua_State *L, size_t size);
static sem_t * ngx_http_lua_sem_map_get(lua_State *L, char *sem_key);
static void ngx_http_lua_sem_map_set(lua_State *L, char *sem_key, sem_t *sem);
static void ngx_http_lua_sem_map_del(lua_State *L, char *sem_key);

static ngx_int_t ngx_http_lua_sem_wait_resume(ngx_http_request_t *r);
static void ngx_http_lua_sem_wait_cleanup(void *data);
static void ngx_http_lua_sem_wait_handler(ngx_event_t *ev);
static int ngx_http_lua_ngx_sem_wait(lua_State *L);
static int ngx_http_lua_ngx_sem_post(lua_State *L);

void
ngx_http_lua_inject_sem_api(lua_State *L)
{
	ngx_http_lua_sem_map_create(L, 0);

    lua_pushcfunction(L, ngx_http_lua_ngx_sem_wait);
    lua_setfield(L, -2, "sem_wait");

    lua_pushcfunction(L, ngx_http_lua_ngx_sem_post);
    lua_setfield(L, -2, "sem_post");
}

static ngx_int_t
ngx_http_lua_sem_wait_resume(ngx_http_request_t *r)
{
    lua_State                   *vm;
    ngx_connection_t            *c;
    ngx_int_t                    rc;
    ngx_http_lua_ctx_t          *ctx;
	sem_t						*sem;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->resume_handler = ngx_http_lua_wev_handler;

    c = r->connection;
    vm = ngx_http_lua_get_lua_vm(r, ctx);

	sem = ctx->cur_co_ctx->sleep.data;
	if (sem->sem_stat == SEM_WOKENUP) {
		lua_pushnumber(ctx->cur_co_ctx->co, 1);
	} else {
		lua_pushnumber(ctx->cur_co_ctx->co, 0);
	}
    rc = ngx_http_lua_run_thread(vm, r, ctx, 1);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua run thread returned %d", rc);

    if (rc == NGX_AGAIN) {
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (rc == NGX_DONE) {
        ngx_http_lua_finalize_request(r, NGX_DONE);
        return ngx_http_lua_run_posted_threads(c, vm, r, ctx);
    }

    if (ctx->entered_content_phase) {
        ngx_http_lua_finalize_request(r, rc);
        return NGX_DONE;
    }

    return rc;
}


static void
ngx_http_lua_sem_wait_cleanup(void *data)
{
    ngx_http_lua_co_ctx_t          *coctx = data;

    if (coctx->sleep.timer_set) {
        dd("cleanup: deleting timer for ngx.sem_wait");

        ngx_del_timer(&coctx->sleep);
    }
}

void
ngx_http_lua_sem_wait_handler(ngx_event_t *ev)
{
    lua_State               *vm;
    ngx_connection_t        *c;
    ngx_http_request_t      *r;
    ngx_http_lua_ctx_t      *ctx;
    ngx_http_log_ctx_t      *log_ctx;
    ngx_http_lua_co_ctx_t   *coctx;
	sem_t					*sem;

	
	sem = ev->data;
    coctx = (ngx_http_lua_co_ctx_t *)sem->data;

    r = coctx->data;
    c = r->connection;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);

    if (ctx == NULL) {
        return;
    }

    log_ctx = c->log->data;
    log_ctx->current_request = r;

    coctx->cleanup = NULL;

	vm = ngx_http_lua_get_lua_vm(r, ctx);
	ngx_http_lua_sem_map_del(vm, sem->sem_key);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "lua sem_wait timer expired: \"%V?%V\"", &r->uri, &r->args);

    ctx->cur_co_ctx = coctx;

    if (ctx->entered_content_phase) {
        (void) ngx_http_lua_sem_wait_resume(r);

    } else {
        ctx->resume_handler = ngx_http_lua_sem_wait_resume;
        ngx_http_core_run_phases(r);
    }

    ngx_http_run_posted_requests(c);
}

static int
ngx_http_lua_ngx_sem_wait(lua_State *L)
{
    int                          n;
	sem_t						*sem;
    ngx_int_t                    delay; /* in msec */
    ngx_http_request_t          *r;
    ngx_http_lua_ctx_t          *ctx;
    ngx_http_lua_co_ctx_t       *coctx;

    n = lua_gettop(L);
    if (n != 2) {
        return luaL_error(L, "attempt to pass %d arguments, but accepted 1", n);
    }

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request found");
    }


	sem = (sem_t *)ngx_palloc(r->pool, sizeof(sem_t));
	sem->sem_key = (char *)luaL_checkstring(L, 1);
	sem->sem_stat = SEM_TIMEOUT;

    delay = (ngx_int_t) (luaL_checknumber(L, 2) * 1000);

    if (delay < 0) {
        return luaL_error(L, "invalid sem_wait duration \"%d\"", delay);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    ngx_http_lua_check_context(L, ctx, NGX_HTTP_LUA_CONTEXT_REWRITE
                               | NGX_HTTP_LUA_CONTEXT_ACCESS
                               | NGX_HTTP_LUA_CONTEXT_CONTENT
                               | NGX_HTTP_LUA_CONTEXT_TIMER);

    coctx = ctx->cur_co_ctx;
    if (coctx == NULL) {
        return luaL_error(L, "no co ctx found");
    }
	sem->data = coctx;

    coctx->data = r;
	
    coctx->sleep.handler = ngx_http_lua_sem_wait_handler;
    coctx->sleep.data = sem;
    coctx->sleep.log = r->connection->log;

    dd("adding timer with delay %lu ms, r:%.*s", (unsigned long) delay,
       (int) r->uri.len, r->uri.data);

    ngx_add_timer(&coctx->sleep, (ngx_msec_t) delay);

    coctx->cleanup = ngx_http_lua_sem_wait_cleanup;

	ngx_http_lua_sem_map_set(L, sem->sem_key, sem);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua ready to sem_wait for %d ms", delay);

    return lua_yield(L, 0);
}

static int
ngx_http_lua_ngx_sem_post(lua_State *L)
{
    int                          n;
	char					 	*sem_key;
	ngx_event_t					*ev;
	ngx_http_request_t          *r;
	sem_t						*sem;
	ngx_http_lua_co_ctx_t 		*coctx;

    r = ngx_http_lua_get_req(L);

    n = lua_gettop(L);
    if (n != 1) {
        return luaL_error(L, "attempt to pass %d arguments, but accepted 1", n);
    }

	sem_key = (char *)luaL_checkstring(L, 1);
	sem = ngx_http_lua_sem_map_get(L, sem_key);
	if (sem == NULL) {
    	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "sem_key:(%s) not found", sem_key);
		lua_pushboolean(L, 0);
		lua_pushstring(L, "sem_key not found");
		return 2;
	}

	sem->sem_stat = SEM_WOKENUP;
	coctx = sem->data;
	ev = &coctx->sleep;
    ngx_del_timer(ev);
    ngx_add_timer(ev, -10000);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua sem_post sem_key:%s", sem_key);

	lua_pushboolean(L, 1);
	return 1;
}

/* create "ngx._sem_map". Only called once at api injection */
static void
ngx_http_lua_sem_map_create(lua_State *L, size_t size) {
	lua_createtable(L, (int)size, 0);
	lua_setfield(L, -2, "_sem_map");
}

static sem_t *
ngx_http_lua_sem_map_get(lua_State *L, char *sem_key)
{
	dd("###ngx_http_lua_sem_map_get###\tgettop(L)=%d", lua_gettop(L));
	lua_getglobal(L, "ngx");
	lua_getfield(L, -1, "_sem_map");
	lua_getfield(L, -1, sem_key);
	sem_t *sem = (sem_t *)lua_touserdata(L, -1);
	lua_pop(L, 3);
	dd("###ngx_http_lua_sem_map_get###\tgettop(L)=%d; sem_key:%s; sem:0x%08x", lua_gettop(L), sem_key, sem);
	return sem;
}

static void 
ngx_http_lua_sem_map_set(lua_State *L, char *sem_key, sem_t *sem)
{
	dd("###ngx_http_lua_sem_map_set###\tgettop(L)=%d", lua_gettop(L));
	lua_getglobal(L, "ngx");
	lua_getfield(L, -1, "_sem_map");
	lua_pushlightuserdata(L, sem);
	lua_setfield(L, -2, sem_key);
	lua_pop(L, 2);
	dd("###ngx_http_lua_sem_map_set###\tgettop(L)=%d; sem_key:%s; sem:0x%08x", lua_gettop(L), sem_key, sem);
}

static void
ngx_http_lua_sem_map_del(lua_State *L, char *sem_key)
{
	dd("###ngx_http_lua_sem_map_del###\tgettop(L)=%d", lua_gettop(L));
	lua_getglobal(L, "ngx");
	lua_getfield(L, -1, "_sem_map");
	lua_pushnil(L);
	lua_setfield(L, -2, sem_key);
	lua_pop(L, 2);
	dd("###ngx_http_lua_sem_map_del###\tgettop(L)=%d; sem_key:%s", lua_gettop(L), sem_key);
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
