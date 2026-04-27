// BridgeCore.cpp
// See BridgeCore.h. The command handler implementation here is the single
// source of truth — both the menu extension and the palette call into this.

#include "StdAfx.h"
#include "BridgeCore.h"
#include "SocketBridge.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <fstream>

namespace vwmcp
{

// ── Settings path helpers ───────────────────────────────────────────────
// Use the APPDATA env var directly to avoid pulling in <shlobj.h>, which
// triggers UUID-ambiguous-symbol errors against the VW SDK's UUID typedef.

static std::string GetAppDataRoamingDir()
{
#ifdef _WIN32
    if (const char* env = std::getenv("APPDATA"))
        return std::string(env);
    return ".";
#else
    if (const char* env = std::getenv("HOME"))
        return std::string(env) + "/.config";
    return ".";
#endif
}

std::string GetSettingsPath()
{
    // SDK_VERSION = e.g. 3000 / 3100 → year tag for the per-version subfolder
    int short_tag = SDK_VERSION / 100;
    int year      = 1995 + short_tag;
    char yearstr[8]; std::snprintf(yearstr, sizeof(yearstr), "%d", year);

    std::string base = GetAppDataRoamingDir();
    return base + "\\Nemetschek\\Vectorworks\\" + yearstr + "\\vectorworks-mcp-config.json";
}

// ── Settings load / save ────────────────────────────────────────────────

Settings LoadSettings()
{
    Settings s; // defaults
    std::string path = GetSettingsPath();

    std::ifstream f(path);
    if (!f.is_open()) return s;

    std::stringstream ss;
    ss << f.rdbuf();
    try
    {
        auto j = nlohmann::json::parse(ss.str());
        if (j.contains("auto_start") && j["auto_start"].is_boolean())
            s.auto_start = j["auto_start"].get<bool>();
        if (j.contains("version") && j["version"].is_number_integer())
            s.version = j["version"].get<int>();
    }
    catch (...) { /* keep defaults on parse error */ }
    return s;
}

void SaveSettings(const Settings& s)
{
    std::string path = GetSettingsPath();

    nlohmann::json j;
    j["auto_start"] = s.auto_start;
    j["version"]    = s.version;

    std::ofstream f(path);
    if (!f.is_open()) return;
    f << j.dump(2);
}

// ── SocketBridge command handler ────────────────────────────────────────

static nlohmann::json HandleCommand(const nlohmann::json& cmd)
{
    nlohmann::json result;
    result["id"] = cmd.value("id", "");

    std::string method = cmd.value("method", "");

    if (method == "ping")
    {
        result["success"] = true;
        result["data"]    = "pong";
        return result;
    }

    if (method == "get_drawing_info")
    {
        MCObjectHandle hDoc = gSDK->GetDrawingHeader();
        nlohmann::json info;
        info["has_document"] = (hDoc != nullptr);

        int layerCount = 0;
        MCObjectHandle hLayer = gSDK->FirstMemberObj(hDoc);
        while (hLayer)
        {
            ++layerCount;
            hLayer = gSDK->NextObject(hLayer);
        }
        info["layer_count"] = layerCount;

        result["success"] = true;
        result["data"]    = info;
        return result;
    }

    if (method == "list_layers")
    {
        MCObjectHandle hDoc = gSDK->GetDrawingHeader();
        nlohmann::json layers = nlohmann::json::array();

        MCObjectHandle hLayer = gSDK->FirstMemberObj(hDoc);
        while (hLayer)
        {
            short objType = gSDK->GetObjectTypeN(hLayer);
            if (objType == kLayerNode)
            {
                TXString name;
                gSDK->GetObjectName(hLayer, name);
                nlohmann::json layer;
                layer["name"] = name.GetCharPtr();
                layers.push_back(layer);
            }
            hLayer = gSDK->NextObject(hLayer);
        }

        result["success"] = true;
        result["data"]    = layers;
        return result;
    }

    if (method == "create_line")
    {
        auto params = cmd.value("params", nlohmann::json::object());
        WorldPt startPt(params.value("x1", 0.0), params.value("y1", 0.0));
        WorldPt endPt  (params.value("x2", 0.0), params.value("y2", 0.0));
        MCObjectHandle h = gSDK->CreateLine(startPt, endPt);
        result["success"] = (h != nullptr);
        result["data"]    = h ? "Line created" : "Failed";
        return result;
    }

    if (method == "create_rect")
    {
        auto params = cmd.value("params", nlohmann::json::object());
        double x = params.value("x", 0.0), y = params.value("y", 0.0);
        double w = params.value("width", 0.0), h = params.value("height", 0.0);
        WorldRect rect(WorldPt(x, y + h), WorldPt(x + w, y));
        MCObjectHandle hRect = gSDK->CreateRectangle(rect);
        result["success"] = (hRect != nullptr);
        result["data"]    = hRect ? "Rectangle created" : "Failed";
        return result;
    }

    if (method == "create_circle")
    {
        auto params = cmd.value("params", nlohmann::json::object());
        double cx = params.value("cx", 0.0), cy = params.value("cy", 0.0), r = params.value("radius", 0.0);
        WorldRect rect(WorldPt(cx - r, cy + r), WorldPt(cx + r, cy - r));
        MCObjectHandle h = gSDK->CreateOval(rect);
        result["success"] = (h != nullptr);
        result["data"]    = h ? "Circle created" : "Failed";
        return result;
    }

    if (method == "run_script")
    {
        auto params = cmd.value("params", nlohmann::json::object());
        std::string script = params.value("script", "");
        std::string lang   = params.value("language", "python");
        TXString    txScript(script.c_str());

        // Defensive guard: refuse to run scripts when there's no active
        // document. IPythonScriptEngine can SEH-crash VW on a fresh launch
        // if auto-start brings up the bridge before VW finishes initializing
        // its scripting runtime / document state.
        if (!gSDK || !gSDK->GetDrawingHeader())
        {
            result["success"] = false;
            nlohmann::json data;
            data["error_code"] = -1;
            data["stderr"]     = "No active document — script execution refused. "
                                 "Open a document in Vectorworks before calling run_script.";
            data["stdout"]     = "";
            result["data"]     = data;
            return result;
        }

        if (lang == "python")
        {
            VectorWorks::Scripting::IPythonScriptEnginePtr py(
                VectorWorks::Scripting::IID_PythonScriptEngine);
            if (!py)
            {
                result["success"] = false;
                result["data"]    = "Could not obtain IPythonScriptEngine";
                return result;
            }
            VectorWorks::Scripting::CDefaultPythonLogger logger;
            VCOMError err = py->ExecuteScript(txScript, &logger);
            nlohmann::json data;
            data["stdout"]     = logger.fOutput.GetStdString();
            data["stderr"]     = logger.fErrors.GetStdString();
            data["error_code"] = static_cast<int>(err);
            result["success"]  = (err == kVCOMError_NoError) && logger.fErrors.IsEmpty();
            result["data"]     = data;
            return result;
        }
        else
        {
            VectorWorks::Scripting::IVectorScriptEnginePtr vs(
                VectorWorks::Scripting::IID_VectorScriptEngine);
            if (!vs)
            {
                result["success"] = false;
                result["data"]    = "Could not obtain IVectorScriptEngine";
                return result;
            }
            VCOMError err = vs->ExecuteScript(txScript);
            nlohmann::json data;
            data["error_code"] = static_cast<int>(err);
            result["success"]  = (err == kVCOMError_NoError);
            result["data"]     = data;
            return result;
        }
    }

    result["success"] = false;
    result["data"]    = std::string("Unknown method: ") + method;
    return result;
}

void RegisterCommandHandler()
{
    SocketBridge::Instance().SetHandler(HandleCommand);
}

bool StartBridgeIfNeeded()
{
    auto& bridge = SocketBridge::Instance();
    if (bridge.IsRunning()) return true;
    RegisterCommandHandler();
    unsigned short port = SocketBridge::GetDefaultPort(SDK_VERSION);
    return bridge.Start(port);
}

void StopBridge()
{
    SocketBridge::Instance().Stop();
}

std::string GetEndpointString()
{
    return SocketBridge::GetDefaultEndpoint(SDK_VERSION);
}

} // namespace vwmcp
