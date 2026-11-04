//
// FILE            corRestInit.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#ifndef CORREST_INIT_H_
#define CORREST_INIT_H_

#include <stdint.h>                     // uint64_t
#include "corAlloc/CorAlloc.h"        // CorAlloc
#include "corRest/CorRestService.h"
#include "corRest/CorRestVerb.h"        // CorRestVerb
#include "corRest/CorRestKeyValue.h"    // CorRestKeyValue



// -----------------------------------------------------------------------------
//
// corRestInit -
//
// Initialize the REST library: prepare service lookup tables, start the HTTP
// server (libmicrohttpd or the built-in one - COR_HTTP_SERVER, decided at
// compile time) and the worker pool that runs requests off its I/O threads.
//
// Parameters:
//   serviceV      - flat array of service definitions (each includes its verb)
//   services      - number of entries in serviceV
//   port          - TCP port to listen on
//   poolSize      - I/O threads, and one dispatch worker each
//
// GET services automatically get HEAD support (same handler, body suppressed).
// OPTIONS is auto-generated for all registered URL paths.
//
// Returns 0 on success, -1 on error.
//
extern int corRestInit(CorRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize);



// -----------------------------------------------------------------------------
//
// corRestPrepare, corRestStart - corRestInit in two steps
//
// corRestPrepare builds the service tables and starts the worker pool; corRestStart starts the HTTP server,
// which accepts connections the moment it is up. Between the two, corRestRunJson runs requests through the
// services and nobody else can - start-up work that clients must not see half done goes there.
//
extern int corRestPrepare(CorRestServiceSimplified serviceV[], int services, int poolSize);
extern int corRestStart(unsigned short port, int poolSize);



// -----------------------------------------------------------------------------
//
// corRestListen - a second listener, with routes of its own
//
// On 'host' (a name or an address - "127.0.0.1" keeps it to the machine) and 'port', plain HTTP, one I/O
// thread; its requests run through the same pipeline and hooks as the first listener's, with
// CorRestState.listener 1, and are matched against 'serviceV' only - a route of corRestInit's is a 404
// there, and one of these a 404 on corRestInit's port. No protocol upgrades. After corRestInit;
// libmicrohttpd only (the built-in server refuses).
//
// Returns 0, or -1 with a reason on stderr.
//
extern int corRestListen(const char* host, unsigned short port, CorRestServiceSimplified serviceV[], int services);


// -----------------------------------------------------------------------------
//
// corRestHttpsServerCredentialsSet - serve HTTPS instead of HTTP
//
// Call before corRestStart with the PEM contents (not file paths) of the private
// key and certificate; the server is then started with TLS enabled. NULL (the
// default) keeps it on plain HTTP. Used by the broker for --https and by TLS
// test receivers (e.g. ftClient).
//
// ⚠️ The BUILT-IN server has no TLS, and corRestInit REFUSES to start rather
// than quietly serving the port in the clear. Build with COR_HTTP_SERVER=mhd
// for an HTTPS listener.
//
extern void corRestHttpsServerCredentialsSet(char* keyPem, char* certPem);



// -----------------------------------------------------------------------------
//
// corRestProcessRequest - run the request-dispatch core on the bound corRest state
//
// Consumes corRest.in, produces corRest.out (incl. the rendered body in
// corRest.out.payload / payloadSize). Connection-free — the send lives in the
// HTTP backend's connection handler.
//
extern void corRestProcessRequest(void);



// -----------------------------------------------------------------------------
//
// corRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// When a forward targets this broker's own endpoint, run it directly on a fresh
// inner CorRestState instead of over a socket. Outputs are borrowed into
// respAllocP (kept alive by the caller). Returns the HTTP status code, or -1 if
// the inner request could not be set up. The caller must save/restore any
// thread-local application state the inner pipeline resets (corNgsild + deferred
// caches).
//
extern int corRestProcessInProcess(CorRestVerb       verb,
                                  const char*      path,
                                  CorRestKeyValue*  headerV,
                                  int              headerCount,
                                  const char*      body,
                                  int              bodyLen,
                                  CorAlloc*        respAllocP,
                                  char**           respBodyP,
                                  int*             respBodyLenP,
                                  CorRestKeyValue** respHeaderVP,
                                  int*             respHeaderCountP);

// -----------------------------------------------------------------------------
//
// corRestDispatchCounts - requests run on their I/O thread (inline) and handed to a worker, since start
//
// Only requests where there was a choice: with the worker pool down everything runs inline and none
// of it is counted.
//
extern void corRestDispatchCounts(uint64_t* inlineP, uint64_t* handedOffP);



#endif  // CORREST_INIT_H_
