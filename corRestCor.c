//
// FILE            corRestCor.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// cor:// - see corRestCor.h, and coraine's doc/cor-protocol-details.md § 5.
//
// A frame: a 16-byte header, then one tree in the cor format.
//
//   magic        4   C0 4F 52 01 - 0xC0 'O' 'R' and the format version
//   type         1   HELLO, HELLO_ACK, REQUEST, RESPONSE, ...
//   flags        1   0
//   reserved     2   0
//   correlation  4   little-endian - a REQUEST's, copied into its RESPONSE
//   length       4   little-endian - the tree's bytes
//
// A request:   { "verb": "GET", "path": "/ngsi-ld/v1/entities/urn:E1?options=keyValues",
//                "headers": { "Link": "...", ... }, "body": <tree> }
// A response:  { "status": 200, "headers": { ... }, "body": <tree> }      ("text": "..." for a body that is no tree)
//
// HELLO / HELLO_ACK: { "version": 1, "terms": <core-term table size>, "namespaces": <fixed namespaces> }, sent
// with no codec and no tables - before them there is nothing to agree on. After it, each direction of the
// connection has its own tables, preloaded with the fixed namespaces both sides have, and kept for the life
// of the connection.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint8_t, uint32_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // memcpy, memcmp, strcmp, strchr, strncmp
#include <errno.h>                                    // errno, EINTR
#include <stdatomic.h>                                // atomic_fetch_add, atomic_exchange
#include <unistd.h>                                   // close, read, write
#include <time.h>                                     // clock_gettime
#include <poll.h>                                     // poll
#include <pthread.h>                                  // pthread_create
#include <netdb.h>                                    // getaddrinfo
#include <sys/socket.h>                               // socket, setsockopt, accept, bind, listen
#include <netinet/in.h>                               // sockaddr_in6
#include <netinet/tcp.h>                              // TCP_NODELAY
#include <fcntl.h>                                    // fcntl, O_NONBLOCK

#include "corBase/corLinux.h"                    // epoll_create1, epoll_ctl, epoll_wait, MSG_NOSIGNAL (macOS: kqueue)
#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc, corAllocStrdup
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corTree/corTreeBin.h"                       // corTreeBinEncode, corTreeBinDecode
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse

#include "corRest/corRestResolve.h"             // corRestResolve
#include "corRest/corRestWait.h"                     // corRestWaitFd, corRestCoLoopInit
#include "corBase/corCo.h"                            // corCoCreate
#include "corBase/corCoLoop.h"                        // corCoLoopResume, corCoLoopEvent, corCoLoopExpire, corCoLoopTimeoutMs
#include "corRest/corRest.h"                          // corRest, corRestP
#include "corRest/CorRestState.h"                     // CorRestState
#include "corRest/corRestHooks.h"                     // CorRestHook, CorRestUserData*Hook
#include "corRest/corRestStateInit.h"                 // corRestStateInit, corRestStateRelease, corRestUrlPathNormalize
#include "corRest/corRestUrlValueEncode.h"            // corRestUrlValueDecode
#include "corRest/corRestBackend.h"                   // corRestHttpHeaderAdd, corRestUriParamsParse, corRestProcessRequest, corRestResponseHeaderVBuild
#include "corRest/corRestCor.h"                       // Own interface

extern CorRestUserDataAllocHook  corRestUserDataAllocHookF;
extern CorRestUserDataFreeHook   corRestUserDataFreeHookF;
extern CorRestHook               corRestPostResponseHook;
extern CorRestFinishInlineHook   corRestFinishInlineHookF;



// -----------------------------------------------------------------------------
//
// The frame
//
enum
{
  FRAME_HELLO       = 1,
  FRAME_HELLO_ACK   = 2,
  FRAME_REQUEST     = 3,
  FRAME_RESPONSE    = 4,
  FRAME_ERROR       = 7
};

static const uint8_t frameMagic[4] = { 0xC0, 'O', 'R', 1 };

enum
{
  FRAME_HEADER_LEN  = 16,
  FRAME_MAX         = 64 * 1024 * 1024,         // a tree larger than this is refused
  NAMESPACES_MAX    = 1024,                     // the connection tables' caps - both ends use the same
  NAMES_MAX         = 8192
};



// -----------------------------------------------------------------------------
//
// The codec, set once by the application
//
static const CorBinCodec*  codecP       = NULL;
static const char**        namespaceV   = NULL;
static int                 namespaces   = 0;
static int                 termCount    = 0;



// -----------------------------------------------------------------------------
//
// CorConn - one cor:// connection, either end
//
typedef struct CorConn
{
  int           fd;
  CorBinTables  out;            // what this end has defined
  CorBinTables  in;             // the mirror of what the peer has defined
  bool          tables;         // both initialised
  uint32_t      correlation;
  char          rbuf[16 * 1024];  // what the socket gave beyond what has been consumed - one read() usually brings a whole frame
  int           rpos;
  int           rlen;
  char          peer[160];      // host:port, for the client cache and the log - host (128) + port (16)
} CorConn;



// -----------------------------------------------------------------------------
//
// corRestCorInit -
//
void corRestCorInit(const CorBinCodec* _codecP, const char** _namespaceV, int _namespaces, int _termCount)
{
  codecP     = _codecP;
  namespaceV = _namespaceV;
  namespaces = _namespaces;
  termCount  = _termCount;
}



// -----------------------------------------------------------------------------
//
// ioWait - until fd is readable/writable or timeoutMs passes
//
static bool ioWait(int fd, short events, int timeoutMs)
{
  return corRestWaitFd(fd, events, timeoutMs, NULL) > 0;
}



// -----------------------------------------------------------------------------
//
// writeAll -
//
static bool writeAll(int fd, const void* p, int n, int timeoutMs)
{
  const char* cP = p;

  (void) timeoutMs;                                  // a full send buffer waits below (ioWait, POLLOUT) - client and server sockets are non-blocking

  while (n > 0)
  {
    ssize_t w = send(fd, cP, n, MSG_NOSIGNAL);
    if (w < 0)
    {
      if (errno == EINTR)
        continue;

      //
      // A server socket is non-blocking: a full send buffer waits here, briefly - a v1 simplification
      //
      if (((errno == EAGAIN) || (errno == EWOULDBLOCK)) && (ioWait(fd, POLLOUT, 30000) == true))
        continue;

      return false;
    }

    cP += w;
    n  -= w;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// connRead - exactly n bytes, from the connection's buffer first and the socket after
//
// One read() asks for as much as the buffer holds, so a frame - header and tree - normally arrives in
// a single system call. poll() only when there is nothing buffered AND a timeout applies; timeoutMs < 0
// (a server waiting for its next request) is a plain blocking read.
//
static bool connRead(CorConn* cP, void* p, int n, int timeoutMs)
{
  char* outP = p;

  while (n > 0)
  {
    int avail = cP->rlen - cP->rpos;

    if (avail > 0)
    {
      int take = (avail < n) ? avail : n;

      memcpy(outP, &cP->rbuf[cP->rpos], take);
      cP->rpos += take;
      outP     += take;
      n        -= take;
      continue;
    }

    if ((timeoutMs >= 0) && (ioWait(cP->fd, POLLIN, timeoutMs) == false))
    {
      errno = ETIMEDOUT;
      return false;
    }

    //
    // A large remainder goes straight to its destination; anything else through the buffer
    //
    ssize_t r = (n >= (int) sizeof(cP->rbuf)) ? read(cP->fd, outP, n) : read(cP->fd, cP->rbuf, sizeof(cP->rbuf));

    if (r < 0)
    {
      // EAGAIN: a non-blocking socket that was ready and then was not - wait again (the client's)
      if ((errno == EINTR) || (((errno == EAGAIN) || (errno == EWOULDBLOCK)) && (timeoutMs >= 0)))
        continue;
      return false;
    }

    if (r == 0)                                      // the peer closed
    {
      errno = ECONNRESET;
      return false;
    }

    if (n >= (int) sizeof(cP->rbuf))
    {
      outP += r;
      n    -= r;
    }
    else
    {
      cP->rpos = 0;
      cP->rlen = (int) r;
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// frameSend - a tree, encoded with the connection's outgoing tables (or none, for HELLO)
//
static bool frameSend(CorConn* cP, uint8_t type, uint32_t correlation, CorNode* treeP, int timeoutMs)
{
  CorBinBuffer frame = { NULL, 0, 0 };
  bool         plain = (type == FRAME_HELLO) || (type == FRAME_HELLO_ACK);
  uint8_t      header[FRAME_HEADER_LEN] = { 0 };

  //
  // Header and tree in ONE buffer, so one send(): the header's length is patched in after the encoding
  //
  frame.buf  = malloc(4096);
  frame.size = (frame.buf != NULL) ? 4096 : 0;

  if (frame.buf == NULL)
    return false;

  memcpy(frame.buf, header, FRAME_HEADER_LEN);
  frame.len = FRAME_HEADER_LEN;

  if (corTreeBinEncode(treeP, plain ? NULL : codecP, plain ? NULL : &cP->out, &frame) == false)
  {
    free(frame.buf);
    return false;
  }

  uint32_t bodyLen = frame.len - FRAME_HEADER_LEN;

  memcpy(frame.buf, frameMagic, 4);
  frame.buf[4] = type;
  memcpy(&frame.buf[8],  &correlation, 4);             // little-endian, as the hosts are
  memcpy(&frame.buf[12], &bodyLen,     4);

  bool ok = writeAll(cP->fd, frame.buf, frame.len, timeoutMs);

  free(frame.buf);
  return ok;
}



// -----------------------------------------------------------------------------
//
// frameRecv - a frame's header and its body; the body from allocP, or malloc'd when allocP is NULL
//
static bool frameRecv(CorConn* cP, uint8_t* typeP, uint32_t* correlationP, char** bufP, int* lenP, CorAlloc* allocP, int timeoutMs, const char** errorP)
{
  uint8_t header[FRAME_HEADER_LEN];

  if (connRead(cP, header, FRAME_HEADER_LEN, timeoutMs) == false)
  {
    *errorP = (errno == ETIMEDOUT) ? "timed out" : "connection closed";
    return false;
  }

  if (memcmp(header, frameMagic, 4) != 0)
  {
    *errorP = "not a cor:// frame (bad magic)";
    return false;
  }

  uint32_t len;

  memcpy(correlationP, &header[8],  4);
  memcpy(&len,         &header[12], 4);

  if (len > FRAME_MAX)
  {
    *errorP = "frame too large";
    return false;
  }

  char* buf = (allocP != NULL) ? corAlloc(allocP, len + 1) : malloc(len + 1);
  if (buf == NULL)
  {
    *errorP = "out of memory";
    return false;
  }

  if (connRead(cP, buf, len, timeoutMs) == false)
  {
    if (allocP == NULL)
      free(buf);
    *errorP = "connection closed in the middle of a frame";
    return false;
  }

  *typeP = header[4];
  *bufP  = buf;
  *lenP  = (int) len;

  return true;
}



// -----------------------------------------------------------------------------
//
// tablesOpen - after HELLO: both directions' tables, preloaded with the namespaces both ends have
//
static bool tablesOpen(CorConn* cP, int peerNamespaces)
{
  int preload = (peerNamespaces < namespaces) ? peerNamespaces : namespaces;

  if ((corTreeBinTablesInit(&cP->out, NAMESPACES_MAX, NAMES_MAX) == false) || (corTreeBinTablesInit(&cP->in, NAMESPACES_MAX, NAMES_MAX) == false))
    return false;

  cP->tables = true;

  return corTreeBinTablesPreload(&cP->out, namespaceV, preload) && corTreeBinTablesPreload(&cP->in, namespaceV, preload);
}



// -----------------------------------------------------------------------------
//
// connClose -
//
static void connClose(CorConn* cP)
{
  if (cP->fd >= 0)
    close(cP->fd);
  cP->fd   = -1;
  cP->rpos = 0;
  cP->rlen = 0;

  if (cP->tables == true)
  {
    corTreeBinTablesRelease(&cP->out);
    corTreeBinTablesRelease(&cP->in);
    cP->tables = false;
  }
}



// -----------------------------------------------------------------------------
//
// helloTree - { "version": 1, "terms": N, "namespaces": M }
//
static CorNode* helloTree(CorAlloc* kaP)
{
  CorNode* helloP = corTreeObject(kaP, NULL);

  corTreeChildAdd(helloP, corTreeInteger(kaP, "version",    1));
  corTreeChildAdd(helloP, corTreeInteger(kaP, "terms",      termCount));
  corTreeChildAdd(helloP, corTreeInteger(kaP, "namespaces", namespaces));

  return helloP;
}



// -----------------------------------------------------------------------------
//
// helloCheck - a peer's HELLO: same version, same term table; its namespace count into *namespacesP
//
static const char* helloCheck(CorNode* helloP, int* namespacesP)
{
  CorNode* versionP    = (helloP != NULL) ? corTreeLookup(helloP, "version")    : NULL;
  CorNode* termsP      = (helloP != NULL) ? corTreeLookup(helloP, "terms")      : NULL;
  CorNode* namespacesP_ = (helloP != NULL) ? corTreeLookup(helloP, "namespaces") : NULL;

  if ((versionP == NULL) || (versionP->type != CorInt) || (versionP->value.i != 1))
    return "unsupported cor:// version";

  //
  // v1: the term tables must be the same. Writing a term the peer does not have as a string is the
  // way to relax this - an encoder option, no format change.
  //
  if ((termsP == NULL) || (termsP->type != CorInt) || (termsP->value.i != termCount))
    return "the peer's core-term table differs from ours";

  *namespacesP = ((namespacesP_ != NULL) && (namespacesP_->type == CorInt)) ? (int) namespacesP_->value.i : 0;

  return NULL;
}



// =============================================================================
//
// SERVER
//
// Event loops, as HTTP has: a listener hands each connection to a loop in turn, and a loop reads
// requests off its connections with epoll. A request that cannot block - the same check HTTP makes
// (corRestAsyncDispatch) - runs right there on the loop. One that can (a forward, waiting on its
// peer) moves its connection off the loops for good, to a thread of its own (connDedicate): those are
// other brokers' forwarding connections, and a hop to the worker pool on every request would cost
// two thread wakeups each. A post-response phase that may block still goes to the worker pool
// (CorRestState.finishF), as HTTP's does.
//
// A connection is armed once, for good, and multiplexed: any number of requests in flight on it,
// answered in the order they finish (ServerConn).
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// ServerConn - a CorConn, plus what a server needs to assemble frames off a non-blocking socket
//
// Multiplexed (coraine doc/cor-protocol-details.md § 5.3): the connection is armed once, for good, and the loop
// reads every frame that comes - a request in, its state of its own (ServerReq), and on to the next;
// they run at once (inline, or as coroutines of the loop) and their responses go out in the order
// they finish. Every request's frame is DECODED when it arrives, and every response ENCODED when it
// is queued - both on the loop's thread, in wire order, which is what keeps the two ends' string
// tables in step.
//
// Lifetime: a reference for the loop (dropped when the connection dies) and one per request in
// flight - a request may finish on a worker - whoever lets go last frees it.
//
typedef struct ServerReq ServerReq;

typedef struct ServerConn
{
  CorConn            conn;
  int                loopFd;           // the epoll instance the connection belongs to
  bool               helloDone;
  uint8_t            hdr[FRAME_HEADER_LEN];
  int                hdrHave;
  char*              body;             // the frame being assembled
  uint32_t           bodyLen;
  uint32_t           bodyHave;
  uint32_t           correlation;
  CorAlloc           ka;               // the HELLO's
  char               kaBuf[2048];
  bool               dedicated;        // moved off the loops to a thread of its own (connDedicate)
  ServerReq*         firstReqP;        // ... and the request that moved it there
  bool               dead;             // the socket is closed (the loop's reference dropped)
  _Atomic int        refs;
  int                pending;          // requests started whose response is not queued yet (the loop's thread)
  CorBinBuffer       outQ;             // responses not yet written - the loop flushes it on EPOLLOUT
  int                outPos;
  bool               outWatch;         // EPOLLOUT armed
  _Atomic(ServerReq*) spare;           // one finished request's memory, for the next
} ServerConn;



// -----------------------------------------------------------------------------
//
// ServerReq - one request in flight: its frame, the memory its decoded tree lives in, its state
//
struct ServerReq
{
  ServerConn*    scP;
  uint32_t       correlation;
  char*          body;             // the request's tree points into it
  uint32_t       bodyLen;
  CorRestState*  stateP;
  CorAlloc       ka;
  char           kaBuf[16 * 1024];
};



// -----------------------------------------------------------------------------
//
// connWatch - what the loop waits for on the connection: input always, output while the queue is not empty
//
static void connWatch(ServerConn* scP, bool output)
{
  struct epoll_event ev;

  ev.events   = EPOLLIN | EPOLLRDHUP | (output ? EPOLLOUT : 0);
  ev.data.ptr = scP;

  epoll_ctl(scP->loopFd, EPOLL_CTL_MOD, scP->conn.fd, &ev);
  scP->outWatch = output;
}



// -----------------------------------------------------------------------------
//
// connUnref - one reference fewer; the last frees the connection
//
static void connUnref(ServerConn* scP)
{
  if (atomic_fetch_sub(&scP->refs, 1) != 1)
    return;

  connClose(&scP->conn);
  free(scP->body);
  free(scP->outQ.buf);
  corAllocBufferReset(&scP->ka, false);

  ServerReq* spareP = atomic_exchange(&scP->spare, NULL);
  if (spareP != NULL)
  {
    corAllocBufferReset(&spareP->ka, false);
    free(spareP);
  }

  free(scP);
}



// -----------------------------------------------------------------------------
//
// connDie - the peer is gone, or spoke nonsense: the socket closed, the loop's reference dropped
//
// The requests still running keep the connection; their responses go nowhere.
//
static void connDie(ServerConn* scP)
{
  if (scP->dead == true)
    return;

  scP->dead = true;
  epoll_ctl(scP->loopFd, EPOLL_CTL_DEL, scP->conn.fd, NULL);
  connClose(&scP->conn);
  connUnref(scP);
}



// -----------------------------------------------------------------------------
//
// outFlush - write what the queue holds, as far as the socket takes it; the rest on EPOLLOUT
//
static void outFlush(ServerConn* scP)
{
  while (scP->outPos < scP->outQ.len)
  {
    ssize_t w = send(scP->conn.fd, &scP->outQ.buf[scP->outPos], scP->outQ.len - scP->outPos, MSG_NOSIGNAL);

    if (w > 0)
    {
      scP->outPos += (int) w;
      continue;
    }

    if ((w < 0) && (errno == EINTR))
      continue;

    if ((w < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
    {
      if (scP->outWatch == false)
        connWatch(scP, true);
      return;
    }

    COR_W("cor:// %s: the response could not be sent - closing", scP->conn.peer);
    connDie(scP);
    return;
  }

  scP->outPos     = 0;
  scP->outQ.len   = 0;

  if (scP->outWatch == true)
    connWatch(scP, false);
}



// -----------------------------------------------------------------------------
//
// frameQueue - a frame, encoded at the end of the queue with the connection's tables, and flushed
//
// On the loop's thread only: the encoding order is the wire order.
//
static bool frameQueue(ServerConn* scP, uint8_t type, uint32_t correlation, CorNode* treeP)
{
  if (scP->dead == true)
    return false;

  CorBinBuffer* qP    = &scP->outQ;
  int           start = qP->len;

  if (qP->size - qP->len < FRAME_HEADER_LEN)
  {
    int   size = (qP->size == 0) ? 16384 : qP->size * 2;
    char* buf  = realloc(qP->buf, size);

    if (buf == NULL)
      return false;
    qP->buf  = buf;
    qP->size = size;
  }

  qP->len += FRAME_HEADER_LEN;

  if (corTreeBinEncode(treeP, codecP, &scP->conn.out, qP) == false)
  {
    qP->len = start;
    return false;
  }

  uint32_t bodyLen = qP->len - start - FRAME_HEADER_LEN;
  char*    h       = &qP->buf[start];

  memset(h, 0, FRAME_HEADER_LEN);
  memcpy(h, frameMagic, 4);
  h[4] = type;
  memcpy(&h[8],  &correlation, 4);
  memcpy(&h[12], &bodyLen,     4);

  if (scP->outWatch == false)                        // a flush already waiting for EPOLLOUT sends this too
    outFlush(scP);

  return true;
}



// -----------------------------------------------------------------------------
//
// frameAssemble - consume what the socket has; true when a whole frame is in, false on EAGAIN (wait)
//
// *deadP set when the peer closed, or the bytes are not a cor:// frame.
//
static bool frameAssemble(ServerConn* scP, bool* deadP)
{
  CorConn* cP = &scP->conn;

  *deadP = false;

  while (true)
  {
    //
    // From the read buffer first
    //
    while (cP->rpos < cP->rlen)
    {
      if (scP->hdrHave < FRAME_HEADER_LEN)
      {
        int take = FRAME_HEADER_LEN - scP->hdrHave;
        if (take > cP->rlen - cP->rpos)
          take = cP->rlen - cP->rpos;

        memcpy(&scP->hdr[scP->hdrHave], &cP->rbuf[cP->rpos], take);
        scP->hdrHave += take;
        cP->rpos     += take;

        if (scP->hdrHave < FRAME_HEADER_LEN)
          continue;

        if (memcmp(scP->hdr, frameMagic, 4) != 0)
        {
          COR_W("cor:// %s: not a cor:// frame (bad magic) - closing", cP->peer);
          *deadP = true;
          return false;
        }

        memcpy(&scP->correlation, &scP->hdr[8],  4);
        memcpy(&scP->bodyLen,     &scP->hdr[12], 4);

        if ((scP->bodyLen > FRAME_MAX) || ((scP->body = malloc(scP->bodyLen + 1)) == NULL))
        {
          COR_W("cor:// %s: frame too large - closing", cP->peer);
          *deadP = true;
          return false;
        }
        scP->bodyHave = 0;
      }

      int take = scP->bodyLen - scP->bodyHave;
      if (take > cP->rlen - cP->rpos)
        take = cP->rlen - cP->rpos;

      memcpy(&scP->body[scP->bodyHave], &cP->rbuf[cP->rpos], take);
      scP->bodyHave += take;
      cP->rpos      += take;

      if (scP->bodyHave == scP->bodyLen)
        return true;
    }

    if ((scP->hdrHave == FRAME_HEADER_LEN) && (scP->bodyHave == scP->bodyLen))
      return true;                                   // an empty tree - never sent, but not an error

    //
    // Then the socket - a large remainder straight into the frame
    //
    ssize_t r;

    if ((scP->hdrHave == FRAME_HEADER_LEN) && (scP->bodyLen - scP->bodyHave >= sizeof(cP->rbuf)))
    {
      r = read(cP->fd, &scP->body[scP->bodyHave], scP->bodyLen - scP->bodyHave);
      if (r > 0)
      {
        scP->bodyHave += r;
        if (scP->bodyHave == scP->bodyLen)
          return true;
        continue;
      }
    }
    else
    {
      r = read(cP->fd, cP->rbuf, sizeof(cP->rbuf));
      if (r > 0)
      {
        cP->rpos = 0;
        cP->rlen = (int) r;
        continue;
      }
    }

    if (r == 0)
    {
      *deadP = true;
      return false;
    }

    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return false;

    if (errno != EINTR)
    {
      *deadP = true;
      return false;
    }
  }
}



// -----------------------------------------------------------------------------
//
// frameTake - the frame just assembled is the caller's: its body, and the assembly reset for the next
//
static char* frameTake(ServerConn* scP, uint32_t* lenP, uint32_t* correlationP)
{
  char* body = scP->body;

  *lenP         = scP->bodyLen;
  *correlationP = scP->correlation;

  scP->body     = NULL;
  scP->hdrHave  = 0;
  scP->bodyLen  = 0;
  scP->bodyHave = 0;

  return body;
}



// -----------------------------------------------------------------------------
//
// reqGet / reqRelease - a request's memory: the connection's spare, or a new one
//
static ServerReq* reqGet(ServerConn* scP)
{
  ServerReq* reqP = atomic_exchange(&scP->spare, NULL);

  if (reqP == NULL)
  {
    if ((reqP = malloc(sizeof(ServerReq))) == NULL)
      return NULL;
    corAllocBufferInit(&reqP->ka, reqP->kaBuf, sizeof(reqP->kaBuf), 64 * 1024, NULL, "cor:// request");
  }

  reqP->scP    = scP;
  reqP->stateP = NULL;
  reqP->body   = NULL;

  atomic_fetch_add(&scP->refs, 1);
  return reqP;
}

static void reqRelease(ServerReq* reqP)
{
  ServerConn* scP      = reqP->scP;
  ServerReq*  expected = NULL;

  free(reqP->body);
  reqP->body = NULL;
  corAllocBufferReset(&reqP->ka, true);

  if (atomic_compare_exchange_strong(&scP->spare, &expected, reqP) == false)
  {
    corAllocBufferReset(&reqP->ka, false);
    free(reqP);
  }

  connUnref(scP);                                    // after the spare: the last reference frees it too
}



// -----------------------------------------------------------------------------
//
// requestFinish - the post-response phase, and the request's release
//
// Runs where the response was sent, or - when the post-response phase may block and that was the
// loop - on a worker (CorRestState.finishF) or as a coroutine, bound to the request's state.
//
static void requestFinish(CorRestState* stateP)
{
  ServerReq* reqP = (ServerReq*) stateP->connection;

  corRestP = stateP;
  corRestPostResponseHook();
  corRestStateRelease();
  if ((corRestUserDataFreeHookF != NULL) && (corRest.userData != NULL))
    corRestUserDataFreeHookF(corRest.userData);
  free(stateP);
  corRestP = NULL;

  reqRelease(reqP);
}



// -----------------------------------------------------------------------------
//
// requestRespond - the response, as the tree the service routine built; then the finish
//
// onLoop: called on the event loop (inline, or a coroutine of it) - the response is queued there. Else
// a dedicated connection's thread, which writes it itself. The post-response phase then runs here only
// when the application's finish-inline hook allows it, like HTTP's; else as a coroutine, or on a worker.
//
static bool finishCoroutineStart(CorRestState* stateP);

static void requestRespond(CorRestState* stateP, bool onLoop)
{
  ServerReq*  reqP = (ServerReq*) stateP->connection;
  ServerConn* scP  = reqP->scP;

  corRestP = stateP;

  CorNode*         responseP = corTreeObject(corRest.kallocP, NULL);
  CorNode*         outHdrP   = corTreeObject(corRest.kallocP, "headers");
  CorRestKeyValue  hv[64];
  int              hc        = corRestResponseHeaderVBuild(hv, 64);

  corTreeChildAdd(responseP, corTreeInteger(corRest.kallocP, "status", corRest.out.httpStatusCode));

  for (int i = 0; i < hc; i++)
    corTreeChildAdd(outHdrP, corTreeString(corRest.kallocP, hv[i].key, hv[i].value));
  corTreeChildAdd(responseP, outHdrP);

  if (corRest.out.responseTree != NULL)
  {
    CorNode* treeP = corRest.out.responseTree;

    treeP->name = (char*) "body";
    treeP->next = NULL;
    corTreeChildAdd(responseP, treeP);
  }
  else if ((corRest.out.payload != NULL) && (corRest.out.payloadSize > 0))
    corTreeChildAdd(responseP, corTreeString(corRest.kallocP, "text", corRest.out.payload));

  if (scP->dedicated == true)
  {
    if (frameSend(&scP->conn, FRAME_RESPONSE, reqP->correlation, responseP, 30000) == false)
      COR_W("cor:// %s: the response could not be sent", scP->conn.peer);
  }
  else
  {
    if ((frameQueue(scP, FRAME_RESPONSE, reqP->correlation, responseP) == false) && (scP->dead == false))
      COR_W("cor:// %s: the response could not be encoded", scP->conn.peer);
    scP->pending -= 1;
  }

  if ((onLoop == false) || ((corRestFinishInlineHookF != NULL) && (corRestFinishInlineHookF() == true)))
  {
    requestFinish(stateP);
    return;
  }

  //
  // A phase that can wait, as a coroutine: in the request's own if it is one, or in one of its own
  //
  if (corRestFinishCoroutineAllowed() == true)
  {
    if (corCoCurrent() != NULL)
    {
      requestFinish(stateP);
      return;
    }

    if (finishCoroutineStart(stateP) == true)
      return;
  }

  stateP->asyncFinishing = true;
  stateP->finishF        = requestFinish;
  corRestP               = NULL;
  corRestAsyncEnqueue(stateP);
}



// =============================================================================
//
// Coroutines on the loops (coraine doc/coroutines.md § 1)
//
// A request that can wait runs as a coroutine on the loop that read it; where it would wait for a
// socket - corRestWaitFd, inside a client - the loop's epoll watches it and the coroutine yields
// (corBase corCoLoop, bound to corRest by corRestCoLoopInit).
//
// =============================================================================

enum { CO_MAX = 1024 };

static __thread int coRunning = 0;                   // this loop's coroutines alive - at CO_MAX a waiting request takes a thread



// -----------------------------------------------------------------------------
//
// requestCoroutine - a request that can wait, run on the loop as a coroutine
//
static void requestCoroutine(void* arg)
{
  ServerReq* reqP = (ServerReq*) arg;

  corRestP = reqP->stateP;
  corRestProcessRequest();
  requestRespond(reqP->stateP, true);

  coRunning -= 1;
}



// -----------------------------------------------------------------------------
//
// finishCoroutine - the post-response phase of an inline request, when it can wait: a coroutine
//
static void finishCoroutine(void* arg)
{
  requestFinish((CorRestState*) arg);
  coRunning -= 1;
}

static bool finishCoroutineStart(CorRestState* stateP)
{
  CorCo* coP = (coRunning < CO_MAX) ? corCoCreate(finishCoroutine, stateP) : NULL;

  if (coP == NULL)
    return false;

  coRunning += 1;
  corRestP   = NULL;
  corCoLoopResume(coP);
  return true;
}



static bool requestStart(ServerConn* scP, char* body, uint32_t bodyLen, uint32_t correlation);



// -----------------------------------------------------------------------------
//
// dedicatedThread - a connection of its own: the request that moved it here, then every later one
//
// A blocking socket and a blocking read for the next request, as cor:// v1 served every connection,
// one request at a time. The connection's buffered bytes are already in scP->conn.rbuf, so nothing
// that arrived is lost.
//
static void* dedicatedThread(void* arg)
{
  ServerConn* scP  = (ServerConn*) arg;
  ServerReq*  reqP = scP->firstReqP;

  corRestP = reqP->stateP;
  corRestProcessRequest();
  requestRespond(reqP->stateP, false);

  while (true)
  {
    uint8_t     type;
    const char* error;
    char*       buf;
    int         len;
    uint32_t    correlation;

    if (frameRecv(&scP->conn, &type, &correlation, &buf, &len, NULL, -1, &error) == false)
      break;

    if (type != FRAME_REQUEST)
    {
      free(buf);
      COR_W("cor:// %s: unexpected frame type %d - closing", scP->conn.peer, type);
      break;
    }

    if (requestStart(scP, buf, (uint32_t) len, correlation) == false)
      break;
  }

  connClose(&scP->conn);
  connUnref(scP);                                    // the loop's reference, the thread's now
  return NULL;
}



// -----------------------------------------------------------------------------
//
// connDedicate - off the loops, for good: a thread of its own for this connection
//
// A request that cannot run on a loop - no coroutine to be had (the application's hook says no: a
// database driver that blocks; or the loop is at its cap) - would otherwise stop the loop. Only a
// connection with nothing else in flight can move: its other requests' responses are the loop's to
// write.
//
static bool connDedicate(ServerConn* scP, ServerReq* reqP)
{
  epoll_ctl(scP->loopFd, EPOLL_CTL_DEL, scP->conn.fd, NULL);
  fcntl(scP->conn.fd, F_SETFL, fcntl(scP->conn.fd, F_GETFL, 0) & ~O_NONBLOCK);
  scP->dedicated = true;
  scP->firstReqP = reqP;

  pthread_t      tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&tid, &attr, dedicatedThread, scP);
  pthread_attr_destroy(&attr);

  return (r == 0);
}



// -----------------------------------------------------------------------------
//
// requestStart - a request frame is in: its state, and then the loop or the connection's own thread
//
// The same sequence as corRestProcessInProcess - a fresh state, the userData hook, URI params,
// headers - except that the body is a tree already, and the response is not rendered. Takes body.
//
static bool requestStart(ServerConn* scP, char* body, uint32_t bodyLen, uint32_t correlation)
{
  ServerReq* reqP = reqGet(scP);

  if (reqP == NULL)
  {
    free(body);
    return false;
  }

  reqP->body        = body;
  reqP->bodyLen     = bodyLen;
  reqP->correlation = correlation;

  const char* error;
  CorNode*    requestP = corTreeBinDecode(reqP->body, reqP->bodyLen, codecP, &scP->conn.in, &reqP->ka, &error);
  CorNode*    verbP    = (requestP != NULL) ? corTreeLookup(requestP, "verb")    : NULL;
  CorNode*    pathP    = (requestP != NULL) ? corTreeLookup(requestP, "path")    : NULL;
  CorNode*    headersP = (requestP != NULL) ? corTreeLookup(requestP, "headers") : NULL;
  CorNode*    bodyP    = (requestP != NULL) ? corTreeLookup(requestP, "body")    : NULL;

  if (requestP == NULL)
    COR_W("cor:// %s: undecodable request (%s) - closing", scP->conn.peer, error);
  else if ((verbP == NULL) || (verbP->type != CorString) || (pathP == NULL) || (pathP->type != CorString))
    COR_W("cor:// %s: a request without verb or path - closing", scP->conn.peer);

  CorRestState* stateP = ((verbP != NULL) && (verbP->type == CorString) && (pathP != NULL) && (pathP->type == CorString)) ? (CorRestState*) malloc(sizeof(CorRestState)) : NULL;

  if (stateP == NULL)
  {
    reqRelease(reqP);
    return false;
  }

  corRestP = stateP;
  corRestStateInit(reqP, pathP->value.s, verbP->value.s);

  //
  // The request's time, as each HTTP backend sets it: createdAt/modifiedAt, TRoE and notification times
  // are all this one instant. Missing, every entity written over cor:// was created in 1970.
  //
  struct timespec ts;
  struct timespec tsM;

  clock_gettime(CLOCK_REALTIME,  &ts);
  clock_gettime(CLOCK_MONOTONIC, &tsM);
  corRest.requestStartTime     = (uint64_t) ts.tv_sec  * 1000000000ULL + (uint64_t) ts.tv_nsec;
  corRest.requestStartTimeMono = (uint64_t) tsM.tv_sec * 1000000000ULL + (uint64_t) tsM.tv_nsec;

  //
  // The path travels as the client wrote it, percent-encoded - decoded here as an HTTP backend
  // decodes it (corRestStateInit has split off the query already, so a '%3F' stays in the path)
  //
  corRestUrlValueDecode(corRest.in.urlPath);
  corRestUrlPathNormalize();

  if (corRestUserDataAllocHookF != NULL)
    corRest.userData = corRestUserDataAllocHookF();

  corRestUriParamsParse();

  if ((headersP != NULL) && (headersP->type == CorObject))
  {
    for (CorNode* hP = headersP->value.head; hP != NULL; hP = hP->next)
    {
      if (hP->type == CorString)
        corRestHttpHeaderAdd(hP->name, hP->value.s);
    }
  }

  if (bodyP != NULL)
  {
    bodyP->name = NULL;
    bodyP->next = NULL;
    corRest.in.requestTree = bodyP;
  }

  corRest.out.noRender = true;
  stateP->shard        = 0;
  reqP->stateP         = stateP;

  //
  // A dedicated connection's thread runs everything itself. On a loop: inline when it cannot block,
  // else as a coroutine of the loop, which yields wherever it waits. Without a coroutine (the
  // application said no, or the loop is at its cap): a thread of its own for the connection - when
  // nothing else of it is in flight; else this one runs right here, on the loop.
  //
  if (scP->dedicated == true)
  {
    corRestProcessRequest();
    requestRespond(stateP, false);
    return true;
  }

  scP->pending += 1;

  if (corRestAsyncDispatch() == true)
  {
    CorCo* coP = ((coRunning < CO_MAX) && (corRestCoroutineAllowed() == true)) ? corCoCreate(requestCoroutine, reqP) : NULL;

    corRestP = NULL;

    if (coP != NULL)
    {
      coRunning += 1;
      corCoLoopResume(coP);
      return true;
    }

    if ((scP->pending == 1) && (scP->outQ.len == 0))
    {
      scP->pending = 0;
      return connDedicate(scP, reqP);
    }

    corRestP = stateP;
  }

  corRestProcessRequest();
  requestRespond(stateP, true);
  return true;
}



// -----------------------------------------------------------------------------
//
// helloAnswer - the connection's first frame
//
static bool helloAnswer(ServerConn* scP)
{
  const char* error;
  int         peerNamespaces = 0;
  CorNode*    helloP         = corTreeBinDecode(scP->body, scP->bodyLen, NULL, NULL, &scP->ka, &error);
  const char* why            = (helloP == NULL) ? "not a HELLO" : helloCheck(helloP, &peerNamespaces);

  if ((scP->hdr[4] != FRAME_HELLO) || (why != NULL))
  {
    COR_W("cor:// %s: refused: %s", scP->conn.peer, (why != NULL) ? why : "the first frame is not HELLO");
    return false;
  }

  //
  // HELLO_ACK encoded without tables, as HELLO - the connection's tables are opened after it
  //
  bool ok = (frameSend(&scP->conn, FRAME_HELLO_ACK, scP->correlation, helloTree(&scP->ka), 10000) == true) &&
            (tablesOpen(&scP->conn, peerNamespaces) == true);

  uint32_t len;
  uint32_t correlation;

  free(frameTake(scP, &len, &correlation));
  corAllocBufferReset(&scP->ka, true);
  scP->helloDone = true;

  if (ok == true)
    COR_V("cor:// %s: connected", scP->conn.peer);

  return ok;
}



// -----------------------------------------------------------------------------
//
// connInput - read every frame there is, and start each
//
static void connInput(ServerConn* scP)
{
  while ((scP->dead == false) && (scP->dedicated == false))
  {
    bool dead;

    if (frameAssemble(scP, &dead) == false)
    {
      if (dead == true)
        connDie(scP);
      return;
    }

    if (scP->helloDone == false)
    {
      if (helloAnswer(scP) == false)
        connDie(scP);
      continue;
    }

    uint32_t len;
    uint32_t correlation;
    char*    body = frameTake(scP, &len, &correlation);

    if (requestStart(scP, body, len, correlation) == false)
      connDie(scP);

    //
    // On only with bytes already buffered: what has not been read yet, the (level-triggered) epoll
    // reports - a read() here would mostly find nothing, a system call per request for an EAGAIN
    //
    if (scP->conn.rpos >= scP->conn.rlen)
      return;
  }
}



// -----------------------------------------------------------------------------
//
// serverLoop - one event loop
//
static void* serverLoop(void* arg)
{
  int                loopFd = (int) (intptr_t) arg;
  struct epoll_event evV[64];

  corRestCoLoopInit(loopFd);

  while (true)
  {
    int n = epoll_wait(loopFd, evV, 64, corCoLoopTimeoutMs());

    for (int i = 0; i < n; i++)
    {
      //
      // A coroutine's socket (the tagged pointer) - or a connection
      //
      if (corCoLoopEvent(evV[i].data.ptr, evV[i].events) == true)
        continue;

      ServerConn* scP    = (ServerConn*) evV[i].data.ptr;
      uint32_t    events = evV[i].events;

      //
      // Held across the handling: a request finishing inside it (inline) must not free the connection
      // under the loop's feet
      //
      atomic_fetch_add(&scP->refs, 1);

      if ((events & EPOLLOUT) && (scP->dead == false))
        outFlush(scP);

      if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) && (scP->dead == false))
        connInput(scP);

      connUnref(scP);
    }

    corCoLoopExpire();
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// The loops, and the listener that hands them connections
//
static int  loopFdV[16];
static int  loops    = 0;
static int  nextLoop = 0;

static void* listener(void* arg)
{
  int listenFd = (int) (intptr_t) arg;

  while (true)
  {
    struct sockaddr_storage peer;
    socklen_t               peerLen = sizeof(peer);
    int                     fd      = accept(listenFd, (struct sockaddr*) &peer, &peerLen);

    if (fd < 0)
    {
      if (errno != EINTR)
        COR_E("cor:// accept failed: %s", strerror(errno));
      continue;
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ServerConn* scP = calloc(1, sizeof(ServerConn));
    if (scP == NULL)
    {
      close(fd);
      continue;
    }

    char host[64], port[16];

    scP->conn.fd = fd;
    scP->loopFd  = loopFdV[nextLoop];
    nextLoop     = (nextLoop + 1) % loops;
    corAllocBufferInit(&scP->ka, scP->kaBuf, sizeof(scP->kaBuf), 4096, NULL, "cor:// connection");
    atomic_init(&scP->refs, 1);                      // the loop's
    atomic_init(&scP->spare, NULL);

    if (getnameinfo((struct sockaddr*) &peer, peerLen, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
      snprintf(scP->conn.peer, sizeof(scP->conn.peer), "%s:%s", host, port);

    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLRDHUP;              // armed once, for good - the loop reads every frame that comes
    ev.data.ptr = scP;

    if (epoll_ctl(scP->loopFd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
      corAllocBufferReset(&scP->ka, false);
      close(fd);
      free(scP);
    }
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// corRestCorListen -
//
bool corRestCorListen(unsigned short port, int loopCount)
{
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  int one = 1;
  int off = 0;
  setsockopt(fd, SOL_SOCKET,   SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,  &off, sizeof(off));      // IPv4 too

  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port   = htons(port);
  addr.sin6_addr   = in6addr_any;

  if ((bind(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0) || (listen(fd, 128) != 0))
  {
    COR_E("cor:// cannot listen on port %d: %s", port, strerror(errno));
    close(fd);
    return false;
  }

  loops = (loopCount < 1) ? 1 : (loopCount > 16) ? 16 : loopCount;


  for (int i = 0; i < loops; i++)
  {
    pthread_t tid;

    if (((loopFdV[i] = epoll_create1(0)) < 0) || (pthread_create(&tid, NULL, serverLoop, (void*) (intptr_t) loopFdV[i]) != 0))
    {
      COR_E("cor:// cannot start event loop %d", i);
      return false;
    }
    pthread_detach(tid);
  }

  pthread_t tid;
  if (pthread_create(&tid, NULL, listener, (void*) (intptr_t) fd) != 0)
  {
    close(fd);
    return false;
  }
  pthread_detach(tid);

  COR_V("cor:// listening on port %d, %d event loop%s", port, loops, (loops == 1) ? "" : "s");
  return true;
}



// =============================================================================
//
// CLIENT
//
// =============================================================================



// -----------------------------------------------------------------------------
//
// The client's connections - per thread, and MULTIPLEXED (coraine doc/cor-protocol-details.md § 5.3)
//
// One connection a peer, shared by every request of the thread: the coroutines of a loop each send
// theirs and wait - any number in flight - and the responses come back in the order the peer finishes
// them, matched by correlation. Nothing is shared between threads, so nothing is locked; inside the
// thread two things are taken in turn:
//
//   the WRITE turn - a frame is encoded and written whole by one coroutine at a time: the encoding
//                    adds to the connection's string tables, so encoding order must be wire order,
//                    and a write that yields on a full socket must not be interleaved with another
//   the READ turn  - one coroutine reads the connection, the first that waits and finds no reader;
//                    it decodes every frame as it comes - wire order again, for the tables - into the
//                    memory of the call it belongs to, and wakes that call's coroutine. When its own
//                    response is in, it hands the turn on to another waiting call
//
// A response nobody waits for any more (its call timed out) is still decoded - the tables need it -
// and then dropped. A thread that is not a loop's (a worker) has one flow only: the same code, never
// a turn to wait for.
//
enum { CLIENT_CONNS_MAX = 8 };

typedef struct Waiter                                // a coroutine waiting for the write turn
{
  void*           park;
  struct Waiter*  next;
} Waiter;

typedef struct MuxConn
{
  CorConn                 conn;                      // conn.fd < 0: not connected
  struct CorRestCorCall*  calls;                     // sent, response not yet in
  bool                    writing;                   // the write turn is taken (also: connecting)
  bool                    reading;                   // the read turn is taken (also: connecting)
  Waiter*                 writers;
  bool                    temporary;                 // made for one call - closed and freed after it
} MuxConn;

struct CorRestCorCall
{
  MuxConn*                mP;
  uint32_t                correlation;
  CorAlloc*               respAllocP;                // the response is decoded into it
  long long               deadline;                  // CLOCK_MONOTONIC ms
  bool                    reused;                    // sent on a connection opened before it
  bool                    done;
  const char*             error;                     // done, and failed
  CorNode*                respTreeP;
  long long               receivedMs;
  void*                   park;                      // its coroutine, parked in corRestCorWait
  struct CorRestCorCall*  next;
};

//
// Pointers, allocated on first use: a connection carries a 16 KB read buffer, and every broker
// thread would otherwise pay for eight of them in thread-local storage, cor:// or not
//
static __thread MuxConn* clientConnV[CLIENT_CONNS_MAX];



// -----------------------------------------------------------------------------
//
// clientConnsRelease - close and free the ending thread's client connections (a pthread key destructor)
//
// They are thread-local, so nothing else can reach them once the thread is gone: each one left behind
// is a MuxConn with its 16 KB read buffer, the code tables, and an open socket - which keeps the peer's
// end, and the peer's thread serving it, open as well. Threads that end are not only the broker's at
// shutdown: a cor:// connection's dedicated thread ends with its connection, and the HTTP server's
// connection threads run post-response work (the source-identity probe of a new registration) that
// forwards over cor://.
//
static pthread_key_t  clientConnsKey;
static pthread_once_t clientConnsKeyOnce = PTHREAD_ONCE_INIT;

static void clientConnsRelease(void* unused)
{
  (void) unused;

  for (int i = 0; i < CLIENT_CONNS_MAX; i++)
  {
    MuxConn* mP = clientConnV[i];

    if (mP == NULL)
      continue;

    connClose(&mP->conn);
    free(mP);
    clientConnV[i] = NULL;
  }
}

static void clientConnsKeyCreate(void)
{
  pthread_key_create(&clientConnsKey, clientConnsRelease);
}



// -----------------------------------------------------------------------------
//
// nowMs - CLOCK_MONOTONIC
//
static long long nowMs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// authority - host and port out of cor://host:port[/...]
//
static bool authority(const char* url, char* host, int hostSize, char* port, int portSize)
{
  if (strncmp(url, "cor://", 6) != 0)
    return false;

  const char* hP    = &url[6];
  const char* endP  = strchr(hP, '/');
  int         len   = (endP != NULL) ? (int) (endP - hP) : (int) strlen(hP);
  const char* colon = memchr(hP, ':', len);

  if ((colon == NULL) || (colon == hP) || (colon - hP >= hostSize) || (len - (colon - hP) - 1 >= portSize) || (len - (colon - hP) - 1 <= 0))
    return false;

  memcpy(host, hP, colon - hP);
  host[colon - hP] = 0;
  memcpy(port, colon + 1, len - (colon - hP) - 1);
  port[len - (colon - hP) - 1] = 0;

  return true;
}



// -----------------------------------------------------------------------------
//
// clientConnect - a connection to host:port, HELLO exchanged
//
static bool clientConnect(CorConn* cP, const char* host, const char* port, int timeoutMs, const char** errorP)
{
  struct addrinfo  hints;
  struct addrinfo* resP = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (corRestResolve(host, port, &hints, &resP) != 0)
  {
    *errorP = "cannot resolve the cor:// host";
    return false;
  }

  cP->fd = -1;
  for (struct addrinfo* aP = resP; (aP != NULL) && (cP->fd < 0); aP = aP->ai_next)
  {
    int fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);

    if (fd < 0)
      continue;

    //
    // Non-blocking, for good: the connect - and every later wait on the socket - is corRestWaitFd, a
    // poll() on a thread and a yield inside a coroutine, where a blocking connect would stop the loop
    //
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    int r = connect(fd, aP->ai_addr, aP->ai_addrlen);

    if ((r != 0) && (errno == EINPROGRESS) && (corRestWaitFd(fd, POLLOUT, timeoutMs, NULL) > 0))
    {
      int       err    = 0;
      socklen_t errLen = sizeof(err);

      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen);
      r = (err == 0) ? 0 : -1;
    }

    if (r == 0)
      cP->fd = fd;
    else
      close(fd);
  }
  freeaddrinfo(resP);

  if (cP->fd < 0)
  {
    *errorP = "cannot connect to the cor:// endpoint";
    return false;
  }

  int            one = 1;
  struct timeval tv  = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

  setsockopt(cP->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(cP->fd, SOL_SOCKET,  SO_SNDTIMEO, &tv,  sizeof(tv));
  cP->rpos        = 0;
  cP->rlen        = 0;
  cP->correlation = 0;

  //
  // HELLO
  //
  CorAlloc ka;
  char     kaBuf[2048];
  uint8_t  type;
  uint32_t correlation;
  char*    buf;
  int      len;
  int      peerNamespaces = 0;
  bool     ok             = false;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 4096, NULL, "cor:// hello");

  if ((frameSend(cP, FRAME_HELLO, 0, helloTree(&ka), timeoutMs) == true) &&
      (frameRecv(cP, &type, &correlation, &buf, &len, &ka, timeoutMs, errorP) == true))
  {
    CorNode*    ackP = (type == FRAME_HELLO_ACK) ? corTreeBinDecode(buf, len, NULL, NULL, &ka, errorP) : NULL;
    const char* why  = (ackP == NULL) ? "no HELLO_ACK" : helloCheck(ackP, &peerNamespaces);

    if (why != NULL)
      *errorP = why;
    else
      ok = tablesOpen(cP, peerNamespaces);
  }

  corAllocBufferReset(&ka, false);

  if (ok == false)
    connClose(cP);

  return ok;
}



// -----------------------------------------------------------------------------
//
// callUnlink - the call is no longer waited for on its connection
//
static void callUnlink(CorRestCorCall* callP)
{
  MuxConn* mP = callP->mP;

  for (CorRestCorCall** pP = &mP->calls; *pP != NULL; pP = &(*pP)->next)
  {
    if (*pP == callP)
    {
      *pP = callP->next;
      break;
    }
  }
  callP->next = NULL;
}



// -----------------------------------------------------------------------------
//
// callDone - a call's response is in (or its connection failed): woken, if its coroutine waits
//
static void callDone(CorRestCorCall* callP, CorNode* treeP, const char* error)
{
  callUnlink(callP);
  callP->done       = true;
  callP->respTreeP  = treeP;
  callP->receivedMs = nowMs();
  callP->error     = error;

  if (callP->park != NULL)
    corCoLoopWake(callP->park);
}



// -----------------------------------------------------------------------------
//
// muxFail - the connection is broken: every call on it fails, and it is closed
//
static void muxFail(MuxConn* mP, const char* error)
{
  connClose(&mP->conn);

  while (mP->calls != NULL)
    callDone(mP->calls, NULL, error);
}



// -----------------------------------------------------------------------------
//
// writeTurn / writeTurnRelease - one frame (or the connect) at a time on a connection
//
static bool writeTurn(MuxConn* mP)
{
  while (mP->writing == true)
  {
    if (corCoCurrent() == NULL)                      // not a coroutine: nothing to wait with (never on a worker's thread)
      return false;

    Waiter w = { NULL, NULL };
    Waiter** tailPP = &mP->writers;

    while (*tailPP != NULL)
      tailPP = &(*tailPP)->next;
    *tailPP = &w;

    CorRestState* savedP = corRestP;
    corCoLoopPark(&w.park, -1);
    corRestP = savedP;
  }

  mP->writing = true;
  return true;
}

static void writeTurnRelease(MuxConn* mP)
{
  mP->writing = false;

  Waiter* wP = mP->writers;

  if (wP != NULL)
  {
    mP->writers = wP->next;
    corCoLoopWake(wP->park);
  }
}



// -----------------------------------------------------------------------------
//
// readTurnHandOff - the reader stops: another waiting call takes the read turn
//
static void readTurnHandOff(MuxConn* mP)
{
  for (CorRestCorCall* callP = mP->calls; callP != NULL; callP = callP->next)
  {
    if (callP->park != NULL)
    {
      corCoLoopWake(callP->park);
      return;
    }
  }
}



// -----------------------------------------------------------------------------
//
// muxGet - the thread's connection to host:port, connected if it is not; the write turn taken
//
// The loop itself (not a coroutine) cannot wait for a turn: a connection busy with its coroutines gets
// it a temporary one of its own.
//
static MuxConn* muxGet(const char* host, const char* port, int timeoutMs, bool* reusedP, const char** errorP)
{
  char     key[160];
  MuxConn* mP    = NULL;
  MuxConn* freeP = NULL;                             // an empty slot
  MuxConn* idleP = NULL;                             // or an idle connection to another peer, to make room

  snprintf(key, sizeof(key), "%s:%s", host, port);

  for (int i = 0; i < CLIENT_CONNS_MAX; i++)
  {
    MuxConn* cP = clientConnV[i];

    if ((cP != NULL) && ((cP->conn.fd >= 0) || (cP->writing == true)) && (strcmp(cP->conn.peer, key) == 0))
    {
      mP = cP;
      break;
    }

    if (cP == NULL)                                  // an empty slot: allocated only when it is the one taken
    {
      if ((freeP == NULL) && ((cP = calloc(1, sizeof(MuxConn))) != NULL))
      {
        cP->conn.fd    = -1;
        clientConnV[i] = cP;
        freeP          = cP;

        //
        // The thread has a connection to release when it ends - a non-NULL value is what makes the
        // key's destructor run
        //
        pthread_once(&clientConnsKeyOnce, clientConnsKeyCreate);
        pthread_setspecific(clientConnsKey, clientConnV);
      }
      continue;
    }

    if ((freeP == NULL) && (cP->conn.fd < 0) && (cP->writing == false) && (cP->calls == NULL))
      freeP = cP;
    else if ((idleP == NULL) && (cP->calls == NULL) && (cP->writing == false) && (cP->reading == false))
      idleP = cP;
  }

  bool loopItself = (corCoCurrent() == NULL) && (mP != NULL) && ((mP->writing == true) || (mP->reading == true));

  if ((mP != NULL) && (loopItself == false))
  {
    if (writeTurn(mP) == false)
    {
      *errorP = "cor:// connection busy";
      return NULL;
    }

    if (mP->conn.fd >= 0)                            // connected - by the coroutine whose turn it was, perhaps
    {
      *reusedP = true;
      return mP;
    }

    writeTurnRelease(mP);                            // that connect failed
    *errorP = "cannot connect to the cor:// endpoint";
    return NULL;
  }

  if (loopItself == true)
    freeP = NULL;
  else if ((freeP == NULL) && (idleP != NULL))
  {
    freeP = idleP;
    connClose(&freeP->conn);
  }

  if (freeP == NULL)                                 // a connection for this call only
  {
    if ((freeP = calloc(1, sizeof(MuxConn))) == NULL)
    {
      *errorP = "out of memory";
      return NULL;
    }
    freeP->conn.fd   = -1;
    freeP->temporary = true;
  }

  //
  // Named, and both turns taken, BEFORE the connect: the HELLO exchange waits for the peer, and in a
  // coroutine that wait yields - another coroutine of the thread finds the connection by its name and
  // waits for the write turn, which it gets once the tables are open
  //
  snprintf(freeP->conn.peer, sizeof(freeP->conn.peer), "%s", key);
  freeP->writing = true;
  freeP->reading = true;

  bool ok = clientConnect(&freeP->conn, host, port, timeoutMs, errorP);

  freeP->reading = false;

  if (ok == false)
  {
    writeTurnRelease(freeP);
    if (freeP->temporary == true)
      free(freeP);
    return NULL;
  }

  *reusedP = false;
  return freeP;                                      // the write turn is the caller's
}



// -----------------------------------------------------------------------------
//
// requestTree - { verb, path, headers, body }
//
static CorNode* requestTree(CorAlloc* kaP, CorRestVerb verb, const char* pathAndQuery, CorRestKeyValue* headerV, int headerCount, CorNode* bodyP)
{
  CorNode* reqP = corTreeObject(kaP, NULL);
  CorNode* hdrP = corTreeObject(kaP, "headers");

  corTreeChildAdd(reqP, corTreeString(kaP, "verb", corRestVerbToString(verb)));
  corTreeChildAdd(reqP, corTreeString(kaP, "path", pathAndQuery));

  for (int i = 0; i < headerCount; i++)
    corTreeChildAdd(hdrP, corTreeString(kaP, headerV[i].key, headerV[i].value));
  corTreeChildAdd(reqP, hdrP);

  if (bodyP != NULL)
  {
    //
    // Borrowed: the caller's tree, named and unlinked for the encoding only - the caller gets it
    // back as it was
    //
    reqP->value.tail->next = bodyP;
    reqP->value.tail       = bodyP;
  }

  return reqP;
}



// -----------------------------------------------------------------------------
//
// corRestCorStart -
//
CorRestCorCall* corRestCorStart(const char*       url,
                                CorRestVerb       verb,
                                const char*       pathAndQuery,
                                CorRestKeyValue*  headerV,
                                int               headerCount,
                                CorNode*          bodyTree,
                                const char*       bodyText,
                                int               timeoutMs,
                                CorAlloc*         respAllocP,
                                const char**      errorP)
{
  char host[128];
  char port[16];

  *errorP = NULL;

  if (authority(url, host, sizeof(host), port, sizeof(port)) == false)
  {
    *errorP = "not a cor://host:port URL";
    return NULL;
  }

  if (timeoutMs <= 0)
    timeoutMs = 30000;

  //
  // A body given as text: parsed here, once - the receiver then never parses it
  //
  if ((bodyTree == NULL) && (bodyText != NULL) && (bodyText[0] != 0))
  {
    CorJson cj;
    char*   copy = corAllocStrdup(respAllocP, bodyText);

    corJsonCreate(&cj, respAllocP);
    if ((copy == NULL) || ((bodyTree = corJsonParse(&cj, copy)) == NULL))
    {
      *errorP = "the request body is not JSON";
      return NULL;
    }
  }

  CorRestCorCall* callP = (CorRestCorCall*) calloc(1, sizeof(CorRestCorCall));

  if (callP == NULL)
  {
    *errorP = "out of memory";
    return NULL;
  }

  callP->respAllocP = respAllocP;
  callP->deadline   = nowMs() + timeoutMs;

  char*    savedName = (bodyTree != NULL) ? bodyTree->name : NULL;
  CorNode* savedNext = (bodyTree != NULL) ? bodyTree->next : NULL;

  if (bodyTree != NULL)
  {
    bodyTree->name = (char*) "body";
    bodyTree->next = NULL;
  }

  CorNode* reqP = requestTree(respAllocP, verb, pathAndQuery, headerV, headerCount, bodyTree);

  //
  // Sent - on the thread's connection to the peer. One the peer dropped since its last use (a restart)
  // fails on the write: that one is retried once, on a fresh connection.
  //
  for (int attempt = 0; attempt < 2; attempt++)
  {
    bool     reused = false;
    MuxConn* mP     = muxGet(host, port, timeoutMs, &reused, errorP);

    if (mP == NULL)
      break;

    callP->mP          = mP;
    callP->reused      = reused;
    callP->correlation = ++mP->conn.correlation;
    callP->next        = mP->calls;
    mP->calls          = callP;

    bool sent = frameSend(&mP->conn, FRAME_REQUEST, callP->correlation, reqP, timeoutMs);

    writeTurnRelease(mP);

    if (sent == true)
    {
      *errorP = NULL;
      break;
    }

    callUnlink(callP);
    callP->mP = NULL;
    muxFail(mP, "connection closed");                // whatever else was in flight on it is lost too
    *errorP   = "connection closed";

    if (mP->temporary == true)
      free(mP);

    if (reused == false)
      break;
  }

  if (bodyTree != NULL)
  {
    bodyTree->name = savedName;
    bodyTree->next = savedNext;
  }

  if (callP->mP == NULL)
  {
    free(callP);
    return NULL;
  }

  return callP;
}



// -----------------------------------------------------------------------------
//
// readFrame - the read turn's work: one frame off the connection, decoded, handed to its call
//
// Returns false when the connection is broken (and fails it).
//
static bool readFrame(MuxConn* mP)
{
  uint8_t  hdr[FRAME_HEADER_LEN];
  uint32_t correlation;
  uint32_t len;

  if (connRead(&mP->conn, hdr, FRAME_HEADER_LEN, 30000) == false)
  {
    muxFail(mP, (errno == ETIMEDOUT) ? "timed out" : "connection closed");
    return false;
  }

  memcpy(&correlation, &hdr[8],  4);
  memcpy(&len,         &hdr[12], 4);

  if ((memcmp(hdr, frameMagic, 4) != 0) || (hdr[4] != FRAME_RESPONSE) || (len > FRAME_MAX))
  {
    muxFail(mP, "unexpected frame in answer");
    return false;
  }

  CorRestCorCall* callP = mP->calls;

  while ((callP != NULL) && (callP->correlation != correlation))
    callP = callP->next;

  //
  // Its call's memory - or, for a call nobody waits for any more, a scratch buffer: decoded all the
  // same, the tables need it
  //
  CorAlloc  scratch;
  char      scratchBuf[4096];
  CorAlloc* kaP = (callP != NULL) ? callP->respAllocP : &scratch;

  if (callP == NULL)
    corAllocBufferInit(&scratch, scratchBuf, sizeof(scratchBuf), 64 * 1024, NULL, "cor:// late response");

  char*       buf   = (char*) corAlloc(kaP, len + 1);
  const char* error = "out of memory";
  CorNode*    treeP = NULL;

  if ((buf != NULL) && (connRead(&mP->conn, buf, (int) len, 30000) == true))
    treeP = corTreeBinDecode(buf, (int) len, codecP, &mP->conn.in, kaP, &error);
  else if (buf != NULL)
    error = "connection closed";

  if (callP == NULL)
    corAllocBufferReset(&scratch, false);

  if ((buf == NULL) || (treeP == NULL))
  {
    muxFail(mP, error);                              // the tables can no longer be trusted
    return false;
  }

  if (callP != NULL)
    callDone(callP, treeP, NULL);

  return true;
}



// -----------------------------------------------------------------------------
//
// responseFill - the response tree, as a CorRestCorResponse
//
static void responseFill(CorNode* respTreeP, CorAlloc* respAllocP, CorRestCorResponse* respP)
{
  CorNode* statusP  = corTreeLookup(respTreeP, "status");
  CorNode* headersP = corTreeLookup(respTreeP, "headers");
  CorNode* bodyP    = corTreeLookup(respTreeP, "body");
  CorNode* textP    = corTreeLookup(respTreeP, "text");

  respP->status = ((statusP != NULL) && (statusP->type == CorInt)) ? (int) statusP->value.i : 500;

  if ((headersP != NULL) && (headersP->type == CorObject))
  {
    int n = 0;

    for (CorNode* hP = headersP->value.head; hP != NULL; hP = hP->next)
      ++n;

    respP->headerV = (n > 0) ? (CorRestKeyValue*) corAlloc(respAllocP, n * sizeof(CorRestKeyValue)) : NULL;
    for (CorNode* hP = headersP->value.head; (hP != NULL) && (respP->headerV != NULL); hP = hP->next)
    {
      if (hP->type != CorString)
        continue;
      respP->headerV[respP->headerCount].key   = hP->name;
      respP->headerV[respP->headerCount].value = hP->value.s;
      respP->headerCount += 1;
    }
  }

  if (bodyP != NULL)
  {
    bodyP->name = NULL;
    bodyP->next = NULL;
    respP->bodyTree = bodyP;
  }
  else if ((textP != NULL) && (textP->type == CorString))
    respP->bodyText = textP->value.s;
}



// -----------------------------------------------------------------------------
//
// corRestCorWait -
//
bool corRestCorWait(CorRestCorCall* callP, CorRestCorResponse* respP, const char** errorP)
{
  MuxConn*      mP     = callP->mP;
  CorRestState* savedP = corRestP;

  memset(respP, 0, sizeof(CorRestCorResponse));

  while (callP->done == false)
  {
    int remaining = (int) (callP->deadline - nowMs());

    if (remaining <= 0)
      break;

    //
    // Another coroutine has the read turn: it hands this call its response - or the turn
    //
    if (mP->reading == true)
    {
      if (corCoCurrent() == NULL)                    // the loop itself, on a connection its coroutines read: cannot wait for them
        break;

      corCoLoopPark(&callP->park, remaining);
      callP->park = NULL;
      corRestP    = savedP;
      continue;
    }

    //
    // The read turn: wait for the connection to have something - with nothing consumed, a timeout
    // here leaves it whole - then one frame
    //
    mP->reading = true;

    int  w     = (mP->conn.rpos < mP->conn.rlen) ? 1 : corRestWaitFd(mP->conn.fd, POLLIN, remaining, NULL);
    bool ready = (w > 0);

    corRestP = savedP;

    //
    // -1: no wait is to be had - the loop is stopping (ECANCELED, corCoLoopCancel) or the wait itself
    // failed. Waiting again would fail again, at once, until the deadline: the connection's calls end here
    //
    if (w < 0)
    {
      mP->reading = false;
      muxFail(mP, "no wait for the response");
      break;
    }

    //
    // Ready: the bytes into the buffer now - connRead would otherwise find it empty and wait a second time
    // for what is already there (an epoll_ctl and a round of the loop, per response)
    //
    if ((ready == true) && (mP->conn.rpos >= mP->conn.rlen))
    {
      ssize_t r = read(mP->conn.fd, mP->conn.rbuf, sizeof(mP->conn.rbuf));

      if (r > 0)
      {
        mP->conn.rpos = 0;
        mP->conn.rlen = (int) r;
      }
    }

    if (ready == true)
      readFrame(mP);

    mP->reading = false;
    corRestP    = savedP;
  }

  if (callP->done == false)                          // timed out: the response, if it comes, is dropped
  {
    callUnlink(callP);
    callP->error = "timed out";
  }

  readTurnHandOff(mP);                               // another call that waits takes the read turn

  bool ok = (callP->error == NULL);

  if (ok == true)
  {
    responseFill(callP->respTreeP, callP->respAllocP, respP);
    respP->receivedMs = callP->receivedMs;
  }
  else
    *errorP = callP->error;

  if ((mP->temporary == true) && (mP->calls == NULL))
  {
    connClose(&mP->conn);
    free(mP);
  }

  free(callP);
  return ok;
}



// -----------------------------------------------------------------------------
//
// corRestCorSend - Start and Wait; a call sent on a connection the peer had closed is retried once
//
bool corRestCorSend(const char*         url,
                    CorRestVerb         verb,
                    const char*         pathAndQuery,
                    CorRestKeyValue*    headerV,
                    int                 headerCount,
                    CorNode*            bodyTree,
                    const char*         bodyText,
                    int                 timeoutMs,
                    CorAlloc*           respAllocP,
                    CorRestCorResponse* respP,
                    const char**        errorP)
{
  for (int attempt = 0; attempt < 2; attempt++)
  {
    CorRestCorCall* callP = corRestCorStart(url, verb, pathAndQuery, headerV, headerCount, bodyTree, bodyText, timeoutMs, respAllocP, errorP);

    if (callP == NULL)
    {
      memset(respP, 0, sizeof(CorRestCorResponse));
      return false;
    }

    bool reused = callP->reused;

    if (corRestCorWait(callP, respP, errorP) == true)
      return true;

    //
    // Retried ONLY on a connection that was reused and that the peer had closed - a restart since its
    // last use. Never after a timeout: the peer may be executing it, and a second POST would create twice.
    //
    if ((reused == false) || (*errorP == NULL) || (strcmp(*errorP, "connection closed") != 0))
      return false;
  }

  return false;
}
