// StdAfx.h — minimal precompiled-header substitute for vectorworks-mcp.
// Source files include this to get the Vectorworks SDK + namespaces. PCH is
// disabled in the .vcxproj so this is a regular header that just centralizes
// the SDK include + using-declarations the upstream code expects.

#pragma once

#if defined(_WINDOWS) || defined(_WIN32)
  #ifndef VC_EXTRALEAN
    #define VC_EXTRALEAN
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  // Prevent <windows.h> from pulling in the legacy <winsock.h>; we use
  // <winsock2.h> via SocketBridge.h.
  #ifndef _WINSOCKAPI_
    #define _WINSOCKAPI_
  #endif
  #pragma warning(disable:4786)
#endif

#include "VectorworksSDK.h"

using namespace VWFC::Math;
using namespace VWFC::VWObjects;
using namespace VWFC::Tools;
using namespace VWFC::VWUI;
