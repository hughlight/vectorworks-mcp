//
// ExtMcpPalette.cpp
// Status/control palette for Fuse VW Gantry.
//

#include "StdAfx.h"
#include "ExtMcpPalette.h"
#include "BridgeCore.h"
#include "SocketBridge.h"

#include <sstream>

using namespace VWMcp;
using namespace VectorWorks::Extension;

// ────────────────────────────────────────────────────────────────────────
// CMcpJSProvider
// ────────────────────────────────────────────────────────────────────────

CMcpJSProvider::CMcpJSProvider(IVWUnknown* parent)
    : VWFC::PluginSupport::VWExtensionPaletteJSProvider(parent)
{
}

CMcpJSProvider::~CMcpJSProvider() {}

void CMcpJSProvider::OnInit(IWebJavaScriptProvider::IInitContext* context)
{
    VWExtensionPaletteJSProvider::OnInit(context);

    // Capture the WebFrame here — OnPaletteVisibilityChange may not fire on
    // first show if the palette was auto-opened from the workspace.
    fWebFrame = context->GetWebFrame();

    context->AddFunctionSync("requestStatus");
    context->AddFunctionSync("startBridge");
    context->AddFunctionSync("stopBridge");
    context->AddFunctionSync("setAutoStart");

    PushStatus();
}

void CMcpJSProvider::OnPaletteVisibilityChange(bool visible, IWebPaletteFrame* frame)
{
    if (visible && frame)
    {
        fWebFrame = frame;
        PushStatus();
    }
}

// SDK dispatch bug fix — see VWX_PALETTE_DEMO/CLAUDE.md.
void CMcpJSProvider::OnFunction(
    const TXString& name,
    const std::vector<nlohmann::json>& args,
    VectorWorks::UI::IJSFunctionCallbackContext* context)
{
    ptrdiff_t at = name.Find('.');
    if (at >= 0)
        VWExtensionPaletteJSProvider::OnFunction(name, args, context);
    else
        this->OnFunctionCall(TXString(), name, args, context);
}

BEGIN_WebPalette_DISPATCH_MAP(CMcpJSProvider)
    ADD_WebPalette_FUNCTION("requestStatus", OnRequestStatus)
    ADD_WebPalette_FUNCTION("startBridge",   OnStartBridge)
    ADD_WebPalette_FUNCTION("stopBridge",    OnStopBridge)
    ADD_WebPalette_FUNCTION("setAutoStart",  OnSetAutoStart)
END_WebPalette_DISPATCH_MAP

// ── OnRequestStatus ────────────────────────────────────────────────────
void CMcpJSProvider::OnRequestStatus(
    const TXString&, const TXString&,
    const std::vector<nlohmann::json>&,
    VectorWorks::UI::IJSFunctionCallbackContext*)
{
    PushStatus();
}

// ── OnStartBridge ──────────────────────────────────────────────────────
void CMcpJSProvider::OnStartBridge(
    const TXString&, const TXString&,
    const std::vector<nlohmann::json>&,
    VectorWorks::UI::IJSFunctionCallbackContext*)
{
    vwmcp::StartBridgeIfNeeded();
    PushStatus();
}

// ── OnStopBridge ───────────────────────────────────────────────────────
void CMcpJSProvider::OnStopBridge(
    const TXString&, const TXString&,
    const std::vector<nlohmann::json>&,
    VectorWorks::UI::IJSFunctionCallbackContext*)
{
    vwmcp::StopBridge();
    PushStatus();
}

// ── OnSetAutoStart ─────────────────────────────────────────────────────
void CMcpJSProvider::OnSetAutoStart(
    const TXString&, const TXString&,
    const std::vector<nlohmann::json>& args,
    VectorWorks::UI::IJSFunctionCallbackContext*)
{
    if (args.empty() || !args[0].is_boolean()) return;

    vwmcp::Settings s = vwmcp::LoadSettings();
    s.auto_start = args[0].get<bool>();
    vwmcp::SaveSettings(s);

    PushStatus();
}

// ── PushStatus ─────────────────────────────────────────────────────────
void CMcpJSProvider::PushStatus()
{
    if (!fWebFrame) return;

    auto& bridge = SocketBridge::Instance();
    vwmcp::Settings s = vwmcp::LoadSettings();

    nlohmann::json payload;
    payload["running"]      = bridge.IsRunning();
    payload["port"]         = bridge.IsRunning() ? bridge.GetPort() : SocketBridge::GetDefaultPort(SDK_VERSION);
    payload["endpoint"]     = vwmcp::GetEndpointString();
    payload["sdk_version"]  = SDK_VERSION;
    payload["vw_year"]      = 1995 + (SDK_VERSION / 100);
    payload["auto_start"]   = s.auto_start;
    payload["settings_path"] = vwmcp::GetSettingsPath();

    TXString js = "if (typeof window.updateStatus === 'function') window.updateStatus(";
    js += payload.dump().c_str();
    js += ");";
    fWebFrame->ExecuteJavaScript(js, [](){});
}

// ────────────────────────────────────────────────────────────────────────
// CMcpStatusPalette
// ────────────────────────────────────────────────────────────────────────

CMcpStatusPalette::CMcpStatusPalette(CallBackPtr /*cbp*/)
    : VWExtensionWebPalette("html", "index.html")
{
}

CMcpStatusPalette::~CMcpStatusPalette() {}

const VWIID& CMcpStatusPalette::_GetIID()
{
    // Distinct from the menu UUID (which is 0xa1b2c3d4-...).
    // {6f4e9a02-9c1f-4d8a-9f2b-8b5c1d2e7f01}
    static VWIID iid = {
        0x6f4e9a02, 0x9c1f, 0x4d8a,
        { 0x9f, 0x2b, 0x8b, 0x5c, 0x1d, 0x2e, 0x7f, 0x01 }
    };
    return iid;
}

void CMcpStatusPalette::DefineSinks()
{
    DefineSink<CMcpJSProvider>(IID_WebJavaScriptProvider);
}

const TXString& CMcpStatusPalette::GetUniversalName()
{
    fUniversalName = "FuseVWGantryPalette";
    return fUniversalName;
}

Sint32 CMcpStatusPalette::GetVersion()
{
    return 1;
}

TXString CMcpStatusPalette::GetTitle()
{
    return "Fuse VW Gantry";
}

bool CMcpStatusPalette::GetInitialSize(ViewCoord& outCX, ViewCoord& outCY)
{
    outCX = 360;
    outCY = 220;
    return true;
}

bool CMcpStatusPalette::GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY)
{
    outCX = 280;
    outCY = 180;
    return true;
}
