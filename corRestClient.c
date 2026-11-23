//
// FILE            corRestClient.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP client with connection pooling, TLS, redirects.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free
#include <string.h>                              // memset, memcmp, memcpy, strlen, strcmp, strcasecmp, strncasecmp, strncmp, strerror
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close, read, write
#include <errno.h>                               // errno
#include <fcntl.h>                               // fcntl
#include <poll.h>                                // poll
#include <sys/socket.h>                          // socket, connect, getsockopt, send, MSG_NOSIGNAL
#include <netinet/in.h>                          // sockaddr_in
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <netdb.h>                               // getaddrinfo, freeaddrinfo

#include "corBase/corLinux.h"                    // MSG_NOSIGNAL (macOS: 0, SIGPIPE ignored)
#include "corRest/corRestResolve.h"             // corRestResolve
#include "corRest/corRestWait.h"                // corRestWaitFd
#include "corRest/corRestClient.h"                 // CorRestClientConn, CorRestClientRequest, CorRestClientResponse
#if COR_WITH_CLIENT_PROXY
#include "corRest/corRestClientProxy.h"            // corRestClientProxyFor, corRestClientProxyTunnel
#endif
#include "corAlloc/corAlloc.h"                   // corAlloc
#include "corAlloc/CorAlloc.h"                     // CorAlloc
#include "corTree/CorNode.h"                     // CorNode
#include "corJson/corJsonRender.h"               // corJsonFastRender
#include "corJson/corJsonRenderSize.h"           // corJsonFastRenderSize



// -----------------------------------------------------------------------------
//
// Global pool instance
//
static bool        tlsInited = false;



// -----------------------------------------------------------------------------
//
// corRestClientDefaultRequestTimeoutMs - boot-time default for every client
// request that doesn't call corRestClientRequestTimeout explicitly. coraine
// exposes this via the --distOpTimeout/-dtmo CLI flag.
//
int corRestClientDefaultRequestTimeoutMs = 5000;



// -----------------------------------------------------------------------------
//
// corRestClientUserAgent - the User-Agent every client path puts on the wire
//
// Set once at boot by corRestClientInit; the pool path and the multi path both
// read it, so a process speaks with a single identity no matter which path a
// request takes.
//
const char* corRestClientUserAgent = "Cor/1.0";



// -----------------------------------------------------------------------------
//
// corRestClientInit - Initialize the client subsystem
//
int corRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent)
{
  if (userAgent != NULL)
    corRestClientUserAgent = userAgent;

  int s;

  s = corRestClientPoolInit(maxIdleConns > 0 ? maxIdleConns : 4,
                           idleTimeoutSec > 0 ? idleTimeoutSec : 30);
  if (s != 0)
    return s;

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientUrlParse - Parse URL into scheme, host, port, path
//
// Parses "http://host:port/path" and "https://host:port/path" by scanning
// the string directly.  Default port 80 for http, 443 for https.
// The port is digits only, 1-65535 - "host:9026x", "host:", "host:99999" are malformed.
//
int corRestClientUrlParse(CorRestClientRequest* req)
{
  const char* url = req->url;

  if (url == NULL)
    return CORR_ERR_URL;

  // Parse scheme
  if (strncmp(url, "https://", 8) == 0)
  {
    memcpy(req->scheme, "https", 6);
    url += 8;
    req->port = 443;
  }
  else if (strncmp(url, "http://", 7) == 0)
  {
    memcpy(req->scheme, "http", 5);
    url += 7;
    req->port = 80;
  }
  else
  {
    return CORR_ERR_URL;
  }

  // Parse host[:port]
  const char* hostStart = url;
  const char* p         = url;

  while (*p != '\0' && *p != '/' && *p != '?')
    p++;

  int hostPortLen = (int)(p - hostStart);

  const char* colon = (const char*)memchr(hostStart, ':', hostPortLen);
  if (colon != NULL)
  {
    int hostLen = (int)(colon - hostStart);
    if (hostLen <= 0 || hostLen >= (int)sizeof(req->host))
      return CORR_ERR_URL;

    long port = 0;
    for (const char* d = colon + 1; d < p; d++)
    {
      if (*d < '0' || *d > '9')
        return CORR_ERR_URL;
      port = port * 10 + (*d - '0');
      if (port > 65535)
        return CORR_ERR_URL;
    }
    if (port == 0)                               // "host:" or "host:0"
      return CORR_ERR_URL;

    memcpy(req->host, hostStart, hostLen);
    req->host[hostLen] = '\0';
    req->port = (unsigned short)port;
  }
  else
  {
    if (hostPortLen <= 0 || hostPortLen >= (int)sizeof(req->host))
      return CORR_ERR_URL;

    memcpy(req->host, hostStart, hostPortLen);
    req->host[hostPortLen] = '\0';
  }

  // Parse path
  req->path = (*p == '/' || *p == '?') ? (char*)p : (char*)"/";

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientRequestInit - Initialize a request
//
void corRestClientRequestInit(CorRestClientRequest* req, CorRestVerb verb, const char* url, CorAlloc* allocP)
{
  memset(req, 0, sizeof(CorRestClientRequest));

  req->verb             = verb;
  req->url              = (char*)url;
  req->headerV          = req->headers;
  req->headerSize       = COR_REST_INITIAL_KV_SLOTS;
  req->connectTimeoutMs = 5000;
  req->requestTimeoutMs = corRestClientDefaultRequestTimeoutMs;
  req->maxRedirects     = 5;
  req->allocP           = allocP;
}



// -----------------------------------------------------------------------------
//
// corRestClientRequestHeader - Add a header to the request
//
void corRestClientRequestHeader(CorRestClientRequest* req, const char* name, const char* value)
{
  if (req->headerCount >= req->headerSize)
  {
    int newSize = req->headerSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV;

    if (req->allocP != NULL)
      newV = (CorRestKeyValue*)corAlloc(req->allocP, newSize * sizeof(CorRestKeyValue));
    else
      newV = (CorRestKeyValue*)malloc(newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, req->headerV, req->headerCount * sizeof(CorRestKeyValue));

    //
    // The old vector is freed only if it was malloc'd: the inline one is part of the request, and one
    // taken from the allocator goes with the allocator. free() of an allocator's chunk is an invalid
    // free - a request with more than 15 headers (a registration's contextSourceInfo becomes headers
    // of every forwarded request) took the broker down.
    //
    if ((req->headerV != req->headers) && (req->allocP == NULL))
      free(req->headerV);

    req->headerV   = newV;
    req->headerSize = newSize;
  }

  req->headerV[req->headerCount].key  = (char*)name;
  req->headerV[req->headerCount].value = (char*)value;
  req->headerCount++;
}



// -----------------------------------------------------------------------------
//
// corRestClientRequestCleanup - release a request's heap-owned parts: the header vector, when it outgrew
// the inline one and the request has no allocator (then it was malloc'd). Idempotent.
//
void corRestClientRequestCleanup(CorRestClientRequest* req)
{
  if (req == NULL)
    return;

  if ((req->headerV != NULL) && (req->headerV != req->headers) && (req->allocP == NULL))
    free(req->headerV);

  req->headerV     = req->headers;
  req->headerSize  = COR_REST_INITIAL_KV_SLOTS;
  req->headerCount = 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientRequestBody - Set request body
//
void corRestClientRequestBody(CorRestClientRequest* req, const char* body, int bodyLen)
{
  req->body    = (char*)body;
  req->bodyLen = bodyLen;
}



// -----------------------------------------------------------------------------
//
// corRestClientRequestTimeout - Set timeouts
//
void corRestClientRequestTimeout(CorRestClientRequest* req, int connectMs, int requestMs)
{
  if (connectMs > 0)
    req->connectTimeoutMs = connectMs;
  if (requestMs > 0)
    req->requestTimeoutMs = requestMs;
}



// -----------------------------------------------------------------------------
//
// responseInit - Initialize a response struct
//
static void responseInit(CorRestClientResponse* resp)
{
  memset(resp, 0, sizeof(CorRestClientResponse));
  resp->headerV    = resp->headers;
  resp->headerSize = COR_REST_INITIAL_KV_SLOTS;
}



// -----------------------------------------------------------------------------
//
// setError - Set error in response
//
static void setError(CorRestClientResponse* resp, int code, const char* detail)
{
  resp->error = code;
  if (detail != NULL)
    snprintf(resp->errorDetail, sizeof(resp->errorDetail), "%s", detail);
}



// -----------------------------------------------------------------------------
//
// tcpConnect - Non-blocking TCP connect with timeout
//
static int tcpConnect(const char* host, unsigned short port, int timeoutMs)
{
  struct addrinfo  hints;
  struct addrinfo* res    = NULL;
  struct addrinfo* rp     = NULL;
  char             portStr[8];

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  snprintf(portStr, sizeof(portStr), "%d", port);

  int r = corRestResolve(host, portStr, &hints, &res);
  if (r != 0 || res == NULL)
    return -1;

  int fd = -1;

  for (rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    r = connect(fd, rp->ai_addr, rp->ai_addrlen);

    //
    // The socket stays non-blocking: every wait on it is corRestWaitFd - a poll() on a thread, a yield
    // inside a coroutine, where a blocking write or read would stop the whole event loop
    //
    (void) flags;
    if (r == 0)
      break;

    if (errno == EINPROGRESS)
    {
      r = corRestWaitFd(fd, POLLOUT, timeoutMs, NULL);
      if (r > 0)
      {
        int err = 0;
        socklen_t errLen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
        if (err == 0)
          break;
      }
    }

    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);

  if (fd < 0)
    return -1;

  int opt = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  return fd;
}



// -----------------------------------------------------------------------------
//
// connRead - Read from connection (dispatches plain/TLS)
//
static int connRead(CorRestClientConn* conn, char* buf, int len)
{
  if (conn->ssl != NULL)
    return corRestClientTlsRead(conn, buf, len);

  return (int)read(conn->fd, buf, len);
}



// -----------------------------------------------------------------------------
//
// connWrite - Write to connection (dispatches plain/TLS)
//
static int connWrite(CorRestClientConn* conn, const char* buf, int len)
{
  if (conn->ssl != NULL)
    return corRestClientTlsWrite(conn, buf, len);

  return (int)send(conn->fd, buf, len, MSG_NOSIGNAL);   // a receiver gone: EPIPE, not SIGPIPE
}



// -----------------------------------------------------------------------------
//
// connWriteAll - Write all bytes to connection
//
static int connWriteAll(CorRestClientConn* conn, const char* buf, int len)
{
  int totalSent = 0;

  while (totalSent < len)
  {
    int n = connWrite(conn, buf + totalSent, len - totalSent);
    if (n < 0)
    {
      // a full send buffer: wait until there is room (a yield, inside a coroutine) - not spin
      if (((errno == EAGAIN) || (errno == EWOULDBLOCK)) && (corRestWaitFd(conn->fd, POLLOUT, 30000, NULL) > 0))
        continue;
      return -1;
    }
    if (n == 0)
      return -1;

    totalSent += n;
  }

  return totalSent;
}



// -----------------------------------------------------------------------------
//
// connEnsureBuf - Ensure read buffer has at least 'needed' extra bytes
//
static int connEnsureBuf(CorRestClientConn* conn, int needed)
{
  if (conn->bufSize - conn->bufLen >= needed)
    return 0;

  int newSize = conn->bufSize * 2;
  if (newSize < conn->bufLen + needed)
    newSize = conn->bufLen + needed;

  char* newBuf = (char*)realloc(conn->buf, newSize);
  if (newBuf == NULL)
    return -1;

  conn->buf     = newBuf;
  conn->bufSize = newSize;

  return 0;
}



// -----------------------------------------------------------------------------
//
// connReadMore - Read more data into conn->buf
//
static int connReadMore(CorRestClientConn* conn, int timeoutMs)
{
  if (connEnsureBuf(conn, 4096) != 0)
    return -1;

  int n;

  while (true)
  {
    //
    // No wait when OpenSSL holds decrypted bytes already: the socket may have nothing more to say,
    // and a wait for it would stall the response until the timeout
    //
    if (corRestClientTlsPending(conn) == 0)
    {
      int r = corRestWaitFd(conn->fd, POLLIN, timeoutMs, NULL);
      if (r <= 0)
        return r == 0 ? -2 : -1;
    }

    n = connRead(conn, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);

    if ((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))   // ready, and then not: wait again
      continue;
    break;
  }

  if (n <= 0)
    return n == 0 ? -3 : -1;

  conn->bufLen += n;
  return n;
}



// Forward declarations
int corRestClientResponseComplete(CorRestClientConn* conn);
int corRestClientParseResponse(CorRestClientConn* conn, CorRestClientResponse* resp, CorAlloc* allocP);



// -----------------------------------------------------------------------------
//
// buildRequestBuf - Build HTTP request into a buffer
//
static int buildRequestBuf(CorRestClientRequest* req, char* buf, int bufSize,
                           const char* renderedBody, int renderedBodyLen)
{
  char* p   = buf;
  char* end = buf + bufSize;
  int   n;

  const char* verbStr = corRestVerbToString(req->verb);

#if COR_WITH_CLIENT_PROXY
  //
  // Through an http proxy: the absolute URI (RFC 9112 § 3.2.2) - an https request goes through a tunnel, as is
  //
  CorRestClientProxyRoute route;
  bool                    viaProxy = (strcmp(req->scheme, "http") == 0) && corRestClientProxyFor(false, req->host, &route);

  if (viaProxy)
  {
    bool v6 = (strchr(req->host, ':') != NULL);
    n = snprintf(p, end - p, "%s http://%s%s%s:%d%s HTTP/1.1\r\n", verbStr, v6 ? "[" : "", req->host, v6 ? "]" : "", req->port, req->path);
  }
  else
#endif
  n = snprintf(p, end - p, "%s %s HTTP/1.1\r\n", verbStr, req->path);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

#if COR_WITH_CLIENT_PROXY
  if (viaProxy && (route.auth[0] != 0))
  {
    n = snprintf(p, end - p, "Proxy-Authorization: %s\r\n", route.auth);
    if (n < 0 || n >= end - p)
      return -1;
    p += n;
  }
#endif

  if (req->port == 80 || req->port == 443)
    n = snprintf(p, end - p, "Host: %s\r\n", req->host);
  else
    n = snprintf(p, end - p, "Host: %s:%d\r\n", req->host, req->port);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

  n = snprintf(p, end - p, "User-Agent: %s\r\n", corRestClientUserAgent);
  if (n < 0 || n >= end - p)
    return -1;
  p += n;

  for (int i = 0; i < req->headerCount; i++)
  {
    n = snprintf(p, end - p, "%s: %s\r\n", req->headerV[i].key, req->headerV[i].value);
    if (n < 0 || n >= end - p)
      return -1;
    p += n;
  }

  int bodyLen = renderedBodyLen;
  if (bodyLen > 0)
  {
    n = snprintf(p, end - p, "Content-Length: %d\r\n", bodyLen);
    if (n < 0 || n >= end - p)
      return -1;
    p += n;
  }

  if (end - p < 2)
    return -1;
  *p++ = '\r';
  *p++ = '\n';

  return (int)(p - buf);
}



// -----------------------------------------------------------------------------
//
// corRestClientSendOnce - Send request on a connection, get response (no redirect)
//
static int corRestClientSendOnce(CorRestClientRequest* req, CorRestClientResponse* resp, CorRestClientConn* conn)
{
  char*  renderedBody    = NULL;
  int    renderedBodyLen = 0;

  if (req->bodyJson != NULL)
  {
    int size = corJsonFastRenderSize(req->bodyJson);
    renderedBody = (char*)malloc(size + 1);
    if (renderedBody == NULL)
    {
      setError(resp, CORR_ERR_ALLOC, "Failed to allocate JSON render buffer");
      return CORR_ERR_ALLOC;
    }
    corJsonFastRender(req->bodyJson, renderedBody);
    renderedBodyLen = (int)strlen(renderedBody);

    bool hasContentType = false;
    for (int i = 0; i < req->headerCount; i++)
    {
      if (strcasecmp(req->headerV[i].key, "Content-Type") == 0)
      {
        hasContentType = true;
        break;
      }
    }
    if (!hasContentType)
      corRestClientRequestHeader(req, "Content-Type", "application/json");
  }
  else if (req->body != NULL && req->bodyLen > 0)
  {
    renderedBody    = req->body;
    renderedBodyLen = req->bodyLen;
  }

  char headerBuf[8192];
  int  headerLen = buildRequestBuf(req, headerBuf, sizeof(headerBuf),
                                   renderedBody, renderedBodyLen);
  if (headerLen < 0)
  {
    if (req->bodyJson != NULL && renderedBody != NULL)
      free(renderedBody);
    setError(resp, CORR_ERR_ALLOC, "Request too large for header buffer");
    return CORR_ERR_ALLOC;
  }

  if (renderedBody != NULL && renderedBodyLen > 0)
  {
    int   totalLen = headerLen + renderedBodyLen;
    char* sendBuf  = (char*)malloc(totalLen);

    if (sendBuf == NULL)
    {
      if (req->bodyJson != NULL && renderedBody != NULL)
        free(renderedBody);
      setError(resp, CORR_ERR_ALLOC, "Failed to allocate send buffer");
      return CORR_ERR_ALLOC;
    }

    memcpy(sendBuf, headerBuf, headerLen);
    memcpy(sendBuf + headerLen, renderedBody, renderedBodyLen);

    if (req->bodyJson != NULL && renderedBody != NULL)
      free(renderedBody);

    int sendResult = connWriteAll(conn, sendBuf, totalLen);
    free(sendBuf);

    if (sendResult < 0)
    {
      setError(resp, CORR_ERR_SEND, strerror(errno));
      return CORR_ERR_SEND;
    }
  }
  else
  {
    if (connWriteAll(conn, headerBuf, headerLen) < 0)
    {
      setError(resp, CORR_ERR_SEND, strerror(errno));
      return CORR_ERR_SEND;
    }
  }

  // Read response
  conn->bufLen = 0;

  while (1)
  {
    int r = connReadMore(conn, req->requestTimeoutMs);
    if (r == -2)
    {
      setError(resp, CORR_ERR_TIMEOUT, "Request timeout");
      return CORR_ERR_TIMEOUT;
    }
    if (r < 0)
    {
      setError(resp, CORR_ERR_RECV, r == -3 ? "Connection closed" : strerror(errno));
      return r == -3 ? CORR_ERR_CLOSED : CORR_ERR_RECV;
    }

    int completeResult = corRestClientResponseComplete(conn);
    if (completeResult == 0)
      break;
    if (completeResult < 0)
    {
      setError(resp, CORR_ERR_PARSE, "Malformed HTTP response");
      return CORR_ERR_PARSE;
    }
  }

  int parseResult = corRestClientParseResponse(conn, resp, req->allocP);
  if (parseResult == -2)
  {
    setError(resp, CORR_ERR_ALLOC, "Failed to allocate the response");
    return CORR_ERR_ALLOC;
  }
  else if (parseResult != 0)
  {
    setError(resp, CORR_ERR_PARSE, "Malformed HTTP response");
    return CORR_ERR_PARSE;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// connCreate - Create a new connection to host:port
//
static CorRestClientConn* connCreate(const char* host, unsigned short port, bool tls, int timeoutMs, char* why, int whyLen)
{
#if COR_WITH_CLIENT_PROXY
  //
  // Through a proxy: connected to it; for https, a tunnel to the host - TLS then goes through it
  //
  CorRestClientProxyRoute route;
  bool                    viaProxy = corRestClientProxyFor(tls, host, &route);
  int                     fd       = viaProxy ? tcpConnect(route.host, route.port, timeoutMs) : tcpConnect(host, port, timeoutMs);

  if ((fd < 0) && viaProxy)
    snprintf(why, whyLen, "Connection to the proxy %.200s:%d failed", route.host, route.port);
  if (fd < 0)
    return NULL;

  if (viaProxy && tls && (corRestClientProxyTunnel(fd, host, port, &route, timeoutMs, why, whyLen) != 0))
  {
    close(fd);
    return NULL;
  }
#else
  (void) why;
  (void) whyLen;

  int fd = tcpConnect(host, port, timeoutMs);
  if (fd < 0)
    return NULL;
#endif

  CorRestClientConn* conn = (CorRestClientConn*)calloc(1, sizeof(CorRestClientConn));
  if (conn == NULL)
  {
    close(fd);
    return NULL;
  }

  conn->fd   = fd;
  conn->tls  = tls;
  conn->port = port;
  snprintf(conn->host, sizeof(conn->host), "%s", host);

  conn->bufSize = 8192;
  conn->buf     = (char*)malloc(conn->bufSize);
  if (conn->buf == NULL)
  {
    close(fd);
    free(conn);
    return NULL;
  }

  conn->keepAlive = true;

  if (tls)
  {
    if (!tlsInited)
    {
      if (corRestClientTlsInit() != 0)
      {
        close(fd);
        free(conn->buf);
        free(conn);
        return NULL;
      }
      tlsInited = true;
    }

    if (corRestClientTlsConnect(conn) != 0)
    {
      snprintf(why, whyLen, "TLS with %.200s failed", host);
      close(fd);
      free(conn->buf);
      free(conn);
      return NULL;
    }
  }

  return conn;
}



// -----------------------------------------------------------------------------
//
// connDestroy - Close and free a connection
//
static void connDestroy(CorRestClientConn* conn)
{
  if (conn == NULL)
    return;

  if (conn->ssl != NULL)
    corRestClientTlsClose(conn);

  if (conn->fd >= 0)
    close(conn->fd);

  free(conn->buf);
  free(conn);
}



// -----------------------------------------------------------------------------
//
// sendFollow - Send request, receive response (with redirects and pooling)
//
// A redirect's Location is in the response, which the next attempt replaces: the URL is copied to
// *redirectUrlP first (malloc'd, freed by corRestClientSend).
//
static int sendFollow(CorRestClientRequest* req, CorRestClientResponse* resp, char** redirectUrlP)
{
  int s = corRestClientUrlParse(req);
  if (s != 0)
  {
    setError(resp, CORR_ERR_URL, "Malformed URL");
    return CORR_ERR_URL;
  }

  bool isTls = (strcmp(req->scheme, "https") == 0) ? true : false;

  int redirects = 0;

  while (1)
  {
    CorRestClientConn* conn = corRestClientPoolGet(req->host, req->port, isTls);
    bool fromPool = (conn != NULL) ? true : false;

    if (conn == NULL)
    {
      char why[256] = "";

      conn = connCreate(req->host, req->port, isTls, req->connectTimeoutMs, why, sizeof(why));
      if (conn == NULL)
      {
        setError(resp, CORR_ERR_CONNECT, (why[0] != 0) ? why : "Connection failed");
        return CORR_ERR_CONNECT;
      }
    }

    s = corRestClientSendOnce(req, resp, conn);

    if (s != 0)
    {
      connDestroy(conn);

      if (fromPool && (s == CORR_ERR_SEND || s == CORR_ERR_RECV || s == CORR_ERR_CLOSED))
      {
        char why[256] = "";

        conn = connCreate(req->host, req->port, isTls, req->connectTimeoutMs, why, sizeof(why));
        if (conn == NULL)
        {
          setError(resp, CORR_ERR_CONNECT, (why[0] != 0) ? why : "Retry connection failed");
          return CORR_ERR_CONNECT;
        }

        corRestClientResponseCleanup(resp);
        responseInit(resp);
        s = corRestClientSendOnce(req, resp, conn);
        if (s != 0)
        {
          connDestroy(conn);
          return s;
        }
      }
      else
      {
        return s;
      }
    }

    // Check for redirect
    if (resp->statusCode == 301 || resp->statusCode == 302 ||
        resp->statusCode == 307 || resp->statusCode == 308)
    {
      if (++redirects > req->maxRedirects)
      {
        connDestroy(conn);
        setError(resp, CORR_ERR_TOO_MANY_REDIR, "Too many redirects");
        return CORR_ERR_TOO_MANY_REDIR;
      }

      const char* location = corRestClientResponseHeader(resp, "Location");
      if (location == NULL)
      {
        connDestroy(conn);
        setError(resp, CORR_ERR_PARSE, "Redirect without Location header");
        return CORR_ERR_PARSE;
      }

      if (conn->keepAlive)
        corRestClientPoolPut(conn);
      else
        connDestroy(conn);

      if (resp->statusCode == 301 || resp->statusCode == 302)
      {
        req->verb    = CorVerbGet;
        req->body    = NULL;
        req->bodyLen = 0;
        req->bodyJson = NULL;
      }

      char* url = strdup(location);
      if (url == NULL)
      {
        setError(resp, CORR_ERR_ALLOC, "Failed to allocate the redirect URL");
        return CORR_ERR_ALLOC;
      }

      free(*redirectUrlP);
      *redirectUrlP = url;

      corRestClientResponseCleanup(resp);
      responseInit(resp);

      req->url = url;
      s = corRestClientUrlParse(req);
      if (s != 0)
      {
        setError(resp, CORR_ERR_URL, "Malformed redirect URL");
        return CORR_ERR_URL;
      }

      isTls = (strcmp(req->scheme, "https") == 0) ? true : false;

      continue;
    }

    if (conn->keepAlive)
      corRestClientPoolPut(conn);
    else
      connDestroy(conn);

    break;
  }

  // No auto-parse of JSON responses - caller can parse manually if needed

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientSend - Send request, receive response (with redirects and pooling)
//
// After a redirect, req->url is the request's own URL again - the last Location was a copy that is
// freed here.
//
int corRestClientSend(CorRestClientRequest* req, CorRestClientResponse* resp)
{
  char* url         = req->url;
  char* redirectUrl = NULL;

  responseInit(resp);

  int s = sendFollow(req, resp, &redirectUrl);

  if (redirectUrl != NULL)
  {
    req->url = url;
    corRestClientUrlParse(req);
    free(redirectUrl);
  }

  return s;
}



// -----------------------------------------------------------------------------
//
// corRestClientResponseHeader - Lookup a response header by name (case-insensitive)
//
const char* corRestClientResponseHeader(CorRestClientResponse* resp, const char* name)
{
  int nameLen = strlen(name);

  for (int i = 0; i < resp->headerCount; i++)
  {
    if (resp->headerV[i].key != NULL &&
        strncasecmp(resp->headerV[i].key, name, nameLen) == 0 &&
        resp->headerV[i].key[nameLen] == '\0')
    {
      return resp->headerV[i].value;
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestClientResponseCleanup - release the response's heap-owned parts
//
// The header vector, when it outgrew the inline 'headers' array and was malloc'd
// (the parser mallocs a grown vector whether or not the request has an allocator),
// and the response's copy of what was read, when it was malloc'd (a request with no
// allocator). Idempotent. Call once per completed corRestClientSend.
//
void corRestClientResponseCleanup(CorRestClientResponse* resp)
{
  if (resp == NULL)
    return;

  if ((resp->headerV != NULL) && (resp->headerV != resp->headers))
  {
    free(resp->headerV);
    resp->headerV    = resp->headers;
    resp->headerSize = COR_REST_INITIAL_KV_SLOTS;
    resp->headerCount = 0;
  }

  if (resp->bufMalloced == true)
  {
    free(resp->buf);
    resp->bufMalloced = false;
  }
  resp->buf = NULL;
}
