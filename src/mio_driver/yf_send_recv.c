#include <ppc/yf_header.h>
#include <base_struct/yf_core.h>
#include <mio_driver/yf_event.h>
#include "yf_send_recv.h"

#ifndef IOV_MAX
#define YF_IOVS  64
#else

#if (IOV_MAX > 64)
#define YF_IOVS  64
#else
#define YF_IOVS  IOV_MAX
#endif

#endif

#define _yf_unix_rcheck(n, size, rev, ctx, err) \
                yf_log_debug3(YF_LOG_DEBUG, rev->log, 0, \
                               "recv: fd:%d %d of %d", rev->fd, n, size); \
                if (n == 0) \
                { \
                        rev->ready = 0; \
                        rev->eof = 1; \
                        return n; \
                } \
                else if (n > 0) \
                { \
                        if ((size_t)n < size) \
                                rev->ready = 0; \
                        ctx->rw_cnt += n; \
                        return n; \
                } \
                err = yf_socket_errno; \
                if (YF_EAGAIN(err)) \
                { \
                        rev->ready = 0; \
                        yf_log_debug0(YF_LOG_DEBUG, rev->log, err, \
                                       "recv() not ready"); \
                        return YF_AGAIN; \
                } \
                else if (err == YF_EINTR) \
                        continue; \
                else { \
                        rev->ready = 0; \
                        rev->error = 1; \
                        yf_log_error(YF_LOG_ERR, rev->log, err, "recv() failed"); \
                        return YF_ERROR; \
                }        

ssize_t
yf_unix_recvfrom(fd_rw_ctx_t *ctx, char *buf, size_t size
                , int flags, struct sockaddr *from, socklen_t *fromlen)
{
        ssize_t n;
        yf_err_t err;
        yf_fd_event_t *rev;

        rev = ctx->fd_evt;

        do {
                n = yf_recvfrom(rev->fd, buf, size, flags, from, fromlen);
                _yf_unix_rcheck(n, size, rev, ctx, err);
        } 
        while (1);
}


ssize_t yf_unix_readv(fd_rw_ctx_t *ctx, const struct iovec *iov, int iovcnt)
{
        ssize_t n, size = 0;
        yf_err_t err;
        yf_fd_event_t *rev;
        int i = 0;

        for (i = 0; i < iovcnt; ++i)
                size += iov[i].iov_len;
        rev = ctx->fd_evt;

        do {
                n = yf_readv(rev->fd, iov, iovcnt);
                _yf_unix_rcheck(n, size, rev, ctx, err);
        } 
        while (1);        
}


ssize_t
yf_readv_chain(fd_rw_ctx_t *ctx, yf_chain_t *chain)
{
        char *prev;
        ssize_t n, size, received;
        yf_err_t err;
        yf_array_t vec;
        yf_fd_event_t *rev;
        yf_chain_t *cl;
        struct iovec *iov, iovs[YF_IOVS];

        prev = NULL;
        iov = NULL;
        size = 0;

        vec.elts = iovs;
        vec.nelts = 0;
        vec.size = sizeof(struct iovec);
        vec.nalloc = YF_IOVS;
        vec.pool = ctx->pool;

        rev = ctx->fd_evt;

        /* coalesce the neighbouring bufs */

        for (cl = chain; cl; cl = cl->next)
        {
                if (prev == cl->buf->last)
                {
                        iov->iov_len += cl->buf->end - cl->buf->last;
                }
                else {
                        iov = yf_array_push(&vec);
                        if (iov == NULL)
                        {
                                return YF_ERROR;
                        }

                        iov->iov_base = (void *)cl->buf->last;
                        iov->iov_len = cl->buf->end - cl->buf->last;
                }

                size += cl->buf->end - cl->buf->last;
                prev = cl->buf->end;
        }

        yf_log_debug2(YF_LOG_DEBUG, rev->log, 0,
                       "readv: %d:%d", vec.nelts, iov->iov_len);
        if (iov->iov_len == 0)
        {
                yf_log_error(YF_LOG_WARN, rev->log, 0, "fd[%d] readv buf size=0", rev->fd);
                return  0;
        }

        do {
                n = yf_readv(rev->fd, (struct iovec *)vec.elts, vec.nelts);
                yf_log_debug2(YF_LOG_DEBUG, rev->log, 0, "fd readv [%d] ret %d", 
                                rev->fd, n);

                if (n == 0)
                {
                        rev->ready = 0;
                        rev->eof = 1;

                        return n;
                }
                else if (n > 0)
                {
                        if (n < size)
                        {
                                rev->ready = 0;
                        }
                        received = n;

                        for (cl = chain; cl; cl = cl->next)
                        {
                                if (received == 0)
                                        break;

                                size = cl->buf->end - cl->buf->last;

                                if (received >= size)
                                {
                                        received -= size;
                                        cl->buf->last = cl->buf->end;
                                        continue;
                                }
                                cl->buf->last += received;
                                break;
                        }
                        
                        ctx->rw_cnt += n;
                        return n;
                }

                err = yf_socket_errno;

                if (YF_EAGAIN(err) || err == YF_EINTR)
                {
                        yf_log_debug0(YF_LOG_DEBUG, rev->log, err,
                                       "readv() not ready");
                        n = YF_AGAIN;
                }
                else {
                        n = YF_ERROR;
                        yf_log_error(YF_LOG_ERR, rev->log, err, "readv() failed");
                        break;
                }
        } 
        while (err == YF_EINTR);

        rev->ready = 0;

        if (n == YF_ERROR)
        {
                rev->error = 1;
        }

        return n;
}


#define _yf_unix_wcheck(n, size, wev, ctx, err) \
                yf_log_debug3(YF_LOG_DEBUG, wev->log, 0, \
                               "send: fd:%d %d of %d", wev->fd, n, size); \
                if (n > 0) \
                { \
                        if (n < (ssize_t)size) \
                                wev->ready = 0; \
                        ctx->rw_cnt += n; \
                        return n; \
                } \
                err = yf_socket_errno; \
                if (n == 0) \
                { \
                        yf_log_error(YF_LOG_ALERT, wev->log, err, "send() returned zero"); \
                        wev->ready = 0; \
                        return n; \
                } \
                if (YF_EAGAIN(err) || err == YF_EINTR) \
                { \
                        wev->ready = 0; \
                        yf_log_debug0(YF_LOG_DEBUG, wev->log, err, \
                                       "send() not ready"); \
                        if (YF_EAGAIN(err)) \
                        { \
                                return YF_AGAIN; \
                        } \
                } \
                else { \
                        wev->error = 1; \
                        yf_log_error(YF_LOG_ERR, wev->log, err, "send() failed"); \
                        return YF_ERROR; \
                }


ssize_t
yf_unix_sendto(fd_rw_ctx_t *ctx, char *buf, size_t size
                , int flags, const struct sockaddr *to, socklen_t tolen)
{
        ssize_t n;
        yf_err_t err;
        yf_fd_event_t *wev;

        wev = ctx->fd_evt;

        for (;; )
        {
                n = yf_sendto(wev->fd, buf, size, flags, to, tolen);
                _yf_unix_wcheck(n, size, wev, ctx, err);
        }
}


ssize_t yf_unix_writev(fd_rw_ctx_t *ctx, const struct iovec *iov, int iovcnt)
{
        int i = 0;
        ssize_t n, size = 0;
        yf_err_t err;
        yf_fd_event_t *wev;

        for (i = 0; i < iovcnt; ++i)
                size += iov[i].iov_len;

        wev = ctx->fd_evt;

        for (;; )
        {
                n = yf_writev(wev->fd, iov, iovcnt);
                _yf_unix_wcheck(n, size, wev, ctx, err);
        }
}


yf_chain_t *
yf_writev_chain(fd_rw_ctx_t *ctx, yf_chain_t *in, off_t limit)
{
        char *prev;
        ssize_t n, size, sent;
        off_t send, prev_send;
        yf_uint_t eintr, complete;
        yf_err_t err;
        yf_array_t vec;
        yf_chain_t *cl;
        yf_fd_event_t *wev;
        struct iovec *iov, iovs[YF_IOVS];

        wev = ctx->fd_evt;

        if (!wev->ready)
        {
                return in;
        }

        /* the maximum limit size is the maximum size_t value - the page size */

        if (limit == 0 || limit > (off_t)(INT32_MAX - yf_pagesize))
        {
                limit = INT32_MAX - yf_pagesize;
        }

        send = 0;
        complete = 0;

        vec.elts = iovs;
        vec.size = sizeof(struct iovec);
        vec.nalloc = YF_IOVS;
        vec.pool = ctx->pool;

        for (;; )
        {
                prev = NULL;
                iov = NULL;
                eintr = 0;
                prev_send = send;

                vec.nelts = 0;

                /* create the iovec and coalesce the neighbouring bufs */

                for (cl = in; cl && vec.nelts < YF_IOVS && send < limit; cl = cl->next)
                {
                        size = cl->buf->last - cl->buf->pos;

                        if (send + size > limit)
                        {
                                size = (ssize_t)(limit - send);
                        }

                        if (prev == cl->buf->pos)
                        {
                                iov->iov_len += size;
                        }
                        else {
                                iov = yf_array_push(&vec);
                                if (iov == NULL)
                                {
                                        return YF_CHAIN_ERROR;
                                }

                                iov->iov_base = (void *)cl->buf->pos;
                                iov->iov_len = size;
                        }

                        prev = cl->buf->pos + size;
                        send += size;
                }

                n = yf_writev(wev->fd, vec.elts, vec.nelts);

                if (n == -1)
                {
                        err = yf_errno;

                        if (!YF_EAGAIN(err))
                        {
                                switch (err)
                                {
                                case YF_EINTR:
                                        eintr = 1;
                                        break;

                                default:
                                        wev->error = 1;
                                        yf_log_error(YF_LOG_ERR, wev->log, err, "writev() failed");
                                        return YF_CHAIN_ERROR;
                                }
                        }

                        yf_log_debug0(YF_LOG_DEBUG, wev->log, err,
                                       "writev() not ready");
                }

                sent = n > 0 ? n : 0;

                yf_log_debug1(YF_LOG_DEBUG, wev->log, 0, "writev: %z", sent);

                if (send - prev_send == sent)
                {
                        complete = 1;
                }

                ctx->rw_cnt += sent;

                for (cl = in; cl; cl = cl->next)
                {
                        if (sent == 0)
                        {
                                break;
                        }

                        size = cl->buf->last - cl->buf->pos;

                        if (sent >= size)
                        {
                                sent -= size;
                                cl->buf->pos = cl->buf->last;

                                continue;
                        }

                        cl->buf->pos += sent;

                        break;
                }

                if (eintr)
                {
                        continue;
                }

                if (!complete)
                {
                        wev->ready = 0;
                        return cl;
                }

                if (send >= limit || cl == NULL)
                {
                        return cl;
                }

                in = cl;
        }
}
