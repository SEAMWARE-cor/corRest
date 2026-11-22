//
// FILE            CorRestIn.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#ifndef CORREST_IN_H_
#define CORREST_IN_H_

#include <stdbool.h>

#include "corTree/CorNode.h"
#include "corRest/CorRestVerb.h"
#include "corRest/CorRestKeyValue.h"



// -----------------------------------------------------------------------------
//
// Constants
//
#define COR_REST_MAX_WILDCARDS       3
#define COR_REST_INITIAL_KV_SLOTS   10
#define COR_REST_KV_GROW_SIZE        5



// -----------------------------------------------------------------------------
//
// CorMimeType - an NGSI-LD media type. The single enum for BOTH the request
// Content-Type (classified on reception by corMimeTypeParse) and the Accept
// header (negotiated by corAcceptParse) — so the code compares an enum instead
// of strcasecmp-ing header strings. Numeric values are fixed: Json = 0 (the
// zero-init / absent-Accept default, § 6.3.4), None = -1 ("not acceptable" /
// unsupported). MergePatchJson is Content-Type only.
//
typedef enum CorMimeType
{
  CorMimeNone           = -1,   // absent / not acceptable / unsupported
  CorMimeJson           =  0,   // application/json (default)
  CorMimeLdJson,                 // application/ld+json
  CorMimeGeoJson,                // application/geo+json
  CorMimeMergePatchJson          // application/merge-patch+json
} CorMimeType;

extern CorMimeType  corMimeTypeParse(const char* contentType);  // a Content-Type value
extern CorMimeType  corAcceptParse(const char* acceptHeader);   // a q-weighted Accept header
extern const char* corMimeString(CorMimeType mime);             // enum -> canonical media-type string



// -----------------------------------------------------------------------------
//
// CorRestIn - incoming request data
//
typedef struct CorRestIn
{
  CorRestVerb  verb;
  char*       verbString;
  char*       urlPath;
  int         urlPathLen;
  char*       urlParams;                          // raw query string
  char*       wildcard[COR_REST_MAX_WILDCARDS];    // extracted wildcard values from URL
  int         wildcards;

  // URI query parameters (dynamic array, inline for small counts)
  CorRestKeyValue  uriParams[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue* uriParamV;
  int             uriParamCount;
  int             uriParamSize;
  uint64_t        uriParamMask;   // OR of all received URI params' registry bits ("was X given?")

  // HTTP headers (dynamic array, inline for small counts)
  CorRestKeyValue  httpHeaders[COR_REST_INITIAL_KV_SLOTS];
  CorRestKeyValue* httpHeaderV;
  int             httpHeaderCount;
  int             httpHeaderSize;

  // Pretty-print indentation (0 = compact, from ?pretty=N)
  int         prettySpaces;

  // § 6.3.4 — set when a POST/PATCH/PUT arrives without Content-Length;
  // the dispatcher emits a body-less 411 in that case.
  bool        contentLengthMissing;

  // Payload
  char*       payload;
  int         payloadSize;
  CorNode*    requestTree;                        // parsed JSON payload body

  // Content-Type header value (convenience pointer into httpHeaders), and its
  // media type classified on reception
  char*       contentType;
  CorMimeType  contentMime;
  char*       accept;
} CorRestIn;

#endif  // CORREST_IN_H_
