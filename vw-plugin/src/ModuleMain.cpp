//
// ModuleMain.cpp — Vectorworks MCP Bridge Plugin entry point.
// Registers two extensions:
//   * Menu     — toggle the bridge on/off
//   * Palette  — status display + Start/Stop button + Auto-start toggle
// And honors the auto_start setting once VW finishes initializing extensions.
//

#include "StdAfx.h"
#include "ExtMcpBridge.h"
#include "ExtMcpPalette.h"
#include "BridgeCore.h"

// Identifies which .vwr resource bundle to load. Must exactly match the
// folder name (or packaged file basename) — vectorworks-mcp.vwr.
const char* DefaultPluginVWRIdentifier() { return "vectorworks-mcp"; }

extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_ver() { return SDK_VERSION; }

extern "C" Sint32 GS_EXTERNAL_ENTRY plugin_module_main(
    Sint32 action, void* moduleInfo, const VWIID& iid,
    IVWUnknown*& inOutInterface, CallBackPtr cbp)
{
    ::GS_InitializeVCOM(cbp);

    Sint32 reply = 0L;
    using namespace VWFC::PluginSupport;
    using namespace Extensions;
    using namespace VWMcp;

    // 1. Menu extension (toggle the bridge).
    REGISTER_Extension<CMcpBridge>(
        GROUPID_ExtensionMenu,
        action, moduleInfo, iid, inOutInterface, cbp, reply);

    // 2. Web palette (status + control + auto-start setting).
    REGISTER_Extension<CMcpStatusPalette>(
        GROUPID_ExtensionWebPalettes,
        action, moduleInfo, iid, inOutInterface, cbp, reply);

    // 3. Auto-start: read the persistent setting and start the bridge if so
    //    configured. Defer until plugin_module_main has been called several
    //    times — the bridge listener is harmless to spin up early, but we want
    //    VW's scripting runtime to be ready before any run_script can land.
    //    The Nth call (where N is small) is a heuristic stand-in for "VW is
    //    sufficiently initialized." run_script also has its own no-document
    //    guard as a backstop.
    static int  g_callCount = 0;
    static bool g_autoStarted = false;
    g_callCount++;
    if (!g_autoStarted && g_callCount >= 4)
    {
        g_autoStarted = true;
        vwmcp::Settings s = vwmcp::LoadSettings();
        if (s.auto_start)
        {
            vwmcp::StartBridgeIfNeeded();
        }
    }

    return reply;
}
