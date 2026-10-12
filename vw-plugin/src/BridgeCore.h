// BridgeCore.h
// Shared SocketBridge command handler + persistent settings.
// Both the menu extension and the palette extension call into this so they
// agree on how the bridge starts, stops, and dispatches RPC.

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace vwmcp
{

// ── SocketBridge command handler ────────────────────────────────────────
// Registers our JSON command dispatch on SocketBridge::Instance(). Idempotent —
// safe to call multiple times. Always wins on most-recent registration.
void RegisterCommandHandler();

// Convenience: ensure handler is registered, then start the bridge on the
// version-tagged port (computed from SDK_VERSION). Returns true if the bridge
// is now running, false if start failed. Safe to call when already running.
bool StartBridgeIfNeeded();

// Stop the bridge if running. Idempotent.
void StopBridge();

// Returns the resolved socket endpoint "127.0.0.1:<port>" for display.
std::string GetEndpointString();

// True once a script raised a contained hardware fault inside the VW
// scripting engine. run_script refuses every call after that until VW
// restarts; the document and interpreter state are unknown.
bool IsEnginePoisoned();

// ── Persistent settings ─────────────────────────────────────────────────
// Lives at %APPDATA%\Nemetschek\Vectorworks\<ver>\vectorworks-mcp-config.json
// Schema: {"auto_start": false, "version": 1}

struct Settings
{
    bool auto_start = false;
    int  version    = 1;
};

Settings LoadSettings();
void     SaveSettings(const Settings& s);

// Returns the absolute path the settings file would live at, for diagnostics.
std::string GetSettingsPath();

} // namespace vwmcp
