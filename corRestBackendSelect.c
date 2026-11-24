//
// FILE            corRestBackendSelect.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// COR_HTTP_SERVER=both: which of the two HTTP servers this process runs (corRestBackendTable.h). Compiled in a
// build with both only.
//
#include <stdbool.h>                      // bool
#include <string.h>                       // strcmp

#include "corRest/corRestBackendTable.h"  // Own interface



const CorRestBackend* corRestBackendP = &corRestBackendTableMhd;



// -----------------------------------------------------------------------------
//
// corRestBackendSelect -
//
bool corRestBackendSelect(const char* name)
{
  if (strcmp(name, "mhd") == 0)
    corRestBackendP = &corRestBackendTableMhd;
  else if (strcmp(name, "builtin") == 0)
    corRestBackendP = &corRestBackendTableBuiltin;
  else
    return false;

  return true;
}
