#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* FIXME: this modules goes far beyond what the lua-nginx-module public API
 * provides and uses the actual headers for now. */
#include <ddebug.h>
#include <ngx_http_lua_common.h>
#include <ngx_http_lua_util.h>

#include "serialize.h"

// FIXME: test only
#include <pthread.h>

#ifndef NGX_THREADS
# error thread support required
#endif

typedef enum {
    LUA_THREADPOOL_TASK_QUEUED,
    LUA_THREADPOOL_TASK_RUNNING,
    LUA_THREADPOOL_TASK_SUCCESS,
    LUA_THREADPOOL_TASK_FAILED,
    LUA_THREADPOOL_TASK_DESTROYED,
} ngx_http_resty_threadpool_task_status_t;

typedef struct {
} ngx_http_resty_threadpool_conf_t;

typedef struct {
    ngx_http_lua_co_ctx_t       *coctx;
    ngx_http_request_t *r;
    lua_State *L;
    ngx_int_t nres; /* result count */
    ngx_http_resty_threadpool_task_status_t status;
} ngx_thread_lua_task_ctx_t;

static ngx_int_t
ngx_http_resty_threadpool_resume(ngx_http_request_t *r);

static ngx_http_module_t  ngx_http_resty_threadpool_module_ctx = {
    NULL,                          /* preconfiguration */
    NULL,                          /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    NULL,                          /* create location configuration */
    NULL                           /* merge location configuration */
};

ngx_module_t  ngx_http_resty_threadpool_module = {
    NGX_MODULE_V1,
    &ngx_http_resty_threadpool_module_ctx, /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void
ngx_http_resty_threadpool_task_handler(void *data, ngx_log_t *log)
{
    /* called from inside the worker thread: responsible to run the actual Lua
     * code in the thread state.
     */
    ngx_thread_lua_task_ctx_t *ctx = data;
    lua_State                 *L = ctx->L;

    // TODO: create an actual lua-nginx-module state
    // TODO: cache states (thread local)
    ctx->status = LUA_THREADPOOL_TASK_RUNNING;

    // TODO: support for loading bytecode
    ngx_http_lua_assert(lua_type(L, 1) == LUA_TSTRING);
    if (luaL_dostring(L, lua_tostring(L, 1))) {
        const char *msg = lua_tostring(L, -1);
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "failed to run lua code in thread: %s", msg);
        goto failed;
    }

    /* serialize returned values */
    /* TODO: bench that, matbe it worth making a special case when the result
     * is a single string (avoid to make too many copies).
     * TODO: support multiple resuts
     */
    switch (lua_gettop(L)) {
    case 0:
        ctx->nres = 0;
        break;
    case 1:
        ctx->nres = 1;
        luaser_encode(L, 1);
        break;
    default:
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "multiple results not handled in lua thread (got %d values)", lua_gettop(L));
        goto failed;
    }

    ctx->status = LUA_THREADPOOL_TASK_SUCCESS;
    lua_close(L);
    return;
failed:
    ctx->nres = 0;
    ctx->status = LUA_THREADPOOL_TASK_FAILED;
    lua_close(L);
}

static void
ngx_http_resty_threadpool_thread_event_handler(ngx_event_t *ev)
{
    /* called in the main event loop after task completion. Responsible of
     * copying task result(s) into the calling coroutine */
    ngx_connection_t            *c;
    ngx_http_request_t          *r;
    ngx_thread_lua_task_ctx_t   *ctx;
    ngx_http_lua_ctx_t          *luactx;
    ngx_http_lua_co_ctx_t       *coctx;

    ctx = ev->data;
    coctx = ctx->coctx;
    ngx_http_lua_assert(coctx->data == ev->data);

    r = ctx->r;
    c = r->connection;

    luactx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (luactx == NULL) {
        lua_close(ctx->L);
        ctx->status = LUA_THREADPOOL_TASK_DESTROYED;
        return; /* not sure what it means in this case */
    }

    if (c->fd != (ngx_socket_t) -1) {  /* not a fake connection */
        ngx_http_log_ctx_t *log_ctx = c->log->data;
        log_ctx->current_request = r;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "lua task completed with %d results: \"%V?%V\"",
                  ctx->nres, &r->uri, &r->args);

    /* push results into the main coroutine */
    /* prepare_retvals(r, u, ctx->cur_co_ctx->co); */
    /*  >>>>>> TODO <<<<<< */
    if (ctx->nres == 1) {
        const char *res;
        size_t reslen;
        ngx_http_lua_assert(lua_type(ctx-L, -1) == LUA_TSTRING);
        res = lua_tolstring(ctx->L, -1, &reslen);
        luaser_decode(coctx->co, res, reslen);
        lua_close(ctx->L);
    }
    ctx->status = LUA_THREADPOOL_TASK_DESTROYED;
    coctx->cleanup = NULL;

    luactx->cur_co_ctx = coctx;
    if (luactx->entered_content_phase) {
        (void) ngx_http_resty_threadpool_resume(r);
    } else {
        luactx->resume_handler = ngx_http_resty_threadpool_resume;
        ngx_http_core_run_phases(r);
    }

    ngx_http_run_posted_requests(c);
}

/* copy of ngx_http_lua_sleep_resume */
static ngx_int_t
ngx_http_resty_threadpool_resume(ngx_http_request_t *r)
{
    lua_State                   *vm;
    ngx_connection_t            *c;
    ngx_int_t                    rc;
    ngx_http_lua_ctx_t          *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->resume_handler = ngx_http_lua_wev_handler;

    c = r->connection;
    vm = ngx_http_lua_get_lua_vm(r, ctx);

    /* FIXME: result handling is a mess, clean it up and do everything here */
    rc = ngx_http_lua_run_thread(vm, r, ctx, ((ngx_thread_lua_task_ctx_t *)(ctx->cur_co_ctx->data))->nres);

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
ngx_http_resty_threadpool_task_cleanup(void *data)
{
    ngx_log_debug0(NGX_LOG_CRIT, ngx_cycle->log, 0,
                   "ngx_http_resty_threadpool_task_cleanup: this is not really supposed to happen.");
    /* TODO: free buffer memory for SUCCESS or FAILED states */
}

static int
ngx_http_resty_threadpool_push_task(lua_State *L) {
    ngx_http_request_t          *r;
    ngx_thread_pool_t           *tp;
    ngx_thread_task_t           *task;
    ngx_thread_lua_task_ctx_t   *ctx;
    ngx_http_lua_ctx_t          *luactx;
    ngx_http_lua_co_ctx_t       *coctx;
    ngx_str_t                    pool;
    const char                  *code;
    size_t                       codelen;

    r = ngx_http_lua_get_req(L);
    if (r == NULL) {
        return luaL_error(L, "no request found");
    }

    pool.data = (u_char *)luaL_checklstring(L, 1, &pool.len);
    code = luaL_checklstring(L, 2, &codelen);

    luactx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (luactx == NULL) {
        return luaL_error(L, "no request ctx found");
    }

    coctx = luactx->cur_co_ctx;
    if (coctx == NULL) {
        return luaL_error(L, "no co ctx found");
    }

    // find the thread pool
    // TODO: handle the default case?
    tp = ngx_thread_pool_get((ngx_cycle_t *) ngx_cycle, &pool);
    if (tp == NULL) {
        return luaL_error(L, "no pool '%s' found", pool.data);
    }

    // create the task
    task = ngx_thread_task_alloc(r->pool, sizeof(ngx_thread_lua_task_ctx_t));
    if (task == NULL) {
        return luaL_error(L, "failed to allocate task");
    }

    task->handler = ngx_http_resty_threadpool_task_handler;
    ctx = task->ctx;
    ctx->status = LUA_THREADPOOL_TASK_QUEUED;
    ctx->coctx = coctx;
    ctx->r = r;

    /* prepare the state: just push the code for now, the actual loading will
     * be done in thread */
    ctx->L = luaL_newstate();
    if (ctx->L == NULL) {
        return luaL_error(L, "failed to create task state");
    }

    luaL_openlibs(ctx->L); /* TODO: no time for that in I/O loop, do it in thread */
    lua_pushlstring(ctx->L, code, codelen);

    /* return handler */
    task->event.data = ctx;
    task->event.handler = ngx_http_resty_threadpool_thread_event_handler;

    // push task in queue
    ngx_http_lua_cleanup_pending_operation(coctx);
    coctx->cleanup = ngx_http_resty_threadpool_task_cleanup;
    coctx->data = ctx;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "post Lua task to thread pool %V", &pool);
    fprintf(stderr, "can I haz a thread: %ld\n", pthread_self());

    if (ngx_thread_task_post(tp, task) != NGX_OK) {
        return luaL_error(L, "failed to post task to queue");
    }

    return lua_yield(L, 0);
}

int
luaopen_resty_threadpool(lua_State *L) {
    fprintf(stderr, "luaded...\n");
    lua_pushcfunction(L, ngx_http_resty_threadpool_push_task);
    return 1;
}

