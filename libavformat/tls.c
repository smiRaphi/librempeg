/*
 * TLS/SSL Protocol
 * Copyright (c) 2011 Martin Storsjo
 *
 * This file is part of Librempeg
 *
 * Librempeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Librempeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "avformat.h"
#include "network.h"
#include "os_support.h"
#include "url.h"
#include "tls.h"
#include "libavutil/avstring.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/mem.h"
#include "libavutil/parseutils.h"

static int set_options(TLSShared *c, const char *uri)
{
    char buf[1024];
    const char *p = strchr(uri, '?');
    if (!p)
        return 0;

    if (!c->ca_file && av_find_info_tag(buf, sizeof(buf), "cafile", p)) {
        c->ca_file = av_strdup(buf);
        if (!c->ca_file)
            return AVERROR(ENOMEM);
    }

    if (!c->verify && av_find_info_tag(buf, sizeof(buf), "verify", p)) {
        char *endptr = NULL;
        c->verify = strtol(buf, &endptr, 10);
        if (buf == endptr)
            c->verify = 1;
    }

    if (!c->cert_file && av_find_info_tag(buf, sizeof(buf), "cert", p)) {
        c->cert_file = av_strdup(buf);
        if (!c->cert_file)
            return AVERROR(ENOMEM);
    }

    if (!c->key_file && av_find_info_tag(buf, sizeof(buf), "key", p)) {
        c->key_file = av_strdup(buf);
        if (!c->key_file)
            return AVERROR(ENOMEM);
    }

    return 0;
}

int ff_tls_open_underlying(TLSShared *c, URLContext *parent, const char *uri, AVDictionary **options)
{
    int port;
    const char *p;
    char buf[200], opts[50] = "";
    struct addrinfo hints = { 0 }, *ai = NULL;
    const char *proxy_path;
    char *env_http_proxy, *env_no_proxy;
    int use_proxy;
    int ret;

    ret = set_options(c, uri);
    if (ret < 0)
        return ret;

    if (c->listen)
        snprintf(opts, sizeof(opts), "?listen=1");

    av_url_split(NULL, 0, NULL, 0, c->underlying_host, sizeof(c->underlying_host), &port, NULL, 0, uri);

    p = strchr(uri, '?');

    if (!p) {
        p = opts;
    } else {
        if (av_find_info_tag(opts, sizeof(opts), "listen", p))
            c->listen = 1;
    }

    ff_url_join(buf, sizeof(buf), "tcp", NULL, c->underlying_host, port, "%s", p);

    hints.ai_flags = AI_NUMERICHOST;
    if (!getaddrinfo(c->underlying_host, NULL, &hints, &ai)) {
        c->numerichost = 1;
        freeaddrinfo(ai);
    }

    if (!c->host && !(c->host = av_strdup(c->underlying_host)))
        return AVERROR(ENOMEM);

    env_http_proxy = getenv_utf8("http_proxy");
    proxy_path = c->http_proxy ? c->http_proxy : env_http_proxy;

    env_no_proxy = getenv_utf8("no_proxy");
    use_proxy = !ff_http_match_no_proxy(env_no_proxy, c->underlying_host) &&
                proxy_path && av_strstart(proxy_path, "http://", NULL);
    freeenv_utf8(env_no_proxy);

    if (use_proxy) {
        char proxy_host[200], proxy_auth[200], dest[200];
        int proxy_port;
        av_url_split(NULL, 0, proxy_auth, sizeof(proxy_auth),
                     proxy_host, sizeof(proxy_host), &proxy_port, NULL, 0,
                     proxy_path);
        ff_url_join(dest, sizeof(dest), NULL, NULL, c->underlying_host, port, NULL);
        ff_url_join(buf, sizeof(buf), "httpproxy", proxy_auth, proxy_host,
                    proxy_port, "/%s", dest);
    }

    freeenv_utf8(env_http_proxy);
    return ffurl_open_whitelist(&c->tcp, buf, AVIO_FLAG_READ_WRITE,
                                &parent->interrupt_callback, options,
                                parent->protocol_whitelist, parent->protocol_blacklist, parent);
}
