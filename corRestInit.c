//
// FILE            corRestInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdlib.h>                     // calloc, realloc, free, atoi
#include <string.h>                     // strncpy, strstr, strcmp, memcpy, strcasecmp, strlen
#include <stdio.h>                      // fprintf, snprintf
#include <time.h>                       // clock_gettime, CLOCK_MONOTONIC
#include <pthread.h>                    // pthread_* (async worker pool)
#include <stdatomic.h>                  // _Atomic, atomic_fetch_add_explicit, atomic_load_explicit

#include "corAlloc/corAlloc.h"          // corAlloc
#include "corAlloc/corAllocStrdup.h"    // corAllocStrdup
#include "corAlloc/CorAlloc.h"          // CorAlloc
#include "corJson/CorJsonStatus.h"      // CorJsonMaxDepthExceeded
#include "corJson/corJsonParse.h"       // corJsonParse
#include "corJson/corJsonRender.h"      // corJsonFastRender
#include "corJson/corJsonRenderSize.h"  // corJsonFastRenderSize
#include "corTree/corTreeBuilder.h"     // corTreeObject, corTreeString, corTreeChildAdd
#include "corLog/corLog.h"              // COR_V

#include "corRest/CorRestVerb.h"          // CorVerbs, corRestVerbToString
#include "corRest/CorRestService.h"       // CorRestService, CorRestServiceVector
#include "corRest/CorRestState.h"         // CorRestState, corRest
#include "corRest/corRestStateInit.h"     // corRestStateInit, corRestStateRelease
#include "corRest/corRestServiceLookup.h" // corRestServiceLookup
#include "corRest/corRestHooks.h"         // CorRestHook, etc.
#include "corRest/corRestProblem.h"       // COR_REST_ERROR_*, corRestProblem
#include "corRest/corRestParamRegistry.h" // corRestParamLookup
#include "corRest/corRestBackend.h"       // corRestBackendStart, corRestBackendLoops, corRestBackendResume
#include "corRest/corRestInit.h"          // Own interface
#include "corRest/corRestUrlValueEncode.h"          // corRestUrlValueDecode



// -----------------------------------------------------------------------------
//
// Globals
//
CorRestServiceVector   corRestServiceV[CorVerbs];
CorRestServiceVector   corRestListenerServiceV[CorVerbs];   // corRestListen's routes



// -----------------------------------------------------------------------------
//
// routesOf - the routes of the listener the request came in on
//
static inline CorRestServiceVector* routesOf(void)
{
  return (corRest.listener == 0) ? corRestServiceV : corRestListenerServiceV;
}



// -----------------------------------------------------------------------------
//
// HTTPS server credentials (PEM contents, NULL == plain HTTP)
//
// Set by the broker for --https (--key, --cert), and by a test server such as
// ftClient that must receive notifications over TLS: each calls
// corRestHttpsServerCredentialsSet() with the key+certificate PEM before
// corRestStart; the backend is then started with TLS enabled - and a backend
// that has no TLS refuses to start rather than serving the port in the clear.
//
static char* httpsServerKey  = NULL;
static char* httpsServerCert = NULL;

void corRestHttpsServerCredentialsSet(char* keyPem, char* certPem)
{
  httpsServerKey  = keyPem;
  httpsServerCert = certPem;
}



// -----------------------------------------------------------------------------
//
// Hook globals (defined in corRestHooks.c)
//
extern CorRestHook            corRestPreDispatchHook;
extern CorRestHook            corRestPrePayloadParseHook;
extern CorRestHook            corRestPayloadParseHook;
extern CorRestHook            corRestPayloadRenderHook;
extern CorRestParamHook       corRestParamHookF;
extern CorRestPreServiceHook  corRestPreServiceHookF;
extern CorRestPreServiceHook  corRestAdmitHookF;
extern CorRestServiceInitHook corRestServiceInitHookF;
extern CorRestHook            corRestPostResponseHook;
extern CorRestInlineHook      corRestInlineHookF;
extern CorRestUserDataAllocHook corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook  corRestUserDataFreeHookF;
extern unsigned long long    corRestMaxRequestSize;
extern CorRestCorsConfig     corRestCors;



// -----------------------------------------------------------------------------
//
// servicePrepare - expand a simplified service descriptor into a full one
//
static void servicePrepare(CorRestService* serviceP, CorRestServiceSimplified* simpleP)
{
  serviceP->url              = (char*) simpleP->url;
  serviceP->serviceRoutine   = simpleP->serviceRoutine;
  serviceP->payloadCheck     = simpleP->payloadCheck;
  serviceP->supportedParams  = simpleP->supportedParams;
  serviceP->ldOp             = simpleP->ldOp;
  serviceP->options          = simpleP->options;   // author-declared features; init hook adds wildcard bits below
  serviceP->wildcards        = 0;
  serviceP->greedy           = false;

  serviceP->charsBeforeFirstWildcard      = 0;
  serviceP->charsBeforeFirstWildcardSum   = 0;
  serviceP->charsBeforeSecondWildcard     = 0;
  serviceP->charsBeforeSecondWildcardSum  = 0;
  serviceP->matchForSecondWildcardLen     = 0;
  serviceP->matchForSecondWildcard[0]     = 0;
  serviceP->matchForThirdWildcardLen      = 0;
  serviceP->matchForThirdWildcard[0]      = 0;
  serviceP->matchAfterLastWildcardLen     = 0;
  serviceP->matchAfterLastWildcard[0]     = 0;

  char*  wildCardStart    = NULL;  // start of literal between *#1 and *#2
  char*  wildCardEnd      = NULL;  // end of literal between *#1 and *#2
  char*  thirdWildStart   = NULL;  // start of literal between *#2 and *#3
  char*  thirdWildEnd     = NULL;  // end of literal between *#2 and *#3
  char*  afterLastWild    = NULL;  // start of literal AFTER the last '*' (suffix, e.g. "/value")
  int    ix               = 0;

  while (serviceP->url[ix] != 0)
  {
    char c = serviceP->url[ix];

    if (c == '*')
    {
      // Check for ** (greedy wildcard) - skip the second '*'
      if (serviceP->url[ix + 1] == '*')
      {
        serviceP->greedy = true;
        ++ix;  // skip second '*'
      }

      if (serviceP->wildcards == 0)
        wildCardStart = &serviceP->url[ix + 1];
      else if (serviceP->wildcards == 1)
      {
        wildCardEnd    = &serviceP->url[ix];
        thirdWildStart = &serviceP->url[ix + 1];
      }
      else if (serviceP->wildcards == 2)
        thirdWildEnd = &serviceP->url[ix];

      afterLastWild = &serviceP->url[ix + 1];  // text after THIS '*' — ends up pointing past the final wildcard

      serviceP->wildcards += 1;
      ++ix;
      continue;
    }

    if (serviceP->wildcards == 0)
    {
      ++serviceP->charsBeforeFirstWildcard;
      serviceP->charsBeforeFirstWildcardSum += c;
    }
    else if (serviceP->wildcards == 1)
    {
      ++serviceP->charsBeforeSecondWildcard;
      serviceP->charsBeforeSecondWildcardSum += c;
    }

    ++ix;
  }

  if (serviceP->wildcards != 0)
  {
    if (wildCardEnd == NULL)
      wildCardEnd = &serviceP->url[ix];

    serviceP->matchForSecondWildcardLen = wildCardEnd - wildCardStart;

    if (serviceP->matchForSecondWildcardLen > 0 &&
        serviceP->matchForSecondWildcardLen < (int) sizeof(serviceP->matchForSecondWildcard))
    {
      strncpy(serviceP->matchForSecondWildcard, wildCardStart, serviceP->matchForSecondWildcardLen);
      serviceP->matchForSecondWildcard[serviceP->matchForSecondWildcardLen] = 0;
    }

    if (serviceP->wildcards >= 3 && thirdWildStart != NULL)
    {
      if (thirdWildEnd == NULL)
        thirdWildEnd = &serviceP->url[ix];

      serviceP->matchForThirdWildcardLen = thirdWildEnd - thirdWildStart;

      if (serviceP->matchForThirdWildcardLen > 0 &&
          serviceP->matchForThirdWildcardLen < (int) sizeof(serviceP->matchForThirdWildcard))
      {
        strncpy(serviceP->matchForThirdWildcard, thirdWildStart, serviceP->matchForThirdWildcardLen);
        serviceP->matchForThirdWildcard[serviceP->matchForThirdWildcardLen] = 0;
      }
    }

    // Literal suffix AFTER the last '*' (e.g. ".../attrs/*/value"). Only for
    // 2+ wildcards — for a single wildcard the after-* literal is already held
    // in matchForSecondWildcard (the "wildcard not at end" case). 0-length =>
    // the last wildcard runs to end-of-string (the common case), a no-op.
    if (serviceP->wildcards >= 2 && afterLastWild != NULL)
    {
      int suffixLen = (int) (&serviceP->url[ix] - afterLastWild);
      if (suffixLen > 0 && suffixLen < (int) sizeof(serviceP->matchAfterLastWildcard))
      {
        strncpy(serviceP->matchAfterLastWildcard, afterLastWild, suffixLen);
        serviceP->matchAfterLastWildcard[suffixLen] = 0;
        serviceP->matchAfterLastWildcardLen = suffixLen;
      }
    }
  }

  // Let the embedding library (corNgsild) inspect the URL pattern and set
  // any per-route options it cares about. Called once per service at init.
  if (corRestServiceInitHookF != NULL)
    corRestServiceInitHookF(serviceP);
}



// -----------------------------------------------------------------------------
//
// corRestUriParamAdd - add URI parameter to dynamic array, growing if needed
//
void corRestUriParamAdd(char* name, char* value)
{
  if (corRest.in.uriParamCount >= corRest.in.uriParamSize)
  {
    int newSize = corRest.in.uriParamSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV = (CorRestKeyValue*) corAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, corRest.in.uriParamV, corRest.in.uriParamCount * sizeof(CorRestKeyValue));

    corRest.in.uriParamV    = newV;
    corRest.in.uriParamSize = newSize;
  }

  corRest.in.uriParamV[corRest.in.uriParamCount].key   = name;
  corRest.in.uriParamV[corRest.in.uriParamCount].value  = value;
  corRest.in.uriParamV[corRest.in.uriParamCount].bit    = 0;   // resolved in the allowlist pass
  corRest.in.uriParamCount++;
}



// -----------------------------------------------------------------------------
//
// corRestUriParamsParse - split the query string, percent-decoding as it goes
//
// IN PLACE, over corRest.in.urlParams: the buffer comes out with a NUL where
// every '&' and the first '=' of each parameter was, so the raw query string
// does not survive this. Nothing reads it afterwards (purgeEntities rebuilds
// the query from the parsed array for exactly that reason), and the alternative
// - a second copy on every request that carries a query - buys nothing.
//
void corRestUriParamsParse(void)
{
  if (corRest.in.urlParams == NULL)
    return;

  char* p = corRest.in.urlParams;

  while (*p != '\0')
  {
    char* name = p;

    while (*p != '\0' && *p != '=' && *p != '&')
      p++;

    char* value = "";

    if (*p == '=')
    {
      *p = '\0';
      p++;
      value = p;

      while (*p != '\0' && *p != '&')
        p++;

      if (*p == '&')
      {
        *p = '\0';
        p++;
      }
    }
    else if (*p == '&')
    {
      *p = '\0';
      p++;
    }

    // Percent-decode name and value
    corRestUrlValueDecode(name);
    if (value[0] != '\0')
      corRestUrlValueDecode(value);

    corRestUriParamAdd(name, value);
  }
}



// -----------------------------------------------------------------------------
//
// corRestHttpHeaderAdd - append a request header to corRest.in, extracting well-knowns
//
// Shared by the HTTP backends and the in-process self-forward path. The
// key/value pointers are borrowed (not copied) — the caller keeps them alive
// for the request's lifetime.
//
void corRestHttpHeaderAdd(const char* key, const char* value)
{
  if (corRest.in.httpHeaderCount >= corRest.in.httpHeaderSize)
  {
    int newSize = corRest.in.httpHeaderSize + COR_REST_KV_GROW_SIZE;
    CorRestKeyValue* newV = (CorRestKeyValue*) corAlloc(&corRest.kalloc, newSize * sizeof(CorRestKeyValue));

    if (newV == NULL)
      return;

    memcpy(newV, corRest.in.httpHeaderV, corRest.in.httpHeaderCount * sizeof(CorRestKeyValue));

    corRest.in.httpHeaderV    = newV;
    corRest.in.httpHeaderSize = newSize;
  }

  corRest.in.httpHeaderV[corRest.in.httpHeaderCount].key   = (char*) key;
  corRest.in.httpHeaderV[corRest.in.httpHeaderCount].value  = (char*) value;
  corRest.in.httpHeaderCount++;

  // Extract well-known headers
  if (strcasecmp(key, "Content-Type") == 0)
  {
    corRest.in.contentType = (char*) value;
    corRest.in.contentMime = corMimeTypeParse(value);
  }
  else if (strcasecmp(key, "Accept") == 0)
    corRest.in.accept = (char*) value;
}



// -----------------------------------------------------------------------------
//
// keyHookReplay - the key hook over a tree that was never parsed: members in document order,
// at corJson's depths (the top-level container's members are at depth 1, each nested container
// one more), each before its own value
//
static bool keyHookReplay(CorNode* containerP, int depth)
{
  for (CorNode* nodeP = containerP->value.head; nodeP != NULL; nodeP = nodeP->next)
  {
    if ((containerP->type == CorObject) && (corRest.corJsonP->keyF(corRest.corJsonP, containerP, nodeP, depth) == false))
      return false;

    if (((nodeP->type == CorObject) || (nodeP->type == CorArray)) && (keyHookReplay(nodeP, depth + 1) == false))
      return false;
  }

  return true;
}


// -----------------------------------------------------------------------------
//
// corRestProcessRequest - run the request-dispatch core on the bound corRest state
//
// Connection-free: consumes corRest.in (verb, url, headers, params, requestTree)
// and produces corRest.out (status, headers, problem / responseTree) plus the
// final rendered body in corRest.out.payload / payloadSize. The send stays in
// the backend's connection handler. Reusable for an in-process self-forward (a distop
// whose endpoint is this broker): bind corRestP to an inner state, populate its
// .in, call this, read its .out — no socket round-trip.
//
void corRestProcessRequest(void)
{
  // Pre-dispatch hook: reset per-request application state (e.g. corNgsild)
  // HERE, at the start of the atomic dispatch — not at first-byte. With the
  // epoll pool another connection's request may run on this thread between
  // our first and final callbacks; resetting at dispatch keeps that state's
  // reset/populate/read all inside this atomic dispatch, so no stale values
  // leak in. (The whole final call runs without yielding to the pool.)
  corRestPreDispatchHook();

  // § 6.3.4 — POST/PATCH/PUT without Content-Length: emit 411 with no
  // body and skip everything else.
  if (corRest.in.contentLengthMissing)
  {
    corRest.out.httpStatusCode = 411;
    goto respond;
  }

  // 413 already set by first-call or middle-call: short-circuit to render.
  if (corRest.out.problemType != NULL && corRest.out.httpStatusCode == 413)
    goto respond;

  // An upgrade the application refused (CorRestUpgradeHook): its error, and nothing else.
  if (corRest.upgradeRefused == true)
    goto respond;

  // A method outside NGSI-LD's seven verbs (corRestVerbFromString → CorVerbs)
  // splits two ways (§ 6.2.1 + RFC 9110 § 9.3):
  //   - a VALID HTTP method the broker does not use (TRACE, CONNECT) is
  //     "recognised but not allowed on this path" → leave serviceP NULL and
  //     fall through to the 405 + Allow handling below;
  //   - anything else is not an HTTP method at all → malformed → 400.
  // Either way, guard before the verb-indexed lookup, which would otherwise
  // read corRestServiceV[] out of bounds at index CorVerbs.
  if (corRest.in.verb == CorVerbs)
  {
    const char* m = corRest.in.verbString;
    if (m == NULL || (strcmp(m, "TRACE") != 0 && strcmp(m, "CONNECT") != 0))
    {
      corRestProblem(400, COR_REST_ERROR_BAD_REQUEST, "Invalid HTTP Method",
                    "unrecognized HTTP method '%s'", m ? m : "?");
      goto respond;
    }
    corRest.serviceP = NULL;   // TRACE/CONNECT — no NGSI-LD service → 405 below
  }
  else
    // Lookup service early — the parse hook needs to know the route so it can
    // tailor validation (e.g. batch ops surface per-element @context errors as
    // 207 entries instead of a global 400). serviceP may stay NULL here; the
    // 404/405 handling below still runs in that case.
    corRest.serviceP = corRestServiceLookup(&routesOf()[corRest.in.verb]);

  //
  // Admission (corRestSetAdmitHook) - before anything is done for the request: a client that is not
  // let in learns nothing from a 404/405/415 and costs no parse
  //
  if ((corRestAdmitHookF != NULL) && (corRest.selfForwardDepth == 0) && (corRestAdmitHookF() == false))
    goto respond;

  // § 6.3.4 — Unsupported Media Type. POST / PATCH / PUT on a body-bearing
  // NGSI-LD endpoint with a Content-Type that isn't application/json or
  // application/ld+json (with or without a charset parameter): 415, body
  // empty (spec wording: "shall result in just a 415 HTTP status code
  // (without any payload body)"). Run before the JSON parse so we don't
  // 400 InvalidRequest on a body that was never meant to be JSON. A body that arrived as a
  // tree (cor://) came with its Content-Type too, and is held to the same rule.
  if ((corRest.in.payloadSize > 0 || corRest.in.requestTree != NULL) && corRest.in.contentType != NULL &&
      (corRest.in.verb == CorVerbPost  ||
       corRest.in.verb == CorVerbPut   ||
       corRest.in.verb == CorVerbPatch))
  {
    CorMimeType mime = corRest.in.contentMime;
    bool ok = (mime == CorMimeJson || mime == CorMimeLdJson);

    // PATCH also accepts application/merge-patch+json (RFC 7396 / NGSI-LD
    // § 5.6.18 Merge / § 5.6.5 Partial Update). Other media types remain 415.
    if (!ok && corRest.in.verb == CorVerbPatch && mime == CorMimeMergePatchJson)
      ok = true;

    // A notification receiver (ftClient) accepts geo+json notifications.
    if (!ok && corRestAcceptGeoJsonInput && mime == CorMimeGeoJson)
      ok = true;

    if (!ok)
    {
      corRest.out.httpStatusCode = 415;
      goto respond;
    }
  }

  // Parse incoming JSON payload (if any)
  if (corRest.in.payloadSize > 0)
  {
    corRestPrePayloadParseHook();
    corRest.in.requestTree = corJsonParse(corRest.corJsonP, corRest.in.payload);

    //
    // Whatever the pre-parse hook configured was for THIS body only: the same CorJson
    // parses every other text of the connection (payloads a bridge hands over, ...)
    //
    corRest.corJsonP->keyF     = NULL;
    corRest.corJsonP->keyDataP = NULL;

    if (corRest.in.requestTree != NULL)
      corRestPayloadParseHook();
  }
  else if (corRest.in.requestTree != NULL)
  {
    //
    // A body that arrived as a tree (cor://) - never text, never parsed. It gets what a parsed body
    // gets: the pre-parse hook, the key hook it sets - replayed over the tree in the order and at
    // the depths corJson would have called it - and the parse hook.
    //
    corRestPrePayloadParseHook();

    bool keysOk = true;

    if ((corRest.corJsonP->keyF != NULL) && ((corRest.in.requestTree->type == CorObject) || (corRest.in.requestTree->type == CorArray)))
      keysOk = keyHookReplay(corRest.in.requestTree, 1);

    corRest.corJsonP->keyF     = NULL;
    corRest.corJsonP->keyDataP = NULL;

    if (keysOk == true)
      corRestPayloadParseHook();
    else
      corRest.in.requestTree = NULL;
  }

  // Extract ?pretty=N (remove from param list so it doesn't hit validation)
  for (int i = 0; i < corRest.in.uriParamCount; i++)
  {
    if (strcmp(corRest.in.uriParamV[i].key, "pretty") == 0)
    {
      corRest.in.prettySpaces = atoi(corRest.in.uriParamV[i].value);

      // Remove from param list
      corRest.in.uriParamCount--;
      if (i < corRest.in.uriParamCount)
        corRest.in.uriParamV[i] = corRest.in.uriParamV[corRest.in.uriParamCount];
      i--;
    }
  }

  if (corRest.serviceP == NULL)
  {
    // Empty path segment (`//` somewhere in the path) almost always
    // means the client URL-built the path with an empty variable in a
    // wildcard slot — `/entities//attrs` is "POST attrs on entity id
    // <empty>". Surface this as 400 BadRequestData rather than 404
    // ResourceNotFound: there's no route the path could have matched,
    // but the request is malformed, not pointing at the wrong place.
    if (corRest.in.urlPath != NULL && strstr(corRest.in.urlPath, "//") != NULL)
    {
      corRestProblem(400, COR_REST_ERROR_BAD_REQUEST, "Invalid URL Path",
                    "empty path segment in '%s' (likely a missing URL "
                    "variable)", corRest.in.urlPath);
      goto respond;
    }

    // § 6.3.2: 404 ResourceNotFound when the path matches NO route, but
    // 405 MethodNotAllowed when the path is registered for some other
    // verb. The Allow: header lists the verbs that ARE allowed on this
    // path (RFC 7231 § 7.4.1).
    //
    // The Allow-probe below calls corRestServiceLookup for the OTHER verbs;
    // a matching wildcard route NUL-terminates urlPath in place to delimit
    // its wildcard value. Capture the full path first so the 404/405 detail
    // reports the resource the client actually requested, not a truncation.
    const char* reqPath = (corRest.in.urlPath != NULL) ? corAllocStrdup(&corRest.kalloc, corRest.in.urlPath) : "?";

    char allow[128];
    int  pos = 0;
    for (int v = 0; v < CorVerbs; v++)
    {
      if (v == corRest.in.verb) continue;

      // The full path back before every probe: one that matched a wildcard route cut it short, and the
      // next verb's routes matched the shorter path - DELETE /temporal/entities/*/attrs/* left
      // /temporal/entities/{id}, and HEAD (from GET /temporal/entities/*) was in the Allow list
      if (corRest.in.urlPath != NULL)
        strcpy(corRest.in.urlPath, reqPath);

      CorRestService* probe = corRestServiceLookup(&routesOf()[v]);
      if (probe == NULL) continue;
      if (pos > 0) { allow[pos++] = ','; allow[pos++] = ' '; }
      const char* vs = corRestVerbToString((CorRestVerb) v);
      int         vl = strlen(vs);
      if (pos + vl >= (int) sizeof(allow)) break;
      memcpy(&allow[pos], vs, vl);
      pos += vl;
    }
    allow[pos] = 0;

    if (pos > 0)
    {
      corRest.out.headerV[corRest.out.headerCount].key   = "Allow";
      corRest.out.headerV[corRest.out.headerCount].value = corAllocStrdup(&corRest.kalloc, allow);
      corRest.out.headerCount++;
      corRestProblem(405, COR_REST_ERROR_METHOD, "Method Not Allowed",
                    "%s %s — supported methods: %s",
                    corRest.in.verbString ? corRest.in.verbString : "?",
                    reqPath,
                    allow);
    }
    else
    {
      corRestProblem(404, COR_REST_ERROR_NOT_FOUND, "Not Found",
                    "%s %s is not a recognized resource",
                    corRest.in.verbString ? corRest.in.verbString : "?",
                    reqPath);
    }
    goto respond;
  }

  // Validate URL parameters against the service's supported set.
  // Sentinel: supportedParams == ~0ULL ("all bits set") means the
  // service accepts any URL param without validation — used by mocks
  // (ftClient) and other passthrough tools that don't pre-register
  // a vocabulary of known params.
  if (corRest.in.uriParamCount > 0 && corRest.serviceP->supportedParams != ~(uint64_t)0)
  {
    for (int i = 0; i < corRest.in.uriParamCount; i++)
    {
      uint64_t bit = corRestParamLookup(corRest.in.uriParamV[i].key);

      // Resolve the param's bit once, here: carry it on the entry and OR it into
      // the request-wide mask, so downstream code tests params by bit, not name.
      corRest.in.uriParamV[i].bit = bit;
      corRest.in.uriParamMask    |= bit;

      if (bit == 0 || (bit & corRest.serviceP->supportedParams) == 0)
      {
        // § 6.3.20: an unknown query parameter is an InvalidRequest, not
        // BadRequestData (which the spec reserves for semantic errors in
        // the NGSI-LD payload).
        corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request",
                      "Unknown/unsupported URL parameter: %s", corRest.in.uriParamV[i].key);
        goto respond;
      }

      if (corRestParamHookF != NULL)
        corRestParamHookF(corRest.in.uriParamV[i].key, corRest.in.uriParamV[i].value);
    }
  }

  // Pre-service hook (tenant resolution, etc.)
  if (!corRestPreServiceHookF())
    goto respond;

  // Wildcard semantic validation (entity/sub/reg/ctx ids, attr name, instance
  // id) belongs to the NGSI-LD layer — corNgsild's preServiceHook does it.

  // Body-presence check for write verbs (POST / PUT / PATCH) on services
  // that carry an LdOp OR live under /ngsi-ld/ (jsonldContexts and
  // entityMaps have ldOp=0 but still need a body). Admin paths (/admin/,
  // anything else not under /ngsi-ld/) opt out — some are body-less by
  // design (POST /admin/subStats/flush, etc.).
  bool isNgsildPath = (corRest.serviceP->url != NULL &&
                       strncmp(corRest.serviceP->url, "/ngsi-ld/", 9) == 0);
  bool needsBody    = (corRest.serviceP->ldOp != 0 || isNgsildPath);
  if (needsBody &&
      (corRest.in.verb == CorVerbPost  ||
       corRest.in.verb == CorVerbPut   ||
       corRest.in.verb == CorVerbPatch))
  {
    if (corRest.in.payload != NULL && corRest.in.requestTree == NULL)
    {
      // Failed JSON parse: per § 4.9 / § 6.3.4 this is InvalidRequest at 400,
      // not 415 (which is reserved for unsupported Content-Type). A body nested
      // deeper than corJson admits may well be valid JSON - it is not called invalid.
      if (corRest.corJsonP->status == CorJsonMaxDepthExceeded)
        corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request",
                      "request body nested deeper than 100 levels");
      else
        corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request",
                      "request body is not valid JSON");
      goto respond;
    }
    if (corRest.in.requestTree == NULL)
    {
      // § 5.5.4: an empty body is "not a valid JSON document" → InvalidRequest.
      corRestProblem(400, COR_REST_ERROR_INVALID_REQ, "Invalid Request", "no payload");
      goto respond;
    }
  }

  // Skip the service routine when an earlier hook (parseHook, paramHook,
  // preServiceHook) already set a problem detail — the response is fully
  // determined and the service routine has nothing to do.
  if (corRest.out.problemType != NULL)
    goto respond;

  // Per-service payload-body validator (NGSI-LD pCheckXxx). Runs on the parsed
  // requestTree before the service routine, so a malformed body is rejected with
  // a pinpointed 400 instead of being half-ignored by the handler. NULL = none.
  if (corRest.serviceP->payloadCheck != NULL)
  {
    corRest.serviceP->payloadCheck();
    if (corRest.out.problemType != NULL)
      goto respond;
  }

  // Dispatch to service routine
  int headersBefore = corRest.out.headerCount;

  corRest.serviceP->serviceRoutine();

  // A late problem (corRestProblemLate) replaces whatever the service routine answered
  if (corRest.out.lateStatus != 0)
  {
    corRest.out.httpStatusCode = corRest.out.lateStatus;
    corRest.out.problemType    = corRest.out.lateType;
    corRest.out.problemTitle   = corRest.out.lateTitle;
    corRest.out.problemExtras  = NULL;
    corRest.out.responseTree   = NULL;
    corRest.out.payload        = NULL;
    corRest.out.payloadSize    = 0;
    corRest.out.streamOpenF    = NULL;
    corRest.out.headerCount    = headersBefore;      // the headers the routine added describe its answer
    snprintf(corRest.out.problemDetail, sizeof(corRest.out.problemDetail), "%s", corRest.out.lateDetail);
  }

  // Call render hook unconditionally — the renderHook does the
  // § 6.3.4 Accept-negotiation 406 check, which must override an
  // earlier 4xx (e.g. retrieving a non-existent entity with an
  // unacceptable Accept header should answer 406, not 404).
  // The hook short-circuits its body-formatting work when problemType
  // is already set, so the only effect on the error path is the 406
  // check at the top.
  corRestPayloadRenderHook();

  // If service set a problem type but forgot status code, default to 500
  if (corRest.out.httpStatusCode == 200 && corRest.out.problemType != NULL)
    corRest.out.httpStatusCode = 500;

 respond:
  ;

  // Build problem details response tree if needed (RFC 7807 / § 5.2.16).
  // `status` SHOULD carry the HTTP status code — clients (e.g. the ETSI
  // testsuite's `${response.json()['errors'][0]['error']['status']}`
  // accessor) rely on it.
  if (corRest.out.problemType != NULL && corRest.out.responseTree == NULL)
  {
    corRest.out.responseTree = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(corRest.out.responseTree, corTreeString (corRest.kallocP, "type", corRest.out.problemType));
    corTreeChildAdd(corRest.out.responseTree, corTreeString (corRest.kallocP, "title", corRest.out.problemTitle));
    corTreeChildAdd(corRest.out.responseTree, corTreeInteger(corRest.kallocP, "status", corRest.out.httpStatusCode));
    corTreeChildAdd(corRest.out.responseTree, corTreeString (corRest.kallocP, "detail", corRest.out.problemDetail));

    // RFC 9457 §3.2 extension members (e.g. registrationId of a failed forward).
    if (corRest.out.problemExtras != NULL)
    {
      CorNode* m = corRest.out.problemExtras->value.head;
      while (m != NULL)
      {
        CorNode* next = m->next;
        corTreeChildAdd(corRest.out.responseTree, m); // splice across (re-parents m, clears m->next)
        m = next;
      }
    }
  }

  // Render response tree to JSON - unless the transport carries the tree itself (cor://)
  char*  responseBody     = NULL;
  int    responseBodySize = 0;

  if (corRest.out.noRender == true)
    return;

  if (corRest.out.responseTree != NULL)
  {
    if (corRest.in.prettySpaces > 0)
    {
      corRest.corJsonP->spacesPerIndent = corRest.in.prettySpaces;
      responseBodySize = corJsonRenderSize(corRest.corJsonP, corRest.out.responseTree) + 1;
      responseBody     = corAlloc(&corRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "corRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        corRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        corJsonRender(corRest.corJsonP, corRest.out.responseTree, responseBody);
      }
    }
    else
    {
      responseBodySize = corJsonFastRenderSize(corRest.out.responseTree) + 1;
      responseBody     = corAlloc(&corRest.kalloc, responseBodySize);
      if (responseBody == NULL)
      {
        fprintf(stderr, "corRest: response body alloc failed (need %d bytes) — arena chunk too small?\n", responseBodySize);
        corRest.out.httpStatusCode = 500;
        responseBody     = (char*) "";
        responseBodySize = 0;
      }
      else
      {
        corJsonFastRender(corRest.out.responseTree, responseBody);
      }
    }
    if (responseBody != NULL && responseBody[0] != 0)
      responseBodySize = strlen(responseBody);
  }
  else if (corRest.out.payload != NULL)
  {
    responseBody     = corRest.out.payload;
    responseBodySize = corRest.out.payloadSize;
  }
  else
  {
    responseBody     = (char*) "";
    responseBodySize = 0;
  }

  // Hand the rendered body back through corRest.out so both the connection
  // handler and in-process self-forward callers read it uniformly.
  corRest.out.payload     = responseBody;
  corRest.out.payloadSize = responseBodySize;
}



// -----------------------------------------------------------------------------
//
// COR_REST_SELF_FORWARD_MAX_DEPTH - guard against runaway in-process forwards
//
// The depth is the request's (CorRestState.selfForwardDepth): a coroutine may wait inside one while
// another request on the same thread goes through its own.
//
#define COR_REST_SELF_FORWARD_MAX_DEPTH 8



// -----------------------------------------------------------------------------
//
// corRestProcessInProcess - run a distributed-op forward in-process (self-forward)
//
// The NGSI-LD layer detected that a forward targets this broker's own endpoint.
// Rather than open a socket back to ourselves (a blocking round-trip that stalls
// the epoll pool thread), run the request directly on a fresh inner CorRestState
// and hand back the rendered result — exactly as if it had come off a socket.
//
// The inner request runs the full dispatch pipeline plus the post-response hook,
// so its own deferred work (e.g. notifications for the forwarded write) fires
// before the inner arena is freed. The inner pipeline resets thread-local
// application state (corNgsild + deferred caches); the CALLER must save/restore
// that around this call — see ldDistOp.c.
//
// Outputs are borrowed into respAllocP, which the caller must keep alive.
// Returns the HTTP status code, or -1 if the inner request could not be set up.
//
int corRestProcessInProcess(CorRestVerb       verb,
                           const char*      path,
                           CorRestKeyValue*  headerV,
                           int              headerCount,
                           const char*      body,
                           int              bodyLen,
                           CorAlloc*        respAllocP,
                           char**           respBodyP,
                           int*             respBodyLenP,
                           CorRestKeyValue** respHeaderVP,
                           int*             respHeaderCountP)
{
  if (respBodyP        != NULL) *respBodyP        = NULL;
  if (respBodyLenP     != NULL) *respBodyLenP     = 0;
  if (respHeaderVP     != NULL) *respHeaderVP     = NULL;
  if (respHeaderCountP != NULL) *respHeaderCountP = 0;

  // Via-based loop detection normally stops an in-process forward from matching
  // the same CSR and self-forwarding again; guard the recursion depth too in
  // case alias matching is ever misconfigured.
  CorRestState* outerP = corRestP;
  int           depth  = (outerP != NULL) ? outerP->selfForwardDepth : 0;

  if (depth >= COR_REST_SELF_FORWARD_MAX_DEPTH)
    return -1;

  CorRestState* innerP = (CorRestState*) malloc(sizeof(CorRestState));
  if (innerP == NULL)
    return -1;

  corRestP = innerP;

  corRestStateInit(NULL, path, corRestVerbToString(verb));
  innerP->selfForwardDepth = depth + 1;

  // The inner request gets its OWN application state (per-conn corNgsild), so it
  // can't clobber the paused outer's — this is what lets the distop layer drop
  // its corNgsild save/restore around the self-forward.
  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  // Same logical request as the outer — reuse its start time so the inner's
  // createdAt/modifiedAt stay consistent with the originating request.
  corRest.requestStartTime     = outerP->requestStartTime;
  corRest.requestStartTimeMono = outerP->requestStartTimeMono;

  // URI params (corRestStateInit split a trailing ?query into corRest.in.urlParams)
  corRestUriParamsParse();

  // Request headers — borrowed; the (outer) arena they live in stays alive
  for (int i = 0; i < headerCount; i++)
    corRestHttpHeaderAdd(headerV[i].key, headerV[i].value);

  // Body — copy into the inner arena (freed by corRestStateRelease)
  if (body != NULL && bodyLen > 0)
  {
    corRest.in.payload = (char*) corAlloc(&corRest.kalloc, bodyLen + 1);
    if (corRest.in.payload != NULL)
    {
      memcpy(corRest.in.payload, body, bodyLen);
      corRest.in.payload[bodyLen] = 0;
      corRest.in.payloadSize      = bodyLen;
    }
  }

  corRestProcessRequest();

  int status = corRest.out.httpStatusCode;

  // Copy the rendered body + response headers into the caller's arena BEFORE the
  // inner arena (where they currently live) is released.
  if (respBodyP != NULL && corRest.out.payload != NULL && corRest.out.payloadSize > 0)
  {
    char* b = (char*) corAlloc(respAllocP, corRest.out.payloadSize + 1);
    if (b != NULL)
    {
      memcpy(b, corRest.out.payload, corRest.out.payloadSize);
      b[corRest.out.payloadSize] = 0;
      *respBodyP = b;
      if (respBodyLenP != NULL) *respBodyLenP = corRest.out.payloadSize;
    }
  }

  if (respHeaderVP != NULL && corRest.out.headerCount > 0)
  {
    int n = corRest.out.headerCount;
    CorRestKeyValue* hv = (CorRestKeyValue*) corAlloc(respAllocP, n * sizeof(CorRestKeyValue));
    if (hv != NULL)
    {
      for (int i = 0; i < n; i++)
      {
        hv[i].key   = corAllocStrdup(respAllocP, corRest.out.headerV[i].key);
        hv[i].value = corAllocStrdup(respAllocP, corRest.out.headerV[i].value);
      }
      *respHeaderVP = hv;
      if (respHeaderCountP != NULL) *respHeaderCountP = n;
    }
  }

  // Flush the inner request's deferred post-response work (notifications for the
  // forwarded write, etc.) while its arena is still alive.
  corRestPostResponseHook();

  corRestStateRelease();
  if (corRestUserDataFreeHookF != NULL && corRest.userData != NULL)
    corRestUserDataFreeHookF(corRest.userData);
  free(innerP);

  corRestP = outerP;

  return status;
}



// -----------------------------------------------------------------------------
//
// -----------------------------------------------------------------------------
//
// Async worker pool (6d)
//
// The backend's I/O threads accept, parse headers and accumulate the body, then
// suspend the connection and hand its CorRestState to this pool. A worker binds
// corRestP to that state and runs the (potentially slow: DB, distops) dispatch
// off the I/O thread, then resumes the connection, and the backend sends the
// already-built response. This decouples per-request processing latency from
// epoll I/O throughput: a slow request no longer blocks the I/O thread from
// servicing every other connection pinned to it.
//
// All request state lives in the per-connection CorRestState (arena, corJson,
// in/out, userData -> per-conn corNgsild), so a worker needs nothing but
// `corRestP = conP` — the I/O thread already ran corRestStateInit. The deferred
// notification caches are per-connection too, and still flushed by the
// post-response hook once the response has gone out, so notification ordering
// is unchanged.
//
//
// The work queues - one per event loop, not one per process
//
// A single queue was one mutex and one condvar that EVERY request crossed
// twice, enqueue and dequeue, with every worker parked on the same condvar.
// With one event loop that cost little. With several it is the ceiling:
// per-request CPU measured 30.7 us at one loop and 63.8 us at eight, on
// identical work. The requests did not get more expensive, the contention did.
//
// So each loop gets a queue of its own and a slice of the workers. A request is
// read by loop i, queued to shard i, run by one of shard i's workers, resumed
// to loop i and written by loop i - never touching a lock another loop holds.
//
// The backend decides the count: the builtin server has one shard per event
// loop, libmicrohttpd exactly one, which leaves that backend as it was.
//
// ⚠ The kernel hashes CONNECTIONS to loops, not requests. A deployment with a
// handful of long-lived keep-alive clients can therefore leave a shard idle
// while another queues, and a worker cannot steal from a neighbour. Work
// stealing is the answer if that shows up in practice; it is strictly more
// machinery, so it waits for evidence.
//
typedef struct CorRestWorkQueue
{
  pthread_mutex_t   mtx;
  pthread_cond_t    cond;
  CorRestState*     head;
  CorRestState*     tail;
} CorRestWorkQueue;

#define COR_REST_MAX_SHARDS 64

static CorRestWorkQueue corRestQueueV[COR_REST_MAX_SHARDS];
static int              corRestShards      = 1;
static pthread_t*       corRestWorkerV     = NULL;
static int*             corRestWorkerShard = NULL;   // which queue each worker serves
static int              corRestWorkerCount = 0;
//
// corRestWorkersRun - is the async pool up? Written by the main thread, read by the HTTP threads.
//
// The pool starts BEFORE the HTTP server (corRestInit) and stops before it (corRestStop). The
// queues are set up by the main thread and used by the HTTP threads, and this flag is what
// publishes them - and, at shutdown, what tells the HTTP threads, still running, that the pool
// is going down. It used to be a `volatile bool`, and volatile orders nothing: the compiler, or a
// CPU with a weaker memory model than x86 (ARM), may make the flag visible before the queue it is
// guarding - a race detector (drd) reports exactly that. So it is published with a RELEASE store
// once the queues are set, and read with ACQUIRE loads: a thread that sees true sees the queues.
//
static bool             corRestWorkersRun  = false;

static inline bool workersRun(void) { return __atomic_load_n(&corRestWorkersRun, __ATOMIC_ACQUIRE); }



// -----------------------------------------------------------------------------
//
// corRestWorkerShardsSet - how many work queues to run
//
// Called BEFORE corRestWorkerPoolStart - corRestInit passes corRestBackendLoops,
// which the backend knows before it starts. Clamped rather than refused.
//
void corRestWorkerShardsSet(int shards)
{
  if (shards < 1)
    shards = 1;
  else if (shards > COR_REST_MAX_SHARDS)
    shards = COR_REST_MAX_SHARDS;

  corRestShards = shards;
}



static void corRestWorkerEnqueue(CorRestState* conP)
{
  CorRestWorkQueue* qP = &corRestQueueV[(conP->shard >= 0 && conP->shard < corRestShards) ? conP->shard : 0];

  conP->asyncNext = NULL;

  pthread_mutex_lock(&qP->mtx);
  if (qP->tail != NULL)
    qP->tail->asyncNext = conP;
  else
    qP->head = conP;
  qP->tail = conP;
  pthread_cond_signal(&qP->cond);
  pthread_mutex_unlock(&qP->mtx);
}

static void* corRestWorkerMain(void* shardP)
{
  CorRestWorkQueue* qP = &corRestQueueV[*(int*) shardP];

  while (true)
  {
    pthread_mutex_lock(&qP->mtx);
    while (qP->head == NULL && workersRun())
      pthread_cond_wait(&qP->cond, &qP->mtx);

    if (qP->head == NULL)           // woken with an empty queue => shutdown drain done
    {
      pthread_mutex_unlock(&qP->mtx);
      break;
    }

    CorRestState* conP = qP->head;
    qP->head = conP->asyncNext;
    if (qP->head == NULL)
      qP->tail = NULL;
    pthread_mutex_unlock(&qP->mtx);

    corRestP = conP;

    //
    // Second visit: the response has gone out and what is left is the request's
    // post-response work. Nothing below applies - there is no connection to
    // resume, and after this the state is gone.
    //
    if (conP->asyncFinishing == true)
    {
      if (conP->finishF != NULL)
        conP->finishF(conP);
      else
        corRestBackendFinish(conP);
      corRestP = NULL;
      continue;
    }

    // Run the dispatch on this worker, bound to the connection's own state.
    // A self-targeted forward (corRestProcessInProcess) runs synchronously
    // inside this same call on this same worker thread — never re-enqueued.
    corRestProcessRequest();
    conP->asyncProcessed = true;

    //
    // Hand the connection back to the backend's event loop. STILL BOUND: the
    // built-in backend reads corRest.out here to build the response before it
    // wakes the loop, and unbinding first would leave it reading the fallback
    // state of this worker thread. After this call the connection may already
    // be finished and freed by the loop, so nothing below may touch conP.
    //
    if (conP->resumeF != NULL)
      conP->resumeF(conP);
    else
      corRestBackendResume(conP);

    corRestP = NULL;                // drop the bind; the request has left this thread
  }

  return NULL;
}

int corRestWorkerPoolStart(int workers)
{
  if (workers < 1)
    workers = 1;

  //
  // More shards than workers would leave a queue with nobody serving it, and a
  // request landing there would wait for ever. One worker per shard is the
  // floor, so the shard count bends to the worker count rather than the other
  // way round.
  //
  if (corRestShards > workers)
    corRestShards = workers;

  for (int ix = 0; ix < corRestShards; ix++)
  {
    pthread_mutex_init(&corRestQueueV[ix].mtx, NULL);
    pthread_cond_init(&corRestQueueV[ix].cond, NULL);
    corRestQueueV[ix].head = NULL;
    corRestQueueV[ix].tail = NULL;
  }

  corRestWorkerV     = (pthread_t*) calloc(workers, sizeof(pthread_t));
  corRestWorkerShard = (int*)       calloc(workers, sizeof(int));

  if ((corRestWorkerV == NULL) || (corRestWorkerShard == NULL))
  {
    free(corRestWorkerV);      // free(NULL) is a no-op; either may be the one that failed
    free(corRestWorkerShard);
    corRestWorkerV     = NULL;
    corRestWorkerShard = NULL;
    return -1;
  }

  __atomic_store_n(&corRestWorkersRun, true, __ATOMIC_RELEASE);   // the queues above, published

  //
  // Round-robin rather than contiguous blocks: with workers not a multiple of
  // shards, round-robin leaves the remainder spread one-per-shard instead of
  // piling it all on the last one.
  //
  // The shard index is passed BY ADDRESS and must therefore outlive the call -
  // hence the array rather than a stack variable or a cast-through-pointer,
  // which would hand every thread the same or an invalid one.
  //
  for (int i = 0; i < workers; i++)
  {
    corRestWorkerShard[i] = i % corRestShards;

    if (pthread_create(&corRestWorkerV[i], NULL, corRestWorkerMain, &corRestWorkerShard[i]) != 0)
    {
      corRestWorkerCount = i;    // join only the threads that started
      return -1;
    }
  }

  corRestWorkerCount = workers;
  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestWorkerPoolStop - stop new suspensions, drain the queue, join workers
//
// Clearing corRestWorkersRun first makes the backend's handler process inline
// again, so no NEW connection is suspended; the workers then drain whatever is
// already queued (processing + resuming each) before exiting. After this
// returns there are no suspended connections, so stopping the backend is safe
// (resuming across MHD_stop_daemon is an API violation).
//
void corRestWorkerPoolStop(void)
{
  if (corRestWorkerV == NULL)
    return;

  //
  // Clear the flag under EVERY shard's lock and wake everyone. A worker parked
  // on its own condvar is woken only by its own shard, so a broadcast on one of
  // them would leave the others asleep for ever and the join below would hang.
  //
  for (int ix = 0; ix < corRestShards; ix++)
  {
    pthread_mutex_lock(&corRestQueueV[ix].mtx);
    __atomic_store_n(&corRestWorkersRun, false, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&corRestQueueV[ix].cond);
    pthread_mutex_unlock(&corRestQueueV[ix].mtx);
  }

  for (int i = 0; i < corRestWorkerCount; i++)
    pthread_join(corRestWorkerV[i], NULL);

  free(corRestWorkerV);
  free(corRestWorkerShard);
  corRestWorkerV     = NULL;
  corRestWorkerShard = NULL;
  corRestWorkerCount = 0;

  for (int ix = 0; ix < corRestShards; ix++)
  {
    pthread_mutex_destroy(&corRestQueueV[ix].mtx);
    pthread_cond_destroy(&corRestQueueV[ix].cond);
  }
}



// -----------------------------------------------------------------------------
//
// corRestAsyncPoolUp / corRestAsyncEnqueue - hand this request to a worker
//
// Two calls, with the connection suspended between them - see corRestBackend.h.
//
bool corRestAsyncPoolUp(void)
{
  return workersRun();
}



// -----------------------------------------------------------------------------
//
// corRestAsyncDispatch - hand this request to a worker (true), or run it on this I/O thread (false)?
//
// Handed off when the pool is up and the app's inline hook does not claim the request - so with no
// hook, exactly what the backends did before the hook existed. See CorRestInlineHook.
//
static _Atomic uint64_t dispatchInline    = 0;
static _Atomic uint64_t dispatchHandedOff = 0;

// (*) The pool is down only during shutdown: corRestStop stops it (corRestWorkerPoolStop clears
//     corRestWorkersRun first) before it stops the backend, and a request arriving in between runs
//     inline. At start-up the pool is up before the backend accepts its first connection
//     (corRestInit), so every request served is counted.
//
bool corRestAsyncDispatch(void)
{
  if (corRestAsyncPoolUp() == false)
    return false;                                     // pool down: inline, and not counted - no choice was made (*)

  if ((corRestInlineHookF != NULL) && (corRestInlineHookF() == true))
  {
    atomic_fetch_add_explicit(&dispatchInline, 1, memory_order_relaxed);
    return false;
  }

  atomic_fetch_add_explicit(&dispatchHandedOff, 1, memory_order_relaxed);
  return true;
}



// -----------------------------------------------------------------------------
//
// corRestDispatchCounts - how many requests ran inline, how many were handed to a worker
//
void corRestDispatchCounts(uint64_t* inlineP, uint64_t* handedOffP)
{
  *inlineP    = atomic_load_explicit(&dispatchInline,    memory_order_relaxed);
  *handedOffP = atomic_load_explicit(&dispatchHandedOff, memory_order_relaxed);
}

void corRestAsyncEnqueue(CorRestState* stateP)
{
  corRestWorkerEnqueue(stateP);
}



// -----------------------------------------------------------------------------
//
// corRestAsyncFinish - the post-response phase, off the I/O thread
//
bool corRestAsyncFinish(CorRestState* stateP)
{
  if (workersRun() == false)
    return false;

  stateP->asyncFinishing = true;
  corRestWorkerEnqueue(stateP);

  return true;
}



// -----------------------------------------------------------------------------
//
// corRestBodyPolicyCheck -
//
// § 6.3.4 — POST / PATCH / PUT to NGSI-LD endpoints must carry Content-Length
// (411 Length Required, and NO payload). § 6.3.2 — 413 when the announced body
// exceeds the broker cap. Non-NGSI-LD endpoints (e.g. /admin/*) are outside the
// spec's scope and accept bodyless POSTs.
//
void corRestBodyPolicyCheck(const char* url, const char* clHeader)
{
  if ((corRest.in.verb != CorVerbPost) &&
      (corRest.in.verb != CorVerbPut)  &&
      (corRest.in.verb != CorVerbPatch))
    return;

  if ((url == NULL) || (strncmp(url, "/ngsi-ld/", 9) != 0))
    return;

  if (clHeader == NULL)
  {
    // § 6.3.4 — just a 411 status, no body.
    corRest.out.httpStatusCode      = 411;
    corRest.in.contentLengthMissing = true;
    return;
  }

  if (corRestMaxRequestSize > 0)
  {
    unsigned long long cl = strtoull(clHeader, NULL, 10);

    if (cl > corRestMaxRequestSize)
      corRestProblem(413, COR_REST_ERROR_REQUEST_LENGTH, "Request Entity Too Large",
                    "request body of %llu bytes exceeds broker limit of %llu bytes",
                    cl, corRestMaxRequestSize);
  }
}



// -----------------------------------------------------------------------------
//
// corRestResponseHeaderVBuild -
//
// One implementation of the response-header policy for both backends. The SET
// and the ORDER are pinned by several hundred functional tests that compare
// captured responses line by line, so this is a specification rather than a
// preference - and two copies of it would be two chances to drift.
//
int corRestResponseHeaderVBuild(CorRestKeyValue* hv, int max)
{
  int n = 0;

  #define HDR_ADD(k, v)                          \
    do {                                         \
      if (n < max)                               \
      {                                          \
        hv[n].key   = (char*) (k);               \
        hv[n].value = (char*) (v);               \
        n++;                                     \
      }                                          \
    } while (0)

  // Content-Type policy:
  //  - No body (size 0): omit Content-Type — it describes a body there is none
  //    of (204 No Content, empty-body 201 Created, ...).
  //  - NGSI-LD § 6.3.x: 201 Created and error (4xx/5xx) responses are always
  //    application/json, regardless of the request's Accept.
  //  - Otherwise: the negotiated type (application/json / ld+json / geo+json).
  //
  // TS 104-176 specifies bare media types throughout (§ 6.2.3, § 6.3.3,
  // § 6.4.7.2 "exactly equal to the media type"); RFC 8259 defines no charset
  // parameter for application/json — so emit the type verbatim, no charset.
  if ((corRest.out.payloadSize > 0) || ((corRest.out.noRender == true) && (corRest.out.responseTree != NULL)))
  {
    int         code = corRest.out.httpStatusCode;
    const char* ct   = (code == 201 || (code >= 400 && code <= 599))
                       ? "application/json"
                       : corRest.out.contentType;

    if (ct != NULL)
      HDR_ADD("Content-Type", ct);
  }

  // § 6.3.6 Prefer / Preference-Applied: when the client sent a
  // "Prefer: ngsi-ld=<x>" header, echo "Preference-Applied: ngsi-ld=<our-version>"
  // so they know what spec version the response body conforms to.
  // We respond with our supported version regardless of <x> — full v1.9.1
  // degradation per § 4.3.6.8 is not implemented.
  for (int hi = 0; hi < corRest.in.httpHeaderCount; hi++)
  {
    if (corRest.in.httpHeaderV[hi].key != NULL &&
        strcasecmp(corRest.in.httpHeaderV[hi].key, "Prefer") == 0 &&
        corRest.in.httpHeaderV[hi].value != NULL &&
        strncasecmp(corRest.in.httpHeaderV[hi].value, "ngsi-ld=", 8) == 0)
    {
      HDR_ADD("Preference-Applied", "ngsi-ld=1.9.1");
      break;
    }
  }

  // The service routine's own headers, in the order it added them
  for (int i = 0; i < corRest.out.headerCount; i++)
    HDR_ADD(corRest.out.headerV[i].key, corRest.out.headerV[i].value);

  // CORS headers (if configured)
  if (corRestCors.allowOrigin != NULL)
  {
    HDR_ADD("Access-Control-Allow-Origin", corRestCors.allowOrigin);

    if (corRest.in.verb == CorVerbOptions)
    {
      // Preflight: echo Allow as Access-Control-Allow-Methods
      for (int i = 0; i < corRest.out.headerCount; i++)
      {
        if (strcmp(corRest.out.headerV[i].key, "Allow") == 0)
        {
          HDR_ADD("Access-Control-Allow-Methods", corRest.out.headerV[i].value);
          break;
        }
      }

      HDR_ADD("Access-Control-Allow-Headers",
              corRestCors.allowHeaders ? corRestCors.allowHeaders : "Content-Type, Accept, Link");

      if (corRestCors.maxAge > 0)
      {
        //
        // Rendered into the request arena rather than a local buffer: the
        // backend sends these AFTER this function has returned, and one of them
        // does not copy what it is given.
        //
        char* maxAgeBuf = (char*) corAlloc(&corRest.kalloc, 16);

        if (maxAgeBuf != NULL)
        {
          snprintf(maxAgeBuf, 16, "%d", corRestCors.maxAge);
          HDR_ADD("Access-Control-Max-Age", maxAgeBuf);
        }
      }
    }

    if (corRestCors.exposeHeaders != NULL)
      HDR_ADD("Access-Control-Expose-Headers", corRestCors.exposeHeaders);
  }

  #undef HDR_ADD

  return n;
}



// -----------------------------------------------------------------------------
//
// optionsHandler - auto-generated OPTIONS handler
//
// Builds an Allow header listing all verbs registered for the same URL pattern.
// Uses the matched service's URL pattern (not the actual request path),
// so wildcard URLs work correctly.
//
static bool optionsHandler(void)
{
  const char* matchedUrl = corRest.serviceP ? corRest.serviceP->url : corRest.in.urlPath;

  char allow[128];
  int  pos = 0;

  CorRestServiceVector* routeV = routesOf();

  for (int verb = 0; verb < CorVerbs; verb++)
  {
    for (int ix = 0; ix < routeV[verb].services; ix++)
    {
      if (strcmp(routeV[verb].serviceV[ix].url, matchedUrl) == 0)
      {
        if (pos > 0)
        {
          allow[pos++] = ',';
          allow[pos++] = ' ';
        }

        const char* vs = corRestVerbToString((CorRestVerb) verb);
        int         vl = strlen(vs);

        memcpy(&allow[pos], vs, vl);
        pos += vl;
        break;
      }
    }
  }

  allow[pos] = 0;

  corRest.out.httpStatusCode = 204;
  corRest.out.headerV[corRest.out.headerCount].key   = "Allow";
  corRest.out.headerV[corRest.out.headerCount].value  = corAllocStrdup(&corRest.kalloc, allow);
  corRest.out.headerCount++;

  return true;
}



// -----------------------------------------------------------------------------
//
// routesBuild - a listener's routes, per verb; HEAD for every GET, OPTIONS for every path
//
static int routesBuild(CorRestServiceVector* routeV, CorRestServiceSimplified serviceV[], int services)
{
  // --- Count services per verb ---
  int counts[CorVerbs];
  memset(counts, 0, sizeof(counts));

  for (int ix = 0; ix < services; ix++)
  {
    if (serviceV[ix].verb < CorVerbs)
      counts[serviceV[ix].verb]++;

    // GET services also get HEAD for free
    if (serviceV[ix].verb == CorVerbGet)
      counts[CorVerbHead]++;
  }

  // OPTIONS gets one entry per unique URL path
  // (over-allocate: at most 'services' unique paths)
  counts[CorVerbOptions] += services;

  // --- Allocate per-verb arrays ---
  for (int verb = 0; verb < CorVerbs; verb++)
  {
    routeV[verb].services = 0;
    routeV[verb].serviceV = NULL;

    if (counts[verb] == 0)
      continue;

    routeV[verb].serviceV = (CorRestService*) calloc(counts[verb], sizeof(CorRestService));
    if (routeV[verb].serviceV == NULL)
      return -1;
  }

  // --- Distribute services into per-verb arrays ---
  for (int ix = 0; ix < services; ix++)
  {
    CorRestVerb verb = serviceV[ix].verb;
    if (verb >= CorVerbs)
      continue;

    CorRestServiceVector* sv = &routeV[verb];
    servicePrepare(&sv->serviceV[sv->services], &serviceV[ix]);
    sv->services++;

    // Auto-add HEAD for every GET (same handler; body suppressed at response time)
    if (verb == CorVerbGet)
    {
      CorRestServiceVector* headSv = &routeV[CorVerbHead];
      servicePrepare(&headSv->serviceV[headSv->services], &serviceV[ix]);
      headSv->services++;
    }
  }

  // --- Auto-generate OPTIONS for each unique URL path ---
  CorRestServiceVector* optSv = &routeV[CorVerbOptions];

  for (int ix = 0; ix < services; ix++)
  {
    // Check if this URL is already in the OPTIONS vector
    bool found = false;
    for (int ox = 0; ox < optSv->services; ox++)
    {
      if (strcmp(optSv->serviceV[ox].url, serviceV[ix].url) == 0)
      {
        found = true;
        break;
      }
    }

    if (!found)
    {
      CorRestServiceSimplified optService = { 0 };   // zero ldOp + payloadCheck (no body validation on OPTIONS)

      optService.verb            = CorVerbOptions;
      optService.url             = serviceV[ix].url;
      optService.serviceRoutine  = optionsHandler;
      optService.supportedParams = 0;

      servicePrepare(&optSv->serviceV[optSv->services], &optService);
      optSv->services++;
    }
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestPrepare - the routes and the worker pool, no HTTP server yet
//
int corRestPrepare(CorRestServiceSimplified serviceV[], int services, int poolSize)
{
  if (routesBuild(corRestServiceV, serviceV, services) != 0)
    return -1;

  //
  // Async worker pool — one worker per I/O thread, one queue per event loop.
  //
  // BEFORE the HTTP server: the server accepts connections the moment it starts, and a request
  // that arrived while the pool was still down ran on the I/O thread whatever it was (a forward
  // to a context source included) and was counted neither inline nor handed off.
  //
  corRestWorkerShardsSet(corRestBackendLoops());

  if (corRestWorkerPoolStart(poolSize) != 0)
  {
    fprintf(stderr, "corRestInit: async worker pool failed to start\n");
    corRestWorkerPoolStop();                          // joins the workers that did start
    return -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestStart - the HTTP server, after corRestPrepare
//
int corRestStart(unsigned short port, int poolSize)
{
  //
  // The HTTP server itself - libmicrohttpd or the built-in one, decided at
  // compile time by COR_HTTP_SERVER. It starts its own threads and returns.
  //
  if (corRestBackendStart(port, poolSize, httpsServerKey, httpsServerCert) != 0)
  {
    corRestWorkerPoolStop();                          // nothing was accepted - nothing is queued
    return -1;
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestInit -
//
int corRestInit(CorRestServiceSimplified serviceV[], int services, unsigned short port, int poolSize)
{
  if (corRestPrepare(serviceV, services, poolSize) != 0)
    return -1;

  return corRestStart(port, poolSize);
}



// -----------------------------------------------------------------------------
//
// corRestListen -
//
int corRestListen(const char* host, unsigned short port, CorRestServiceSimplified serviceV[], int services)
{
  if (routesBuild(corRestListenerServiceV, serviceV, services) != 0)
    return -1;

  return corRestBackendListen(host, port);
}
