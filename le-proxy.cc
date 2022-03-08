/*
  This example code shows how to write an (optionally encrypting) SSL proxy
  with Libevent's bufferevent layer.

  XXX It's a little ugly and should probably be cleaned up.
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <unordered_map>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "le-proxy.h"
#include "stream.h"
#include "utils.h"
#include "xlog.h"

LE_PROXY proxy;

static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *a, int slen, void *p) {
  char client_ip[256] = {0};
  int client_port = 0;
  sock_ntop(a, client_ip, sizeof(client_ip), &client_port);
  xlog("New conn from client(%s:%d), fd:%d\n", client_ip, client_port, fd);

  struct bufferevent *bev_in;

  bev_in = bufferevent_socket_new(
      proxy.base, fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);

  bufferevent_setcb(bev_in, stream_readcb, NULL, stream_eventcb, NULL);
  bufferevent_enable(bev_in, EV_READ | EV_WRITE);
}

static void syntax(void) {
  fputs("Syntax:\n", stderr);
  fputs("   le-proxy [listen-on-addr]\n", stderr);
  fputs("Example(default):\n", stderr);
  fputs("   le-proxy 0.0.0.0:80\n", stderr);

  exit(1);
}

int main(int argc, char **argv) {
  char addr_port[128] = {0};
  if (argc == 1)
    sprintf(addr_port, "0.0.0.0:80");
  else if (argc == 2)
    sprintf(addr_port, "%s", argv[1]);
  else
    syntax();

  int socklen;

  struct evconnlistener *listener;

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

  listener = evconnlistener_new_bind(
      proxy.base, accept_cb, NULL,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC | LEV_OPT_REUSEABLE, -1,
      (struct sockaddr *)&listen_on_addr, socklen);

  xlog("Listening on %s\n", addr_port);
  event_base_dispatch(proxy.base);

  evconnlistener_free(listener);
  event_base_free(proxy.base);

  return 0;
}
