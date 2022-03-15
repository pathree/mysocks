/*
  This example code shows how to write an (optionally encrypting) SSL proxy
  with Libevent's bufferevent layer.

  XXX It's a little ugly and should probably be cleaned up.
 */

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <unordered_map>

#include "le-proxy.h"
#include "stream.h"
#include "utils.h"
#include "xlog.h"

/* To generate a cert and pkey and self-signed certificate
 * run:
 * openssl genrsa -out pkey 2048
 * openssl req -new -key pkey -out cert.req
 * openssl x509 -req -days 365 -in cert.req -signkey pkey -out cert
 * put cert and pkey into certs directory
 * */

LE_PROXY proxy;

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *p) {
  char client_ip[256] = {0};
  int client_port = 0;
  sock_ntop(a, client_ip, sizeof(client_ip), &client_port);
  xlog("New conn from client(%s:%d), fd:%d\n", client_ip, client_port, fd);

  struct bufferevent *bev_in = NULL;

  if (proxy.use_ssl) {
    SSL *ssl = SSL_new(proxy.ssl_ctx);
    bev_in = bufferevent_openssl_socket_new(
        proxy.base, fd, ssl, BUFFEREVENT_SSL_ACCEPTING,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    bufferevent_openssl_set_allow_dirty_shutdown(bev_in, 1);
  } else {
    bev_in = bufferevent_socket_new(
        proxy.base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  }

  bufferevent_setcb(bev_in, stream_readcb, NULL, stream_eventcb, NULL);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   le-proxy -H <listen-on-addr> -P <Port> [-s]\n", stderr);
  fputs("Example(default):\n", stderr);
  fputs("   le-proxy -H 0.0.0.0 -P 80 -s\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  proxy.use_ssl = 0;
  strcpy(proxy.listen_addr, "0.0.0.0");
  proxy.listen_port = 80;

  opterr = 0;
  int c;
  while ((c = getopt(argc, argv, "H:P:s")) != -1) {
    switch (c) {
      case 'H':
        if (optarg)
          strncpy(proxy.listen_addr, optarg, sizeof(proxy.listen_addr));
        break;
      case 'P':
        proxy.listen_port = atoi(optarg);
        break;
      case 's':
        proxy.use_ssl = 1;
        break;
      case '?':
        syntax();
    }
  }

  argc -= optind;
  argv += optind;

  int socklen;
  char addr_port[256] = {0};
  sprintf(addr_port, "%s:%d", proxy.listen_addr, proxy.listen_port);

  struct sockaddr_storage listen_on_addr;
  memset(&listen_on_addr, 0, sizeof(listen_on_addr));
  socklen = sizeof(listen_on_addr);
  if (evutil_parse_sockaddr_port(addr_port, (struct sockaddr *)&listen_on_addr,
                                 &socklen) < 0)
    syntax();

  proxy.base = event_base_new();
  /*装入/etc/hosts文件进行域名解析,
   *否则,需要调用evdns_base_resolv_conf_parse做解析*/
  proxy.dns_base =
      evdns_base_new(proxy.base, EVDNS_BASE_INITIALIZE_NAMESERVERS);

  proxy.ssl_ctx = ssl_ctx_new(1, "certs/cert", "certs/pkey");
  if (!proxy.ssl_ctx) {
    event_base_free(proxy.base);
    return 1;
  }

  struct evconnlistener *listener = NULL;
  listener = evconnlistener_new_bind(
      proxy.base, accept_cb, NULL,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE, -1,
      (struct sockaddr *)&listen_on_addr, socklen);

  if (proxy.use_ssl)
    xlog("Listening on %s using SSL\n", addr_port);
  else
    xlog("Listening on %s\n", addr_port);

  event_base_dispatch(proxy.base);

  /*释放资源*/
  evconnlistener_free(listener);
  SSL_CTX_free(proxy.ssl_ctx);
  // evdns_base_free(proxy.dns_base, 0);
  event_base_free(proxy.base);

  return 0;
}
