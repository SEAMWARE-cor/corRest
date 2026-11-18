//
// FILE            corRestBackendMhd.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The libmicrohttpd backend.
//
// Everything here is MHD's shape: its multi-call request callback, its value
// iterators, its response object, its suspend/resume. The request itself -
// what the URL means, what the body has to be, which headers go back and in
// what order - is on the other side of corRestBackend.h and is shared with the
// built-in server. This file translates, and decides nothing.
//
#include <stdlib.h>                       // strtoull, malloc, free, realloc
#include <string.h>                       // memcpy, strncmp
#include <stdio.h>                        // fprintf
#include <time.h>                         // clock_gettime
#include <stdarg.h>                       // va_list
#include <stdint.h>                       // intptr_t
#include <netdb.h>                        // getaddrinfo, freeaddrinfo
#include <netinet/in.h>                   // sockaddr_in, sockaddr_in6
#include <pthread.h>                      // pthread_mutex_*
#include <sys/socket.h>                   // recv, MSG_PEEK, MSG_DONTWAIT

#include <microhttpd.h>

#include "corLog/corLog.h"                // COR_V

#include "corRest/CorRestVerb.h"          // CorVerbPost, ...
#include "corRest/CorRestState.h"         // CorRestState, corRest
#include "corRest/corRestStateInit.h"     // corRestStateInit, corRestStateRelease
#include "corRest/corRestHooks.h"         // CorRestHook, corRestSetMaxRequestSize
#include "corRest/corRestProblem.h"       // COR_REST_ERROR_*, corRestProblem
#include "corRest/corRestStream.h"        // CorRestStream, corRestStreamWrite, corRestStreamEnd
#include "corRest/corRestBackend.h"       // Own interface

extern CorRestUpgradeHook corRestUpgradeHookF;          // corRestHooks.c



// -----------------------------------------------------------------------------
//
// The daemon
//
static struct MHD_Daemon*  mhdDaemon = NULL;
static struct MHD_Daemon*  mhdDaemon2 = NULL;          // corRestBackendListen's




#if COR_WITH_STREAM
// -----------------------------------------------------------------------------
//
// CorRestStream - a response from a content reader (MHD_create_response_from_callback, size unknown:
// chunked on HTTP/1.1)
//
// The writers append to 'buf' under 'mutex', from any thread. The reader, on MHD's thread, hands MHD what
// is there; with nothing there it suspends the connection and says 0 - the only 0 MHD allows with its
// own polling - and the next write, or the end, resumes it. A suspended connection has no timeout and
// MHD does not watch its socket: a write of 0 bytes looks (peerLeft) - the client closed its side, and
// the connection is cut.
//
// Two references: the application's (corRestStreamEnd) and the response's (streamFree, MHD's free
// callback). The last one frees.
//
struct CorRestStream
{
  pthread_mutex_t         mutex;
  struct MHD_Connection*  connection;
  char*                   buf;
  size_t                  len;            // bytes in buf
  size_t                  off;            // of them, taken by MHD
  size_t                  size;           // allocated
  bool                    suspended;      // the reader suspended the connection, nothing resumed it yet
  bool                    ended;          // corRestStreamEnd, or the server stops: the body ends after what is in buf
  bool                    cut;            // fell COR_REST_STREAM_PENDING_MAX behind: the connection is closed
  bool                    gone;           // the response is destroyed: the connection is over
  int                     refs;
};

static pthread_mutex_t    streamsMutex = PTHREAD_MUTEX_INITIALIZER;
static int                streams      = 0;          // responses not destroyed yet - corRestBackendStop waits for 0



//
// resume - the connection back into MHD's loop, if the reader suspended it; called with sP->mutex held
//
static void resume(CorRestStream* sP)
{
  if ((sP->suspended == true) && (sP->gone == false))
  {
    sP->suspended = false;
    MHD_resume_connection(sP->connection);
  }
}



//
// release - one reference let go; the last one frees
//
static void release(CorRestStream* sP)
{
  pthread_mutex_lock(&sP->mutex);
  int refs = --sP->refs;
  pthread_mutex_unlock(&sP->mutex);

  if (refs == 0)
  {
    pthread_mutex_destroy(&sP->mutex);
    free(sP->buf);
    free(sP);
  }
}



//
// peerLeft - the client closed its side of a suspended connection: a read would say end of file. A stream's
// client sends nothing after its request, so anything there (TLS records, say) is a client still there.
// Called with sP->mutex held: MHD does not read a suspended connection, and does not close it before the
// response is destroyed (gone).
//
static bool peerLeft(CorRestStream* sP)
{
  if ((sP->suspended == false) || (sP->gone == true))
    return false;

  const union MHD_ConnectionInfo* infoP = MHD_get_connection_info(sP->connection, MHD_CONNECTION_INFO_CONNECTION_FD);
  char                            c;

  return (infoP != NULL) && (recv(infoP->connect_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0);
}



//
// streamRead - MHD's content reader
//
static ssize_t streamRead(void* cls, uint64_t pos, char* buf, size_t max)
{
  CorRestStream* sP = (CorRestStream*) cls;
  ssize_t        n  = 0;

  (void) pos;

  pthread_mutex_lock(&sP->mutex);

  if (sP->cut == true)
    n = MHD_CONTENT_READER_END_WITH_ERROR;
  else if (sP->len > sP->off)
  {
    n = (ssize_t) ((sP->len - sP->off < max) ? sP->len - sP->off : max);
    memcpy(buf, &sP->buf[sP->off], (size_t) n);
    sP->off += (size_t) n;

    if (sP->off == sP->len)
      sP->off = sP->len = 0;
  }
  else if (sP->ended == true)
    n = MHD_CONTENT_READER_END_OF_STREAM;
  else
  {
    sP->suspended = true;
    MHD_suspend_connection(sP->connection);
  }

  pthread_mutex_unlock(&sP->mutex);
  return n;
}



//
// streamFree - MHD's free callback: the response is destroyed, the connection is over
//
static void streamFree(void* cls)
{
  CorRestStream* sP = (CorRestStream*) cls;

  pthread_mutex_lock(&sP->mutex);
  sP->gone       = true;
  sP->connection = NULL;
  pthread_mutex_unlock(&sP->mutex);

  pthread_mutex_lock(&streamsMutex);
  --streams;
  pthread_mutex_unlock(&streamsMutex);

  release(sP);
}



//
// streamResponse - the response for corRestStreamResponse; NULL: none could be made (the handle went to
// the application as NULL)
//
static struct MHD_Response* streamResponse(struct MHD_Connection* connection)
{
  CorRestStream*       sP        = (CorRestStream*) calloc(1, sizeof(CorRestStream));
  struct MHD_Response* responseP = NULL;

  if (sP != NULL)
  {
    pthread_mutex_init(&sP->mutex, NULL);
    sP->connection = connection;
    sP->refs       = 2;

    responseP = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4096, streamRead, sP, streamFree);
    if (responseP == NULL)
    {
      pthread_mutex_destroy(&sP->mutex);
      free(sP);
      sP = NULL;
    }
  }

  if (sP != NULL)
  {
    pthread_mutex_lock(&streamsMutex);
    ++streams;
    pthread_mutex_unlock(&streamsMutex);
  }

  corRest.out.streamOpenF(sP, corRest.out.streamCtx);

  return responseP;
}



// -----------------------------------------------------------------------------
//
// corRestStreamWrite -
//
bool corRestStreamWrite(CorRestStream* sP, const char* data, int len)
{
  bool ok = false;

  pthread_mutex_lock(&sP->mutex);

  if ((len == 0) && (peerLeft(sP) == true))
    sP->cut = true;                                   // the reader closes the connection (resume, below)

  if ((sP->gone == false) && (sP->ended == false) && (sP->cut == true))
    resume(sP);
  else if ((sP->gone == false) && (sP->ended == false) && (len == 0))
    ok = true;
  else if ((sP->gone == false) && (sP->ended == false))
  {
    size_t need = sP->len + (size_t) len;

    if (need - sP->off > COR_REST_STREAM_PENDING_MAX)
      sP->cut = true;                                 // the reader closes the connection
    else
    {
      if (need > sP->size)
      {
        size_t size = (sP->size == 0) ? 4096 : sP->size;

        while (size < need)
          size *= 2;

        char* bufP = (char*) realloc(sP->buf, size);

        if (bufP == NULL)
          sP->cut = true;
        else
        {
          sP->buf  = bufP;
          sP->size = size;
        }
      }

      if (sP->cut == false)
      {
        memcpy(&sP->buf[sP->len], data, (size_t) len);
        sP->len = need;
        ok      = true;
      }
    }

    resume(sP);
  }

  pthread_mutex_unlock(&sP->mutex);
  return ok;
}



// -----------------------------------------------------------------------------
//
// corRestStreamEnd -
//
void corRestStreamEnd(CorRestStream* sP)
{
  pthread_mutex_lock(&sP->mutex);
  sP->ended = true;
  resume(sP);
  pthread_mutex_unlock(&sP->mutex);

  release(sP);
}
#endif


// -----------------------------------------------------------------------------
//
// Hook globals (defined in corRestHooks.c)
//
extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;
extern unsigned long long        corRestMaxRequestSize;



// -----------------------------------------------------------------------------
//
// mhdHeaderIterator - MHD callback to collect request headers
//
static enum MHD_Result mhdHeaderIterator
(
  void*              cls,
  enum MHD_ValueKind kind,
  const char*        key,
  const char*        value
)
{
  corRestHttpHeaderAdd(key, value);
  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// mhdUriParamIterator - MHD callback to collect URI query parameters
//
// MHD strips query params from the URL and provides them via MHD_GET_ARGUMENT_KIND,
// already percent-decoded — which is why this hands them straight over rather
// than going through corRestUriParamsParse (the built-in backend's route, since
// that server decodes nothing).
//
static enum MHD_Result mhdUriParamIterator
(
  void*              cls,
  enum MHD_ValueKind kind,
  const char*        key,
  const char*        value
)
{
  corRestUriParamAdd((char*) key, (char*) (value ? value : ""));
  return MHD_YES;
}



// -----------------------------------------------------------------------------
//
// mhdConnectionHandler - MHD callback, called for each incoming request
//
// MHD calls this multiple times per request:
//   1. First call:  *con_cls == NULL  -> init corRest state
//   2. Middle calls: upload_data_size > 0 -> accumulate payload
//   3. Final call:   upload_data_size == 0 -> parse, dispatch, render, respond
//
// -----------------------------------------------------------------------------
//
// MhdUpgrade - what the upgrade handler needs: the application's function and its context
//
typedef struct MhdUpgrade
{
  CorRestUpgradeTake  take;
  void*               ctx;
} MhdUpgrade;



// -----------------------------------------------------------------------------
//
// mhdUpgradeClose - the application is done with the socket: libmicrohttpd closes it
//
static void mhdUpgradeClose(void* urh)
{
  MHD_upgrade_action((struct MHD_UpgradeResponseHandle*) urh, MHD_UPGRADE_ACTION_CLOSE);
}



// -----------------------------------------------------------------------------
//
// mhdUpgradeHandler - the 101 is written: the socket, and the bytes behind the request, to the application
//
static void mhdUpgradeHandler
(
  void*                             cls,
  struct MHD_Connection*            connection,
  void*                             req_cls,
  const char*                       extra_in,
  size_t                            extra_in_size,
  MHD_socket                        sock,
  struct MHD_UpgradeResponseHandle* urh
)
{
  MhdUpgrade* uP = (MhdUpgrade*) cls;

  (void) connection;
  (void) req_cls;

  uP->take(sock, extra_in, (int) extra_in_size, mhdUpgradeClose, urh, uP->ctx);
  free(uP);
}



// -----------------------------------------------------------------------------
//
// mhdUpgradeQueue - the 101 with the headers the application set (corRest.out), for mhdUpgradeHandler
//
static enum MHD_Result mhdUpgradeQueue(struct MHD_Connection* connection, CorRestUpgradeTake take, void* ctx)
{
  MhdUpgrade* uP = (MhdUpgrade*) malloc(sizeof(MhdUpgrade));

  if (uP == NULL)
    return MHD_NO;

  uP->take = take;
  uP->ctx  = ctx;

  struct MHD_Response* response = MHD_create_response_for_upgrade(mhdUpgradeHandler, uP);

  if (response == NULL)
  {
    free(uP);
    return MHD_NO;
  }

  CorRestKeyValue headerV[COR_REST_RESPONSE_HEADERS_MAX];
  int             headers = corRestResponseHeaderVBuild(headerV, COR_REST_RESPONSE_HEADERS_MAX);

  for (int i = 0; i < headers; i++)
    MHD_add_response_header(response, headerV[i].key, headerV[i].value);

  enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_SWITCHING_PROTOCOLS, response);
  MHD_destroy_response(response);

  return ret;
}



static enum MHD_Result mhdConnectionHandler
(
  void*                  cls,
  struct MHD_Connection* connection,
  const char*            url,
  const char*            method,
  const char*            version,
  const char*            uploadData,
  size_t*                uploadDataSize,
  void**                 con_cls
)
{
  // --- First call: allocate this connection's per-request state ---
  if (*con_cls == NULL)
  {
    COR_V("Request: %s %s", method, url); // one line per request (verbose mode, -v)

    // Each connection owns its CorRestState (hung on con_cls), so when the
    // epoll pool thread interleaves connection B's callbacks between
    // connection A's body-read callbacks it can no longer clobber A's state.
    // corRestP is bound to it before corRestStateInit (whose memset/init runs
    // through the corRest macro).
    CorRestState* conP = (CorRestState*) malloc(sizeof(CorRestState));
    if (conP == NULL)
      return MHD_NO;
    *con_cls = conP;
    corRestP  = conP;

    corRestStateInit(connection, url, method);
    corRest.listener = (int) (intptr_t) cls;           // the daemon's: 0 corRestBackendStart's, 1 corRestBackendListen's

    // Create this connection's application state (e.g. per-conn corNgsild),
    // stored in corRest.userData and freed in mhdRequestCompleted.
    if (corRestUserDataAllocHookF != NULL)
      corRest.userData = corRestUserDataAllocHookF();

    // Capture request start time — REALTIME for timestamps, MONOTONIC for duration metrics
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    corRest.requestStartTime = (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;

    struct timespec tsM;
    clock_gettime(CLOCK_MONOTONIC, &tsM);
    corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

    // Collect request headers from MHD
    MHD_get_connection_values(connection, MHD_HEADER_KIND, mhdHeaderIterator, NULL);

    // Collect URI query parameters from MHD (already percent-decoded by MHD)
    MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, mhdUriParamIterator, NULL);

    corRestBodyPolicyCheck(url, MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Content-Length"));

    return MHD_YES;
  }

  // Subsequent calls (body chunks, final dispatch): rebind corRestP to THIS
  // connection's state — another connection's callback may have re-pointed
  // corRestP on this pool thread since our last invocation here.
  corRestP = (CorRestState*) *con_cls;

  // --- Middle calls: accumulate payload ---
  if (*uploadDataSize > 0)
  {
    // If the first-call check flagged 411/413, drop the bytes.
    if (corRest.in.contentLengthMissing || corRest.out.httpStatusCode == 413)
    {
      *uploadDataSize = 0;
      return MHD_YES;
    }

    // Streaming size cap — defends against clients that lie in
    // Content-Length or use chunked encoding without a length.
    if (corRestMaxRequestSize > 0 &&
        (unsigned long long)(corRest.in.payloadSize + *uploadDataSize) > corRestMaxRequestSize)
    {
      corRestProblem(413, COR_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                    "request body exceeds broker limit of %llu bytes", corRestMaxRequestSize);
      *uploadDataSize = 0;
      return MHD_YES;
    }

    int needed = corRest.in.payloadSize + *uploadDataSize + 1;

    if (needed > corRest.payloadBufSize)
    {
      int newSize = (needed + 4096) & ~4095;
      char* newBuf = (char*) realloc(corRest.in.payload, newSize);
      if (newBuf == NULL)
        return MHD_NO;
      corRest.in.payload     = newBuf;
      corRest.payloadBufSize = newSize;
    }

    memcpy(corRest.in.payload + corRest.in.payloadSize, uploadData, *uploadDataSize);
    corRest.in.payloadSize += *uploadDataSize;
    corRest.in.payload[corRest.in.payloadSize] = 0;

    *uploadDataSize = 0;
    return MHD_YES;
  }

  //
  // --- Final call, an upgrade (CorRestUpgradeHook): accepted - the 101, and the socket to the
  // application once it is written (mhdUpgradeHandler); refused - its error, as any response ---
  //
  if ((corRest.asyncProcessed == false) && (corRestUpgradeHookF != NULL) && (corRest.listener == 0))   // the second listener has no upgrades
  {
    const char* upgrade = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Upgrade");

    if (upgrade != NULL)
    {
      void*              ctx  = NULL;
      CorRestUpgradeTake take = corRestUpgradeHookF(upgrade, &ctx);

      if (take != NULL)
        return mhdUpgradeQueue(connection, take, ctx);

      corRest.upgradeRefused = true;
    }
  }

  // --- Final call: process (off the I/O thread, unless the app's inline hook claims it), respond ---
  if (corRest.asyncProcessed == false)
  {
    if (corRestAsyncDispatch() == true)
    {
      // Suspend this connection and hand it to a worker so DB/distop latency
      // doesn't block this epoll thread (a request that waits on nothing runs
      // right here instead - see CorRestInlineHook). The worker runs corRestProcessRequest
      // and resumes us; MHD then re-invokes this handler with asyncProcessed
      // set and we fall through to build + send the response. Suspend BEFORE
      // enqueue so a worker can never resume a not-yet-suspended connection.
      MHD_suspend_connection(connection);
      corRestAsyncEnqueue(corRestP);
      return MHD_YES;
    }

    corRestProcessRequest();    // pool down (shutdown / tests): run inline here
  }

  char* responseBody     = (corRest.out.payload != NULL) ? corRest.out.payload : (char*) "";
  int   responseBodySize = corRest.out.payloadSize;

  // Send HTTP response
  // For HEAD: pass the full body — MHD will set Content-Length correctly
  // but suppress the body in the actual response.
  struct MHD_Response* response;

#if COR_WITH_STREAM
  if (corRest.out.streamOpenF != NULL)
    response = streamResponse(connection);
  else
#endif
  response = MHD_create_response_from_buffer(
    responseBodySize,
    (void*) responseBody,
    MHD_RESPMEM_MUST_COPY
  );

  if (response == NULL)
    response = MHD_create_response_from_buffer(0, (void*) "", MHD_RESPMEM_PERSISTENT);

  CorRestKeyValue headerV[COR_REST_RESPONSE_HEADERS_MAX];
  int             headers = corRestResponseHeaderVBuild(headerV, COR_REST_RESPONSE_HEADERS_MAX);

  for (int i = 0; i < headers; i++)
    MHD_add_response_header(response, headerV[i].key, headerV[i].value);

  enum MHD_Result ret = MHD_queue_response(connection, corRest.out.httpStatusCode, response);
  MHD_destroy_response(response);

  return ret;
}



// -----------------------------------------------------------------------------
//
// corRestBackendFinish -
//
// Runs on the MHD I/O thread that finished the request. NOT handed to a worker
// the way the built-in backend has to hand it: MHD has poolSize I/O threads, so
// a post-response phase that blocks - and it can, on an @context this broker
// hosts itself - stalls one of them and the others keep serving. That is why
// this backend is left exactly as it was.
//
void corRestBackendFinish(CorRestState* stateP)
{
  corRestP = stateP;

  // Run post-response hook BEFORE releasing the per-request arena so the
  // hook can still touch arena-allocated data (e.g. deferred notification
  // dispatch reading the entity tree built by the service routine).
  corRestPostResponseHook();

  // Free payload buffer (malloc'd during accumulation, not in kalloc)
  free(corRest.in.payload);
  corRest.in.payload = NULL;

  corRestStateRelease();

  // Destroy this connection's application state (per-conn corNgsild, ...).
  if (corRestUserDataFreeHookF != NULL && corRest.userData != NULL)
    corRestUserDataFreeHookF(corRest.userData);

  free(stateP);

  corRestP = NULL;   // no dangling pointer to freed state on this thread
}



// -----------------------------------------------------------------------------
//
// mhdRequestCompleted - MHD callback when a request is fully handled
//
static void mhdRequestCompleted
(
  void*                          cls,
  struct MHD_Connection*         connection,
  void**                         con_cls,
  enum MHD_RequestTerminationCode toe
)
{
  if (*con_cls != NULL)
  {
    CorRestState* conP = (CorRestState*) *con_cls;

    *con_cls = NULL;
    corRestBackendFinish(conP);
  }
}



// -----------------------------------------------------------------------------
//
// corRestBackendResume -
//
// The worker built the response into corRest.out; MHD sends it by re-invoking
// the connection handler, which it does once the connection is resumed (the ITC
// pipe wakes the polling thread).
//
void corRestBackendResume(CorRestState* stateP)
{
  MHD_resume_connection((struct MHD_Connection*) stateP->connection);
}



//
// The connection memory - libmicrohttpd's per-connection pool, that holds a request's line and headers,
// MHD's own header table and the response's headers; a body passes through it in pieces. 0: not set -
// libmicrohttpd's own default (32 kB). MHD zeroes the whole pool at the end of every request, so its size is
// a cost on every request (doc/performance.md, "The HTTP server's connection memory").
//
static size_t connectionMemory = 0;



//
// corRestConnectionMemorySet -
//
void corRestConnectionMemorySet(int kb)
{
  connectionMemory = (kb > 0) ? (size_t) kb * 1024 : 0;
}



//
// mhdLog - libmicrohttpd's messages: one is said, the refusal of a request whose line and headers do not
// fit the connection memory - MHD answers it itself (414 / 431) before the request reaches us, so this is
// the only place it can be seen. Every other message is dropped, as before there was a logger. Said at most
// once a second, with how many more since: a client that keeps sending too much must not fill the log.
//
static time_t  mhdLogSaidAt = 0;
static int     mhdLogUnsaid = 0;

static void mhdLog(void* cls, const char* fmt, va_list ap)
{
  char        line[256];
  const char* codeP;

  (void) cls;

  vsnprintf(line, sizeof(line), fmt, ap);

  codeP = strstr(line, "HTTP response code is ");
  if (codeP == NULL)
    return;

  int code = atoi(&codeP[22]);

  if ((code != 414) && (code != 431))
    return;

  time_t now  = time(NULL);
  time_t last = __atomic_load_n(&mhdLogSaidAt, __ATOMIC_RELAXED);

  if ((now == last) || (__atomic_compare_exchange_n(&mhdLogSaidAt, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) == false))
  {
    __atomic_add_fetch(&mhdLogUnsaid, 1, __ATOMIC_RELAXED);   // said already this second (or by another thread now)
    return;
  }

  int  unsaid = __atomic_exchange_n(&mhdLogUnsaid, 0, __ATOMIC_RELAXED);
  char more[64] = "";

  if (unsaid != 0)
    snprintf(more, sizeof(more), " (and %d more since the last one said)", unsaid);

  COR_W("a request was refused with %d by the HTTP server: its %s exceed%s the connection memory (%zu kB, --httpConnectionMemory) - "
        "a long query fits in the body of POST /ngsi-ld/v1/entityOperations/query; or raise --httpConnectionMemory%s",
        code,
        (code == 414) ? "URL" : "headers",
        (code == 414) ? "s" : "",
        ((connectionMemory != 0) ? connectionMemory : 32 * 1024) / 1024,
        more);
}



//
// corRestHttpLoopsSet - no-op here
//
// libmicrohttpd has a thread per connection and no event loop of ours to
// multiply, so there is nothing for this to do. It exists so that the caller
// does not have to know which backend it was built against - the same option is
// accepted either way and simply has no effect on this one.
//
void corRestHttpLoopsSet(int loops)
{
  (void) loops;
}



// -----------------------------------------------------------------------------
//
// corRestBackendLoops - one: libmicrohttpd has no event loop of ours, so the pool runs one queue
//
int corRestBackendLoops(void)
{
  return 1;
}



// -----------------------------------------------------------------------------
//
// -----------------------------------------------------------------------------
//
// corRestBackendStart -
//
int corRestBackendStart(unsigned short port, int poolSize, char* keyPem, char* certPem)
{
  // MHD_ALLOW_SUSPEND_RESUME (which bundles MHD_USE_ITC) lets the I/O threads
  // suspend a connection and hand it to the async worker pool; the worker
  // resumes it once the response is built.
  //
  // When HTTPS server credentials have been set (a TLS test receiver, not the
  // broker), MHD_USE_TLS is added together with the in-memory key/cert.
  //
  // MHD_USE_ERROR_LOG: libmicrohttpd's messages to mhdLog (which says only a refused request's). The connection
  // memory: corRestConnectionMemorySet's, else libmicrohttpd's own default, 32 kB.
  //
  //
  // ⚠️ poll, NOT epoll. libmicrohttpd's epoll mode (MHD_USE_EPOLL, edge-triggered) loses read events when a
  // few hundred connections are opened at once: with 256 (also 257, 512 - not 250 or 300) every socket
  // held an unread request while the I/O thread slept in epoll_wait until the clients gave up - ~500
  // req/s instead of 175 000. libmicrohttpd's own, 1.0.2 and 1.0.10 alike: a 30-line server with these
  // flags and an instant answer does it too. Poll mode never did, 16 to 2000 connections, and is as fast
  // (175 000 req/s at 256, 157 000 at 2000 - one core, 2026-11-18).
  //
  unsigned int flags = MHD_USE_SELECT_INTERNALLY | MHD_USE_POLL | MHD_ALLOW_SUSPEND_RESUME | MHD_ALLOW_UPGRADE | MHD_USE_ERROR_LOG;
  size_t       memory = (connectionMemory != 0) ? connectionMemory : 32 * 1024;

  if ((keyPem != NULL) && (certPem != NULL))
    mhdDaemon = MHD_start_daemon(
      flags | MHD_USE_TLS,
      port,
      NULL,
      NULL,
      mhdConnectionHandler,
      NULL,
      MHD_OPTION_NOTIFY_COMPLETED,
      mhdRequestCompleted,
      NULL,
      MHD_OPTION_THREAD_POOL_SIZE,
      (unsigned int) poolSize,
      MHD_OPTION_CONNECTION_TIMEOUT,
      (unsigned int) 30,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT,
      memory,
      MHD_OPTION_EXTERNAL_LOGGER,
      mhdLog,
      NULL,
      MHD_OPTION_HTTPS_MEM_KEY,
      keyPem,
      MHD_OPTION_HTTPS_MEM_CERT,
      certPem,
      MHD_OPTION_END
    );
  else
    mhdDaemon = MHD_start_daemon(
      flags,
      port,
      NULL,
      NULL,
      mhdConnectionHandler,
      NULL,
      MHD_OPTION_NOTIFY_COMPLETED,
      mhdRequestCompleted,
      NULL,
      MHD_OPTION_THREAD_POOL_SIZE,
      (unsigned int) poolSize,
      MHD_OPTION_CONNECTION_TIMEOUT,
      (unsigned int) 30,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT,
      memory,
      MHD_OPTION_EXTERNAL_LOGGER,
      mhdLog,
      NULL,
      MHD_OPTION_END
    );

  if (mhdDaemon == NULL)
  {
    fprintf(stderr, "corRestInit: MHD_start_daemon failed on port %d\n", port);
    return -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestBackendListen -
//
int corRestBackendListen(const char* host, unsigned short port)
{
  struct addrinfo  hints;
  struct addrinfo* aiP = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int rc = getaddrinfo(host, NULL, &hints, &aiP);

  if ((rc != 0) || (aiP == NULL))
  {
    fprintf(stderr, "corRestListen: '%s': %s\n", host, (rc != 0) ? gai_strerror(rc) : "no address");
    return -1;
  }

  struct sockaddr_storage addr;
  unsigned int            flags = MHD_USE_SELECT_INTERNALLY | MHD_USE_POLL | MHD_ALLOW_SUSPEND_RESUME | MHD_USE_ERROR_LOG;   // poll, not epoll: see corRestBackendStart
  size_t                  memory = (connectionMemory != 0) ? connectionMemory : 32 * 1024;

  memset(&addr, 0, sizeof(addr));
  memcpy(&addr, aiP->ai_addr, aiP->ai_addrlen);

  if (aiP->ai_family == AF_INET6)
  {
    ((struct sockaddr_in6*) &addr)->sin6_port = htons(port);
    flags |= MHD_USE_IPv6;
  }
  else
    ((struct sockaddr_in*) &addr)->sin_port = htons(port);

  freeaddrinfo(aiP);

  mhdDaemon2 = MHD_start_daemon(
    flags,
    port,
    NULL,
    NULL,
    mhdConnectionHandler,
    (void*) (intptr_t) 1,
    MHD_OPTION_NOTIFY_COMPLETED,
    mhdRequestCompleted,
    NULL,
    MHD_OPTION_SOCK_ADDR,
    (struct sockaddr*) &addr,
    MHD_OPTION_CONNECTION_TIMEOUT,
    (unsigned int) 30,
    MHD_OPTION_CONNECTION_MEMORY_LIMIT,
    memory,
    MHD_OPTION_EXTERNAL_LOGGER,
    mhdLog,
    NULL,
    MHD_OPTION_END
  );

  if (mhdDaemon2 == NULL)
  {
    fprintf(stderr, "corRestListen: MHD_start_daemon failed on %s port %d\n", host, port);
    return -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestBackendStop -
//
void corRestBackendStop(void)
{
#if COR_WITH_STREAM
  //
  // The application has ended its streams (corRestStream.h); a connection it resumed is still MHD's to
  // run to its end, and stopping the daemon with one still suspended is an API violation - so the stop
  // waits for every stream response to be destroyed, 2 s at most
  //
  for (int ix = 0; ix < 200; ix++)
  {
    pthread_mutex_lock(&streamsMutex);
    int left = streams;
    pthread_mutex_unlock(&streamsMutex);

    if (left == 0)
      break;

    struct timespec tenMs = { 0, 10 * 1000 * 1000 };
    nanosleep(&tenMs, NULL);
  }
#endif

  if (mhdDaemon2 != NULL)
  {
    MHD_stop_daemon(mhdDaemon2);
    mhdDaemon2 = NULL;
  }

  if (mhdDaemon != NULL)
  {
    MHD_stop_daemon(mhdDaemon);
    mhdDaemon = NULL;
  }
}
