//
// ExtMcpPalette.h
// Web Palette: status display + Start/Stop toggle + Auto-start checkbox.
// HTML/CSS/JS lives in vectorworks-mcp.vwr/html/.
//

#pragma once

namespace VWMcp
{

class CMcpJSProvider : public VWFC::PluginSupport::VWExtensionPaletteJSProvider
{
public:
    CMcpJSProvider(IVWUnknown* parent);
    virtual ~CMcpJSProvider();

    virtual void VCOM_CALLTYPE OnInit(
        VectorWorks::Extension::IWebJavaScriptProvider::IInitContext* context) override;

    virtual void VCOM_CALLTYPE OnPaletteVisibilityChange(
        bool visible,
        VectorWorks::Extension::IWebPaletteFrame* frame) override;

    // Per the SDK dispatch bug documented in VWX_PALETTE_DEMO/CLAUDE.md:
    // VW splits function names on '.' and plain (non-dotted) names get an
    // empty shortName so the dispatch map never matches. We fix that here.
    virtual void OnFunction(
        const TXString& name,
        const std::vector<nlohmann::json>& args,
        VectorWorks::UI::IJSFunctionCallbackContext* context) override;

    DEFINE_WebPalette_DISPATCH_MAP

private:
    // JS → C++ handlers
    void OnRequestStatus(
        const TXString& objName, const TXString& funcName,
        const std::vector<nlohmann::json>& args,
        VectorWorks::UI::IJSFunctionCallbackContext* context);

    void OnStartBridge(
        const TXString& objName, const TXString& funcName,
        const std::vector<nlohmann::json>& args,
        VectorWorks::UI::IJSFunctionCallbackContext* context);

    void OnStopBridge(
        const TXString& objName, const TXString& funcName,
        const std::vector<nlohmann::json>& args,
        VectorWorks::UI::IJSFunctionCallbackContext* context);

    void OnSetAutoStart(
        const TXString& objName, const TXString& funcName,
        const std::vector<nlohmann::json>& args,
        VectorWorks::UI::IJSFunctionCallbackContext* context);

    // C++ → JS push helpers
    void PushStatus();
};


class CMcpStatusPalette : public VWFC::PluginSupport::VWExtensionWebPalette
{
public:
    explicit CMcpStatusPalette(CallBackPtr cbp);
    virtual ~CMcpStatusPalette();

    static const VWIID& _GetIID();

    virtual void DefineSinks() override;
    virtual const TXString& VCOM_CALLTYPE GetUniversalName() override;
    virtual Sint32   VCOM_CALLTYPE GetVersion()       override;
    virtual TXString VCOM_CALLTYPE GetTitle()         override;
    virtual bool     VCOM_CALLTYPE GetInitialSize(ViewCoord& outCX, ViewCoord& outCY) override;
    virtual bool     VCOM_CALLTYPE GetMinimalSize(ViewCoord& outCX, ViewCoord& outCY) override;

private:
    TXString fUniversalName;
};

} // namespace VWMcp
