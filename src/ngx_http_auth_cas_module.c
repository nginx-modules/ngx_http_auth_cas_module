#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <ctype.h>

#define CAS_SERVICE_PARAM  "?service="
#define CAS_COOKIE_NAME    "CASC"

typedef struct {
	/* CAS authentication required? */
	ngx_flag_t auth_cas;

	/* name of service ticket cookie */
	ngx_str_t auth_cas_cookie;

	/* CAS server login URL */
	ngx_str_t auth_cas_login_url;

	/* our base URL - don't reconstruct service URL from Host header, see https://wiki.jasig.org/display/CASC/CASFilter */
	ngx_str_t auth_cas_service_url;
} ngx_http_auth_cas_ctx_t;

ngx_module_t ngx_http_auth_cas_module;

static int find_cookie(ngx_http_request_t *r, ngx_str_t name, ngx_str_t *value) {
	const ngx_table_elt_t *cookie = *(ngx_table_elt_t **) r->headers_in.cookies.elts;
	int nelts;

	for (nelts = r->headers_in.cookies.nelts; nelts > 0; nelts--, cookie++) {
		u_char *start = cookie->value.data;
		u_char *end   = cookie->value.data + cookie->value.len;

		while (start < end) {
			/* skip leading whitespace */
			while (isspace((*start))) start++;

			u_char *equals_sign = memchr(start, '=', end - start);
			if (equals_sign == NULL) {
				break;
			}

			u_char *val = equals_sign + 1;
			u_char *semicolon = memchr(val, ';', end - val);

			if ((size_t) (equals_sign - start) == name.len
				&& ngx_memcmp(start, name.data, name.len) == 0)
			{
				value->len = end - val;
				if (!semicolon) {
					value->data = val;
				} else {
					/* part of a "foo=42; bar=1337" string, make a copy */
					if (NULL == (value->data = ngx_pnalloc(r->pool, value->len + 1))) {
						return 0;
					}
					*(ngx_cpymem(value->data, val, value->len)) = '\0';
				}
				return 1;
			}

			if (semicolon) {
				start = semicolon + 1;
			} else {
				break;
			}
		}
	}

	return 0;
}

static ngx_int_t send_redirect(ngx_http_request_t *r, const ngx_str_t location) {
	ngx_table_elt_t *loc = ngx_list_push(&r->headers_out.headers);

	if (loc == NULL) {
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	loc->hash = 1;
	ngx_str_set(&loc->key, "Location");
	loc->value = location;
	r->headers_out.location = loc;

	return NGX_HTTP_MOVED_TEMPORARILY;
}

static int create_login_url(ngx_http_request_t *r, ngx_str_t *s) {
	const ngx_http_auth_cas_ctx_t *ctx = ngx_http_get_module_loc_conf(r, ngx_http_auth_cas_module);

	s->len = ctx->auth_cas_login_url.len
			+ sizeof(CAS_SERVICE_PARAM)
			+ ctx->auth_cas_service_url.len
			+ (r->uri.len * 3)
			+ 3 /* %3F == '?' */
			+ (r->args.len * 3);

	s->data = ngx_pnalloc(r->pool, s->len);

	if (!s->data) {
		return 0;
	}

	u_char *p = s->data;
	p = ngx_cpymem(p, ctx->auth_cas_login_url.data, ctx->auth_cas_login_url.len);
	p = ngx_cpymem(p, CAS_SERVICE_PARAM, sizeof(CAS_SERVICE_PARAM) - 1);
	p = ngx_cpymem(p, ctx->auth_cas_service_url.data, ctx->auth_cas_service_url.len);
	p = (u_char *) ngx_escape_uri(p, r->uri.data, r->uri.len, NGX_ESCAPE_ARGS);

	/* nginx stores the path and query string as a single string so that
	 * (r->uri.data + r->uri.len + 1) == r->args.data but is that a stable API?
	 */
	if (r->args.len) {
		*p++ = '%';
		*p++ = '3';
		*p++ = 'F';
		p = (u_char *) ngx_escape_uri(p, r->args.data, r->args.len, NGX_ESCAPE_ARGS);
	}

	*p = '\0';

	s->len = p - s->data;

	return 1;
}
static ngx_int_t ngx_http_auth_cas_handler(ngx_http_request_t *r) {
	const ngx_http_auth_cas_ctx_t *ctx = ngx_http_get_module_loc_conf(r, ngx_http_auth_cas_module);

	if (!ctx->auth_cas) {
		return NGX_DECLINED;
	}

	ngx_str_t cookie = ngx_null_string;

	if (!find_cookie(r, ctx->auth_cas_cookie, &cookie)) {
		/* redirect to CAS server */
		ngx_str_t location;

		if (!create_login_url(r, &location)) {
			return NGX_HTTP_INTERNAL_SERVER_ERROR;
		}

		return send_redirect(r, location);
	}

	return NGX_HTTP_UNAUTHORIZED;
}

static char *set_auth_cas_service_url(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
	ngx_http_auth_cas_ctx_t *ctx = conf;
	ngx_str_t *value = (ngx_str_t *) cf->args->elts + 1;

	/* URL-escape service URL */
	ctx->auth_cas_service_url.len = value->len + ngx_escape_uri(NULL, value->data, value->len, NGX_ESCAPE_ARGS);
	ctx->auth_cas_service_url.data = ngx_pcalloc(cf->pool, ctx->auth_cas_service_url.len + 1);
	ngx_escape_uri(ctx->auth_cas_service_url.data, value->data, value->len, NGX_ESCAPE_ARGS);

	return NGX_CONF_OK;
}

static void *ngx_http_auth_cas_create_loc_conf(ngx_conf_t *cf) {
	ngx_http_auth_cas_ctx_t *ctx = ngx_pcalloc(cf->pool, sizeof(*ctx));

	ctx->auth_cas = NGX_CONF_UNSET;
	ngx_str_null(&ctx->auth_cas_login_url);
	ngx_str_null(&ctx->auth_cas_service_url);
	ngx_str_null(&ctx->auth_cas_cookie);

	return ctx;
}

static char *ngx_http_auth_cas_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
	const ngx_http_auth_cas_ctx_t *prev = parent;
	ngx_http_auth_cas_ctx_t *conf = child;

	ngx_conf_merge_value(conf->auth_cas, prev->auth_cas, 0);
	ngx_conf_merge_str_value(conf->auth_cas_cookie, prev->auth_cas_cookie, CAS_COOKIE_NAME);

	if (conf->auth_cas_login_url.data == NULL) {
		conf->auth_cas_login_url = prev->auth_cas_login_url;
	}

	if (conf->auth_cas_service_url.data == NULL) {
		conf->auth_cas_service_url = prev->auth_cas_service_url;
	}

	return NGX_CONF_OK;
}

static ngx_int_t ngx_http_auth_cas_init(ngx_conf_t *cf) {
	ngx_http_core_main_conf_t *corecf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

	ngx_http_handler_pt *handler = ngx_array_push(&corecf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
	if (handler == NULL) {
		return NGX_ERROR;
	}
	*handler = ngx_http_auth_cas_handler;

	return NGX_OK;
}

static ngx_command_t commands[] = {
	{
		ngx_string("auth_cas"),
		NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
		ngx_conf_set_flag_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_http_auth_cas_ctx_t, auth_cas),
		NULL
	},
	{
		ngx_string("auth_cas_cookie"),
		NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
		ngx_conf_set_str_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_http_auth_cas_ctx_t, auth_cas_cookie),
		NULL
	},
	{
		ngx_string("auth_cas_login_url"),
		NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
		ngx_conf_set_str_slot,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_http_auth_cas_ctx_t, auth_cas_login_url),
		NULL
	},
	{
		ngx_string("auth_cas_service_url"),
		NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
		set_auth_cas_service_url,
		NGX_HTTP_LOC_CONF_OFFSET,
		offsetof(ngx_http_auth_cas_ctx_t, auth_cas_service_url),
		NULL
	},
	ngx_null_command
};

static ngx_http_module_t ctx = {
	NULL,
	ngx_http_auth_cas_init,
	NULL,
	NULL,
	NULL,
	NULL,
	ngx_http_auth_cas_create_loc_conf,
	ngx_http_auth_cas_merge_loc_conf
};

ngx_module_t ngx_http_auth_cas_module = {
	NGX_MODULE_V1,
	&ctx,
	commands,
	NGX_HTTP_MODULE,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NGX_MODULE_V1_PADDING
};
