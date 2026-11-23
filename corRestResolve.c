//
// FILE            corRestResolve.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdatomic.h>                              // atomic_fetch_sub
#include <stdbool.h>                                // bool
#include <stdlib.h>                                 // calloc, free
#include <string.h>                                 // strcmp, strdup
#include <unistd.h>                                 // write, close
#include <poll.h>                                   // POLLIN
#include <pthread.h>                                // pthread_create
#include <arpa/inet.h>                              // inet_pton
#include <netdb.h>                                  // getaddrinfo, EAI_SYSTEM

#include "corBase/corLinux.h"                    // eventfd, corEventfdSignal, corEventfdClose (macOS: a pipe)
#include "corBase/corCo.h"                          // corCoCurrent

#include "corRest/corRestWait.h"                    // corRestWaitFd
#include "corRest/corRestResolve.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// Lookup - one name resolved on a thread; freed by whichever of the two lets go last
//
typedef struct Lookup
{
  char*             host;
  char*             port;
  struct addrinfo   hints;
  struct addrinfo*  resP;
  int               rc;
  int               efd;
  _Atomic int       refs;
} Lookup;

static void lookupRelease(Lookup* lP)
{
  if (atomic_fetch_sub(&lP->refs, 1) != 1)
    return;

  if (lP->resP != NULL)
    freeaddrinfo(lP->resP);
  corEventfdClose(lP->efd);
  free(lP->host);
  free(lP->port);
  free(lP);
}

static void* lookupThread(void* arg)
{
  Lookup* lP = (Lookup*) arg;

  lP->rc = getaddrinfo(lP->host, lP->port, &lP->hints, &lP->resP);
  corEventfdSignal(lP->efd);

  lookupRelease(lP);
  return NULL;
}



// -----------------------------------------------------------------------------
//
// needsNameServer - not an address, and not localhost
//
static bool needsNameServer(const char* host)
{
  unsigned char buf[16];

  if ((strcmp(host, "localhost") == 0) || (inet_pton(AF_INET, host, buf) == 1) || (inet_pton(AF_INET6, host, buf) == 1))
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// corRestResolve -
//
int corRestResolve(const char* host, const char* port, const struct addrinfo* hintsP, struct addrinfo** resPP)
{
  if ((corCoCurrent() == NULL) || (needsNameServer(host) == false))
    return getaddrinfo(host, port, hintsP, resPP);

  Lookup* lP = (Lookup*) calloc(1, sizeof(Lookup));

  if ((lP == NULL) || ((lP->efd = eventfd(0, EFD_CLOEXEC)) < 0))
  {
    free(lP);
    return getaddrinfo(host, port, hintsP, resPP);  // no thread to be had: the old way
  }

  lP->host  = strdup(host);
  lP->port  = (port != NULL) ? strdup(port) : NULL;
  lP->hints = *hintsP;
  lP->refs  = 2;                                    // this coroutine's and the thread's

  pthread_t      tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&tid, &attr, lookupThread, lP);
  pthread_attr_destroy(&attr);

  if (r != 0)
  {
    lP->refs = 1;
    lookupRelease(lP);
    return getaddrinfo(host, port, hintsP, resPP);
  }

  int rc = EAI_AGAIN;

  if (corRestWaitFd(lP->efd, POLLIN, 10000, NULL) > 0)
  {
    rc     = lP->rc;
    *resPP = lP->resP;                              // the caller's now - freeaddrinfo
    lP->resP = NULL;
  }

  lookupRelease(lP);                                // a lookup that timed out: the thread frees it when done
  return rc;
}
