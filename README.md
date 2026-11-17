# corRest — REST Server & HTTP Client Library

A compact REST layer for C: an HTTP **server** - on libmicrohttpd or on the
built-in epoll server of [`corHttp`](https://github.com/SEAMWARE/corHttp), chosen at
build time (`COR_HTTP_SERVER`) - with a flat, verb-indexed service table and fast wildcard URL matching, plus a full HTTP
**client** (blocking, parallel-multi, connection-pooled, TLS). It is the transport
layer under the coraine NGSI-LD context broker, but has no NGSI-LD dependency of
its own.

- **Version:** 0.1.0
- **Language:** C
- **License:** [Apache License 2.0](LICENSE) — Copyright 2026 Seamware

## Features

- **Service table** — register a flat array of `(verb, url, routine)` services;
  the library splits them by HTTP method into per-verb vectors at init.
- **Fast URL matching** — character-sum hashing and pre-computed wildcard offsets
  avoid `strcmp` on the hot path; supports single (`*`) and greedy (`**`) wildcards.
- **Per-request state** — each request has a state of its own (`corRest`, a
  `__thread` pointer behind a macro), bound to the thread that runs the request -
  the I/O thread, a worker or a coroutine's loop - while it runs it.
- **Hooks** — the application plugs in at fixed points of a request
  (`corRestHooks.h`): pre-dispatch, payload parse and render, URL parameters,
  admission, pre-service, post-response, whether a request runs inline or as a
  coroutine of the loop, a protocol upgrade (`Upgrade:`, WebSocket), per-request
  user data, CORS.
- **Streamed responses** (`COR_WITH_STREAM`) — a body written over time, chunked
  (`corRestStream.h`, server-sent events), on both HTTP servers.
- **cor://** (`COR_WITH_COR_PROTOCOL`) — the same service routines over the binary
  cor format, server and client, multiplexed (`corRestCor.h`).
- **HTTP client** — request builder, JSON bodies, per-request timeouts, response
  header access.
- **Parallel multi-client** — fire N requests and harvest them together (used for
  distributed-operation fan-out).
- **Connection pool + TLS** — keep-alive pooling per host and an OpenSSL transport.
- **HTTP proxy** (`COR_WITH_CLIENT_PROXY`) — both engines: `https` through a `CONNECT` tunnel, `http` with
  the absolute URI; set by the application, from the environment or given.

## API reference

The umbrella header pulls in the whole API:

```c
#include "corRest/corRest.h"
```

### Types

```c
// HTTP method — also the index into the per-verb service vectors (CorRestVerb.h)
typedef enum CorRestVerb {
  CorVerbGet = 0, CorVerbPut, CorVerbPost, CorVerbDelete,
  CorVerbPatch, CorVerbHead, CorVerbOptions
} CorRestVerb;

// A service routine returns true on success (CorRestService.h)
typedef bool (*CorRestServiceRoutine)(void);

// One service entry, as authored by the embedding app/plugin
typedef struct CorRestServiceSimplified {
  CorRestVerb            verb;
  const char*           url;             // path pattern, may contain * / **
  CorRestServiceRoutine  serviceRoutine;
  uint64_t              supportedParams; // bitmask of accepted URL params
  uint64_t              ldOp;            // atomic-op bit (0 for non-NGSI-LD routes)
  CorRestServiceRoutine  payloadCheck;    // optional body validator (NULL = none)
  CorRestServiceOptions  options;         // declared features: wildcard checks, body kind (CorRestService.h)
} CorRestServiceSimplified;

// URL-param name → bitmask bit (for supportedParams)
typedef struct CorRestParam { const char* name; uint64_t bit; } CorRestParam;
```

### Server

```c
int  corRestInit(CorRestServiceSimplified serviceV[], int services,
                unsigned short port, int poolSize);
void corRestStop(void);

void corRestStateInit(void* connection, const char* url, const char* method);
void corRestStateRelease(void);
```

`corRestInit` expands the flat service array into per-verb vectors, starts the
worker pool (`poolSize`) and then the HTTP server on `port`. `corRestStateInit` /
`corRestStateRelease` set up and release the bound request's state; the
`connection` is the backend's own handle.

### Client

```c
int  corRestClientInit(int maxIdleConns, int idleTimeoutSec, const char* userAgent);

// Request builder
void corRestClientRequestInit(CorRestClientRequest* req, CorRestVerb verb, const char* url, CorAlloc* allocP);
void corRestClientRequestHeader(CorRestClientRequest* req, const char* name, const char* value);
void corRestClientRequestBody(CorRestClientRequest* req, const char* body, int bodyLen);
void corRestClientRequestTimeout(CorRestClientRequest* req, int connectMs, int requestMs);
int  corRestClientSend(CorRestClientRequest* req, CorRestClientResponse* resp);

const char* corRestClientResponseHeader(CorRestClientResponse* resp, const char* name);
```

A default per-request timeout is exposed as `int corRestClientDefaultRequestTimeoutMs`.

A response holds its own copy of what was read: its status text, headers and body
stay valid after the connection has gone back to the pool or been closed. The copy
comes from the request's allocator and lives as long as it; without an allocator it
is malloc'd and released by `corRestClientResponseCleanup` (`corRestClientMultiDestroy`
for a response of the multi engine). The body is zero-terminated.

### Parallel multi-client

```c
CorRestClientMulti*    corRestClientMultiCreate(int capacity);
int                   corRestClientMultiAdd(CorRestClientMulti* multi, CorRestVerb verb, const char* url, /* … */);
int                   corRestClientMultiPerform(CorRestClientMulti* multi, int timeoutMs);
CorRestClientResponse* corRestClientMultiResponse(CorRestClientMulti* multi, int index);
void                  corRestClientMultiDestroy(CorRestClientMulti* multi);
```

A connection pool (`corRestClientPool*`) and an OpenSSL TLS transport
(`corRestClientTls*`) sit underneath for keep-alive and `https://`.

### Access tokens (JWT)

```c
CorRestJwtCheck corRestJwtVerify(const char* token, const CorRestJwtExpect* expectP, CorAlloc* kaP, CorNode** claimsPP);
const char*     corRestJwtCheckString(CorRestJwtCheck check);
bool            corRestJwkLoad(char* jwksText, void (*changed)(void), char* errorBuf, int errorBufSize);
int             corRestJwkCount(void);
void            corRestJwkRelease(void (*changed)(void));
char*           corRestFetch(const char* where, CorAlloc* kaP, char* errorBuf, int errorBufSize);
void            corRestJwtCacheInit(void (*ref)(void* identity), void (*unref)(void* identity));
void*           corRestJwtCacheGet(const char* token, int len);
```

A JWT (RFC 7519, compact JWS) checked: the signature - RS256 or ES256 - against a JWK Set (RFC 7517), then
`exp`, `nbf`, `iss`, `aud`. The key set is one per process, replaced whole by `corRestJwkLoad` (a refresh thread)
while other threads verify against it. `corRestFetch` reads a JWK Set (or any document) from `http(s)://`, `file://`
or a path - blocking, never on a request's path. `corRestJwtCache*` keeps the tokens checked already, each with the
application's identity for it; emptied when the key set changes. Left out with `COR_WITH_JWT=0`. Used by coraine's
auth plugins and corCep.

### HTTP proxy

```c
bool corRestClientProxyFromEnv(char* errBuf, int errBufLen);   // https_proxy, http_proxy, no_proxy (or upper case)
bool corRestClientProxySet(const char* httpUrl, const char* httpsUrl, const char* noProxy, char* errBuf, int errBufLen);
```

No proxy is used until the application calls one of them. A proxy is an `http://` URL -
`[user:password@]host[:port]`, port 1080 by default, the user and password sent as `Proxy-Authorization:
Basic`. An `https` request goes through a `CONNECT host:port` tunnel, TLS then to the host itself (its name
for SNI and the certificate check); an `http` request goes to the proxy with its absolute URI. `no_proxy`:
comma-separated - `*`, host names (a domain covers its hosts: `.example.org` or `example.org`), addresses;
ports are ignored. A proxy that cannot be reached, or that refuses the tunnel, is the error detail - "Connection
to the proxy host:port failed", the proxy's status line. `COR_WITH_CLIENT_PROXY=0` builds without it (about
6.7 KB of code less): both engines connect straight to the host, and `corRestClientProxy.h`'s functions are
not in the library.

## Usage example

```c
#include "corRest/corRest.h"

static bool pingGet(void)
{
  // … build the response via the CorRestOut API …
  return true;
}

static CorRestServiceSimplified services[] =
{
  { CorVerbGet, "/ping",            pingGet, 0, 0, NULL, {} },
  { CorVerbGet, "/things/*",        thingGet, 0, 0, NULL, {} },   // single-wildcard
};

int main(void)
{
  if (corRestInit(services, sizeof(services)/sizeof(services[0]), 1026, 16) != 0)
    return 1;

  // … serve until shutdown …
  corRestStop();
  return 0;
}
```

## Building

```bash
make            # build libcorRest.a (+ .so) and the test binary
make ci         # clean + install
make di         # debug + install
make clientLifetimeTest   # the lifetime of a client response, under AddressSanitizer
make COR_HTTP_SERVER=builtin   # the built-in server (corHttp) instead of libmicrohttpd
make COR_WITH_CLIENT_PROXY=0   # without the client's HTTP proxy
make COR_WITH_COR_PROTOCOL=0   # without cor://
make COR_WITH_STREAM=0         # without streamed responses
```

With `COR_HTTP_SERVER=builtin` a consumer links `../corHttp/libcorHttp.a` as well:
an archive cannot hold another one.

`libcorRest.a` links statically into its consumers. Sibling repos must be
present (the build references `../<lib>/lib<lib>.a`).

## Dependencies

Sibling repos (one `.a` each):

- [`corAlloc`](https://github.com/SEAMWARE/corAlloc) — arena allocator (`CorAlloc`)
- [`corTree`](https://github.com/SEAMWARE/corTree) — the tree (`CorNode`)
- [`corJson`](https://github.com/SEAMWARE/corJson) — JSON parsing and rendering
- [`corBase`](https://github.com/SEAMWARE/corBase) — core utilities and the library log (`COR_LIB_*`)
- [`corLog`](https://github.com/SEAMWARE/corLog) — logging and trace levels
- [`corHttp`](https://github.com/SEAMWARE/corHttp) — the built-in HTTP server, with
  `COR_HTTP_SERVER=builtin` only

System libraries: `libmicrohttpd` (HTTP server, with `COR_HTTP_SERVER=mhd`, the default), `openssl` (`ssl`/`crypto`, TLS
client), `pthread`, `m`.
