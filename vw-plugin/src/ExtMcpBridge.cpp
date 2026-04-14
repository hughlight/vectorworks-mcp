//
// ExtMcpBridge.cpp — MCP Bridge Menu Extension
// Unix Domain Socket サーバーを起動し、外部 MCP サーバーと通信
//

#include "StdAfx.h"
#include "ExtMcpBridge.h"
#include "SocketBridge.h"

using namespace Extensions;

// --------------------------------------------------------------------------------------------------------
// Menu Definition
// --------------------------------------------------------------------------------------------------------
namespace Extensions
{
    static SMenuDef gMenuDef = {
        /*Needs*/       EMenuEnableFlags::DocIsActive,
        /*NeedsNot*/    EMenuEnableFlags::None,
        /*Title*/       {"CMcpBridge", "title"},
        /*Category*/    {"CMcpBridge", "category"},
        /*HelpText*/    {"CMcpBridge", "help"},
        /*VersionCreated*/  26,
        /*VersionModified*/ 0,
        /*VersionRetired*/  0
    };
}

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
IMPLEMENT_VWMenuExtension(
    /*Extension class*/ CMcpBridge,
    /*Event sink*/      CMcpBridge_EventSink,
    /*Universal name*/  "VWMCPBridge",
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
// Event Sink
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

    if (bridge.IsRunning()) {
        bridge.Stop();
        gSDK->AlertInform(TXString("MCP Bridge stopped."), TXString(""), false);
    } else {
        // コマンドハンドラを登録
        bridge.SetHandler([](const nlohmann::json& cmd) -> nlohmann::json {
            std::string method = cmd.value("method", "");
            nlohmann::json result;
            result["id"] = cmd.value("id", "");

            if (method == "ping") {
                result["success"] = true;
                result["data"] = "pong";
            }
            else if (method == "get_drawing_info") {
                // ドキュメント情報を取得
                MCObjectHandle hDoc = gSDK->GetDrawingHeader();
                nlohmann::json info;
                info["has_document"] = (hDoc != nullptr);

                // レイヤー数をカウント
                int layerCount = 0;
                MCObjectHandle hLayer = gSDK->FirstMemberObj(hDoc);
                while (hLayer) {
                    layerCount++;
                    hLayer = gSDK->NextObject(hLayer);
                }
                info["layer_count"] = layerCount;

                result["success"] = true;
                result["data"] = info;
            }
            else if (method == "list_layers") {
                MCObjectHandle hDoc = gSDK->GetDrawingHeader();
                nlohmann::json layers = nlohmann::json::array();

                MCObjectHandle hLayer = gSDK->FirstMemberObj(hDoc);
                while (hLayer) {
                    short objType = gSDK->GetObjectTypeN(hLayer);
                    if (objType == kLayerNode) {
                        TXString name;
                        gSDK->GetObjectName(hLayer, name);
                        nlohmann::json layer;
                        layer["name"] = name.GetCharPtr();
                        layers.push_back(layer);
                    }
                    hLayer = gSDK->NextObject(hLayer);
                }

                result["success"] = true;
                result["data"] = layers;
            }
            else if (method == "create_line") {
                auto params = cmd.value("params", nlohmann::json::object());
                double x1 = params.value("x1", 0.0);
                double y1 = params.value("y1", 0.0);
                double x2 = params.value("x2", 0.0);
                double y2 = params.value("y2", 0.0);

                WorldPt startPt(x1, y1);
                WorldPt endPt(x2, y2);
                MCObjectHandle hLine = gSDK->CreateLine(startPt, endPt);

                result["success"] = (hLine != nullptr);
                result["data"] = hLine ? "Line created" : "Failed to create line";
            }
            else if (method == "create_rect") {
                auto params = cmd.value("params", nlohmann::json::object());
                double x = params.value("x", 0.0);
                double y = params.value("y", 0.0);
                double w = params.value("width", 0.0);
                double h = params.value("height", 0.0);

                WorldRect rect(WorldPt(x, y + h), WorldPt(x + w, y));
                MCObjectHandle hRect = gSDK->CreateRectangle(rect);

                result["success"] = (hRect != nullptr);
                result["data"] = hRect ? "Rectangle created" : "Failed";
            }
            else if (method == "create_circle") {
                auto params = cmd.value("params", nlohmann::json::object());
                double cx = params.value("cx", 0.0);
                double cy = params.value("cy", 0.0);
                double r = params.value("radius", 0.0);

                WorldPt center(cx, cy);
                MCObjectHandle hCircle = gSDK->CreateOval(
                    WorldPt(cx - r, cy + r), WorldPt(cx + r, cy - r));

                result["success"] = (hCircle != nullptr);
                result["data"] = hCircle ? "Circle created" : "Failed";
            }
            else if (method == "run_script") {
                // VectorScript / Python スクリプト実行
                auto params = cmd.value("params", nlohmann::json::object());
                std::string script = params.value("script", "");
                std::string lang = params.value("language", "python");

                // Python スクリプトの場合
                if (lang == "python") {
                    TXString txScript(script.c_str());
                    Boolean ok = gSDK->RunTempTool(nullptr, nullptr);
                    // Note: 実際の Python 実行は別のアプローチが必要
                    result["success"] = false;
                    result["data"] = "Python execution via SDK is limited. Use VectorScript.";
                }
                else {
                    // VectorScript
                    TXString txScript(script.c_str());
                    gSDK->RunScriptText(txScript);
                    result["success"] = true;
                    result["data"] = "Script executed";
                }
            }
            else {
                result["success"] = false;
                result["data"] = "Unknown method: " + method;
            }

            return result;
        });

        if (bridge.Start("/tmp/vw-mcp-bridge.sock")) {
            gSDK->AlertInform(
                TXString("MCP Bridge started at /tmp/vw-mcp-bridge.sock"),
                TXString("Use Claude Code to connect."), false);
        } else {
            gSDK->AlertInform(TXString("Failed to start MCP Bridge."), TXString(""), false);
        }
    }
}
