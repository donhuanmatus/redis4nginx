/*
 * Copyright (c) 2011-2012, Alexander Lyalin <alexandr.lyalin@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DDEBUG
#define DDEBUG 0
#endif

#include "ddebug.h"
#include "ngx_http_r4x_module.h"
#include "sha1.h"

static u_char null_value[] = "null";
static u_char array_value[] = "{\"err\": \"redis arrays do not send by http\"}";

static 
ngx_int_t ngx_http_r4x_prepare_reply(ngx_http_request_t *r, redisReply *reply, ngx_buf_t *buf) 
{
    switch(reply->type)
    {
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_ERROR:
            buf->pos = (u_char*)reply->str;
            buf->last = (u_char*)reply->str + reply->len; 
            r->headers_out.content_length_n = reply->len;
            break;
        case REDIS_REPLY_INTEGER:
        {
            char* str = ngx_palloc(r->pool, 64);
            int len;
            len = sprintf(str, "%lld", reply->integer);
            buf->pos = (u_char*)str;
            buf->last = (u_char*)str + len; 
            r->headers_out.content_length_n = len;            
            break;
        }
        case REDIS_REPLY_NIL:  
            buf->pos = (u_char*)null_value;
            buf->last = (u_char*)null_value + sizeof(null_value); 
            r->headers_out.content_length_n = sizeof(null_value);            
            break;
        case REDIS_REPLY_ARRAY:
            //TODO: add array to string
            buf->pos = (u_char*)array_value;
            buf->last = (u_char*)array_value + sizeof(array_value); 
            r->headers_out.content_length_n = sizeof(array_value); 
            break;
    };
    
    return NGX_OK;
}

void
ngx_http_r4x_send_redis_reply(ngx_http_request_t *r, redisAsyncContext *c, redisReply *reply)
{   
    ngx_int_t    rc;
    ngx_buf_t   *buf;
    ngx_chain_t  out;
     
    // allocate a buffer for your response body
    buf = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (buf == NULL) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
 
    // attach this buffer to the buffer chain
    out.buf = buf;
    out.next = NULL;
 
    // set the status line
    r->headers_out.status = NGX_HTTP_OK;
    buf->memory = 1;    // this buffer is in memory
    buf->last_buf = 1;  // this is the last buffer in the buffer chain
    
    if(ngx_http_r4x_prepare_reply(r, reply, buf) != NGX_OK)
    {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    
    // send the headers of your response
    rc = ngx_http_send_header(r);
 
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
 
    // send the buffer chain of your response
    rc = ngx_http_output_filter(r, &out);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    
    ngx_http_finalize_request(r, NGX_DONE);
}

/* Hash the scripit into a SHA1 digest. We use this as Lua function name.
 * Digest should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void ngx_http_r4x_sha1(ngx_str_t *digest, ngx_str_t *script) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)script->data, script->len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++) {
        digest->data[j*2] = cset[((hash[j]&0xF0)>>4)];
        digest->data[j*2+1] = cset[(hash[j]&0xF)];
    }
    
    digest->len = 40;
}

char* 
ngx_http_r4x_create_cstr_by_ngxstr(ngx_pool_t *pool, ngx_str_t *source, size_t offset, size_t len)
{
    ngx_pool_t *use_pool;
    char* result = NULL;
    use_pool = pool == NULL ? ngx_cycle->pool : pool;
    
    if(source != NULL && source->len > 0)
    {
        result = ngx_palloc(use_pool, len + 1);
        memcpy(result, source->data+offset, len);
        result[len] = '\0';   
    }
    
    return result;
}

ngx_int_t 
ngx_http_r4x_copy_ngxstr(ngx_pool_t *pool, ngx_str_t *dest, ngx_str_t *src, size_t offset, size_t len)
{
    ngx_pool_t *use_pool;
    use_pool = pool == NULL ? ngx_cycle->pool : pool;
    
    dest->data = ngx_palloc(use_pool, len);
    
    if(dest->data == NULL)
        return NGX_ERROR;
    
    memcpy(dest->data, src->data + offset, len);
    dest->len = len;
    
    return NGX_OK;
}

char* ngx_http_r4x_read_conf_file(ngx_conf_t *cf, ngx_str_t *file_path, ngx_str_t *buff)
{
    ngx_file_t                  file;
    ngx_file_info_t             fi;
    ngx_err_t                   err;
    size_t                      size;
    ssize_t                     n;
    
    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *file_path;
    file.log = cf->log;
    
    file.fd = ngx_open_file(file_path->data, NGX_FILE_RDONLY, 0, 0);
    if (file.fd == NGX_INVALID_FILE) {
        err = ngx_errno;
        if (err != NGX_ENOENT) {
            ngx_conf_log_error(NGX_LOG_CRIT, cf, err,
                               ngx_open_file_n " \"%s\" failed", file_path->data);
        }
        return NGX_CONF_ERROR;;
    }
    
    if (ngx_fd_info(file.fd, &fi) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, ngx_errno,
                           ngx_fd_info_n " \"%s\" failed", file_path->data);
        
        return NGX_CONF_ERROR;
    }
    
    size = (size_t) ngx_file_size(&fi);
    
    buff->data = ngx_palloc(cf->pool, size);
    buff->len = size;
    
    n = ngx_read_file(&file, buff->data, size, 0);
    
    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, ngx_errno,
                           ngx_read_file_n " \"%s\" failed", file_path->data);
        return NGX_CONF_ERROR;
    }

    if ((size_t) n != size) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, 0,
            ngx_read_file_n " \"%s\" returned only %z bytes instead of %z",
            file_path->data, n, size);
        
        return NGX_CONF_ERROR;
    }
    
    return NGX_CONF_OK;
}