#ifndef CORREST_CORRESTFETCH_H_
#define CORREST_CORRESTFETCH_H_

//
// FILE            corRestFetch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corAlloc/CorAlloc.h"                       // CorAlloc



// -----------------------------------------------------------------------------
//
// corRestFetch - the text behind a URL (http://, https://) or in a file (a path, or file://), in kaP.
// NULL: not there - errorBuf says why. NEVER on a request's path: it blocks.
//
extern char* corRestFetch(const char* where, CorAlloc* kaP, char* errorBuf, int errorBufSize);

#endif  // CORREST_CORRESTFETCH_H_
