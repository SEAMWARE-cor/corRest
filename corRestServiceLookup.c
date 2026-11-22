//
// FILE            corRestServiceLookup.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdbool.h>                         // bool
#include <string.h>                          // strcmp, strncmp, strstr, strchr

#include "corRest/CorRestState.h"              // corRest
#include "corRest/CorRestService.h"            // CorRestService, CorRestServiceVector
#include "corRest/corRestServiceLookup.h"      // Own interface



// -----------------------------------------------------------------------------
//
// isUrlForm - true if [s..end) starts with "<scheme>://" (RFC 3986 § 3.1)
//
// Used to let non-greedy wildcards accept URL-form NGSI-LD identifiers
// (https://..., http://...) in a single path segment. URN-form (urn:...)
// has no embedded '/' so passes the plain slash-check already; this only
// matters for the URL flavor of an entity / attribute / context id.
//
static bool isUrlForm(const char* s, const char* end)
{
  if (s == NULL || end == NULL || end - s < 4) return false;
  if (s[0] < 'a' || s[0] > 'z') return false;

  const char* p = s + 1;
  while (p < end &&
         ((*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') ||
          *p == '+' || *p == '.' || *p == '-'))
    p++;

  return (p + 2 < end) && p[0] == ':' && p[1] == '/' && p[2] == '/';
}



// -----------------------------------------------------------------------------
//
// requestPrepare - compute character sums for fast URL matching
//
#define COR_REST_MAX_CHARS_BEFORE_WILDCARD 64

static void requestPrepare(const char* url, int* cSumV, int* cSumsP, int* sLenP)
{
  *cSumsP = 0;

  if (url[0] == 0)
  {
    *sLenP = 0;
    return;
  }

  cSumV[0] = url[0];

  int ix = 1;

  while ((url[ix] != 0) && (ix < COR_REST_MAX_CHARS_BEFORE_WILDCARD))
  {
    cSumV[ix] = cSumV[ix - 1] + url[ix];
    ++ix;
  }
  *cSumsP = ix;

  while (url[ix] != 0)
    ++ix;

  *sLenP = ix;
}



// -----------------------------------------------------------------------------
//
// corRestServiceLookup -
//
CorRestService* corRestServiceLookup(CorRestServiceVector* serviceV)
{
  int cSumV[COR_REST_MAX_CHARS_BEFORE_WILDCARD];
  int cSums;
  int sLen;

  requestPrepare(corRest.in.urlPath, cSumV, &cSums, &sLen);

  for (int ix = 0; ix < serviceV->services; ix++)
  {
    CorRestService* serviceP = &serviceV->serviceV[ix];

    if (serviceP->wildcards == 0)
    {
      // Exact match: compare length, char-sum, then strcmp
      if (serviceP->charsBeforeFirstWildcard == sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[sLen - 1])
        {
          if (strcmp(serviceP->url, corRest.in.urlPath) == 0)
            return serviceP;
        }
      }
    }
    else if (serviceP->wildcards == 1)
    {
      if (serviceP->charsBeforeFirstWildcard < sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[serviceP->charsBeforeFirstWildcard - 1])
        {
          if (strncmp(serviceP->url, corRest.in.urlPath, serviceP->charsBeforeFirstWildcard) == 0)
          {
            if (serviceP->matchForSecondWildcardLen != 0)
            {
              // Wildcard in the middle: /prefix/*/suffix
              int endIndex = sLen - serviceP->matchForSecondWildcardLen;

              if (endIndex > serviceP->charsBeforeFirstWildcard &&
                  strncmp(&corRest.in.urlPath[endIndex], serviceP->matchForSecondWildcard, serviceP->matchForSecondWildcardLen) == 0)
              {
                char* wildcardValue = &corRest.in.urlPath[serviceP->charsBeforeFirstWildcard];

                // For non-greedy *, wildcard must not contain '/' (single component only).
                // Exception: URL-form identifiers (scheme://...) are accepted as one segment
                // because NGSI-LD ids may be full URLs (§ 4.3.2 / § 4.6.2).
                if (!serviceP->greedy)
                {
                  int wildcardLen = endIndex - serviceP->charsBeforeFirstWildcard;
                  bool hasSlash = false;
                  for (int j = 0; j < wildcardLen; j++)
                  {
                    if (wildcardValue[j] == '/')
                    {
                      hasSlash = true;
                      break;
                    }
                  }
                  if (hasSlash && !isUrlForm(wildcardValue, wildcardValue + wildcardLen))
                    continue;  // Not a match for single-component wildcard
                }

                corRest.in.wildcard[0] = wildcardValue;
                corRest.in.urlPath[endIndex] = 0;
                return serviceP;
              }
            }
            else
            {
              // Wildcard at the end: /prefix/*
              char* wildcardValue = &corRest.in.urlPath[serviceP->charsBeforeFirstWildcard];

              // For non-greedy *, wildcard must not contain '/' — unless the
              // value is URL-form (scheme://...), which counts as one segment
              // per § 4.3.2 / § 4.6.2 (URI ids may be URLs).
              if (!serviceP->greedy &&
                  strchr(wildcardValue, '/') != NULL &&
                  !isUrlForm(wildcardValue, wildcardValue + strlen(wildcardValue)))
                continue;

              corRest.in.wildcard[0] = wildcardValue;
              return serviceP;
            }
          }
        }
      }
    }
    else if (serviceP->wildcards == 2)
    {
      if (serviceP->charsBeforeFirstWildcard < sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[serviceP->charsBeforeFirstWildcard - 1])
        {
          char* matchP;

          if ((matchP = strstr(&corRest.in.urlPath[serviceP->charsBeforeFirstWildcard], serviceP->matchForSecondWildcard)) != NULL)
          {
            char* wc0    = &corRest.in.urlPath[serviceP->charsBeforeFirstWildcard];
            char* wc1    = &matchP[serviceP->matchForSecondWildcardLen];
            char* wc1End = wc1 + strlen(wc1);

            // Literal suffix after the last wildcard (e.g. ".../attrs/*/value"):
            // the URL must end with it, and the last wildcard ends before it.
            // A 0-length suffix (the common case) skips this entirely.
            // The path is written only once the route matches: a 'continue' after a write left it cut short for
            // the routes after this one.
            if (serviceP->matchAfterLastWildcardLen > 0)
            {
              if (wc1End - wc1 <= serviceP->matchAfterLastWildcardLen)
                continue;  // nothing left for the wildcard once the suffix is removed
              wc1End -= serviceP->matchAfterLastWildcardLen;
              if (strcmp(wc1End, serviceP->matchAfterLastWildcard) != 0)
                continue;  // URL does not end with the required suffix
            }

            // For non-greedy, each wildcard must be single component (no '/'),
            // unless it's URL-form (scheme://...) - else /entities/{id}/attrs/a/b
            // matched attribute-by-id with attrId "a/b".
            if (!serviceP->greedy)
            {
              bool hasSlash = false;
              for (char* p = wc0; p < matchP; p++)
              {
                if (*p == '/')
                {
                  hasSlash = true;
                  break;
                }
              }
              if (hasSlash && !isUrlForm(wc0, matchP))
                continue;

              hasSlash = false;
              for (char* p = wc1; p < wc1End; p++)
              {
                if (*p == '/')
                {
                  hasSlash = true;
                  break;
                }
              }
              if (hasSlash && !isUrlForm(wc1, wc1End))
                continue;
            }

            corRest.in.wildcard[0] = wc0;
            corRest.in.wildcard[1] = wc1;
            *matchP = 0;
            *wc1End = 0;   // a no-op without a suffix: wc1End is the path's own end

            return serviceP;
          }
        }
      }
    }
    else  // 3 wildcards
    {
      if (serviceP->charsBeforeFirstWildcard < sLen)
      {
        if (serviceP->charsBeforeFirstWildcardSum == cSumV[serviceP->charsBeforeFirstWildcard - 1])
        {
          // wc0 ... matchForSecondWildcard ... wc1 ... matchForThirdWildcard ... wc2
          char* wc0 = &corRest.in.urlPath[serviceP->charsBeforeFirstWildcard];
          char* sep1 = strstr(wc0, serviceP->matchForSecondWildcard);
          if (sep1 == NULL)
            continue;

          char* wc1 = &sep1[serviceP->matchForSecondWildcardLen];
          char* sep2 = (serviceP->matchForThirdWildcardLen > 0)
                       ? strstr(wc1, serviceP->matchForThirdWildcard)
                       : wc1;
          if (sep2 == NULL)
            continue;

          char* wc2 = &sep2[serviceP->matchForThirdWildcardLen];

          // For non-greedy, each wildcard must be single component (no '/'),
          // unless that wildcard is URL-form (scheme://...).
          if (!serviceP->greedy)
          {
            bool reject = false;
            // Wildcard 0 (wc0..sep1)
            bool s0 = false;
            for (char* p = wc0; p < sep1; p++) if (*p == '/') { s0 = true; break; }
            if (s0 && !isUrlForm(wc0, sep1)) reject = true;
            // Wildcard 1 (wc1..sep2)
            if (!reject)
            {
              bool s1 = false;
              for (char* p = wc1; p < sep2; p++) if (*p == '/') { s1 = true; break; }
              if (s1 && !isUrlForm(wc1, sep2)) reject = true;
            }
            // Wildcard 2 (wc2..end)
            if (!reject)
            {
              char* wc2end = wc2;
              while (*wc2end != 0) wc2end++;
              bool s2 = false;
              for (char* p = wc2; p < wc2end; p++) if (*p == '/') { s2 = true; break; }
              if (s2 && !isUrlForm(wc2, wc2end)) reject = true;
            }
            if (reject)
              continue;
          }

          *sep1 = 0;
          *sep2 = 0;
          corRest.in.wildcard[0] = wc0;
          corRest.in.wildcard[1] = wc1;
          corRest.in.wildcard[2] = wc2;
          return serviceP;
        }
      }
    }
  }

  return NULL;
}
