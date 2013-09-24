/*
 * Copyright (C) Michael Skvortsov
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <zlib.h>

typedef struct {
	ngx_uint_t enable;
} ngx_http_gzip_static_auto_conf_t;

static ngx_int_t ngx_http_gzip_static_auto_handler(ngx_http_request_t *r);
static void *ngx_http_gzip_static_auto_create_conf(ngx_conf_t *cf);
static char *ngx_http_gzip_static_auto_merge_conf(ngx_conf_t *cf, void *parent,
		void *child);
static ngx_int_t ngx_http_gzip_static_auto_init(ngx_conf_t *cf);

static ngx_command_t ngx_http_gzip_static_auto_commands[] = {

{ ngx_string("gzip_static_auto"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF
		| NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_FLAG,
		ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_http_gzip_static_auto_conf_t, enable), NULL },

ngx_null_command };

ngx_http_module_t ngx_http_gzip_static_auto_module_ctx = { NULL, /* preconfiguration */
ngx_http_gzip_static_auto_init, /* postconfiguration */

NULL, /* create main configuration */
NULL, /* init main configuration */

NULL, /* create server configuration */
NULL, /* merge server configuration */

ngx_http_gzip_static_auto_create_conf, /* create location configuration */
ngx_http_gzip_static_auto_merge_conf /* merge location configuration */
};

ngx_module_t ngx_http_gzip_static_auto_module = { NGX_MODULE_V1,
		&ngx_http_gzip_static_auto_module_ctx, /* module context */
		ngx_http_gzip_static_auto_commands, /* module directives */
		NGX_HTTP_MODULE, /* module type */
		NULL, /* init master */
		NULL, /* init module */
		NULL, /* init process */
		NULL, /* init thread */
		NULL, /* exit thread */
		NULL, /* exit process */
		NULL, /* exit master */
		NGX_MODULE_V1_PADDING };

static ngx_int_t ngx_http_gzip_static_auto_handler(ngx_http_request_t *r) {
	u_char *p, *zin, *zout;
	size_t root;
	ssize_t have;
	ngx_fd_t cfd;
    z_stream strm;
	ngx_str_t path, op;
	ngx_buf_t *b;
	ngx_int_t rc, flush;
	ngx_uint_t chunk;
	ngx_chain_t out;
	ngx_table_elt_t *h;
    struct gztrailer *trailer;
	ngx_open_file_info_t of, ofo;
	ngx_http_core_loc_conf_t *clcf;
	ngx_http_gzip_static_auto_conf_t *gzcf;

	gzcf = ngx_http_get_module_loc_conf(r, ngx_http_gzip_static_auto_module);

	if (!gzcf->enable) {
		return NGX_DECLINED;
	}

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	//TODO: check if path has .gz in the end - if it does then consider it as oringal path AND cache path and if the original file couldn't be open - end the handling, else - read straight from the file

	p = ngx_http_map_uri_to_path(r, &path, &root, sizeof(".gz") - 1);
	if (p == NULL ) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}
	op.data = ngx_pcalloc(r->pool, sizeof(u_char)*path.len);
	op.len = path.len;
	ngx_cpystrn(op.data, path.data, path.len);

	*p++ = '.';
	*p++ = 'g';
	*p++ = 'z';
	*p = '\0';

	path.len = p - path.data;

	ngx_memzero(&ofo, sizeof(ngx_open_file_info_t));

	ofo.read_ahead = clcf->read_ahead;
	ofo.directio = clcf->directio;
	ofo.valid = clcf->open_file_cache_valid;
	ofo.min_uses = clcf->open_file_cache_min_uses;
	ofo.errors = clcf->open_file_cache_errors;
	ofo.events = clcf->open_file_cache_events;

	if (ngx_http_set_disable_symlinks(r, clcf, &op, &ofo) != NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	if (ngx_open_cached_file(clcf->open_file_cache, &op, &ofo, r->pool) != NGX_OK) {
		return NGX_DECLINED;
	}

	if (ofo.is_dir) {
		return NGX_DECLINED;
	}

#if !(NGX_WIN32) /* the not regular files are probably Unix specific */

	if (!ofo.is_file) {
		return NGX_HTTP_NOT_FOUND;
	}

#endif

	if (!ofo.fd)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	ngx_memzero(&of, sizeof(ngx_open_file_info_t));

	of.read_ahead = clcf->read_ahead;
	of.directio = clcf->directio;
	of.valid = clcf->open_file_cache_valid;
	of.min_uses = clcf->open_file_cache_min_uses;
	of.errors = clcf->open_file_cache_errors;
	of.events = clcf->open_file_cache_events;

	if (ngx_http_set_disable_symlinks(r, clcf, &path, &of) != NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool) != NGX_OK) {
		switch (of.err) {

		case 0:
			return NGX_HTTP_INTERNAL_SERVER_ERROR;

		case NGX_ENOENT:
			break;
		default:

			return NGX_DECLINED;

		}

		//Cache path not found - lets create one

		cfd = ngx_open_file(path.data, NGX_FILE_RDWR, NGX_FILE_CREATE_OR_OPEN, NGX_FILE_DEFAULT_ACCESS);
		if (cfd == NGX_INVALID_FILE)
		{
			//Cannot create new cache file
			return NGX_DECLINED;
		}

	    /* allocate deflate state */
	    strm.zalloc = Z_NULL;
	    strm.zfree = Z_NULL;
	    strm.opaque = Z_NULL;
	    rc = deflateInit(&strm, Z_BEST_COMPRESSION);
	    if (rc != Z_OK)
	        return NGX_DECLINED;

	    chunk = 16384;
	    zin = ngx_palloc(r->pool, chunk);
	    zout = ngx_palloc(r->pool, chunk);

	    /* compress until end of file */
	    do {
	        rc = ngx_read_fd(ofo.fd, zin, chunk);
	        if (rc == -1) {
	            (void)deflateEnd(&strm);
	            return NGX_DECLINED;
	        }
	        strm.avail_in = rc;
	        flush = (0 == strm.avail_in) ? Z_FINISH : Z_NO_FLUSH;
	        strm.next_in = zin;

	        /* run deflate() on input until output buffer not full, finish
	           compression if all of source has been read in */
	        do {
	            strm.avail_out = chunk;
	            strm.next_out = zout;
	            rc = deflate(&strm, flush);    /* no bad return value */

	            have = chunk - strm.avail_out;
	            if (ngx_write_fd(cfd, zout, have) != have) {
	                (void)deflateEnd(&strm);
	                return NGX_DECLINED;
	            }
	        } while (strm.avail_out == 0);

	        /* done when last data in file processed */
	    } while (flush != Z_FINISH);

	    /* clean up and return */
	    (void)deflateEnd(&strm);

	    ngx_close_file(cfd);

	    strm.avail_in = 0;
	    strm.avail_out = 0;

		if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool) != NGX_OK) {
			return NGX_DECLINED;
		}
	}

	if (of.is_dir) {
		return NGX_DECLINED;
	}

#if !(NGX_WIN32) /* the not regular files are probably Unix specific */

	if (!of.is_file) {
		return NGX_HTTP_NOT_FOUND;
	}

#endif

	r->root_tested = !r->error_page;

	rc = ngx_http_discard_request_body(r);

	if (rc != NGX_OK) {
		return rc;
	}

	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_length_n = of.size;
	r->headers_out.last_modified_time = of.mtime;

	if (ngx_http_set_etag(r) != NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	if (ngx_http_set_content_type(r) != NGX_OK) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	h = ngx_list_push(&r->headers_out.headers);
	if (h == NULL ) {
		return NGX_ERROR;
	}

	h->hash = 1;
	ngx_str_set(&h->key, "Content-Encoding");
	ngx_str_set(&h->value, "deflate");
	r->headers_out.content_encoding = h;

	r->ignore_content_encoding = 1;

	/* we need to allocate all before the header would be sent */

	b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
	if (b == NULL ) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	if (b->file == NULL ) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	rc = ngx_http_send_header(r);

	if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
		return rc;
	}

	b->file_pos = 0;
	b->file_last = of.size;

	b->in_file = b->file_last ? 1 : 0;
	b->last_buf = (r == r->main) ? 1 : 0;
	b->last_in_chain = 1;

	b->file->fd = of.fd;
	b->file->name = path;
	b->file->directio = of.is_directio;

	out.buf = b;
	out.next = NULL;

	return ngx_http_output_filter(r, &out);
}

static void *
ngx_http_gzip_static_auto_create_conf(ngx_conf_t *cf) {
	ngx_http_gzip_static_auto_conf_t *conf;

	conf = ngx_palloc(cf->pool, sizeof(ngx_http_gzip_static_auto_conf_t));
	if (conf == NULL ) {
		return NULL ;
	}

	conf->enable = NGX_CONF_UNSET_UINT;

	return conf;
}

static char *
ngx_http_gzip_static_auto_merge_conf(ngx_conf_t *cf, void *parent, void *child) {
	ngx_http_gzip_static_auto_conf_t *prev = parent;
	ngx_http_gzip_static_auto_conf_t *conf = child;

	ngx_conf_merge_uint_value(conf->enable, prev->enable, 0);

	return NGX_CONF_OK ;
}

static ngx_int_t ngx_http_gzip_static_auto_init(ngx_conf_t *cf) {
	ngx_http_handler_pt *h;
	ngx_http_core_main_conf_t *cmcf;

	cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module) ;

	h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);
	if (h == NULL ) {
		return NGX_ERROR;
	}

	*h = ngx_http_gzip_static_auto_handler;

	return NGX_OK;
}
