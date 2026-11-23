//
// FILE            corRestClientProxy.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The client's HTTP proxy - a request to a host goes through the proxy configured for its scheme, unless the
// host is in the no-proxy list:
//
//   https: a tunnel - CONNECT host:port to the proxy, then TLS through it, to the host itself (its name for
//          SNI and for the certificate check)
//   http:  the request to the proxy, its request-line the absolute URI (RFC 9112 § 3.2.2)
//
// The proxy's URL may carry user:password - sent as Proxy-Authorization: Basic. No proxy is configured
// until the application asks: corRestClientProxyFromEnv (https_proxy, http_proxy, no_proxy - each also in
// upper case, the lower one first, as curl reads them) or corRestClientProxySet.
//
#include <errno.h>                                     // errno
#include <poll.h>                                      // poll
#include <stdbool.h>                                   // bool
#include <stdio.h>                                     // snprintf
#include <stdlib.h>                                    // getenv, free, strtol
#include <string.h>                                    // strlen, strncmp, strchr, strdup
#include <strings.h>                                   // strncasecmp, strcasecmp
#include <sys/socket.h>                                // send, recv
#include <time.h>                                      // clock_gettime

#include "corBase/corLinux.h"                    // MSG_NOSIGNAL (macOS: 0, SIGPIPE ignored)
#include "corBase/corUriPasswordMask.h"                // corUriPasswordMask

#include "corRest/corRestClientProxy.h"                // Own interface



// -----------------------------------------------------------------------------
//
// Proxy - one proxy: where, and its Proxy-Authorization (or "")
//
typedef struct Proxy
{
  bool            set;
  char            host[256];
  unsigned short  port;
  char            auth[512];                           // "Basic <base64>", or ""
} Proxy;

static Proxy  httpProxy;
static Proxy  httpsProxy;
static char*  noProxy = NULL;                          // the list, as given - malloc'd



// -----------------------------------------------------------------------------
//
// base64 - standard, padded, into out (room for len * 4 / 3 + 4)
//
static void base64(const unsigned char* in, int len, char* out)
{
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int o = 0;

  for (int i = 0; i < len; i += 3)
  {
    unsigned int v = in[i] << 16;
    if (i + 1 < len) v |= in[i + 1] << 8;
    if (i + 2 < len) v |= in[i + 2];

    out[o++] = alphabet[(v >> 18) & 63];
    out[o++] = alphabet[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? alphabet[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? alphabet[v & 63] : '=';
  }

  out[o] = 0;
}



// -----------------------------------------------------------------------------
//
// proxyParse - a proxy URL: [http://][user:password@]host[:port][/] (port default 1080, as curl's); false: not one
//
// An error names the URL with its password as "***" (corUriPasswordMask): the caller logs it, or prints it.
//
static bool proxyParse(const char* url, Proxy* pP, char* errBuf, int errLen)
{
  char shown[512];

  memset(pP, 0, sizeof(Proxy));

  if ((url == NULL) || (url[0] == 0))
    return true;                                       // none

  const char* s = url;

  if (strncasecmp(s, "http://", 7) == 0)
    s += 7;
  else if (strstr(s, "://") != NULL)
  {
    snprintf(errBuf, errLen, "proxy '%s': only an http:// proxy is supported", corUriPasswordMask(url, shown, sizeof(shown)));
    return false;
  }

  //
  // user:password@
  //
  const char* at    = strchr(s, '@');
  const char* slash = strchr(s, '/');

  if ((at != NULL) && ((slash == NULL) || (at < slash)))
  {
    char           userinfo[360];                      // base64 of it, after "Basic ", fits auth[512]
    int            len = at - s;

    if (len >= (int) sizeof(userinfo))
    {
      snprintf(errBuf, errLen, "proxy '%s': its user:password is too long", corUriPasswordMask(url, shown, sizeof(shown)));
      return false;
    }

    memcpy(userinfo, s, len);
    userinfo[len] = 0;

    char encoded[sizeof(userinfo) * 4 / 3 + 8];
    base64((const unsigned char*) userinfo, len, encoded);
    snprintf(pP->auth, sizeof(pP->auth), "Basic %s", encoded);
    s = at + 1;
  }

  //
  // host[:port] - an IPv6 literal in brackets
  //
  const char* end   = s + strcspn(s, "/");
  const char* colon = NULL;

  if (*s == '[')
  {
    const char* close = strchr(s, ']');
    if ((close == NULL) || (close > end))
    {
      snprintf(errBuf, errLen, "proxy '%s': an IPv6 address without its ']'", corUriPasswordMask(url, shown, sizeof(shown)));
      return false;
    }
    snprintf(pP->host, sizeof(pP->host), "%.*s", (int) (close - s - 1), s + 1);
    colon = (close[1] == ':') ? &close[1] : NULL;
  }
  else
  {
    colon = memchr(s, ':', end - s);
    snprintf(pP->host, sizeof(pP->host), "%.*s", (int) (((colon != NULL) ? colon : end) - s), s);
  }

  long port = 1080;

  if (colon != NULL)
  {
    char* after = NULL;
    port = strtol(colon + 1, &after, 10);
    if ((after != end) || (port <= 0) || (port > 65535))
    {
      snprintf(errBuf, errLen, "proxy '%s': not a port", corUriPasswordMask(url, shown, sizeof(shown)));
      return false;
    }
  }

  if (pP->host[0] == 0)
  {
    snprintf(errBuf, errLen, "proxy '%s': no host", corUriPasswordMask(url, shown, sizeof(shown)));
    return false;
  }

  pP->port = (unsigned short) port;
  pP->set  = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// corRestClientProxySet -
//
bool corRestClientProxySet(const char* httpUrl, const char* httpsUrl, const char* noProxyList, char* errBuf, int errBufLen)
{
  Proxy h;
  Proxy s;

  if ((proxyParse(httpUrl, &h, errBuf, errBufLen) == false) || (proxyParse(httpsUrl, &s, errBuf, errBufLen) == false))
    return false;

  httpProxy  = h;
  httpsProxy = s;

  free(noProxy);
  noProxy = ((noProxyList != NULL) && (noProxyList[0] != 0)) ? strdup(noProxyList) : NULL;
  return true;
}



// -----------------------------------------------------------------------------
//
// envOf - a variable in lower case, else in upper case (curl's order); NULL: neither
//
static const char* envOf(const char* lower, const char* upper)
{
  const char* v = getenv(lower);

  if ((v == NULL) || (v[0] == 0))
    v = getenv(upper);

  return ((v != NULL) && (v[0] != 0)) ? v : NULL;
}



// -----------------------------------------------------------------------------
//
// corRestClientProxyFromEnv -
//
bool corRestClientProxyFromEnv(char* errBuf, int errBufLen)
{
  return corRestClientProxySet(envOf("http_proxy", "HTTP_PROXY"), envOf("https_proxy", "HTTPS_PROXY"), envOf("no_proxy", "NO_PROXY"), errBuf, errBufLen);
}



// -----------------------------------------------------------------------------
//
// noProxyMatch - the host is in the no-proxy list: "*" (every host), a name (the host, or a domain the host is
// in - ".example.org" and "example.org" alike), an address as written; ports are ignored, as curl ignores them
//
static bool noProxyMatch(const char* host)
{
  if (noProxy == NULL)
    return false;

  int         hlen = strlen(host);
  const char* s    = noProxy;

  while (*s != 0)
  {
    while ((*s == ',') || (*s == ' '))
      ++s;

    int len = strcspn(s, ", ");

    if (len > 0)
    {
      const char* entry = s;
      int         elen  = len;
      const char* colon = memchr(entry, ':', elen);

      if ((colon != NULL) && (memchr(entry, ']', elen) == NULL) && (memchr(colon + 1, ':', entry + elen - colon - 1) == NULL))
        elen = colon - entry;                          // "host:port" - the port ignored (not an IPv6 address)

      if ((elen == 1) && (*entry == '*'))
        return true;

      if (*entry == '.')
      {
        ++entry;
        --elen;
      }

      if ((elen == hlen) && (strncasecmp(entry, host, elen) == 0))
        return true;

      if ((hlen > elen) && (host[hlen - elen - 1] == '.') && (strncasecmp(entry, &host[hlen - elen], elen) == 0))
        return true;
    }

    s += len;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// corRestClientProxyFor -
//
bool corRestClientProxyFor(bool tls, const char* host, CorRestClientProxyRoute* routeP)
{
  const Proxy* pP = (tls == true) ? &httpsProxy : &httpProxy;

  if ((pP->set == false) || (noProxyMatch(host) == true))
    return false;

  snprintf(routeP->host, sizeof(routeP->host), "%s", pP->host);
  routeP->port = pP->port;
  snprintf(routeP->auth, sizeof(routeP->auth), "%s", pP->auth);
  return true;
}



// -----------------------------------------------------------------------------
//
// msLeft - what is left of a timeout begun at 'start'
//
static int msLeft(const struct timespec* startP, int timeoutMs)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  long elapsed = (now.tv_sec - startP->tv_sec) * 1000 + (now.tv_nsec - startP->tv_nsec) / 1000000;
  return (elapsed >= timeoutMs) ? 0 : (int) (timeoutMs - elapsed);
}



// -----------------------------------------------------------------------------
//
// corRestClientProxyTunnel -
//
int corRestClientProxyTunnel(int fd, const char* host, unsigned short port, const CorRestClientProxyRoute* routeP, int timeoutMs, char* errBuf, int errBufLen)
{
  char            req[1024];
  bool            v6 = (strchr(host, ':') != NULL);
  struct timespec start;

  clock_gettime(CLOCK_MONOTONIC, &start);

  int n = snprintf(req, sizeof(req), "CONNECT %s%s%s:%d HTTP/1.1\r\nHost: %s%s%s:%d\r\n%s%s%s\r\n",
                   v6 ? "[" : "", host, v6 ? "]" : "", port,
                   v6 ? "[" : "", host, v6 ? "]" : "", port,
                   (routeP->auth[0] != 0) ? "Proxy-Authorization: " : "", routeP->auth, (routeP->auth[0] != 0) ? "\r\n" : "");

  if ((n < 0) || (n >= (int) sizeof(req)))
  {
    snprintf(errBuf, errBufLen, "CONNECT %s: the request does not fit", host);
    return -1;
  }

  //
  // Sent - the socket may be non-blocking (the multi engine's): poll for room
  //
  for (int sent = 0; sent < n; )
  {
    struct pollfd pfd = { fd, POLLOUT, 0 };

    if (poll(&pfd, 1, msLeft(&start, timeoutMs)) <= 0)
    {
      snprintf(errBuf, errBufLen, "proxy %s:%d: CONNECT not sent (timeout)", routeP->host, routeP->port);
      return -1;
    }

    ssize_t w = send(fd, &req[sent], n - sent, MSG_NOSIGNAL);

    if ((w < 0) && ((errno == EAGAIN) || (errno == EINTR)))
      continue;
    if (w <= 0)
    {
      snprintf(errBuf, errBufLen, "proxy %s:%d: CONNECT not sent (%s)", routeP->host, routeP->port, strerror(errno));
      return -1;
    }

    sent += w;
  }

  //
  // The proxy's answer, up to its blank line - read a byte at a time: what follows is the host's (TLS), and
  // must stay in the socket
  //
  char answer[4096];
  int  len = 0;

  while ((len < 4 ) || (memcmp(&answer[len - 4], "\r\n\r\n", 4) != 0))
  {
    if (len >= (int) sizeof(answer) - 1)
    {
      snprintf(errBuf, errBufLen, "proxy %s:%d: its answer to CONNECT is too long", routeP->host, routeP->port);
      return -1;
    }

    struct pollfd pfd = { fd, POLLIN, 0 };

    if (poll(&pfd, 1, msLeft(&start, timeoutMs)) <= 0)
    {
      snprintf(errBuf, errBufLen, "proxy %s:%d: no answer to CONNECT (timeout)", routeP->host, routeP->port);
      return -1;
    }

    ssize_t r = recv(fd, &answer[len], 1, 0);

    if ((r < 0) && ((errno == EAGAIN) || (errno == EINTR)))
      continue;
    if (r <= 0)
    {
      snprintf(errBuf, errBufLen, "proxy %s:%d: closed while answering CONNECT", routeP->host, routeP->port);
      return -1;
    }

    ++len;
  }

  answer[len] = 0;

  //
  // HTTP/1.x 2xx: the tunnel is open
  //
  const char* sp = strchr(answer, ' ');
  int status = (sp != NULL) ? atoi(sp + 1) : 0;

  if ((strncmp(answer, "HTTP/1.", 7) != 0) || (status < 200) || (status > 299))
  {
    int lineLen = strcspn(answer, "\r\n");
    snprintf(errBuf, errBufLen, "proxy %s:%d: CONNECT %s:%d refused - %.*s", routeP->host, routeP->port, host, port, (lineLen > 200) ? 200 : lineLen, answer);
    return -1;
  }

  return 0;
}
