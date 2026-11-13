//
// FILE            corRestClientTls.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// TLS support for the HTTP client.
//

#include <ctype.h>                              // isxdigit
#include <poll.h>                               // POLLIN, POLLOUT
#include <pthread.h>                            // pthread_once
#include <stdio.h>                               // snprintf
#include <stdbool.h>                             // bool
#include <string.h>                              // strlen
#include <unistd.h>                              // close

#include <openssl/ssl.h>                         // SSL_*, SSL_CTX_*
#include <openssl/err.h>                         // ERR_*
#include <openssl/evp.h>                         // EVP_sha256, EVP_MAX_MD_SIZE
#include <openssl/x509.h>                        // X509_digest

#include "corRest/corRestWait.h"                // corRestWaitFd
#include "corRest/corRestClient.h"                 // CorRestClientConn



// -----------------------------------------------------------------------------
//
// Global SSL context (one per process)
//
static SSL_CTX* sslCtx = NULL;



// -----------------------------------------------------------------------------
//
// corRestClientTlsInsecure - skip peer/host verification on outbound TLS
//
// Off by default (certificates are verified against the system CA store). When
// the broker is started with --insecureNotif this is turned on, so notifications
// and forwards to TLS endpoints accept self-signed certificates — the common
// case for an endpoint inside a trusted/firewalled network.
//
static bool insecure = false;

void corRestClientTlsInsecureSet(bool onoff)
{
  insecure = onoff;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsPinSet - trust ONE certificate, by its SHA-256 (64 hex digits; NULL or "": none)
//
// For a client that talks to one server whose certificate is its own - self-signed, no CA vouches for it, its
// name not the one the client uses (a home hub's broker reached at localhost). With a pin, every outbound TLS
// connection of the process is checked against it INSTEAD of the CA store and the host name: the leaf
// certificate's SHA-256 must be the pinned one, or the handshake fails. Process-wide, as insecure. false: not
// 64 hex digits (the pin unchanged).
//
static unsigned char pin[32];
static bool          pinned = false;

bool corRestClientTlsPinSet(const char* sha256hex)
{
  if ((sha256hex == NULL) || (sha256hex[0] == 0))
  {
    pinned = false;
    return true;
  }

  unsigned char bytes[32];

  if (strlen(sha256hex) != 64)
    return false;

  for (int ix = 0; ix < 32; ix++)
  {
    unsigned int b;
    char         hex[3] = { sha256hex[ix * 2], sha256hex[ix * 2 + 1], 0 };

    if ((isxdigit((unsigned char) hex[0]) == 0) || (isxdigit((unsigned char) hex[1]) == 0) || (sscanf(hex, "%2x", &b) != 1))
      return false;
    bytes[ix] = (unsigned char) b;
  }

  memcpy(pin, bytes, sizeof(pin));
  pinned = true;
  return true;
}



// -----------------------------------------------------------------------------
//
// pinMatches - the peer's leaf certificate is the pinned one
//
static bool pinMatches(SSL* ssl)
{
  X509* certP = SSL_get1_peer_certificate(ssl);

  if (certP == NULL)
    return false;

  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int  mdLen = 0;
  bool          same  = (X509_digest(certP, EVP_sha256(), md, &mdLen) == 1) && (mdLen == sizeof(pin)) && (memcmp(md, pin, sizeof(pin)) == 0);

  X509_free(certP);
  return same;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsInit - Create global SSL_CTX, load system CA certs
//
int corRestClientTlsInit(void)
{
  if (sslCtx != NULL)
    return 0;

  const SSL_METHOD* method = TLS_client_method();
  sslCtx = SSL_CTX_new(method);
  if (sslCtx == NULL)
    return -1;

  if (SSL_CTX_set_default_verify_paths(sslCtx) != 1)
  {
    // Failed to load system CA certificates - continue anyway
  }

  SSL_CTX_set_verify(sslCtx, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);

  SSL_CTX_set_min_proto_version(sslCtx, TLS1_2_VERSION);

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsConnect - Perform TLS handshake on an existing connection
//
static pthread_once_t tlsOnce = PTHREAD_ONCE_INIT;

static void tlsInitOnce(void)
{
  corRestClientTlsInit();
}

int corRestClientTlsConnect(CorRestClientConn* conn)
{
  //
  // The context made at the first TLS connection, whoever makes it: the single-request path initialised it
  // itself, the multi engine (corRestClientMulti) never did - its TLS connections all failed in a process with
  // no single request before (corCep: every https broker)
  //
  if (sslCtx == NULL)
    pthread_once(&tlsOnce, tlsInitOnce);

  if (sslCtx == NULL)
    return -1;

  SSL* ssl = SSL_new(sslCtx);
  if (ssl == NULL)
    return -1;

  SSL_set_tlsext_host_name(ssl, conn->host);
  if (pinned)
    SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);   // the pin decides, after the handshake - not the CA store, not the name
  else if (!insecure)
    SSL_set1_host(ssl, conn->host);

  if (SSL_set_fd(ssl, conn->fd) != 1)
  {
    SSL_free(ssl);
    return -1;
  }

  //
  // The socket is non-blocking: the handshake waits for it through corRestWaitFd - a poll() on a
  // thread, a yield inside a coroutine - as long as OpenSSL asks for more
  //
  while (true)
  {
    int r = SSL_connect(ssl);

    if (r == 1)
      break;

    int err = SSL_get_error(ssl, r);

    if (((err != SSL_ERROR_WANT_READ) && (err != SSL_ERROR_WANT_WRITE)) ||
        (corRestWaitFd(conn->fd, (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT, 10000, NULL) <= 0))
    {
      SSL_free(ssl);
      return -1;
    }
  }

  if (pinned && !pinMatches(ssl))
  {
    SSL_free(ssl);
    return -1;
  }

  conn->ssl = ssl;

  return 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsRead - Read from TLS connection
//
int corRestClientTlsRead(CorRestClientConn* conn, char* buf, int len)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return -1;

  //
  // A non-blocking socket: OpenSSL may need more bytes (or to write some) before it has any to give -
  // wait for that, and read again. 0 is the end of the stream only when the peer said so.
  //
  while (true)
  {
    int r = SSL_read(ssl, buf, len);

    if (r > 0)
      return r;

    int err = SSL_get_error(ssl, r);

    if (err == SSL_ERROR_ZERO_RETURN)
      return 0;

    if (((err != SSL_ERROR_WANT_READ) && (err != SSL_ERROR_WANT_WRITE)) ||
        (corRestWaitFd(conn->fd, (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT, 30000, NULL) <= 0))
      return -1;
  }
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsWrite - Write to TLS connection
//
int corRestClientTlsWrite(CorRestClientConn* conn, const char* buf, int len)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return -1;

  while (true)
  {
    int r = SSL_write(ssl, buf, len);

    if (r > 0)
      return r;

    int err = SSL_get_error(ssl, r);

    if (((err != SSL_ERROR_WANT_READ) && (err != SSL_ERROR_WANT_WRITE)) ||
        (corRestWaitFd(conn->fd, (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT, 30000, NULL) <= 0))
      return -1;
  }
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsPending - bytes OpenSSL holds already decrypted: a read needs no wait for the socket
//
int corRestClientTlsPending(CorRestClientConn* conn)
{
  return (conn->ssl != NULL) ? SSL_pending((SSL*) conn->ssl) : 0;
}



// -----------------------------------------------------------------------------
//
// corRestClientTlsClose - Shut down TLS and free SSL handle
//
void corRestClientTlsClose(CorRestClientConn* conn)
{
  SSL* ssl = (SSL*)conn->ssl;
  if (ssl == NULL)
    return;

  SSL_shutdown(ssl);
  SSL_free(ssl);
  conn->ssl = NULL;
}
