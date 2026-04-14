#pragma once

#include "StdAfx.h"

namespace Extensions
{
    using namespace VWFC::PluginSupport;

    // Menu Extension — MCP Bridge の開始/停止
    class CMcpBridge : public VWExtensionMenu
    {
        DEFINE_VWMenuExtensionClass;
    public:
        CMcpBridge(CallBackPtr cbp);
        virtual ~CMcpBridge();
    };

    // Event Sink — メニューコマンド実行時のハンドラ
    class CMcpBridge_EventSink : public VWMenu_EventSink
    {
        DEFINE_VWMenuEventSinkClass;
    public:
        CMcpBridge_EventSink(IVWUnknown* parent);
        virtual ~CMcpBridge_EventSink();

        virtual void DoInterface() override;
    };
}
