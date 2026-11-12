//
// FILE            corRestBackendBuiltin.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The corHttp backend - the built-in HTTP server, no libmicrohttpd.
//
// The counterpart of corRestBackendMhd.c, and the same division of labour: this
// file translates one server's shape into corRest's, and decides nothing. What
// a URL means, what a body has to be, which headers go back and in what order
// all live on the other side of corRestBackend.h and are shared with the MHD
// backend byte for byte - which is the whole point, since several hundred
// functional tests compare captured responses line by line and must not be able
// to tell which server produced them.
//
// TWO THINGS MHD DID THAT corHttp DOES NOT, and both are done here:
//
//   PERCENT-DECODING. MHD hands over a decoded URL path and decoded query
//   parameter values. corHttp decodes nothing - deliberately, because the
//   decoding rules differ per parameter in NGSI-LD and the layer that knows
//   which is which is this one. So the path is decoded here (after the query
//   has been split off, so a `%3F` in a path cannot masquerade as the query
//   delimiter) and the query goes through corRestUriParamsParse, the same
//   split-and-decode the in-process self-forward path uses.
//
//   ...and '+' IN THE QUERY IS A SPACE, which is not what RFC 3986 says and is
//   what every HTTP client does. The rule belongs to
//   application/x-www-form-urlencoded, but a query string is what that format
//   is FOR, so urlencode/quote_plus is what client libraries produce: Python's
//   requests - and therefore the ETSI conformance suite - sends
//   `q=name=="Eiffel Tower"` on the wire as `q=name%3D%3D%22Eiffel+Tower%22`.
//   Read literally, that query asks for a name with a plus in it and matches
//   nothing, with a 200 and an empty array to show for it. A value that really
//   does contain a plus - the NGSI-LD single-level scope wildcard,
//   `scopeQ=/Madrid/%2B/ParqueNorte` - has to be percent-encoded, and then both
//   servers agree.
//
//   THE BODY LIMIT. MHD delivers a body in chunks and can be told to stop
//   mid-stream; corHttp delivers a whole request or none, so "stop reading" has
//   to become "do not start". Its cap is set from corRest's § 6.3.2 threshold
//   here, and over it corHttp hands the request up WITHOUT its body - which is
//   exactly what corRestBodyPolicyCheck needs, since that answers from the
//   Content-Length header and not from the bytes.
//
// AND ONE THING THIS BACKEND DOES THAT MHD'S DOES NOT: the request is COPIED
// out of corHttp's read buffer - method, header keys and values, body - rather
// than pointed at. corHttp is zero-copy and the pointers are good for exactly
// as long as the connection stays on this request; the post-response phase runs
// AFTER the response is out and the connection has moved on, on another thread,
// and it reads the request (the tenant, the Via chain) while it builds the
// notifications. Copying is what makes the request state outlive the connection
// it arrived on. MHD needs none of this because MHD owns copies of its own and
// keeps them until NOTIFY_COMPLETED.
//
#include <stdint.h>                              // intptr_t
#include <unistd.h>                              // close
#include <stdlib.h>                       // malloc, free
#include <stdio.h>                        // fprintf
#include <string.h>                       // memcpy, strerror
#include <errno.h>                        // errno
#include <pthread.h>                      // pthread_create
#include <time.h>                         // clock_gettime

#include "corAlloc/corAlloc.h"            // corAlloc
#include "corAlloc/corAllocStrdup.h"      // corAllocStrdup
#include "corLog/corLog.h"                // COR_V, COR_W

#include "corHttp/CorHttp.h"              // CorHttpServer, CorHttpConn

#include "corBase/corCo.h"                       // corCoCreate
#include "corBase/corCoLoop.h"                   // corCoLoopResume, corCoLoopCancel, corCoLoopPending, corCoLoopExpire
#include "corRest/corRestWait.h"                 // corRestCoLoopInit
#include "corRest/CorRestState.h"         // CorRestState, corRest
#include "corRest/corRestStateInit.h"     // corRestStateInit, corRestUrlPathNormalize
#include "corRest/corRestHooks.h"         // CorRestHook, ...
#include "corRest/corRestUrlValueEncode.h"  // corRestUrlValueDecode
#include "corRest/corRestStream.h"        // CorRestStream, corRestStreamWrite, corRestStreamEnd
#include "corRest/corRestBackend.h"       // Own interface

extern CorRestUpgradeHook corRestUpgradeHookF;          // corRestHooks.c



// -----------------------------------------------------------------------------
//
// The server, and the thread its event loop runs on
//
// corHttpServe() does not return until it is told to stop, and corRestInit's
// caller has other things to do before it parks - so the loop gets a thread of
// its own here, which is also what MHD_USE_SELECT_INTERNALLY does on the other
// side.
//
//
// corHttpLoops - how many event loops share the listening port
//
// One loop reads, parses and WRITES every connection it owns (the workers only
// run the service routine in between), and that single thread is what keeps the
// builtin server from filling the machine: measured at 8 cores it used 4.4 of
// them while libmicrohttpd used 7.9, at a LOWER cost per request - 70.7 us
// against 89.0 on a small response. The deficit was never cost, it was
// under-utilisation.
//
// So: N loops, each with its own epoll, its own connection pool and its own
// resume queue. The first one listens, accepts every connection and deals them
// out in turn (corHttpAcceptShare); a connection stays with its loop for its
// whole life, so the one-connection-one-writer invariant is preserved exactly.
//
// Default 1 - today's behaviour, byte for byte - until there are suite runs and
// measurements at N > 1 to choose a better one from.
//
#define COR_REST_MAX_LOOPS 64

static CorHttpServer  corHttpServerV[COR_REST_MAX_LOOPS];
static pthread_t      corHttpThreadV[COR_REST_MAX_LOOPS];
static int            corHttpLoops   = 1;
static bool           corHttpRunning = false;



// -----------------------------------------------------------------------------
//
// corRestConnectionMemorySet - no-op here: libmicrohttpd's connection pool (corHttp has a buffer of its own)
//
void corRestConnectionMemorySet(int kb)
{
  (void) kb;
}



//
// corRestHttpLoopsSet - how many event loops to run (builtin server only)
//
// Called before corRestInit. Clamped rather than rejected: the caller's job is
// to pass what the user asked for, not to know this file's limits.
//
void corRestHttpLoopsSet(int loops)
{
  if (loops < 1)
    loops = 1;
  else if (loops > COR_REST_MAX_LOOPS)
    loops = COR_REST_MAX_LOOPS;

  corHttpLoops = loops;
}



// -----------------------------------------------------------------------------
//
// corRestBackendLoops - how many event loops corRestBackendStart is going to run
//
// Known before the start - it is corRestHttpLoopsSet's value - and corRestInit
// sizes the worker pool's queues off it (one per loop) before any loop exists.
//
int corRestBackendLoops(void)
{
  return corHttpLoops;
}



// -----------------------------------------------------------------------------
//
// Hook globals (defined in corRestHooks.c)
//
extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;
extern CorRestFinishInlineHook   corRestFinishInlineHookF;
extern unsigned long long        corRestMaxRequestSize;



#if COR_WITH_STREAM
// -----------------------------------------------------------------------------
//
// CorRestStream - corHttp's stream (corHttpResponseStream), which is all a stream needs: writes from any
// thread, queued, a client too far behind disconnected (COR_HTTP_STREAM_PENDING_MAX, the same 16 MiB as
// COR_REST_STREAM_PENDING_MAX), the end from any thread
//
struct CorRestStream
{
  CorHttpStream*  httpStreamP;
};



// -----------------------------------------------------------------------------
//
// streamOpen - the response a stream (corRestStreamResponse): corHttp's, and the handle to the application
//
static void streamOpen(CorHttpConn* connP)
{
  CorRestStream* streamP = (CorRestStream*) malloc(sizeof(CorRestStream));
  CorHttpStream* httpP   = corHttpResponseStream(connP);

  if ((streamP == NULL) || (httpP == NULL))
  {
    free(streamP);
    if (httpP != NULL)
      corHttpStreamEnd(httpP);                       // the body ends at once

    corRest.out.streamOpenF(NULL, corRest.out.streamCtx);
    return;
  }

  streamP->httpStreamP = httpP;
  corRest.out.streamOpenF(streamP, corRest.out.streamCtx);
}



// -----------------------------------------------------------------------------
//
// corRestStreamWrite -
//
bool corRestStreamWrite(CorRestStream* streamP, const char* data, int len)
{
  return corHttpStreamWrite(streamP->httpStreamP, data, len);
}



// -----------------------------------------------------------------------------
//
// corRestStreamEnd -
//
void corRestStreamEnd(CorRestStream* streamP)
{
  corHttpStreamEnd(streamP->httpStreamP);
  free(streamP);
}
#endif



// -----------------------------------------------------------------------------
//
// requestResponseFill - move corRest.out onto the connection
//
// Runs on whichever thread built the response - a worker for the async path,
// the loop thread for the inline one - and never touches the socket. Nothing is
// copied: corHttp borrows what it is given, and everything given here lives in
// the request arena, which is released in httpRequestDone, after the bytes are
// out.
//
static void requestResponseFill(CorHttpConn* connP)
{
  CorRestKeyValue headerV[COR_REST_RESPONSE_HEADERS_MAX];
  int             headers = corRestResponseHeaderVBuild(headerV, COR_REST_RESPONSE_HEADERS_MAX);

  corHttpResponseStatus(connP, corRest.out.httpStatusCode);

  for (int ix = 0; ix < headers; ix++)
    corHttpResponseHeader(connP, headerV[ix].key, headerV[ix].value);

  //
  // HEAD gets the length of the body and none of its bytes, and that decision
  // is corHttp's (it knows the method) - so the body goes over either way.
  //
  if ((corRest.out.payload != NULL) && (corRest.out.payloadSize > 0))
    corHttpResponseBody(connP, corRest.out.payload, corRest.out.payloadSize);

#if COR_WITH_STREAM
  if (corRest.out.streamOpenF != NULL)
    streamOpen(connP);
#endif
}



// -----------------------------------------------------------------------------
//
// requestCoroutine - a request that can wait, run as a coroutine of the loop that read it
//
// The request, its response, and the response on its way - all on the loop's thread (corHttpResumeHere:
// no worker hand-back, no eventfd).
//
enum { CO_MAX = 1024 };

static __thread int coRunning = 0;                  // this loop's coroutines alive

static void requestCoroutine(void* arg)
{
  CorHttpConn* connP = (CorHttpConn*) arg;

  corRestP = (CorRestState*) connP->userData;
  corRestProcessRequest();
  requestResponseFill(connP);
  corRestP = NULL;

  corHttpResumeHere(connP);                          // may finish the request - httpRequestDone - before it returns

  coRunning -= 1;
}



// -----------------------------------------------------------------------------
//
// finishCoroutine - a post-response phase that can wait, run as a coroutine of the loop
//
static void finishCoroutine(void* arg)
{
  corRestBackendFinish((CorRestState*) arg);
  coRunning -= 1;
}



// -----------------------------------------------------------------------------
//
// httpRequestCb - one complete request, on the event-loop thread
//
// -----------------------------------------------------------------------------
//
// BuiltinUpgrade - what the upgrade callback needs: the application's function and its context
//
typedef struct BuiltinUpgrade
{
  CorRestUpgradeTake  take;
  void*               ctx;
} BuiltinUpgrade;



// -----------------------------------------------------------------------------
//
// builtinUpgradeClose - the application is done with the socket
//
static void builtinUpgradeClose(void* fdP)
{
  close((int) (intptr_t) fdP);
}



// -----------------------------------------------------------------------------
//
// builtinUpgradeHand - the 101 is written (corHttpUpgrade): the socket to the application
//
static void builtinUpgradeHand(int fd, char* extra, int extraLen, void* cls)
{
  BuiltinUpgrade* uP = (BuiltinUpgrade*) cls;

  uP->take(fd, extra, extraLen, builtinUpgradeClose, (void*) (intptr_t) fd, uP->ctx);
  free(uP);
}



// -----------------------------------------------------------------------------
//
// builtinUpgrade - the 101 with the headers the application set (corRest.out), the socket handed over
// once it is written. The header strings are corRest's, alive until httpRequestDone - after the write.
//
static bool builtinUpgrade(CorHttpConn* connP, CorRestUpgradeTake take, void* ctx)
{
  BuiltinUpgrade* uP = (BuiltinUpgrade*) malloc(sizeof(BuiltinUpgrade));

  if (uP == NULL)
    return false;

  uP->take = take;
  uP->ctx  = ctx;

  CorRestKeyValue headerV[COR_REST_RESPONSE_HEADERS_MAX];
  int             headers = corRestResponseHeaderVBuild(headerV, COR_REST_RESPONSE_HEADERS_MAX);

  corHttpResponseStatus(connP, 101);
  for (int i = 0; i < headers; i++)
    corHttpResponseHeader(connP, headerV[i].key, headerV[i].value);

  corHttpUpgrade(connP, builtinUpgradeHand, uP);
  return true;
}



static void httpRequestCb(CorHttpConn* connP)
{
  COR_V("Request: %s %s", connP->method.s, connP->path.s); // one line per request (-v)

  //
  // One CorRestState per request, hung on the connection - the same arrangement
  // as MHD's con_cls, and for the same reason: the loop may run another
  // connection's request between this one's dispatch and its send.
  //
  CorRestState* stateP = (CorRestState*) malloc(sizeof(CorRestState));

  if (stateP == NULL)
  {
    corHttpResponseStatus(connP, 503);
    return;
  }

  connP->userData = stateP;
  corRestP        = stateP;

  corRestStateInit(connP, connP->path.s, connP->method.s);

  //
  // Which loop read this request - and therefore which work queue it belongs
  // to, and which loop will write its answer. corRestStateInit has just zeroed
  // the state, so a single-loop build needs nothing and gets shard 0 anyway.
  //
  // Pointer arithmetic into our own array, which is why this lives here and not
  // in corRest proper: this file owns corHttpServerV, and corRest has no idea
  // event loops exist.
  //
  stateP->shard = (int) (connP->serverP - corHttpServerV);

  // The verb string is corHttp's too - copied, like everything else below.
  corRest.in.verbString = corAllocStrdup(&corRest.kalloc, connP->method.s);

  //
  // The path arrived percent-ENCODED (MHD would have decoded it before we ever
  // saw it). Decoded here, on corRest's own strdup'd copy rather than in the
  // read buffer, and AFTER corRestStateInit split off a query that corHttp had
  // already separated - so a path containing `%3F` decodes to a '?' inside the
  // path, which is what it is, instead of splitting the URL in two.
  //
  corRestUrlValueDecode(corRest.in.urlPath);
  corRestUrlPathNormalize();

  // Create this connection's application state (e.g. per-conn corNgsild),
  // stored in corRest.userData and freed in httpRequestDone.
  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  // Capture request start time — REALTIME for timestamps, MONOTONIC for duration metrics
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  corRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

  struct timespec tsM;
  clock_gettime(CLOCK_MONOTONIC, &tsM);
  corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

  //
  // Request headers, COPIED into the request arena - see the note at the top of
  // this file. A header that will not fit is dropped rather than borrowed: a
  // missing header is a wrong answer, a dangling one is a crash under load
  // weeks later.
  //
  for (int ix = 0; ix < connP->headers; ix++)
  {
    char* key   = corAllocStrdup(&corRest.kalloc, connP->header[ix].key.s);
    char* value = corAllocStrdup(&corRest.kalloc, connP->header[ix].value.s);

    if ((key != NULL) && (value != NULL))
      corRestHttpHeaderAdd(key, value);
  }

  //
  // The query string, split and percent-decoded by corRest itself - corHttp
  // split it too, but on a copy and without decoding, which is not what the
  // layers above expect. Parsed from a copy of our own because the parse is
  // destructive and corHttp's raw query is not ours to consume.
  //
  // corRest.in.urlParams is left NULL afterwards, exactly as it is under MHD:
  // it does not survive its own parser (see corRestUriParamsParse), and a
  // half-eaten query string is worse than none.
  //
  if (connP->query.len > 0)
  {
    char* query = (char*) corAlloc(&corRest.kalloc, connP->query.len + 1);

    if (query != NULL)
    {
      memcpy(query, connP->query.s, connP->query.len);
      query[connP->query.len] = 0;

      //
      // '+' means space - see the note at the top of this file. BEFORE the
      // percent-decoding in corRestUriParamsParse and not after, so that a
      // `%2B` (a plus the client meant literally) decodes to a plus and stays
      // one, instead of being turned into a space by this loop.
      //
      for (char* p = query; *p != 0; p++)
      {
        if (*p == '+')
          *p = ' ';
      }

      corRest.in.urlParams = query;
      corRestUriParamsParse();
      corRest.in.urlParams = NULL;
    }
  }

  corRestBodyPolicyCheck(corRest.in.urlPath, corHttpHeader(connP, "Content-Length"));

  //
  // An upgrade (CorRestUpgradeHook): accepted - the 101, and the socket to the application once it is
  // written; refused - its error, answered as any response (corRestProcessRequest goes straight to it)
  //
  const char* upgrade = (corRestUpgradeHookF != NULL) ? corHttpHeader(connP, "Upgrade") : NULL;

  if (upgrade != NULL)
  {
    void*              ctx  = NULL;
    CorRestUpgradeTake take = corRestUpgradeHookF(upgrade, &ctx);

    if ((take != NULL) && (builtinUpgrade(connP, take, ctx) == true))
      return;

    corRest.upgradeRefused = true;
  }

  //
  // The body, into the request arena. Released with everything else by
  // corRestStateRelease, which is why corRestBackendFinish here does not
  // free(corRest.in.payload) the way the MHD backend has to - there, the body
  // is a malloc'd accumulation buffer.
  //
  if ((connP->body.s != NULL) && (connP->body.len > 0) &&
      (corRest.in.contentLengthMissing == false) && (corRest.out.httpStatusCode != 413))
  {
    char* body = (char*) corAlloc(&corRest.kalloc, connP->body.len + 1);

    if (body != NULL)
    {
      memcpy(body, connP->body.s, connP->body.len);
      body[connP->body.len] = 0;

      corRest.in.payload     = body;
      corRest.in.payloadSize = connP->body.len;
    }
  }

  if (corRestAsyncDispatch() == true)
  {
    //
    // A request that may wait (a distributed operation, an @context to download) must not stop
    // every other connection of the loop for its duration - one that waits on nothing runs on the
    // loop instead, see CorRestInlineHook. It runs as a COROUTINE of the loop (doc/coroutines.md):
    // wherever it waits for a socket it yields, the loop serving the rest. The connection is out of
    // the loop's epoll meanwhile - the coroutine owns it. A worker only when the loop has as many
    // coroutines as it takes, or no stack is to be had.
    //
    corHttpSuspend(connP);

    CorCo* coP = ((coRunning < CO_MAX) && (corRestCoroutineAllowed() == true)) ? corCoCreate(requestCoroutine, connP) : NULL;

    if (coP == NULL)
    {
      corRestAsyncEnqueue(stateP);                  // suspend BEFORE enqueue - a worker can finish before it returns
      return;
    }

    coRunning += 1;
    corRestP   = NULL;
    corCoLoopResume(coP);
    return;
  }

  corRestProcessRequest();          // pool down (shutdown / tests): run inline here
  requestResponseFill(connP);
}



// -----------------------------------------------------------------------------
//
// httpRequestDone - the response is on the wire; the request state can go
//
// corHttp calls this on the LOOP thread after the last byte of the response has
// been written (or when the connection died with a request on it). That is the
// same moment MHD's NOTIFY_COMPLETED fires, and it has to be: the response
// headers and body are borrowed from the arena released at the end of it.
//
// ⚠️ THE WORK ITSELF MUST NOT RUN HERE, BLOCKING. The post-response phase dispatches the
// notifications for a write, and a notification is compacted with the
// subscription's @context - which can be one THIS BROKER HOSTS. The loop thread
// would then be waiting for an answer only the loop thread can produce. It does
// not hang forever, which is worse: the download times out, the notification
// goes out ten seconds late with an uncompacted body, and nothing says why.
//
// So it goes to a worker - or runs as a coroutine of the loop, which yields where it waits and lets
// the loop answer that @context request itself (CorRestFinishCoroutineHook) - and the loop returns
// immediately. Everything the
// phase reads was copied out of the connection in httpRequestCb, so the
// connection is free to take its next request the instant this returns.
//
static void httpRequestDone(CorHttpConn* connP)
{
  CorRestState* stateP = (CorRestState*) connP->userData;

  connP->userData    = NULL;        // before anything can fail: this must not run twice
  stateP->connection = NULL;        // the connection is not this request's any more

  //
  // ...unless the request left nothing for the phase that could wait - the app says (see
  // CorRestFinishInlineHook). Then a worker would only cost two thread switches.
  //
  if (corRestFinishInlineHookF != NULL)
  {
    corRestP = stateP;
    bool here = corRestFinishInlineHookF();
    corRestP  = NULL;

    if (here == true)
    {
      corRestBackendFinish(stateP);
      return;
    }
  }

  //
  // ...or as a coroutine of this loop, which yields where the phase waits (a notification's POST).
  // Started right here - this may be inside the request's own coroutine (it answered through
  // corHttpResumeHere), and a coroutine resumed from a coroutine yields back to it: the connection's
  // own work goes on, and the loop resumes the phase when what it waits for is ready.
  //
  corRestP = stateP;
  CorCo* coP = ((coRunning < CO_MAX) && (corRestFinishCoroutineAllowed() == true)) ? corCoCreate(finishCoroutine, stateP) : NULL;
  corRestP = NULL;

  if (coP != NULL)
  {
    coRunning += 1;
    corCoLoopResume(coP);
    corRestP = NULL;                // the phase may have yielded with its request bound
    return;
  }

  if (corRestAsyncFinish(stateP) == true)
    return;

  corRestBackendFinish(stateP);     // no pool (shutdown / tests): here, then
}



// -----------------------------------------------------------------------------
//
// corRestBackendFinish - the post-response phase, and the end of the request
//
void corRestBackendFinish(CorRestState* stateP)
{
  corRestP = stateP;

  // Post-response hook BEFORE releasing the arena, so deferred work (the
  // notifications for a write, say) can still read what the service routine built.
  corRestPostResponseHook();

  corRestStateRelease();

  if (corRestUserDataFreeHookF != NULL && corRest.userData != NULL)
    corRestUserDataFreeHookF(corRest.userData);

  free(stateP);

  corRestP = NULL;                  // no dangling pointer to freed state on this thread
}



// -----------------------------------------------------------------------------
//
// corRestBackendResume -
//
// On the worker thread, with corRestP still bound: build the response, then
// hand the connection back to the loop. corHttpResume does not write to the
// socket - the loop does - which is what keeps two answers off one connection.
//
void corRestBackendResume(CorRestState* stateP)
{
  CorHttpConn* connP = (CorHttpConn*) stateP->connection;

  requestResponseFill(connP);
  corHttpResume(connP);
}



// -----------------------------------------------------------------------------
//
// coroutinesEnd - the loop has stopped: its coroutines run to their end, their waits cancelled
//
// A request or post-response phase that runs as a coroutine of this loop may be waiting - for a
// Context Source to connect, a notification's endpoint to answer, a lookup - when the loop stops.
// Left there, it is never resumed: its request state, its coroutine and whatever it holds on its stack
// (an address list, a connection) are never freed. Cancelled instead (corCoLoopCancel), every wait it is
// in, or comes to, fails at once: it runs its failure path - a forward that failed, a notification that
// failed - to its end, answers its request if it has one, and frees what it holds, as at any failure.
//
// The workers are done by now (corRestStop drained them first). Then the connections still holding a
// request (a response not all written) are closed - their doneCb ends the request, which may start a
// post-response coroutine - and that one is run to its end too.
//
static void coroutinesEnd(CorHttpServer* serverP)
{
  corCoLoopCancel();

  for (int round = 0; round < 2; round++)
  {
    while ((coRunning > 0) && (corCoLoopPending() == true))
      corCoLoopExpire();

    if (round == 0)
      corHttpServeEnd(serverP);
  }

  if (coRunning > 0)
    COR_W("built-in HTTP server: %d request(s) never ended at the stop - their state is not freed", coRunning);
}



// -----------------------------------------------------------------------------
//
// serveThread -
//
static void* serveThread(void* serverP)
{
  //
  // This thread is the loop: a request that waits runs as a coroutine of it (doc/coroutines.md)
  //
  corRestCoLoopInit(((CorHttpServer*) serverP)->epollFd);

  corHttpServe((CorHttpServer*) serverP);
  coroutinesEnd((CorHttpServer*) serverP);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestBackendStart -
//
int corRestBackendStart(unsigned short port, int poolSize, char* keyPem, char* certPem)
{
  if ((keyPem != NULL) || (certPem != NULL))
  {
    //
    // Refused, not ignored. The alternative is a server that was asked for TLS
    // and serves the port in the clear, which nothing downstream would notice
    // until a client's traffic was already on the wire unencrypted.
    //
    fprintf(stderr, "corRestInit: the built-in HTTP server has no TLS - "
                    "rebuild with COR_HTTP_SERVER=mhd for an HTTPS listener\n");
    return -1;
  }

  //
  // The connection pool, not a thread pool: this server is one thread and the
  // parallelism is corRest's worker pool behind it. Sized off poolSize all the
  // same, since that is the caller's statement about how much concurrency to
  // expect, and 16 connections per worker is room rather than a limit.
  //
  int connPoolSize = (poolSize > 0) ? poolSize * 16 : COR_HTTP_CONN_POOL_SIZE;

  if (connPoolSize < COR_HTTP_CONN_POOL_SIZE)
    connPoolSize = COR_HTTP_CONN_POOL_SIZE;

  //
  // DIVIDED among the loops, never replicated.
  //
  // The pool is slots x 16 KiB of buffer, allocated at start-up so that no
  // request ever mallocs its own machinery. Giving each of 8 loops a full 1024
  // slots would reserve eight times the memory to serve the same number of
  // concurrent connections, and take the idle resident set from ~18 MiB to
  // something that would rewrite the footprint numbers. The total is the
  // caller's statement about expected concurrency; how many loops it is spread
  // over is this file's business.
  //
  int perLoop = connPoolSize / corHttpLoops;

  if (perLoop < 16)          // a loop with almost no slots is worse than fewer loops
    perLoop = 16;

  //
  // The first loop alone listens: it accepts every connection and deals them out (corHttpAcceptShare
  // below), so the others need no listen socket (noListener) and the port no SO_REUSEPORT - a second
  // process on the port fails to start (EADDRINUSE) instead of being handed part of the connections.
  //
  CorHttpListenOptions listenOptions = { NULL, false, false };
  CorHttpListenOptions handedOptions = { NULL, false, true };

  //
  // ⭐ The listening loop LAST: the port opens when the server is a server. Loop 0 first opened it and the
  // others were set up after - a client connecting then (a readiness probe, a test harness) waited for them.
  //
  for (int ix = corHttpLoops - 1; ix >= 0; ix--)
  {
    if (corHttpInitOptions(&corHttpServerV[ix], port, (ix == 0) ? &listenOptions : &handedOptions, perLoop, httpRequestCb) != CorHttpOk)
    {
      fprintf(stderr, "corRestInit: the built-in HTTP server failed to listen on port %d: %s\n", port, strerror(errno));

      // Undo the ones that did come up - a half-listening server is not a server.
      for (int done = ix + 1; done < corHttpLoops; done++)
        corHttpRelease(&corHttpServerV[done]);

      return -1;
    }

    corHttpServerV[ix].doneCb = httpRequestDone;
  }

  //
  // One loop accepts and deals the connections out in turn: the kernel's hash (SO_REUSEPORT) split 16
  // connections as unevenly as 11 and 5, and with a request running as a coroutine of the loop that
  // read it, the busier loop queued - the p99 of a 16-caller chain doubled in half the runs
  // (coraine doc/coroutines.md § 4).
  //
  if (corHttpAcceptShare(corHttpServerV, corHttpLoops) != CorHttpOk)
  {
    fprintf(stderr, "corRestInit: the built-in HTTP server's loops failed to share the port\n");

    for (int all = 0; all < corHttpLoops; all++)
      corHttpRelease(&corHttpServerV[all]);

    return -1;
  }

  //
  // The same § 6.3.2 threshold the dispatch enforces, plus room for the headers
  // that arrive in the same buffer as the body.
  //
  // Over it, corHttp delivers the request WITHOUT its body rather than reading
  // one it is going to refuse, and corRestBodyPolicyCheck then answers from the
  // Content-Length header with the ProblemDetails the spec asks for - the same
  // answer MHD's streaming path produces, from the same header, in the same
  // words. What the engine's own cap is left holding is a client that LIES
  // about its length, which is the one case where there is no announcement to
  // believe.
  //
  if (corRestMaxRequestSize > 0)
  {
    unsigned long long cap = corRestMaxRequestSize + (64 * 1024);

    // maxRequestSize is an int over there; a --maxRequestSize of a few GiB would
    // otherwise wrap and cap every request at a negative number of bytes.
    for (int ix = 0; ix < corHttpLoops; ix++)
      corHttpServerV[ix].maxRequestSize = (cap > 0x7fffffffULL) ? 0 : (int) cap;
  }

  for (int ix = 0; ix < corHttpLoops; ix++)
  {
    if (pthread_create(&corHttpThreadV[ix], NULL, serveThread, &corHttpServerV[ix]) != 0)
    {
      fprintf(stderr, "corRestInit: the built-in HTTP server's event loop failed to start\n");

      //
      // Stop and join the loops already running before releasing anything -
      // a thread still inside corHttpServe owns its server's fds.
      //
      for (int up = 0; up < ix; up++)
        corHttpStop(&corHttpServerV[up]);

      for (int up = 0; up < ix; up++)
        pthread_join(corHttpThreadV[up], NULL);

      for (int all = 0; all < corHttpLoops; all++)
        corHttpRelease(&corHttpServerV[all]);

      return -1;
    }
  }

  corHttpRunning = true;

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestBackendListen - the built-in server has one listener
//
int corRestBackendListen(const char* host, unsigned short port)
{
  fprintf(stderr, "corRestListen: %s port %d: the built-in HTTP server has one listener - libmicrohttpd has two\n", host, port);
  return -1;
}



// -----------------------------------------------------------------------------
//
// corRestBackendStop -
//
void corRestBackendStop(void)
{
  if (corHttpRunning == false)
    return;

  //
  // Signal them ALL first, then join them all, so the loops wind down side by
  // side rather than one after the other.
  //
  for (int ix = 0; ix < corHttpLoops; ix++)
    corHttpStop(&corHttpServerV[ix]);   // a flag, and its eventfd written - the loop wakes at once

  for (int ix = 0; ix < corHttpLoops; ix++)
    pthread_join(corHttpThreadV[ix], NULL);

  for (int ix = 0; ix < corHttpLoops; ix++)
    corHttpRelease(&corHttpServerV[ix]);

  corHttpRunning = false;
}
