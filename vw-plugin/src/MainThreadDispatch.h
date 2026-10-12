// MainThreadDispatch.h
//
// Cross-thread → main-thread dispatcher for the Vectorworks MCP plugin.
//
// Background: VW's SDK (VCOM, gSDK, IPythonScriptEngine, the resource manager)
// is not thread-safe. Calling it from the socket worker thread races VW's main
// UI thread and crashes the process on any mutation that touches a
// cross-referenced internal table — most reliably the resource/record manager
// (e.g. vs.NewField creating a record format). See FORK_NOTES.md
// "Threading model" for the diagnostic trail.
//
// This dispatcher uses the canonical Windows pattern: a hidden message-only
// window created on the main thread. The worker enqueues a task and
// PostMessage()s the window; VW's main-thread message pump dispatches the
// custom message back into our WndProc, which drains the queue on the main
// thread. Results return via std::future.
//
// On non-Windows builds the dispatcher falls back to direct invocation. That
// preserves the existing (broken) behavior on Mac rather than introducing a
// new failure mode, and lets the file compile cross-platform. A proper
// libdispatch implementation is TODO when Mac is back in scope.

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

namespace vwmcp
{

class MainThreadDispatch
{
public:
    // Must be called once from the main thread (i.e. from inside
    // plugin_module_main). Subsequent calls are no-ops. Returns false if the
    // hidden window couldn't be created.
    static bool Initialize();

    // Best-effort teardown. Not strictly required — VW will clean up when the
    // plugin unloads. Provided for symmetry.
    static void Shutdown();

    // Returns true if Initialize() succeeded and the dispatcher is ready.
    static bool IsReady();

    // Submit a callable to run on the main thread. Returns a future that
    // resolves to the callable's return value, or carries an exception.
    //
    // IMPORTANT: capture lambda state BY VALUE, not by reference. The worker
    // thread may time out waiting on the future; if so, the task may still
    // run later, and reference captures would dangle.
    template <typename Fn>
    static auto SubmitToMain(Fn fn) -> std::future<decltype(fn())>
    {
        using R = decltype(fn());
        auto promise = std::make_shared<std::promise<R>>();
        auto future  = promise->get_future();

        EnqueueErased([promise, fn = std::move(fn)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    promise->set_value();
                } else {
                    promise->set_value(fn());
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });

        return future;
    }

private:
    // Type-erased queue push + wake-up signal. Implemented in the .cpp so the
    // queue/mutex/HWND state stays out of this header.
    static void EnqueueErased(std::function<void()> task);
};

} // namespace vwmcp
