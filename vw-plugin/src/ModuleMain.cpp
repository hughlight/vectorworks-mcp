//
// ModuleMain.cpp — Vectorworks MCP Bridge Plugin
// Entry point for the Vectorworks module plug-in
//

#include "StdAfx.h"
#include "ExtMcpBridge.h"

const char* DefaultPluginVWRIdentifier() { return "VWMCPBridge"; }

extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_ver() { return SDK_VERSION; }

extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_main(
    Sint32 action, void* moduleInfo, const VWIID& iid,
    IVWUnknown*& inOutInterface, CallBackPtr cbp)
{
    ::GS_InitializeVCOM(cbp);

    Sint32 reply = 0L;
    using namespace VWFC::PluginSupport;

    REGISTER_Extension<CMcpBridge>(GROUPID_ExtensionMenu, action, moduleInfo, iid, inOutInterface, cbp, reply);

    return reply;
}
