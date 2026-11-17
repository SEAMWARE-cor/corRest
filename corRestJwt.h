#ifndef CORREST_CORRESTJWT_H_
#define CORREST_CORRESTJWT_H_

//
// FILE            corRestJwt.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A JWT (RFC 7519) in compact JWS form - an access token, as VCVerifier or an OpenID provider issues it: the
// signature checked against the key set (corRestJwk), then the registered claims - exp, nbf, iss, aud. Used by
// coraine's auth plugins (fdsc.so, oidc.so) and corCep.
//
#include <stdint.h>                                  // int64_t

#include "corAlloc/CorAlloc.h"                       // CorAlloc
#include "corTree/CorNode.h"                         // CorNode



// -----------------------------------------------------------------------------
//
// CorRestJwtCheck - corRestJwtVerify's answer
//
typedef enum CorRestJwtCheck
{
  CorRestJwtOk           = 0,
  CorRestJwtMalformed    = 1,                           // not a compact JWS, or its parts are not base64url JSON
  CorRestJwtUnknownKey   = 2,                           // signed by a key the set does not have (yet: the issuer may have rotated)
  CorRestJwtBadSignature = 3,
  CorRestJwtBadAlg       = 7,                           // an alg other than RS256 / ES256 ("none" among them), or not the key's
  CorRestJwtExpired      = 4,                           // exp passed, or nbf not reached
  CorRestJwtWrongIssuer  = 5,
  CorRestJwtWrongAudience = 6
} CorRestJwtCheck;



// -----------------------------------------------------------------------------
//
// CorRestJwtExpect - what the claims must say. NULL / 0: not checked.
//
typedef struct CorRestJwtExpect
{
  const char*  issuer;                               // iss
  const char*  audience;                             // one of aud (a string or an array)
  int64_t      now;                                  // seconds since the epoch
  int          leeway;                               // seconds of clock skew allowed on exp / nbf
} CorRestJwtExpect;



// -----------------------------------------------------------------------------
//
// corRestJwtVerify - the token checked; on CorRestJwtOk its claims (the payload object) in *claimsPP, built in
// kaP. 'token' is not modified.
//
extern CorRestJwtCheck corRestJwtVerify(const char* token, const CorRestJwtExpect* expectP, CorAlloc* kaP, CorNode** claimsPP);



// -----------------------------------------------------------------------------
//
// corRestJwtCheckString - a check's name, for the 401's detail
//
extern const char* corRestJwtCheckString(CorRestJwtCheck check);

#endif  // CORREST_CORRESTJWT_H_
