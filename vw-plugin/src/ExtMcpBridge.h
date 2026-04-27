#pragma once

#include "StdAfx.h"

namespace Extensions
{
    using namespace VWFC::PluginSupport;

    // Menu Extension — MCP Bridge start/stop
    class CMcpBridge : public VWExtensionMenu
    {
        DEFINE_VWMenuExtension;
    public:
        CMcpBridge(CallBackPtr cbp);
        virtual ~CMcpBridge();
    };

    // Event Sink — handler invoked when the menu command fires
    class CMcpBridge_EventSink : public VWMenu_EventSink
    {
    public:
        CMcpBridge_EventSink(IVWUnknown* parent);
        virtual ~CMcpBridge_EventSink();

        virtual void DoInterface() override;
    };
}
