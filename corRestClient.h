//
// FILE            corRestClient.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// HTTP client with connection pooling, TLS, redirects, and concurrent requests.
//
#ifndef CORREST_CLIENT_H_
#define CORREST_CLIENT_H_

#include <stdbool.h>
#include <netdb.h>                              // struct addrinfo
#include <stdint.h>
#include <pthread.h>

#include "corAlloc/CorAlloc.h"
#include "corTree/CorNode.h"

#include "corRest/CorRestVerb.h"
#include "corRest/CorRestKeyValue.h"
#include "corRest/CorRestIn.h"              // COR_REST_INITIAL_KV_SLOTS, COR_REST_KV_GROW_SIZE



// -----------------------------------------------------------------------------
//
// Client error codes
//
#define CORR_OK                  0
#define CORR_ERR_URL            -1
#define CORR_ERR_CONNECT        -2
#define CORR_ERR_TIMEOUT        -3
#define CORR_ERR_SEND           -4
#define CORR_ERR_RECV           -5
#define CORR_ERR_PARSE          -6
#define CORR_ERR_ALLOC          -7
#define CORR_ERR_TLS            -8
#define CORR_ERR_TOO_MANY_REDIR -9
#define CORR_ERR_CLOSED         -10
#define CORR_ERR_TOO_LARGE      -11   // the response body passed the multi engine's cap (corRestClientMultiMaxResponse)



// -----------------------------------------------------------------------------
//
// CorRestClientConn - outbound connection
//
typedef struct CorRestClientConn
{
  int                         fd;
  void*                       ssl;
  char                        host[256];
  unsigned short              port;
  bool                        tls;
  bool                        keepAlive;
  uint64_t                    lastUsed;
  char*                       buf;
  int                         bufSize;
  int                         bufLen;

  //
  // The addresses getaddrinfo returned, and the next one to try. A non-blocking
  // connect to a closed port answers EINPROGRESS, not a refusal, so which
  // address works can only be discovered asynchronously - and "localhost"
  // commonly resolves ::1 ahead of 127.0.0.1 while the listener is IPv4-only.
  // Both are freed the moment a connection is established.
  //
  struct addrinfo*            aiList;
  struct addrinfo*            aiNext;

  struct CorRestClientConn*    next;
} CorRestClientConn;



// -----------------------------------------------------------------------------
//
// CorRestClientPool - thread-safe connection pool
//
#define CORR_POOL_BUCKETS  64

typedef struct CorRestClientPool
{
  struct { CorRestClientConn* head; int count; } buckets[CORR_POOL_BUCKETS];
  pthread_mutex_t   mutex;
  int               maxIdlePerHost;
  int               idleTimeoutSec;
  int               totalIdle;
} CorRestClientPool;



// -----------------------------------------------------------------------------
//
// CorRestClientRequest - outbound request description
//
typedef struct CorRestClientRequest
{
  CorRestVerb        verb;
  char*             url;

  // Parsed from URL
  char              scheme[8];
  char              host[256];
  unsigned short    port;
  char*             path;

  // Headers (dynamic array, inline for small counts)
  CorRestKeyValue    headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue*   headerV;
  int               headerCount;
  int               headerSize;

  // Body
  char*             body;
  int               bodyLen;
  CorNode*          bodyJson;

  // Config
  int               connectTimeoutMs;
  int               requestTimeoutMs;
  int               maxRedirects;

  // Allocator for response allocation (NULL = use malloc)
  CorAlloc*         allocP;
} CorRestClientRequest;



// -----------------------------------------------------------------------------
//
// CorRestClientResponse - response from server
//
// statusText, the header names and values, and body point into the response's OWN copy of what was
// read (buf), not into the connection's receive buffer: the connection goes back to the pool, or is
// closed, before the caller reads the response. The copy is taken from the request's allocator, and
// lives as long as the allocator; without one it is malloc'd and released by
// corRestClientResponseCleanup (corRestClientMultiDestroy for a response of the multi engine).
// body is zero-terminated.
//
typedef struct CorRestClientResponse
{
  int               statusCode;
  char*             statusText;

  CorRestKeyValue    headers[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue*   headerV;
  int               headerCount;
  int               headerSize;

  char*             body;
  int               bodyLen;
  CorNode*          bodyJson;

  int               error;
  char              errorDetail[256];

  char*             buf;           // the response's copy of what was read - malloc'd only when bufMalloced
  bool              bufMalloced;
} CorRestClientResponse;



// -----------------------------------------------------------------------------
//
// CorRestClientMulti - concurrent request engine (opaque)
//
typedef struct CorRestClientMulti CorRestClientMulti;



// -----------------------------------------------------------------------------
//
// Init / Cleanup
//
extern int   corRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);

// Default per-request timeout (ms, 5000) that corRestClientRequestInit puts on a
// request; a caller that calls corRestClientRequestTimeout replaces it. Settable
// at boot (coraine's --distOpTimeout/-dtmo). It applies only to the requests
// that set no timeout of their own - in coraine, a forward to a Context Source
// whose registration has none. Notifications and @context downloads set theirs
// (10 s unless the subscription says otherwise).
extern int   corRestClientDefaultRequestTimeoutMs;

// The User-Agent every client path puts on the wire. Set once at boot via
// corRestClientInit's userAgent parameter; read by both the pool path and the
// multi path, so a process speaks with a single identity.
extern const char* corRestClientUserAgent;



// -----------------------------------------------------------------------------
//
// Synchronous request building
//
extern void  corRestClientRequestInit(CorRestClientRequest* req, CorRestVerb verb, const char* url, CorAlloc* allocP);
extern void  corRestClientRequestHeader(CorRestClientRequest* req, const char* name, const char* value);
extern void  corRestClientRequestBody(CorRestClientRequest* req, const char* body, int bodyLen);
extern void  corRestClientRequestTimeout(CorRestClientRequest* req, int connectMs, int requestMs);



// -----------------------------------------------------------------------------
//
// Synchronous send
//
extern int   corRestClientSend(CorRestClientRequest* req, CorRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// Convenience functions
//



// -----------------------------------------------------------------------------
//
// Response helpers
//
extern const char* corRestClientResponseHeader(CorRestClientResponse* resp, const char* name);

// -----------------------------------------------------------------------------
//
// corRestClientResponseCleanup - release a completed response's heap-owned parts
//
// Frees the response header vector if the parser grew it beyond the inline
// array, and the response's copy of what was read when it was malloc'd (a request
// with no allocator) - statusText, the headers and body are not to be read after
// it. Idempotent; call once per completed corRestClientSend.
//
extern void  corRestClientResponseCleanup(CorRestClientResponse* resp);



// -----------------------------------------------------------------------------
//
// corRestClientRequestCleanup - release a request's heap-owned parts
//
// Frees the request header vector if it outgrew the inline array in a request with no allocator (the
// vector was malloc'd then; with an allocator it goes with the allocator). Idempotent; call once the
// request has been sent - needed only for a request initialised with a NULL allocator.
//
extern void  corRestClientRequestCleanup(CorRestClientRequest* req);



// -----------------------------------------------------------------------------
//
// Multi (concurrent requests)
//
extern CorRestClientMulti*       corRestClientMultiCreate(int capacity);
extern int                      corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url,
                                                     CorRestKeyValue* headers, int headerCount,
                                                     const char* body, int bodyLen,
                                                     CorAlloc* allocP, void* userData);
extern int                      corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs);
extern CorRestClientResponse*    corRestClientMultiResponse(CorRestClientMulti* multi, int index);
extern void                     corRestClientMultiDestroy(CorRestClientMulti* multi);

//
// corRestClientMultiMaxResponse - cap the body each response of the engine may have (0 = none, the default)
//
// Checked while the response is read: a body that passes maxBytes ends that request with
// CORR_ERR_TOO_LARGE, its connection closed and nothing more read from it - so a peer answering
// with more than the caller can use is not read into memory whole. Call before corRestClientMultiPerform.
//
extern void                     corRestClientMultiMaxResponse(CorRestClientMulti* multi, int64_t maxBytes);

//
// CORREST_CLIENT_MULTI_MAX_RESPONSE - this header has corRestClientMultiMaxResponse and CORR_ERR_TOO_LARGE
//
#define CORREST_CLIENT_MULTI_MAX_RESPONSE



// -----------------------------------------------------------------------------
//
// Pool management (used internally, exposed for testing)
//
extern int                corRestClientPoolInit(int maxIdlePerHost, int idleTimeoutSec);
extern CorRestClientConn*  corRestClientPoolGet(const char* host, unsigned short port, bool tls);
extern void               corRestClientPoolPut(CorRestClientConn* conn);



// -----------------------------------------------------------------------------
//
// URL parsing (used internally by both engines, exposed for testing): scheme, host, port and path of req->url
// into req; CORR_ERR_URL when malformed
//
extern int                corRestClientUrlParse(CorRestClientRequest* req);



// -----------------------------------------------------------------------------
//
// TLS (used internally, exposed for testing)
//
extern int   corRestClientTlsInit(void);
extern void  corRestClientTlsInsecureSet(bool onoff);
extern bool  corRestClientTlsPinSet(const char* sha256hex);    // one certificate trusted, by its SHA-256 - corRestClientTls.c
extern int   corRestClientTlsConnect(CorRestClientConn* conn);
extern int   corRestClientTlsPending(CorRestClientConn* conn);
extern int   corRestClientTlsRead(CorRestClientConn* conn, char* buf, int len);
extern int   corRestClientTlsWrite(CorRestClientConn* conn, const char* buf, int len);
extern void  corRestClientTlsClose(CorRestClientConn* conn);

#endif  // CORREST_CLIENT_H_
