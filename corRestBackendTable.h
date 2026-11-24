//
// FILE            corRestBackendTable.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// COR_HTTP_SERVER=both - both HTTP servers in one build, one of them chosen at start (corRestBackendSelect).
// Included by corRestBackend.h and corRestStream.h before their prototypes, and only in a build with both;
// a build with one server calls its functions directly, through nothing.
//
// The ten functions of the seam (corRestBackend.h, corRestStream.h) have two names in such a build:
//
// - In a backend file - corRestBackendMhd.c, corRestBackendBuiltin.c, each of which defines
//   COR_REST_BACKEND_SUFFIX before its includes - the plain name is that file's own function, named with the
//   suffix: corRestBackendStart is corRestBackendStartMhd in one and corRestBackendStartBuiltin in the other.
//   Each fills its CorRestBackend table (corRestBackendTableMhd, corRestBackendTableBuiltin) with them.
//
// - Everywhere else the plain name is a call through corRestBackendP, the table selected at start, and the
//   prototypes of the plain names are left out (COR_REST_BACKEND_VIA_TABLE).
//
// So no caller changes, in corRest or above it, and the backends define their functions under the names they
// always had.
//
#ifndef CORREST_BACKEND_TABLE_H_
#define CORREST_BACKEND_TABLE_H_

#include <stdbool.h>                      // bool

struct CorRestState;
struct CorRestStream;



// -----------------------------------------------------------------------------
//
// CorRestBackend - one HTTP server's functions
//
typedef struct CorRestBackend
{
  const char* name;                                                        // "mhd" | "builtin"
  int   (*start)(unsigned short port, int poolSize, char* keyPem, char* certPem);
  int   (*listen)(const char* host, unsigned short port);
  void  (*stop)(void);
  void  (*finish)(struct CorRestState* stateP);
  void  (*resume)(struct CorRestState* stateP);
  int   (*loops)(void);
  void  (*httpLoopsSet)(int loops);
  void  (*connectionMemorySet)(int kb);
  bool  (*streamWrite)(struct CorRestStream* streamP, const char* data, int len);   // NULL: COR_WITH_STREAM=0
  void  (*streamEnd)(struct CorRestStream* streamP);                                // NULL: COR_WITH_STREAM=0
} CorRestBackend;



#define COR_REST_BACKEND_CAT2(a, b)  a##b
#define COR_REST_BACKEND_CAT(a, b)   COR_REST_BACKEND_CAT2(a, b)

#ifdef COR_REST_BACKEND_SUFFIX
//
// A backend file: its own functions, under its suffix
//
#define corRestBackendStart         COR_REST_BACKEND_CAT(corRestBackendStart,        COR_REST_BACKEND_SUFFIX)
#define corRestBackendListen        COR_REST_BACKEND_CAT(corRestBackendListen,       COR_REST_BACKEND_SUFFIX)
#define corRestBackendStop          COR_REST_BACKEND_CAT(corRestBackendStop,         COR_REST_BACKEND_SUFFIX)
#define corRestBackendFinish        COR_REST_BACKEND_CAT(corRestBackendFinish,       COR_REST_BACKEND_SUFFIX)
#define corRestBackendResume        COR_REST_BACKEND_CAT(corRestBackendResume,       COR_REST_BACKEND_SUFFIX)
#define corRestBackendLoops         COR_REST_BACKEND_CAT(corRestBackendLoops,        COR_REST_BACKEND_SUFFIX)
#define corRestHttpLoopsSet         COR_REST_BACKEND_CAT(corRestHttpLoopsSet,        COR_REST_BACKEND_SUFFIX)
#define corRestConnectionMemorySet  COR_REST_BACKEND_CAT(corRestConnectionMemorySet, COR_REST_BACKEND_SUFFIX)
#define corRestStreamWrite          COR_REST_BACKEND_CAT(corRestStreamWrite,         COR_REST_BACKEND_SUFFIX)
#define corRestStreamEnd            COR_REST_BACKEND_CAT(corRestStreamEnd,           COR_REST_BACKEND_SUFFIX)
#define corRestBackendTable         COR_REST_BACKEND_CAT(corRestBackendTable,        COR_REST_BACKEND_SUFFIX)

extern const CorRestBackend corRestBackendTable;

#else
//
// Everyone else: the table selected at start
//
#define COR_REST_BACKEND_VIA_TABLE  1

extern const CorRestBackend  corRestBackendTableMhd;
extern const CorRestBackend  corRestBackendTableBuiltin;
extern const CorRestBackend* corRestBackendP;                // corRestBackendTableMhd until corRestBackendSelect

#define corRestBackendStart         (corRestBackendP->start)
#define corRestBackendListen        (corRestBackendP->listen)
#define corRestBackendStop          (corRestBackendP->stop)
#define corRestBackendFinish        (corRestBackendP->finish)
#define corRestBackendResume        (corRestBackendP->resume)
#define corRestBackendLoops         (corRestBackendP->loops)
#define corRestHttpLoopsSet         (corRestBackendP->httpLoopsSet)
#define corRestConnectionMemorySet  (corRestBackendP->connectionMemorySet)
#define corRestStreamWrite          (corRestBackendP->streamWrite)
#define corRestStreamEnd            (corRestBackendP->streamEnd)
#endif



// -----------------------------------------------------------------------------
//
// corRestBackendSelect - the HTTP server this process runs: "mhd" or "builtin"
//
// Once, at start: before corRestHttpLoopsSet, corRestConnectionMemorySet and corRestInit - every one of the
// ten goes to the server selected when it is called. Without a call the server is libmicrohttpd.
// Returns false, selecting nothing, for any other name.
//
extern bool corRestBackendSelect(const char* name);

#endif  // CORREST_BACKEND_TABLE_H_
