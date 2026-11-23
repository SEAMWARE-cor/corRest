//
// FILE            corRestClientMulti.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Concurrent multi-request engine for the HTTP client.
//

#include <stdio.h>                               // snprintf
#include <stdlib.h>                              // calloc, malloc, free
#include <string.h>                              // memset, memcmp, memcpy, strlen, strcmp, strncmp, strerror
#include <stdbool.h>                             // bool, true, false
#include <unistd.h>                              // close, read, write
#include <errno.h>                               // errno
#include <fcntl.h>                               // fcntl
#include <poll.h>                                // poll
#include <sys/socket.h>                          // socket, connect, recv, send, MSG_NOSIGNAL
#include <netinet/in.h>                          // sockaddr_in
#include <netinet/tcp.h>                         // TCP_NODELAY
#include <netdb.h>                               // getaddrinfo, freeaddrinfo
#include <time.h>                                // clock_gettime

#include "corBase/corLinux.h"                    // epoll_create1, epoll_ctl, epoll_wait, MSG_NOSIGNAL (macOS: kqueue)
#include "corBase/corCo.h"                      // corCoCurrent
#include "corRest/corRestResolve.h"             // corRestResolve
#if COR_WITH_CLIENT_PROXY
#include "corRest/corRestClientProxy.h"         // corRestClientProxyFor, corRestClientProxyTunnel
#endif
#include "corRest/corRestWait.h"                // corRestWaitFd
#include "corRest/corRestClient.h"                 // CorRestClientConn, CorRestClientMulti, CorRestClientRequest, CorRestClientResponse
#include "corAlloc/CorAlloc.h"                   // CorAlloc



// Forward declarations from corRestClientParse.c
extern int corRestClientResponseComplete(CorRestClientConn* conn);
extern int corRestClientParseResponse(CorRestClientConn* conn, CorRestClientResponse* resp, CorAlloc* allocP);



// -----------------------------------------------------------------------------
//
// Entry states
//
typedef enum CorrMultiState
{
  CorrStateInit = 0,
  CorrStateConnecting,
  CorrStateWriting,
  CorrStateReading,
  CorrStateDone,
} CorrMultiState;



// -----------------------------------------------------------------------------
//
// CorrMultiEntry - per-request state within multi engine
//
typedef struct CorrMultiEntry
{
  CorrMultiState          state;
  CorRestClientConn*      conn;
  CorRestClientRequest    req;
  CorRestClientResponse   resp;
  CorAlloc*              allocP;
  void*                  userData;

  char*                  sendBuf;
  int                    sendLen;
  int                    sendPos;

  int                    error;

  bool                   fromPool;
  bool                   retried;
} CorrMultiEntry;



// -----------------------------------------------------------------------------
//
// CorRestClientMulti - the multi engine
//
struct CorRestClientMulti
{
  CorrMultiEntry*  entries;
  int             count;
  int             capacity;
  int             done;
  int64_t         maxResponseBytes;  // the cap of each response's body (0 = none) - corRestClientMultiMaxResponse
};



// -----------------------------------------------------------------------------
//
// aiDone - release the address list a connection may still be walking (below)
//
static void aiDone(CorRestClientConn* conn);



// -----------------------------------------------------------------------------
//
// setNonBlocking -
//
static void setNonBlocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}



// -----------------------------------------------------------------------------
//
// nowMs -
//
static uint64_t nowMs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiCreate - Create a multi-request engine
//
CorRestClientMulti* corRestClientMultiCreate(int capacity)
{
  CorRestClientMulti* multi = (CorRestClientMulti*)calloc(1, sizeof(CorRestClientMulti));
  if (multi == NULL)
    return NULL;

  multi->entries = (CorrMultiEntry*)calloc(capacity, sizeof(CorrMultiEntry));
  if (multi->entries == NULL)
  {
    free(multi);
    return NULL;
  }

  multi->capacity = capacity;
  return multi;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiMaxResponse - cap the body of each response (0 = none)
//
void corRestClientMultiMaxResponse(CorRestClientMulti* multi, int64_t maxBytes)
{
  if (multi != NULL)
    multi->maxResponseBytes = (maxBytes > 0) ? maxBytes : 0;
}



// -----------------------------------------------------------------------------
//
// responseTooLarge - has what is read of a response passed the engine's cap on a body?
//
// The body is what follows the header block; until the header block has ended, the bytes read are
// all header, and a header block of more than 64 KiB on top of the cap counts as too large too.
// Searched only once the buffer holds more than the cap - below it, nothing can have passed it.
//
static bool responseTooLarge(CorRestClientMulti* multi, CorRestClientConn* conn)
{
  if ((multi->maxResponseBytes == 0) || (conn->bufLen <= multi->maxResponseBytes))
    return false;

  const char* endP = (const char*) memmem(conn->buf, conn->bufLen, "\r\n\r\n", 4);

  if (endP == NULL)
    return conn->bufLen > multi->maxResponseBytes + 65536;

  int64_t bodyBytes = conn->bufLen - ((endP + 4) - conn->buf);

  return bodyBytes > multi->maxResponseBytes;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiAdd - Add a request to the multi engine
//
int corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url,
                         CorRestKeyValue* headers, int headerCount,
                         const char* body, int bodyLen,
                         CorAlloc* allocP, void* userData)
{
  if (multi->count >= multi->capacity)
    return -1;

  int idx = multi->count++;
  CorrMultiEntry* entry = &multi->entries[idx];

  memset(entry, 0, sizeof(CorrMultiEntry));

  corRestClientRequestInit(&entry->req, verb, url, allocP);

  for (int i = 0; i < headerCount; i++)
    corRestClientRequestHeader(&entry->req, headers[i].key, headers[i].value);

  if (body != NULL && bodyLen > 0)
    corRestClientRequestBody(&entry->req, body, bodyLen);

  entry->allocP  = allocP;
  entry->userData = userData;
  entry->state    = CorrStateInit;

  memset(&entry->resp, 0, sizeof(CorRestClientResponse));
  entry->resp.headerV    = entry->resp.headers;
  entry->resp.headerSize = COR_REST_INITIAL_KV_SLOTS;

  return idx;
}



// -----------------------------------------------------------------------------
//
// buildSendBuf - Build complete request into a malloc'd buffer
//
static int buildSendBuf(CorrMultiEntry* entry)
{
  CorRestClientRequest* req = &entry->req;
  const char* verbStr  = corRestVerbToString(req->verb);
  int bodyLen          = req->bodyLen;

  //
  // The request line holds the path (with its query), the Host header the host: both counted, or a
  // path of a few hundred characters does not fit
  //
  int estimate = 1600 + (int) strlen(req->path) + 2 * (int) strlen(req->host) + (int) strlen(corRestClientUserAgent);   // a proxy's absolute URI and Proxy-Authorization too
  for (int i = 0; i < req->headerCount; i++)
    estimate += (int)strlen(req->headerV[i].key) + (int)strlen(req->headerV[i].value) + 4;
  estimate += bodyLen;

  char* buf = (char*)malloc(estimate);
  if (buf == NULL)
    return -1;

  char* p   = buf;
  char* end = buf + estimate;
  int   n;

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
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

#if COR_WITH_CLIENT_PROXY
  if (viaProxy && (route.auth[0] != 0))
  {
    n = snprintf(p, end - p, "Proxy-Authorization: %s\r\n", route.auth);
    if (n < 0 || n >= end - p) { free(buf); return -1; }
    p += n;
  }
#endif

  if (req->port == 80 || req->port == 443)
    n = snprintf(p, end - p, "Host: %s\r\n", req->host);
  else
    n = snprintf(p, end - p, "Host: %s:%d\r\n", req->host, req->port);
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

  n = snprintf(p, end - p, "User-Agent: %s\r\n", corRestClientUserAgent);
  if (n < 0 || n >= end - p) { free(buf); return -1; }
  p += n;

  for (int i = 0; i < req->headerCount; i++)
  {
    n = snprintf(p, end - p, "%s: %s\r\n", req->headerV[i].key, req->headerV[i].value);
    if (n < 0 || n >= end - p) { free(buf); return -1; }
    p += n;
  }

  if (bodyLen > 0)
  {
    n = snprintf(p, end - p, "Content-Length: %d\r\n", bodyLen);
    if (n < 0 || n >= end - p) { free(buf); return -1; }
    p += n;
  }

  if (end - p < 2) { free(buf); return -1; }
  *p++ = '\r';
  *p++ = '\n';

  if (bodyLen > 0 && req->body != NULL)
  {
    if (end - p < bodyLen) { free(buf); return -1; }
    memcpy(p, req->body, bodyLen);
    p += bodyLen;
  }

  entry->sendBuf = buf;
  entry->sendLen = (int)(p - buf);
  entry->sendPos = 0;

  return 0;
}



// -----------------------------------------------------------------------------
//
// startConnect - Initiate non-blocking TCP connect
//
static int startConnect(CorrMultiEntry* entry)
{
  CorRestClientRequest* req = &entry->req;

  bool isTls = (strcmp(req->scheme, "https") == 0) ? true : false;
  CorRestClientConn* conn = corRestClientPoolGet(req->host, req->port, isTls);

  if (conn != NULL)
  {
    struct pollfd pfd;
    pfd.fd     = conn->fd;
    pfd.events = POLLIN;

    if (poll(&pfd, 1, 0) > 0)
    {
      char c;
      int  n = (int)recv(conn->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);

      if (n <= 0)
      {
        if (conn->ssl != NULL)
          corRestClientTlsClose(conn);
        close(conn->fd);
        free(conn->buf);
        aiDone(conn);
        free(conn);
        conn = NULL;
      }
    }

    if (conn != NULL)
    {
      entry->conn     = conn;
      entry->state    = CorrStateWriting;
      entry->fromPool = true;
      setNonBlocking(conn->fd);
      return 0;
    }
  }

  struct addrinfo  hints;
  struct addrinfo* res = NULL;
  char             portStr[8];
  const char*      connectHost = req->host;          // where the socket goes: the host, or its proxy
  unsigned short   connectPort = req->port;

#if COR_WITH_CLIENT_PROXY
  CorRestClientProxyRoute route;

  if (corRestClientProxyFor(isTls, req->host, &route))
  {
    connectHost = route.host;
    connectPort = route.port;

    //
    // What a failure to reach it is - said by the connect steps' failures; cleared once connected
    //
    snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection to the proxy %.200s:%d failed", route.host, route.port);
  }
#endif

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portStr, sizeof(portStr), "%d", connectPort);

  if (corRestResolve(connectHost, portStr, &hints, &res) != 0 || res == NULL)
    return -1;

  //
  // EVERY address, not just the first. AF_UNSPEC means getaddrinfo may answer
  // with IPv6 ahead of IPv4 - "localhost" commonly resolves ::1 first - and a
  // listener bound to IPv4 only then refuses that address instantly. Taking the
  // first result and giving up turns a perfectly reachable endpoint into
  // "Connection failed", which is exactly what happened the first time these
  // tests ran somewhere other than a workstation: every distributed operation to
  // localhost failed while the same endpoint answered curl.
  //
  // The blocking client (corRestClient.c) has always walked the list; this one
  // did not. Note the asymmetry with connect() here being non-blocking: a
  // candidate that returns EINPROGRESS is ACCEPTED, and a genuine failure only
  // shows later - so this loop can only skip addresses that fail immediately,
  // which is precisely the ::1-refused case it exists for.
  //
  int              fd = -1;
  int              r  = -1;    // 0 = connected outright, EINPROGRESS = still connecting
  struct addrinfo* rp;

  for (rp = res; rp != NULL; rp = rp->ai_next)
  {
    fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    setNonBlocking(fd);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    r = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (r == 0 || errno == EINPROGRESS)
      break;

    close(fd);
    fd = -1;
  }

  if (fd < 0)
  {
    freeaddrinfo(res);
    return -1;
  }

  conn = (CorRestClientConn*)calloc(1, sizeof(CorRestClientConn));
  if (conn == NULL)
  {
    close(fd);
    freeaddrinfo(res);
    return -1;
  }

  conn->fd     = fd;
  conn->aiList = res;                       // owned by the conn from here
  conn->aiNext = (rp != NULL) ? rp->ai_next : NULL;
  conn->port   = req->port;
  conn->tls  = isTls;
  snprintf(conn->host, sizeof(conn->host), "%s", req->host);

  conn->bufSize = 8192;
  conn->buf     = (char*)malloc(conn->bufSize);
  if (conn->buf == NULL)
  {
    close(fd);
    aiDone(conn);
    free(conn);
    return -1;
  }

  conn->keepAlive = true;
  entry->conn     = conn;

  if (r == 0)
  {
    //
    // Connected on the first address, without waiting - the usual outcome over
    // loopback. The remaining addresses are moot, exactly as they are when a
    // deferred connect completes, so release the list here too.
    //
    // Skipping this did not lose much per connection, but a keep-alive
    // connection is never freed while the broker runs: the list it was still
    // holding survived to process exit, once per pooled connection.
    //
    aiDone(conn);
    entry->state = CorrStateWriting;
    entry->resp.errorDetail[0] = 0;
  }
  else
    entry->state = CorrStateConnecting;

  return 0;
}



// -----------------------------------------------------------------------------
//
// aiDone - release the address list a connection was still walking
//
static void aiDone(CorRestClientConn* conn)
{
  if (conn->aiList != NULL)
  {
    freeaddrinfo(conn->aiList);
    conn->aiList = NULL;
    conn->aiNext = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// connectNextAddress - the previous candidate failed; try the one after it
//
// Returns the new fd, or -1 when the list is exhausted. The caller re-arms epoll
// on the new descriptor: it is a different socket, and the old one is gone.
//
static int connectNextAddress(CorRestClientConn* conn)
{
  while (conn->aiNext != NULL)
  {
    struct addrinfo* rp = conn->aiNext;
    conn->aiNext = rp->ai_next;

    int fd = socket(rp->ai_family, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    setNonBlocking(fd);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
    if (r == 0 || errno == EINPROGRESS)
    {
      close(conn->fd);
      conn->fd = fd;
      return fd;
    }

    close(fd);
  }

  return -1;
}


// -----------------------------------------------------------------------------
//
// finishConnect - Check connect result and do TLS if needed
//
static int finishConnect(CorrMultiEntry* entry)
{
  CorRestClientConn* conn = entry->conn;

  int err = 0;
  socklen_t errLen = sizeof(err);
  getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
  if (err != 0)
    return -1;

  if (conn->tls)
  {
    int flags = fcntl(conn->fd, F_GETFL, 0);
    fcntl(conn->fd, F_SETFL, flags & ~O_NONBLOCK);

#if COR_WITH_CLIENT_PROXY
    //
    // Through a proxy: the tunnel to the host first - its TLS goes through it, as the handshake below, blocking
    //
    CorRestClientProxyRoute route;
    char                    why[256];

    if (corRestClientProxyFor(true, conn->host, &route) &&
        (corRestClientProxyTunnel(conn->fd, conn->host, conn->port, &route, entry->req.connectTimeoutMs, why, sizeof(why)) != 0))
    {
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "%s", why);
      return -1;
    }
#endif

    if (corRestClientTlsConnect(conn) != 0)
    {
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "TLS with %.200s failed", conn->host);
      return -1;
    }

    fcntl(conn->fd, F_SETFL, flags);
  }

  entry->state = CorrStateWriting;
  entry->resp.errorDetail[0] = 0;               // connected: no proxy failure to tell
  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiPerform - Execute all pending requests concurrently
//
int corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs)
{
  if (multi->count == 0)
    return 0;

  int epollFd = epoll_create1(0);
  if (epollFd < 0)
    return -1;

  uint64_t startTime = nowMs();

  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    if (corRestClientUrlParse(&entry->req) != 0)
    {
      entry->resp.error = CORR_ERR_URL;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Malformed URL");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    if (buildSendBuf(entry) != 0)
    {
      entry->resp.error = CORR_ERR_ALLOC;
      snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
               "Failed to build send buffer");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    if (startConnect(entry) != 0)
    {
      entry->resp.error = CORR_ERR_CONNECT;
      if (entry->resp.errorDetail[0] == 0)   // a proxy's is said already
        snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
      entry->state = CorrStateDone;
      multi->done++;
      continue;
    }

    struct epoll_event ev;
    ev.data.u32 = (uint32_t)i;

    if (entry->state == CorrStateConnecting)
      ev.events = EPOLLOUT;
    else if (entry->state == CorrStateWriting)
      ev.events = EPOLLOUT;
    else
      ev.events = EPOLLIN;

    epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
  }

  // Event loop
  struct epoll_event events[64];

  while (multi->done < multi->count)
  {
    uint64_t elapsed = nowMs() - startTime;
    if ((int)elapsed >= timeoutMs)
    {
      for (int i = 0; i < multi->count; i++)
      {
        CorrMultiEntry* entry = &multi->entries[i];
        if (entry->state != CorrStateDone)
        {
          entry->resp.error = CORR_ERR_TIMEOUT;
          snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Request timeout");
          entry->state = CorrStateDone;
          multi->done++;
        }
      }
      break;
    }

    int waitMs = timeoutMs - (int)elapsed;
    if (waitMs > 1000)
      waitMs = 1000;

    //
    // Inside a coroutine, the wait is the scheduler's (corRestWaitFd - an epoll fd is pollable itself),
    // and the set is then read without waiting. Elsewhere one epoll_wait, as always.
    //
    int nev;
    if (corCoCurrent() != NULL)
    {
      int w = corRestWaitFd(epollFd, POLLIN, waitMs, NULL);

      //
      // -1: no wait is to be had - the loop is stopping (ECANCELED, corCoLoopCancel) or the wait itself
      // failed. Waiting again would fail again, at once, round after round until the deadline: every
      // request still open ends here
      //
      if (w < 0)
      {
        for (int i = 0; i < multi->count; i++)
        {
          CorrMultiEntry* entry = &multi->entries[i];

          if (entry->state != CorrStateDone)
          {
            entry->resp.error = CORR_ERR_RECV;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "No wait for the response: %s", strerror(errno));
            entry->state = CorrStateDone;
            multi->done++;
          }
        }
        break;
      }

      nev = (w > 0) ? epoll_wait(epollFd, events, 64, 0) : 0;
    }
    else
      nev = epoll_wait(epollFd, events, 64, waitMs);

    for (int e = 0; e < nev; e++)
    {
      int idx = (int)events[e].data.u32;
      CorrMultiEntry* entry = &multi->entries[idx];

      if (entry->state == CorrStateDone)
        continue;

      CorRestClientConn* conn = entry->conn;

      switch (entry->state)
      {
        case CorrStateConnecting:
        {
          if (finishConnect(entry) != 0)
          {
            //
            // This candidate refused. A non-blocking connect answers EINPROGRESS
            // even for a port nobody listens on, so the choice of address can
            // only be judged here - and "localhost" resolves ::1 before
            // 127.0.0.1 on most systems while a server may be listening on IPv4
            // alone. Try the next address before calling the endpoint dead.
            //
            int newFd = connectNextAddress(conn);

            if (newFd >= 0)
            {
              struct epoll_event nev;
              nev.data.u32 = (uint32_t)idx;
              nev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, newFd, &nev);
              break;                       // still CorrStateConnecting
            }

            aiDone(conn);
            entry->resp.error = CORR_ERR_CONNECT;
            if (entry->resp.errorDetail[0] == 0)   // a proxy's refusal is said already
              snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          aiDone(conn);                    // connected: the rest of the list is moot

          struct epoll_event ev;
          ev.data.u32 = (uint32_t)idx;
          ev.events   = EPOLLOUT;
          epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          break;
        }

        case CorrStateWriting:
        {
          int remaining = entry->sendLen - entry->sendPos;
          int n;

          if (conn->ssl != NULL)
            n = corRestClientTlsWrite(conn, entry->sendBuf + entry->sendPos, remaining);
          else
            n = (int)send(conn->fd, entry->sendBuf + entry->sendPos, remaining, MSG_NOSIGNAL);   // a receiver gone: EPIPE, not SIGPIPE

          if (n < 0)
          {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
              break;

            if (entry->fromPool && !entry->retried)
            {
              epoll_ctl(epollFd, EPOLL_CTL_DEL, conn->fd, NULL);

              if (conn->ssl != NULL)
                corRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              aiDone(conn);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = CORR_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            entry->resp.error = CORR_ERR_SEND;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "send() failed (errno=%d %s)", errno, strerror(errno));
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          entry->sendPos += n;

          if (entry->sendPos >= entry->sendLen)
          {
            entry->state = CorrStateReading;
            conn->bufLen = 0;

            struct epoll_event ev;
            ev.data.u32 = (uint32_t)idx;
            ev.events   = EPOLLIN;
            epoll_ctl(epollFd, EPOLL_CTL_MOD, conn->fd, &ev);
          }
          break;
        }

        case CorrStateReading:
        {
          //
          // Over TLS, one read may leave bytes OpenSSL has already taken off the socket and decrypted
          // (SSL_pending): the socket then never becomes readable again for them, and epoll would wait for
          // the timeout. So, while the response is incomplete and OpenSSL holds more, read again here.
          //
        readMore:
          if (conn->bufSize - conn->bufLen < 4096)
          {
            int newSize = conn->bufSize * 2;
            char* newBuf = (char*)realloc(conn->buf, newSize);
            if (newBuf == NULL)
            {
              entry->resp.error = CORR_ERR_ALLOC;
              snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                       "Failed to grow recv buffer (size=%d)", newSize);
              entry->state = CorrStateDone;
              multi->done++;
              break;
            }
            conn->buf     = newBuf;
            conn->bufSize = newSize;
          }

          int n;
          if (conn->ssl != NULL)
            n = corRestClientTlsRead(conn, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);
          else
            n = (int)read(conn->fd, conn->buf + conn->bufLen, conn->bufSize - conn->bufLen);

          if (n <= 0)
          {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
              break;

            if (conn->bufLen > 0)
            {
              int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
              if (pr == 0)
              {
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }
            }

            if (entry->fromPool && !entry->retried && conn->bufLen == 0)
            {
              epoll_ctl(epollFd, EPOLL_CTL_DEL, conn->fd, NULL);

              if (conn->ssl != NULL)
                corRestClientTlsClose(conn);
              close(conn->fd);
              free(conn->buf);
              aiDone(conn);
              free(conn);

              entry->conn     = NULL;
              entry->fromPool = false;
              entry->retried  = true;
              entry->sendPos  = 0;

              if (startConnect(entry) != 0)
              {
                entry->resp.error = CORR_ERR_CONNECT;
                snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail), "Connection failed");
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }

              struct epoll_event ev;
              ev.data.u32 = (uint32_t)idx;
              ev.events   = EPOLLOUT;
              epoll_ctl(epollFd, EPOLL_CTL_ADD, entry->conn->fd, &ev);
              break;
            }

            // Peer closed cleanly (n == 0): for HTTP/1.0-with-Connection-close
            // framing (no Content-Length, no chunked) EOF is the legitimate
            // body terminator — what's in the buffer IS the complete response.
            // Try to parse it before declaring an error; if the parser
            // succeeds (it knows about the !keepAlive case), succeed too.
            if (n == 0 && conn->bufLen > 0)
            {
              int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
              if (pr == 0)
              {
                entry->state = CorrStateDone;
                multi->done++;
                break;
              }
              // pr != 0 → genuinely truncated/malformed; fall through to error
            }

            entry->resp.error = (n == 0) ? CORR_ERR_CLOSED : CORR_ERR_RECV;
            // include the head of the buffer (CR/LF replaced with | and ~)
            // so the trace stays single-line yet shows HTTP framing.
            char buf[256];
            int  dumpLen = conn->bufLen < 200 ? conn->bufLen : 200;
            int  j = 0;
            for (int k = 0; k < dumpLen && j < (int)sizeof(buf)-1; k++)
            {
              char c = conn->buf[k];
              buf[j++] = (c == '\r') ? '|' : (c == '\n') ? '~' : c;
            }
            buf[j] = 0;
            //
            // The precisions are what make this fit: errorDetail is 256 bytes and
            // the head alone can be 255, so without them the compiler is right to
            // warn that the tail of the message may be lost. Bounded, the worst
            // case is a truncated head - which is what a "head" is for anyway.
            //
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "%s (n=%d, errno=%d %.40s, bufLen=%d, head=%.120s)",
                     (n == 0) ? "Connection closed by peer" : "recv() failed",
                     n, errno, strerror(errno), conn->bufLen, buf);
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          conn->bufLen += n;

          //
          // The cap on a body (corRestClientMultiMaxResponse): passed, the request ends here and the
          // connection is closed (an error is never pooled) - the rest of the body is not read
          //
          if (responseTooLarge(multi, conn))
          {
            entry->resp.error = CORR_ERR_TOO_LARGE;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "response body larger than %lld bytes - not read further", (long long) multi->maxResponseBytes);
            entry->state = CorrStateDone;
            multi->done++;
            break;
          }

          int cr = corRestClientResponseComplete(conn);
          if (cr == 0)
          {
            int pr = corRestClientParseResponse(conn, &entry->resp, entry->allocP);
            if (pr == 0)
            {
              entry->state = CorrStateDone;
              multi->done++;
            }
            else
            {
              entry->resp.error = (pr == -2) ? CORR_ERR_ALLOC : CORR_ERR_PARSE;
              snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                       (pr == -2) ? "Failed to allocate the response" : "Failed to parse response");
              entry->state = CorrStateDone;
              multi->done++;
            }
          }
          else if (cr < 0)
          {
            entry->resp.error = CORR_ERR_PARSE;
            snprintf(entry->resp.errorDetail, sizeof(entry->resp.errorDetail),
                     "Malformed HTTP framing");
            entry->state = CorrStateDone;
            multi->done++;
          }
          else if ((conn->ssl != NULL) && (corRestClientTlsPending(conn) > 0))
            goto readMore;
          break;
        }

        default:
          break;
      }
    }
  }

  // Cleanup
  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);
    entry->sendBuf = NULL;

    if (entry->conn != NULL)
    {
      //
      // Whatever happens to the connection now, it has no further use for the
      // address list: either it is going into the pool already connected, or it
      // is being destroyed. A connection that never got to the "connected"
      // branch - a timeout, most often - still held its list here, and this is
      // the last place that could release it.
      //
      aiDone(entry->conn);

      if (entry->resp.error == 0 && entry->conn->keepAlive)
      {
        int flags = fcntl(entry->conn->fd, F_GETFL, 0);
        fcntl(entry->conn->fd, F_SETFL, flags & ~O_NONBLOCK);
        corRestClientPoolPut(entry->conn);
      }
      else
      {
        if (entry->conn->ssl != NULL)
          corRestClientTlsClose(entry->conn);
        if (entry->conn->fd >= 0)
          close(entry->conn->fd);
        free(entry->conn->buf);
        free(entry->conn);
      }
      entry->conn = NULL;
    }
  }

  close(epollFd);

  return multi->done;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiResponse - Get the response for entry at index
//
CorRestClientResponse* corRestClientMultiResponse(CorRestClientMulti* multi, int index)
{
  if (index < 0 || index >= multi->count)
    return NULL;

  return &multi->entries[index].resp;
}



// -----------------------------------------------------------------------------
//
// corRestClientMultiDestroy - Free the multi engine
//
void corRestClientMultiDestroy(CorRestClientMulti* multi)
{
  if (multi == NULL)
    return;

  for (int i = 0; i < multi->count; i++)
  {
    CorrMultiEntry* entry = &multi->entries[i];

    free(entry->sendBuf);

    corRestClientResponseCleanup(&entry->resp);  // the header vector and the response's copy, if malloc'd

    corRestClientRequestCleanup(&entry->req);   // the header vector, if malloc'd - not an allocator's

    if (entry->conn != NULL)
    {
      if (entry->conn->ssl != NULL)
        corRestClientTlsClose(entry->conn);
      if (entry->conn->fd >= 0)
        close(entry->conn->fd);
      free(entry->conn->buf);
      free(entry->conn);
    }
  }

  free(multi->entries);
  free(multi);
}
