//
// ExtMcpBridge.cpp — FuseVWGantry menu extension that toggles the SocketBridge.
// All command handling + bridge lifecycle lives in BridgeCore.{h,cpp}.
// This file is just the menu shell.
//

#include "StdAfx.h"
#include "ExtMcpBridge.h"
#include "BridgeCore.h"
#include "SocketBridge.h"

using namespace Extensions;

// --------------------------------------------------------------------------------------------------------
namespace Extensions
{
    static SMenuDef gMenuDef = {
        /*Needs*/       EMenuEnableFlags::DocIsActive,
        /*NeedsNot*/    EMenuEnableFlags::None,
        /*Title*/       {"FuseVWGantry",  "menu_title"},
        /*Category*/    {"FuseVWGantry",  "menu_category"},
        /*HelpText*/    {"FuseVWGantry",  "menu_help"},
        // Created in VW2025 line so the same source compiles + ships against
        // both VW2025 (SDK 3000) and VW2026 (SDK 3100). VW won't load a menu
        // tagged "created in 26" against a 25 build.
        /*VersionCreated*/  25,
        /*VersionModified*/ 0,
        /*VersionRetired*/  0
    };
}

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
IMPLEMENT_VWMenuExtension(
    /*Extension class*/ CMcpBridge,
    /*Event sink*/      CMcpBridge_EventSink,
    /*Universal name*/  "FuseVWGantry",
    /*Version*/         1,
    /*UUID*/            0xa1b2c3d4, 0xe5f6, 0x7890, 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

// --------------------------------------------------------------------------------------------------------
CMcpBridge::CMcpBridge(CallBackPtr cbp)
    : VWExtensionMenu(cbp, gMenuDef, NULL)
{
}

CMcpBridge::~CMcpBridge()
{
}

// --------------------------------------------------------------------------------------------------------
CMcpBridge_EventSink::CMcpBridge_EventSink(IVWUnknown* parent)
    : VWMenu_EventSink(parent)
{
}

CMcpBridge_EventSink::~CMcpBridge_EventSink()
{
}

void CMcpBridge_EventSink::DoInterface()
{
    auto& bridge = SocketBridge::Instance();

    if (bridge.IsRunning())
    {
        vwmcp::StopBridge();
        gSDK->AlertInform(TXString("Fuse VW Gantry stopped."), TXString(""), false);
    }
    else
    {
        if (vwmcp::StartBridgeIfNeeded())
        {
            TXString msg("Fuse VW Gantry started at ");
            msg += TXString(vwmcp::GetEndpointString().c_str());
            gSDK->AlertInform(msg, TXString("Use Claude Code to connect."), false);
        }
        else
        {
            gSDK->AlertInform(TXString("Failed to start Fuse VW Gantry."), TXString(""), false);
        }
    }
}
