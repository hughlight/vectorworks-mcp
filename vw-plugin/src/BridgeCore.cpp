// BridgeCore.cpp
// See BridgeCore.h. The command handler implementation here is the single
// source of truth — both the menu extension and the palette call into this.

#include "StdAfx.h"
#include "BridgeCore.h"
#include "SocketBridge.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <fstream>

#ifdef _WIN32
  // StdAfx.h defines _WINSOCKAPI_, so <windows.h> does not drag in winsock.h.
  #include <windows.h>
  #include <malloc.h>   // _resetstkoflw
#endif

namespace vwmcp
{

// ── Script-engine fault containment ─────────────────────────────────────
//
// A vs.* call that dereferences bad VW internals raises a Win32 structured
// exception, not a C++ one. Without a __try barrier it unwinds out of the
// plug-in frame and takes Vectorworks down. The shims below put each engine
// call inside a __try frame that holds only POD locals (MSVC C2712 forbids
// __try in a frame that needs C++ unwinding), so the smart pointers, logger
// and json objects stay in the caller. Only hardware faults are handled;
// C++ exceptions (0xE06D7363) continue the search and are caught by the
// dispatcher's catch(...) as before.
//
// After a contained fault the interpreter and document state are unknown,
// so a process-wide latch refuses every later script until VW restarts.

static std::atomic<bool>          g_enginePoisoned{false};
static std::atomic<unsigned long> g_firstFaultCode{0};

bool IsEnginePoisoned() { return g_enginePoisoned.load(); }

static std::string HexCode(unsigned long code)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", code);
    return buf;
}

#ifdef _WIN32

static int ScriptFaultFilter(unsigned long code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return EXCEPTION_EXECUTE_HANDLER;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

// Every shim returns 0 when the call completed (then *outErr is valid), or
// the exception code of a contained hardware fault.

// The script-context path: the SDK's own pattern (ImageComparisonTesting.cpp)
// and the one BhaveshY/vectorworks-mcp found necessary for object creation.
static unsigned long GuardedPyContextRun(
    VectorWorks::Scripting::IPythonScriptEngine* eng,
    const TXString* script,
    VectorWorks::Scripting::IPythonLogger* logger,
    VCOMError* outErr)
{
    __try
    {
        *outErr = eng->ScriptContext_Begin(*script, logger);
        if (*outErr == kVCOMError_NoError)
            *outErr = eng->ScriptContext_RunEx(logger);
        return 0;
    }
    __except (ScriptFaultFilter(GetExceptionCode()))
    {
        unsigned long code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? code : 1u;
    }
}

// The pre-2026-10 path (bare ExecuteScript), kept for A/B control runs.
static unsigned long GuardedPyExecute(
    VectorWorks::Scripting::IPythonScriptEngine* eng,
    const TXString* script,
    VectorWorks::Scripting::IPythonLogger* logger,
    VCOMError* outErr)
{
    __try
    {
        *outErr = eng->ExecuteScript(*script, logger);
        return 0;
    }
    __except (ScriptFaultFilter(GetExceptionCode()))
    {
        unsigned long code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? code : 1u;
    }
}

static unsigned long GuardedVsExecute(
    VectorWorks::Scripting::IVectorScriptEngine* eng,
    const TXString* script,
    VCOMError* outErr)
{
    __try
    {
        *outErr = eng->ExecuteScript(*script);
        return 0;
    }
    __except (ScriptFaultFilter(GetExceptionCode()))
    {
        unsigned long code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? code : 1u;
    }
}

static unsigned long GuardedEndUndoEvent()
{
    __try
    {
        gSDK->EndUndoEvent();
        return 0;
    }
    __except (ScriptFaultFilter(GetExceptionCode()))
    {
        return GetExceptionCode() ? GetExceptionCode() : 1u;
    }
}

static unsigned long GuardedUndoAndRemove()
{
    __try
    {
        gSDK->UndoAndRemove();
        return 0;
    }
    __except (ScriptFaultFilter(GetExceptionCode()))
    {
        return GetExceptionCode() ? GetExceptionCode() : 1u;
    }
}

#else // !_WIN32 — no SEH; direct calls so the file still compiles.

static unsigned long GuardedPyContextRun(
    VectorWorks::Scripting::IPythonScriptEngine* eng, const TXString* script,
    VectorWorks::Scripting::IPythonLogger* logger, VCOMError* outErr)
{
    *outErr = eng->ScriptContext_Begin(*script, logger);
    if (*outErr == kVCOMError_NoError) *outErr = eng->ScriptContext_RunEx(logger);
    return 0;
}
static unsigned long GuardedPyExecute(
    VectorWorks::Scripting::IPythonScriptEngine* eng, const TXString* script,
    VectorWorks::Scripting::IPythonLogger* logger, VCOMError* outErr)
{
    *outErr = eng->ExecuteScript(*script, logger);
    return 0;
}
static unsigned long GuardedVsExecute(
    VectorWorks::Scripting::IVectorScriptEngine* eng, const TXString* script, VCOMError* outErr)
{
    *outErr = eng->ExecuteScript(*script);
    return 0;
}
static unsigned long GuardedEndUndoEvent()  { gSDK->EndUndoEvent();  return 0; }
static unsigned long GuardedUndoAndRemove() { gSDK->UndoAndRemove(); return 0; }

#endif // _WIN32

// Program variable 9876 suppresses VectorScript error dialogs while set
// (SDK sample ImageComparisonTesting.cpp:181-182). A modal dialog on the
// main thread would otherwise block the dispatcher until its 25 s ceiling.
struct ScopedVsErrorSuppress
{
    bool active = false;
    ScopedVsErrorSuppress()
    {
        Boolean on = true;
        active = gSDK && gSDK->SetProgramVariable(9876, &on);
    }
    ~ScopedVsErrorSuppress()
    {
        if (active) { Boolean off = false; gSDK->SetProgramVariable(9876, &off); }
    }
    ScopedVsErrorSuppress(const ScopedVsErrorSuppress&) = delete;
    ScopedVsErrorSuppress& operator=(const ScopedVsErrorSuppress&) = delete;
};

static void MarkEnginePoisoned(unsigned long fault)
{
    unsigned long expected = 0;
    g_firstFaultCode.compare_exchange_strong(expected, fault);
    g_enginePoisoned.store(true);
}

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
        // This handler runs on VW's main thread (SocketBridge dispatches
        // through MainThreadDispatch). Python goes through the SDK's
        // ScriptContext_Begin/RunEx pair rather than bare ExecuteScript:
        // bare ExecuteScript runs read-only scripts but faults on the first
        // object creation even from the main thread (vicquick, BhaveshY,
        // 2026-10-11). Each call is one undo event so a failed script rolls
        // back and a successful one is a single Ctrl+Z step.
        auto params = cmd.value("params", nlohmann::json::object());
        std::string script = params.value("script", "");
        std::string lang   = params.value("language", "python");

        bool transaction = true;
        if (params.contains("transaction") && params["transaction"].is_boolean())
            transaction = params["transaction"].get<bool>();

        std::string undoLabel;
        if (params.contains("undo_label") && params["undo_label"].is_string())
            undoLabel = params["undo_label"].get<std::string>();
        if (undoLabel.empty()) undoLabel = "MCP run_script";
        if (undoLabel.size() > 120) undoLabel.resize(120);

        // py_mode: the request wins over VWMCP_PY_MODE in VW's environment;
        // anything other than "execute" selects the script-context path.
        std::string pyMode;
        if (params.contains("py_mode") && params["py_mode"].is_string())
            pyMode = params["py_mode"].get<std::string>();
        if (pyMode.empty())
            if (const char* env = std::getenv("VWMCP_PY_MODE")) pyMode = env;
        const bool bareExecute = (pyMode == "execute");
        pyMode = bareExecute ? "execute" : "context";

        nlohmann::json data;
        data["stdout"]      = "";
        data["stderr"]      = "";
        data["transaction"] = transaction;
        data["rolled_back"] = false;
        data["py_mode"]     = pyMode;
        data["undo_label"]  = undoLabel;

        if (IsEnginePoisoned())
        {
            const std::string code = HexCode(g_firstFaultCode.load());
            data["error"]      = "engine_poisoned";
            data["error_code"] = -3;
            data["fault_code"] = code;
            data["stderr"]     = "Scripting is disabled for the rest of this Vectorworks "
                                 "session: an earlier script faulted inside the engine (" +
                                 code + ") and its state is unknown. Save your work and "
                                 "restart Vectorworks.";
            result["success"]  = false;
            result["data"]     = data;
            return result;
        }

        // Defensive guard: refuse to run scripts when there's no active
        // document. IPythonScriptEngine can SEH-crash VW on a fresh launch
        // if auto-start brings up the bridge before VW finishes initializing
        // its scripting runtime / document state.
        if (!gSDK || !gSDK->GetDrawingHeader())
        {
            data["error"]      = "no_document";
            data["error_code"] = -1;
            data["stderr"]     = "No active document — script execution refused. "
                                 "Open a document in Vectorworks before calling run_script.";
            result["success"]  = false;
            result["data"]     = data;
            return result;
        }

        const bool isPython = (lang == "python");
        VectorWorks::Scripting::IPythonScriptEnginePtr py(
            VectorWorks::Scripting::IID_PythonScriptEngine);
        VectorWorks::Scripting::IVectorScriptEnginePtr vs(
            VectorWorks::Scripting::IID_VectorScriptEngine);
        if (isPython ? !py : !vs)
        {
            result["success"] = false;
            result["data"]    = isPython ? "Could not obtain IPythonScriptEngine"
                                         : "Could not obtain IVectorScriptEngine";
            return result;
        }

        TXString txScript(script.c_str());
        VectorWorks::Scripting::CDefaultPythonLogger logger;
        VCOMError     err   = kVCOMError_NoError;
        unsigned long fault = 0;
        const char*   stage = isPython ? "Python" : "VectorScript";

        {
            ScopedVsErrorSuppress suppressDialogs;

            if (transaction)
            {
                gSDK->SupportUndoAndRemove();
                gSDK->SetUndoMethod(kUndoSwapObjects);
                gSDK->NameUndoEvent(TXString(undoLabel.c_str()));
            }

            if (isPython)
                fault = bareExecute
                      ? GuardedPyExecute(py.operator->(), &txScript, &logger, &err)
                      : GuardedPyContextRun(py.operator->(), &txScript, &logger, &err);
            else
                fault = GuardedVsExecute(vs.operator->(), &txScript, &err);
        }

        data["stdout"] = logger.fOutput.GetStdString();
        data["stderr"] = logger.fErrors.GetStdString();
        const bool ok  = (fault == 0) && (err == kVCOMError_NoError) && logger.fErrors.IsEmpty();

        if (fault != 0)
        {
            // Contained crash: close the undo event rather than replaying it
            // (the replay could re-fault), skip DrawScreen, and poison.
            if (transaction) GuardedEndUndoEvent();
        }
        else if (transaction)
        {
            stage = "undo";
            if (ok)
            {
                fault = GuardedEndUndoEvent();
            }
            else
            {
                fault = GuardedUndoAndRemove();
                if (fault == 0) data["rolled_back"] = true;
            }
        }

        if (fault != 0)
        {
            MarkEnginePoisoned(fault);
            const std::string code = HexCode(fault);
            data["error"]      = "script_crashed";
            data["error_code"] = -2;
            data["fault_code"] = code;
            data["stderr"]     = std::string("The script faulted inside the Vectorworks ") + stage +
                                 " engine (" + code + "). The fault was contained, but the "
                                 "engine state is unknown: scripting is disabled until "
                                 "Vectorworks restarts and the document may be inconsistent. "
                                 "Save As a new file, then restart Vectorworks.";
            result["success"]  = false;
            result["data"]     = data;
            return result;
        }

        gSDK->DrawScreen();
        data["error_code"] = static_cast<int>(err);
        result["success"]  = ok;
        result["data"]     = data;
        return result;
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
