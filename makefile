#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
LIB_SO        = libcorRest.so
LIB           = libcorRest.a
CC            = gcc
INCLUDE       = -I..
DFLAGS        =
#
# EXTRA_CFLAGS - the hook for a caller that needs to ADD flags to this build.
#
# Not DFLAGS. DFLAGS is a plain variable, so `make DFLAGS=...` REPLACES it -
# the command line beats the makefile - and a `DFLAGS +=` inside the makefile is
# ignored along with it, because += never appends to a command-line variable. A
# caller reaching for DFLAGS to add one flag therefore drops every default this
# lib sets for itself. DFLAGS is empty here today, so nothing is lost yet; the
# first -D added to it would be, silently. corNgsild lost -DANSI and
# -DCOR_WITH_ICU that way and compiled the wrong collation path under coverage.
#
# EXTRA_CFLAGS is appended LAST, so a caller's -O0 / -Wno-error also win over the
# -O2 / -Werror here, which is what an instrumented build needs.
#
#
# COR_HTTP_SERVER - which HTTP server this build carries.
#
#   mhd      libmicrohttpd, an external shared library
#   builtin  the epoll server in this repo, no external dependency
#   both     the two, one of them selected at start (corRestBackendSelect, corRestBackendTable.h): both
#            backend files, each defining its functions under its own suffix, and calls through the table
#            of the one selected. A build with one server calls its functions directly.
#
# Emitted as a 0/1 PAIR rather than one flag, so the source reads
# `#if COR_HTTP_SERVER_MHD` and -Wundef turns a misspelling into a compile error
# instead of silently selecting the other implementation. Same discipline as the
# COR_FEATURE_* defines the broker compiles with.
#
# In CFLAGS and not DFLAGS on purpose - see the note above: a caller who passes
# DFLAGS on the command line would drop these along with every other default.
#
# The value picks the backend SOURCE FILES as well as the defines: with one server the
# other corRestBackend*.c is left out, as compiling it would need the library it exists to avoid.
#
COR_HTTP_SERVER ?= mhd
ifeq ($(COR_HTTP_SERVER),mhd)
  HTTP_SERVER_FLAGS   = -DCOR_HTTP_SERVER_MHD=1 -DCOR_HTTP_SERVER_BUILTIN=0
  HTTP_SERVER_SOURCE  = corRestBackendMhd.c
  HTTP_SERVER_LIBS    = -lmicrohttpd
  HTTP_SERVER_ARCHIVE =
else ifeq ($(COR_HTTP_SERVER),builtin)
  HTTP_SERVER_FLAGS   = -DCOR_HTTP_SERVER_MHD=0 -DCOR_HTTP_SERVER_BUILTIN=1
  HTTP_SERVER_SOURCE  = corRestBackendBuiltin.c
  HTTP_SERVER_LIBS    =
  #
  # corHttp is a sibling repo and a static archive, like every other lib here.
  # It is NOT folded into libcorRest.a - an archive cannot contain another one -
  # so a consumer linking the built-in flavour links both, which is what
  # coraine's CMakeLists does.
  #
  HTTP_SERVER_ARCHIVE = ../corHttp/libcorHttp.a
else ifeq ($(COR_HTTP_SERVER),both)
  HTTP_SERVER_FLAGS   = -DCOR_HTTP_SERVER_MHD=1 -DCOR_HTTP_SERVER_BUILTIN=1
  HTTP_SERVER_SOURCE  = corRestBackendMhd.c corRestBackendBuiltin.c corRestBackendSelect.c
  HTTP_SERVER_LIBS    = -lmicrohttpd
  HTTP_SERVER_ARCHIVE = ../corHttp/libcorHttp.a
else
  $(error COR_HTTP_SERVER must be 'mhd', 'builtin' or 'both', not '$(COR_HTTP_SERVER)')
endif

#
# COR_WITH_COR_PROTOCOL - 1 | 0. cor://, the binary NGSI-LD protocol (corRestCor.c: its server and its
# client). 0 leaves the file out, and corRestCor.h's functions with it: coraine's makefile passes the
# same value to this lib and, as COR_FEATURE_COR_PROTOCOL, to the broker - the broker built without it
# calls none of them. The codec (corTree's corTreeBin.c) is not this file's: corDB's log is written in it.
#
COR_WITH_COR_PROTOCOL ?= 1
# The value is in CFLAGS too, so that the flag stamp below changes with it: the archive is then rebuilt,
# and one built with corRestCor.o does not keep it.
#
ifeq ($(COR_WITH_COR_PROTOCOL),1)
  COR_PROTOCOL_SOURCE = corRestCor.c
else ifeq ($(COR_WITH_COR_PROTOCOL),0)
  COR_PROTOCOL_SOURCE =
else
  $(error COR_WITH_COR_PROTOCOL must be 1 or 0, not '$(COR_WITH_COR_PROTOCOL)')
endif

#
# COR_WITH_CLIENT_PROXY - 1 | 0. The client's HTTP proxy (corRestClientProxy.c): https through a CONNECT
# tunnel, http with the absolute URI - set by the application (corRestClientProxyFromEnv, corRestClientProxySet).
# 0 leaves the file out, and corRestClientProxy.h's functions with it; both engines then connect straight to
# the host, as before.
#
COR_WITH_CLIENT_PROXY ?= 1

ifeq ($(COR_WITH_CLIENT_PROXY),1)
  CLIENT_PROXY_SOURCE = corRestClientProxy.c
else ifeq ($(COR_WITH_CLIENT_PROXY),0)
  CLIENT_PROXY_SOURCE =
else
  $(error COR_WITH_CLIENT_PROXY must be 1 or 0, not '$(COR_WITH_CLIENT_PROXY)')
endif

#
# COR_WITH_STREAM - 1 | 0. A response whose body is written over time (corRestStream.h: server-sent
# events), on both HTTP servers. 0 leaves its code out of the backends; corRestStreamResponse says false.
#
COR_WITH_STREAM ?= 1

ifneq ($(COR_WITH_STREAM),1)
  ifneq ($(COR_WITH_STREAM),0)
    $(error COR_WITH_STREAM must be 1 or 0, not '$(COR_WITH_STREAM)')
  endif
endif

#
# COR_WITH_JWT - 1 | 0. An access token checked (corRestJwt.h): a JWT, RS256 / ES256, against a JWK Set
# (corRestJwk.h, libcrypto), the tokens checked already (corRestJwtCache.h), and a JWK Set fetched
# (corRestFetch.h). 0 leaves the four files out, and their functions with them.
#
COR_WITH_JWT ?= 1

ifeq ($(COR_WITH_JWT),1)
  JWT_SOURCES = corRestJwt.c corRestJwk.c corRestJwtCache.c corRestFetch.c
else ifeq ($(COR_WITH_JWT),0)
  JWT_SOURCES =
else
  $(error COR_WITH_JWT must be 1 or 0, not '$(COR_WITH_JWT)')
endif

CFLAGS        = -O2 -Wall -Werror -Wundef -fPIC -fstack-protector-all $(DFLAGS) $(HTTP_SERVER_FLAGS) -DCOR_WITH_COR_PROTOCOL=$(COR_WITH_COR_PROTOCOL) -DCOR_WITH_CLIENT_PROXY=$(COR_WITH_CLIENT_PROXY) -DCOR_WITH_STREAM=$(COR_WITH_STREAM) -DCOR_WITH_JWT=$(COR_WITH_JWT) $(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)
LIB_SOURCES   = corRestInit.c           \
                $(HTTP_SERVER_SOURCE)   \
                corMimeType.c           \
                corRestStop.c           \
                corRestInHeader.c       \
                corRestStateInit.c      \
                corRestServiceLookup.c  \
                corRestVerbFromString.c \
                corRestUrlValueEncode.c \
                corRestVersion.c        \
                corRestProblem.c        \
                corRestOutHeader.c      \
                corRestStream.c         \
                corRestHooks.c          \
                corRestParamRegistry.c  \
                corRestClient.c         \
                $(COR_PROTOCOL_SOURCE)   \
                corRestRun.c            \
                corRestWait.c           \
                corRestResolve.c        \
                corRestClientPool.c     \
                corRestClientParse.c    \
                corRestClientTls.c      \
                corRestClientMulti.c    \
                $(CLIENT_PROXY_SOURCE)  \
                $(JWT_SOURCES)

#
# BUILD - which flavour of build this is, and where its objects live.
#
# Objects used to sit next to their sources, one set for every flavour, and that
# is a silent-wrong-answer machine: `make coverage` leaves instrumented objects
# behind, a later ordinary build finds them NEWER than the sources and relinks
# them into a binary that calls itself ordinary. This lib had the sharper end of
# it - its own test binary links without -lgcov and every instrumented object
# then wants __gcov_init, so the build failed outright rather than lying.
#
# A plain variable and not a target-specific one on purpose: target-specific
# variables (`debug: CFLAGS += -g`) are not visible when the makefile is parsed,
# so a directory derived from them would be the same directory for every target.
#
BUILD        ?= debug
OBJDIR       := obj/$(BUILD)

ifeq ($(BUILD),debug)
CFLAGS       += -g -DDEBUG
endif

LIB_OBJS      = $(addprefix $(OBJDIR)/,$(LIB_SOURCES:.c=.o))
LIB_DEPS      = $(addprefix $(OBJDIR)/,$(LIB_SOURCES:.c=.d))

#
# $(OBJDIR)/.flags - the flags these objects were built with.
#
# The directory separates the flavours; this catches a change WITHIN one. A
# caller adding EXTRA_CFLAGS changes the compile line and nothing else: sources
# are untouched, objects stay newer than them, and make rebuilds nothing. The
# stamp is rewritten only when the flags actually differ, so its timestamp moves
# exactly when a rebuild is due, and every object depends on it.
#
FLAGSTAMP    := $(OBJDIR)/.flags

TEST          = corRestTest
TEST_SOURCES  = corRestTest.c
TEST_OBJS     = $(addprefix $(OBJDIR)/,$(TEST_SOURCES:.c=.o))

SO_LDFLAGS    = -L../corAlloc -L../corJson -L../corTree -L../corBase -L../corLog
SO_LIBS       = -lcorAlloc -lcorJson -lcorTree -lcorLog -lcorBase $(HTTP_SERVER_ARCHIVE) $(HTTP_SERVER_LIBS) -lssl -lcrypto -lpthread
SO_RPATH      = -Wl,-rpath,'$$ORIGIN/../corAlloc:$$ORIGIN/../corJson:$$ORIGIN/../corTree:$$ORIGIN/../corBase:$$ORIGIN/../corLog'

LIBS          = ../corAlloc/libcorAlloc.a ../corJson/libcorJson.a ../corTree/libcorTree.a ../corLog/libcorLog.a ../corBase/libcorBase.a $(HTTP_SERVER_ARCHIVE) $(HTTP_SERVER_LIBS) -lssl -lcrypto -lpthread -lm

#
# Built per flavour, then STAGED to the repo root where every consumer expects
# them. Unconditionally: comparing timestamps here would reintroduce the bug the
# object directories fix, since obj/debug/libX.a is easily older than a libX.a
# left behind by a coverage build.
#
all: $(OBJDIR)/$(LIB_SO) $(OBJDIR)/$(LIB) $(OBJDIR)/$(TEST)
						@cp -f $(OBJDIR)/$(LIB) $(LIB)
						@cp -f $(OBJDIR)/$(LIB_SO) $(LIB_SO)
						@cp -f $(OBJDIR)/$(TEST) $(TEST)

$(FLAGSTAMP): FORCE
						@mkdir -p $(OBJDIR)
						@echo '$(CFLAGS)' | cmp -s - $@ 2>/dev/null || echo '$(CFLAGS)' > $@

FORCE:

clean:
						rm -rf obj
						#
						# ...and the legacy in-tree artefacts. Objects live under obj/ now, but a tree
						# built before that still has .o/.d beside its sources - and, worse, .gcno:
						# gcovr reads those and reports a file nobody compiled as entirely unexecuted,
						# which once moved the published figure by three points.
						#
						rm -f *.o *.d *.gcno *.gcda
						rm -f *.o
						rm -f *.a
						rm -f *~
						rm -f *.so
						rm -f $(TEST)

install:    all
						@if [ ! -d bin ]; then mkdir bin; fi
						cp $(TEST) bin/

di:         install

ci:         clean install

#
# The staged artefacts are targets in their own right, so a caller can ask for
# `make libcorX.a` and get the current flavour's archive copied into place. The
# coverage target does exactly that, by name.
#
$(LIB): $(OBJDIR)/$(LIB)
						@cp -f $< $@

$(LIB_SO): $(OBJDIR)/$(LIB_SO)
						@cp -f $< $@

#
# Removed and rebuilt, never updated in place. `ar r` REPLACES and ADDS but
# never removes, so an archive built once with corRestBackendMhd.o keeps it
# after a switch to the built-in backend - and the link then has two definitions
# of every corRestBackend* function, one of which wants libmicrohttpd.
#
$(OBJDIR)/$(LIB):	$(LIB_OBJS)
						@rm -f $@
						ar r $@ $(LIB_OBJS)
						ranlib $@

$(OBJDIR)/$(LIB_SO):	$(LIB_OBJS)
						$(CC) -shared $(LIB_OBJS) -o $@ $(SO_LDFLAGS) $(SO_LIBS) $(SO_RPATH)

$(OBJDIR)/$(TEST):	$(TEST_OBJS) $(OBJDIR)/$(LIB)
						$(CC) -o $@ $(TEST_OBJS) $(OBJDIR)/$(LIB) $(LIBS)


$(OBJDIR)/%.o: %.c $(FLAGSTAMP)
						@mkdir -p $(OBJDIR)
						$(CC) $(CFLAGS) -c $< -o $@

%.i: %.c
						$(CC) $(CFLAGS) -c $^ -E > $@

-include $(LIB_DEPS)

#
# clientLifetimeTest - the lifetime of a client response (test/clientLifetime). Built with
# AddressSanitizer in its own object directory, and run:
#
#   make clientLifetimeTest
#
LIFETIME_FLAGS = BUILD=asan EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -O1"

clientLifetimeTest:
						$(MAKE) $(LIFETIME_FLAGS) obj/asan/$(LIB)
						$(CC) -g -fsanitize=address -fno-omit-frame-pointer $(INCLUDE) -o obj/asan/clientLifetimeTest \
						  test/clientLifetime/clientLifetimeTest.c obj/asan/$(LIB) $(LIBS)
						obj/asan/clientLifetimeTest

.PHONY: clientLifetimeTest
