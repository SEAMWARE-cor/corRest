//
// FILE            corRestStream.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A response whose body is written over time, after the service routine has returned - server-sent
// events (text/event-stream) are this. Both HTTP servers: the built-in one streams through corHttp's
// stream (corHttpResponseStream), libmicrohttpd through a content reader, the connection suspended
// while there is nothing to send. Built with COR_WITH_STREAM=1 (the default); 0 leaves the code out,
// and corRestStreamResponse says false.
//
#ifndef CORREST_STREAM_H_
#define CORREST_STREAM_H_

#if COR_HTTP_SERVER_MHD && COR_HTTP_SERVER_BUILTIN
#include "corRest/corRestBackendTable.h"  // both servers in the build: the ten through the table selected at start
#endif

#include <stdbool.h>                      // bool



// -----------------------------------------------------------------------------
//
// CorRestStream - one streamed response; the application's from its open until corRestStreamEnd
//
typedef struct CorRestStream CorRestStream;



// -----------------------------------------------------------------------------
//
// CorRestStreamOpen - the stream is there: called once, with the handle, when the status and the headers
// are on their way - on the thread that built the response (the server's, or a worker's), so it must
// not wait. The application keeps the handle and
// writes on it from any thread. NULL: no stream could be made (out of memory) - the response ends with
// the headers, and there is nothing to end.
//
typedef void (*CorRestStreamOpen)(CorRestStream* streamP, void* ctx);



// -----------------------------------------------------------------------------
//
// COR_REST_STREAM_PENDING_MAX - what a client may fall behind: a stream with more than this written and
// not taken by the client is cut, and its writes say false
//
#define COR_REST_STREAM_PENDING_MAX  (16 * 1024 * 1024)



// -----------------------------------------------------------------------------
//
// corRestStreamPossible - may this request's response be a stream? Only a request of the HTTP server's own:
// an in-process one and a WebSocket's (corRestRunJson) have no connection, a cor:// one finishes through
// its own transport - and a build without streams has none. For a routine that must know before it does
// anything (a subscription made on its stream)
//
extern bool corRestStreamPossible(void);



// -----------------------------------------------------------------------------
//
// corRestStreamResponse - this request's response is a stream
//
// Called by the service routine, which sets the status and the headers as for any response (the
// Content-Type too: a response without a body gets none from corRest) and no body. The headers go out
// with `Transfer-Encoding: chunked` (HTTP/1.1), and openFn is called with the stream.
//
// false: this request cannot have one (corRestStreamPossible). Nothing is set then; the routine answers
// otherwise.
//
extern bool corRestStreamResponse(CorRestStreamOpen openFn, void* ctx);



// -----------------------------------------------------------------------------
//
// corRestStreamWrite - 'len' bytes, copied and queued; from any thread, never waits for the socket
//
// false: the client is gone, or fell COR_REST_STREAM_PENDING_MAX behind, or the server is stopping -
// nothing more will reach it. The handle stays the application's until corRestStreamEnd.
//
// len 0 writes nothing and says whether the client is still there - a client that closed its connection
// while there was nothing to send is seen by it; an application that must know soon calls it now and then.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern bool corRestStreamWrite(CorRestStream* streamP, const char* data, int len);
#endif



// -----------------------------------------------------------------------------
//
// corRestStreamEnd - the end of the body (what is queued goes out first), and the handle is released
//
// From any thread, exactly once per stream - also after a write said false. Not used after this. Every
// stream is ended before corRestStop: the server a stream is written through is gone after it.
//
#ifndef COR_REST_BACKEND_VIA_TABLE
extern void corRestStreamEnd(CorRestStream* streamP);
#endif

#endif  // CORREST_STREAM_H_
