#ifndef CORREST_CORRESTJWK_H_
#define CORREST_CORRESTJWK_H_

//
// FILE            corRestJwk.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A token issuer's signing keys - a JWK Set (RFC 7517 § 5), RSA and EC P-256 public keys - and the
// signature check of a JWS (RFC 7515) against them: RS256 and ES256 (RFC 7518 § 3.3, § 3.4), what
// VCVerifier signs with, and what OpenID providers sign with by default (RS256). The set in use is replaced
// whole (corRestJwkLoad) by an application's refresh thread while request threads check signatures against it.
//
#include <stdbool.h>                                 // bool



// -----------------------------------------------------------------------------
//
// CorRestJwkCheck - corRestJwkVerify's answer
//
typedef enum CorRestJwkCheck
{
  CorRestJwkOk          = 0,
  CorRestJwkUnknown     = 1,                            // no key with that kid (or, with no kid, none for the alg)
  CorRestJwkBadSig      = 2,
  CorRestJwkBadAlg      = 3                             // an alg other than RS256 / ES256 - or not the key's
} CorRestJwkCheck;



// -----------------------------------------------------------------------------
//
// corRestJwkLoad - a JWK Set's text parsed and installed in place of the set in use. false: not one
// (the set in use stays) - errorBuf says why. Keys of a kind it does not know are skipped. changed (or NULL)
// is called when the set in use is replaced - not for the same text again.
//
extern bool corRestJwkLoad(char* jwksText, void (*changed)(void), char* errorBuf, int errorBufSize);



// -----------------------------------------------------------------------------
//
// corRestJwkVerify - is sig (raw, base64url-decoded) the signature of 'input' by the key 'kid' (NULL:
// any key that fits the alg)
//
extern CorRestJwkCheck corRestJwkVerify(const char* kid, const char* alg, const char* input, int inputLen, const unsigned char* sig, int sigLen);



// -----------------------------------------------------------------------------
//
// corRestJwkCount - how many keys the set in use has (0: none loaded yet)
//
extern int corRestJwkCount(void);



// -----------------------------------------------------------------------------
//
// corRestJwkRelease - the set in use freed (the broker stops); changed (or NULL) called after
//
extern void corRestJwkRelease(void (*changed)(void));

#endif  // CORREST_CORRESTJWK_H_
