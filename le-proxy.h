#ifndef MYSOCKS_LE_PROXY_H_
#define MYSOCKS_LE_PROXY_H_

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/dns.h>
#include <event2/listener.h>
#include <event2/util.h>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <string>
#include <unordered_map>

#include "stream.h"

struct LE_PROXY {
  struct event_base* base;
  struct evdns_base* dns_base;

  char listen_addr[128];
  int listen_port;

  int use_ssl;
  SSL_CTX* ssl_ctx;

  std::unordered_map<int, STREAM*> streams;
};

extern LE_PROXY proxy;

#endif  // MYSOCKS_LE_PROXY_H_