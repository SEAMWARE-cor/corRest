//
// FILE            corRestBackend.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The seam between corRest and the HTTP server underneath it.
//
// There are two servers - libmicrohttpd and the built-in epoll one in corHttp -
// and exactly one of them is in a build (COR_HTTP_SERVER). Everything on the
// corRest side of this header is the same either way: the service table, the
// dispatch, the worker pool, the response policy. Everything on the other side
// is a socket and a callback shape.
//
// Not a public header: it is the INTERNAL contract between corRestInit.c and
// corRestBackend<Name>.c, and nothing above corRest includes it.
//
#ifndef CORREST_BACKEND_H_
#define CORREST_BACKEND_H_

#include <stdbool.h>                      // bool

#include "corRest/CorRestKeyValue.h"      // CorRestKeyValue
#include "corRest/CorRestState.h"         // CorRestState

#if COR_HTTP_SERVER_MHD && COR_HTTP_SERVER_BUILTIN
#include "corRest/corRestBackendTable.h"  // both servers in the build: the ten through the table selected at start
#endif



// -----------------------------------------------------------------------------
//
// What the backend implements
//
// corRestBackendStart - listen on 'port' and serve, and RETURN.
//
//   Both backends run their own threads; neither blocks the caller, because
//   corRestInit's caller goes on to do other things and then parks. keyPem /
//   certPem are the HTTPS server credentials, NULL for plain HTTP - a backend
//   without TLS refuses rather than silently serving the port unencrypted.
//
//   Returns 0, or -1 with a reason on stderr.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern int   corRestBackendStart(unsigned short port, int poolSize, char* keyPem, char* certPem);
#endif



// -----------------------------------------------------------------------------
//
// corRestBackendListen - a second listener, on 'host' (a name or an address) and 'port', plain HTTP, one
// I/O thread: its requests have CorRestState.listener 1. After corRestBackendStart.
//
//   Returns 0, or -1 with a reason on stderr - the built-in server has one listener.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern int   corRestBackendListen(const char* host, unsigned short port);
#endif



// -----------------------------------------------------------------------------
//
// corRestHttpLoopsSet - how many event loops share the listening port
//
// BUILTIN SERVER ONLY, and a no-op on the libmicrohttpd one, which runs its own
// pool of epoll threads (MHD_OPTION_THREAD_POOL_SIZE) and has no loop of ours to
// multiply. Call it before corRestInit; out-of-range values are clamped (1-64)
// rather than refused. Default 1.
//
// One loop does all the socket I/O for every connection it owns - read, parse
// and write - with the workers running only the service routine in between.
// Measured 2026-09-15 on 8 cores with 319-byte responses, one loop used 4.4 of
// the cores and libmicrohttpd 7.9, at a lower cost per request (70.7 us against
// 89.0). With more loops each has its own epoll, its own connection pool (the
// slots are divided among the loops) and its own resume queue; the first loop
// alone listens, accepts every connection and deals them out in turn
// (corHttpAcceptShare), and a connection stays with its loop for its whole life.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void  corRestHttpLoopsSet(int loops);
#endif



// -----------------------------------------------------------------------------
//
// corRestConnectionMemorySet - libmicrohttpd's memory per connection, in kB (0: its own default, 32 kB)
//
// LIBMICROHTTPD ONLY, a no-op on the built-in server. Call it before corRestInit. A request whose line and
// headers do not fit it is refused by libmicrohttpd itself - 414 (the URL) or 431 (the headers) - and said
// in the log once per refusal, with what to do about it.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void  corRestConnectionMemorySet(int kb);
#endif



// -----------------------------------------------------------------------------
//
// corRestBackendLoops - how many event loops corRestBackendStart will run
//
// Answered BEFORE the start: corRestInit sizes the worker pool off it and starts the pool first,
// so that no connection is accepted while the pool is down. corRestHttpLoopsSet's value on the
// built-in server, 1 on libmicrohttpd.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern int   corRestBackendLoops(void);
#endif


//
// corRestBackendStop - stop serving; called after the worker pool has drained.
//
// Start and stop nest: corRestInit starts the worker pool and THEN the backend, corRestStop stops
// the pool (drained) and THEN the backend - so the pool is up for as long as a connection can be
// accepted, except in the shutdown window between the two stops.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void  corRestBackendStop(void);
#endif


//
// corRestBackendFinish - the request is over; run its post-response work and
// release it
//
// The backend's, because only it knows what the request state BORROWED and what
// therefore has to be given back. Binds corRestP to stateP, runs the
// post-response hook, releases the arena, frees the state, and unbinds.
//
// Built-in server: on a worker whenever there is a pool (corRestAsyncFinish),
// on the calling loop otherwise. libmicrohttpd: on its I/O thread, from the
// request-completed callback (MHD_OPTION_NOTIFY_COMPLETED).
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void  corRestBackendFinish(CorRestState* stateP);
#endif


//
// corRestBackendResume - a worker has built the response; send it
//
// Called ON THE WORKER THREAD with corRestP still bound to 'stateP', which is
// what lets a backend read corRest.out here. It must not write to the socket
// itself - that belongs to whichever thread owns the connection - and both
// backends therefore only hand the connection back to their event loop.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void  corRestBackendResume(CorRestState* stateP);
#endif



// -----------------------------------------------------------------------------
//
// What corRestInit.c provides to the backend
//
// corRestHttpHeaderAdd  - one request header; key/value are BORROWED, so they
//                         must live as long as the request does.
// corRestUriParamsParse - split + percent-decode corRest.in.urlParams into
//                         corRest.in.uriParamV. Destroys the buffer it parses.
// corRestUriParamAdd    - one already-split, already-decoded parameter.
//
extern void  corRestHttpHeaderAdd(const char* key, const char* value);
extern void  corRestUriParamsParse(void);
extern void  corRestUriParamAdd(char* name, char* value);


//
// corRestBodyPolicyCheck - the two answers that are decided before the body
//
// § 6.3.4: POST/PATCH/PUT to an NGSI-LD route must carry Content-Length (411,
// no payload). § 6.3.2: an announced body over the broker's cap is a 413.
// Both are known from the request line and the headers alone, which is why
// they are here and not in the dispatch: a backend that streams the body wants
// to stop reading it, and one that has it already wants to not parse it.
//
// 'clHeader' is the Content-Length header value, NULL when absent.
//
extern void  corRestBodyPolicyCheck(const char* url, const char* clHeader);


//
// corRestResponseHeaderVBuild - the response headers, in the order they go out
//
// ONE implementation of the header policy - content type, Preference-Applied,
// the service routine's own headers, CORS - for both backends, because the set
// and the ORDER of these headers is compared line by line by several hundred
// functional tests. Two copies of it would be two chances to drift.
//
// Fills 'hv' and returns how many. Keys and values are borrowed from the
// request arena and from static configuration, so they last as long as the
// request state does.
//
extern int   corRestResponseHeaderVBuild(CorRestKeyValue* hv, int max);

#define COR_REST_RESPONSE_HEADERS_MAX  64


//
// corRestAsyncPoolUp / corRestAsyncEnqueue - hand the request to a worker
//
// Two calls and not one, because the connection has to be SUSPENDED BETWEEN
// THEM: a worker can pick the request up and finish it before the enqueue has
// returned, and resuming a connection that was never suspended is an API
// violation in one backend and a lost response in the other.
//
//   if (corRestAsyncPoolUp())  { suspend(); corRestAsyncEnqueue(stateP); return; }
//   corRestProcessRequest();   // pool down (shutdown, or a server without one)
//
extern bool  corRestAsyncPoolUp(void);
extern void  corRestAsyncEnqueue(CorRestState* stateP);


//
// corRestAsyncDispatch - the question the backends ask instead of corRestAsyncPoolUp alone:
// hand this request off (true), or process it here (false). The pool must be up AND the app's
// inline hook (CorRestInlineHook) must not claim the request.
//
//   if (corRestAsyncDispatch())  { suspend(); corRestAsyncEnqueue(stateP); return; }
//   corRestProcessRequest();
//
extern bool  corRestAsyncDispatch(void);


//
// corRestAsyncFinish - hand the POST-RESPONSE phase to a worker
//
// Returns true when a worker has taken it - the caller must then not touch
// stateP again. False means there is no pool and the caller runs
// corRestBackendFinish itself.
//
// The response has already gone out, so this is not about latency: it is about
// the thread. The phase can issue an HTTP request the broker itself has to
// answer (a notification compacted with an @context the broker hosts), and a
// single-threaded event loop running it would be waiting on itself.
//
extern bool  corRestAsyncFinish(CorRestState* stateP);


//
// corRestWorkerPoolStart / Stop - one worker per I/O thread
//
extern int   corRestWorkerPoolStart(int workers);



// -----------------------------------------------------------------------------
//
// corRestWorkerShardsSet - how many work queues the pool runs
//
// Call BEFORE corRestWorkerPoolStart - corRestInit does, with corRestBackendLoops,
// before it starts the pool and then the backend. One queue per event loop keeps
// a request on one loop's threads end to end; the default of 1 is what
// libmicrohttpd wants and leaves that backend unchanged.
//
// Clamped down to the worker count: more queues than workers would leave one
// with nobody serving it, and a request landing there would wait for ever.
//
extern void  corRestWorkerShardsSet(int shards);
extern void  corRestWorkerPoolStop(void);


//
// corRestProcessRequest - the dispatch itself (public; corRestInit.h)
//
extern void  corRestProcessRequest(void);

#endif  // CORREST_BACKEND_H_
