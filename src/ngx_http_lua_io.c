
/*
 * Copyright (C) Alex Zhang
 */


#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_lua_io.h"


static ngx_chain_t *ngx_http_lua_io_chain_to_iovec(ngx_iovec_t *vec,
    ngx_chain_t *cl);
static ngx_int_t ngx_http_lua_io_thread_post_task(ngx_thread_task_t *task,
    ngx_http_lua_io_file_ctx_t *file_ctx);
static void ngx_http_lua_io_thread_write_chain_to_file(void *data,
    ngx_log_t *log);
static void ngx_http_lua_io_thread_read_file(void *data, ngx_log_t *log);


static ngx_chain_t *
ngx_http_lua_io_chain_to_iovec(ngx_iovec_t *vec, ngx_chain_t *cl)
{
    size_t         total, size;
    u_char        *prev;
    ngx_uint_t     n;
    struct iovec  *iov;

    iov = NULL;
    prev = NULL;
    total = 0;
    n = 0;

    for ( /* void */ ; cl; cl = cl->next) {

        if (ngx_buf_special(cl->buf)) {
            continue;
        }

        size = cl->buf->last - cl->buf->pos;

        if (prev == cl->buf->pos) {
            iov->iov_len += size;

        } else {
            if (n == vec->nalloc) {
                break;
            }

            iov = &vec->iovs[n++];

            iov->iov_base = (void *) cl->buf->pos;
            iov->iov_len = size;
        }

        prev = cl->buf->pos + size;
        total += size;
    }

    vec->count = n;
    vec->size = total;

    return cl;
}


static ngx_int_t
ngx_http_lua_io_thread_post_task(ngx_thread_task_t *task,
    ngx_http_lua_io_file_ctx_t *file_ctx)
{
    ngx_http_request_t  *r;

    r = file_ctx->request;

    task->event.data = file_ctx;
    task->event.handler = file_ctx->handler;

    if (ngx_thread_task_post(file_ctx->thread_pool, task) != NGX_OK) {
        return NGX_ERROR;
    }

    r->main->blocked++;
    r->aio = 1;

    return NGX_OK;
}


static void
ngx_http_lua_io_thread_write_chain_to_file(void *data, ngx_log_t *log)
{
    ngx_http_lua_io_thread_ctx_t *ctx = data;

    ssize_t        n;
    ngx_err_t      err;
    ngx_chain_t   *cl;
    ngx_iovec_t    vec;
    struct iovec   iovs[NGX_IOVS_PREALLOCATE];

    vec.iovs = iovs;
    vec.nalloc = NGX_IOVS_PREALLOCATE;

    cl = ctx->chain;

    ctx->nbytes = 0;
    ctx->err = 0;

    if (cl == NULL && ctx->flush) {
        goto flush;
    }

    do {
        /* create the iovec and coalesce the neighbouring bufs */
        cl = ngx_http_lua_io_chain_to_iovec(&vec, cl);

eintr:

        n = writev(ctx->fd, iovs, vec.count);

        if (n == -1) {
            err = ngx_errno;

            if (err == NGX_EINTR) {
                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, err,
                               "writev() was interrupted");
                goto eintr;
            }

            ctx->err = err;
            return;
        }

        if ((size_t) n != vec.size) {
            ctx->nbytes = 0;
            return;
        }

        ctx->nbytes += n;
    } while (cl);

flush:

    if(ctx->flush && fsync(ctx->fd) < 0) {
        ctx->err = ngx_errno;
    }
}


static void
ngx_http_lua_io_thread_read_file(void *data, ngx_log_t *log)
{
    ngx_http_lua_io_thread_ctx_t *ctx = data;

    ssize_t        n;
    size_t         size;

    ctx->nbytes = 0;
    ctx->err = 0;
    ctx->eof = 0;

    size = ctx->size;
    if (size == 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
                       "lua io thread read zero bytes");
        return;
    }

    n = read(ctx->fd, ctx->buf, size);

    if (n == -1) {
        ctx->err = ngx_errno;

    } else {
        ctx->nbytes = n;
        ctx->err = 0;

        if ((size_t) n < size) {
            ctx->eof = 1;
        }
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, log, 0,
                   "lua io thread read %z (err: %d) of %uz, eof:%d",
                   n, ctx->err, size, ctx->eof);
}


ngx_int_t
ngx_http_lua_io_thread_post_write_task(ngx_http_lua_io_file_ctx_t *file_ctx,
    ngx_chain_t *cl, ngx_int_t flush)
{
    ngx_thread_task_t             *task;
    ngx_http_lua_io_thread_ctx_t  *thread_ctx;
    ngx_http_request_t            *r;

    r = file_ctx->request;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua io thread write chain: %d, %p flush:%d",
                   file_ctx->fd, cl, flush);

    task = file_ctx->thread_task;

    if (task == NULL) {
        task = ngx_thread_task_alloc(r->pool,
                                     sizeof(ngx_http_lua_io_thread_ctx_t));
        if (task == NULL) {
            file_ctx->ft_type |= NGX_HTTP_LUA_IO_FT_NO_MEMORY;
            return NGX_ERROR;
        }

        file_ctx->thread_task = task;
    }

    task->handler = ngx_http_lua_io_thread_write_chain_to_file;

    thread_ctx = task->ctx;
    thread_ctx->fd = file_ctx->fd;
    thread_ctx->chain = cl;
    thread_ctx->flush = flush;

    if (ngx_http_lua_io_thread_post_task(task, file_ctx) != NGX_OK) {
        file_ctx->ft_type |= NGX_HTTP_LUA_IO_FT_TASK_POST_ERROR;
        return NGX_ERROR;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_lua_io_thread_post_read_task(ngx_http_lua_io_file_ctx_t *file_ctx,
    ngx_buf_t *buf)
{
    ngx_thread_task_t             *task;
    ngx_http_lua_io_thread_ctx_t  *thread_ctx;
    ngx_http_request_t            *r;

    r = file_ctx->request;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "lua io thread read: %d", file_ctx->fd);

    task = file_ctx->thread_task;

    if (task == NULL) {
        task = ngx_thread_task_alloc(r->pool,
                                     sizeof(ngx_http_lua_io_thread_ctx_t));
        if (task == NULL) {
            file_ctx->ft_type |= NGX_HTTP_LUA_IO_FT_NO_MEMORY;
            return NGX_ERROR;
        }

        file_ctx->thread_task = task;
    }

    task->handler = ngx_http_lua_io_thread_read_file;

    thread_ctx = task->ctx;
    thread_ctx->fd = file_ctx->fd;
    thread_ctx->buf = buf->last;
    thread_ctx->size = buf->end - buf->last;

    if (ngx_http_lua_io_thread_post_task(task, file_ctx) != NGX_OK) {
        file_ctx->ft_type |= NGX_HTTP_LUA_IO_FT_TASK_POST_ERROR;
        return NGX_ERROR;
    }

    return NGX_OK;
}
