// MainThreadDispatch.cpp — see MainThreadDispatch.h for the design rationale.

#include "StdAfx.h"
#include "MainThreadDispatch.h"

#include <cstdio>
#include <mutex>
#include <queue>

#ifdef _WIN32
  // StdAfx.h sets _WINSOCKAPI_ to prevent <windows.h> from pulling in the
  // legacy winsock.h. We're not using sockets here, so plain <windows.h> is fine.
  #include <windows.h>
#endif

namespace vwmcp
{

namespace
{
    void DbgLog(const char* fmt, ...)
    {
        const char* tmp = std::getenv("TEMP");
        if (!tmp) tmp = "C:\\Windows\\Temp";
        char path[1024];
        std::snprintf(path, sizeof(path), "%s\\vwmcp_dispatch_trace.log", tmp);
        FILE* f = std::fopen(path, "a");
        if (!f) return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        std::fputc('\n', f);
        va_end(ap);
        std::fflush(f);
        std::fclose(f);
    }

    std::mutex                          g_queueMutex;
    std::queue<std::function<void()>>   g_queue;

#ifdef _WIN32
    HWND            g_hwnd            = nullptr;
    DWORD           g_mainThreadId    = 0;
    constexpr UINT  WM_VWMCP_DRAIN    = WM_USER + 1;
    const wchar_t*  kClassName        = L"VwmcpMainThreadDispatch_v1";
    bool            g_classRegistered = false;

    void DrainAllPendingTasks()
    {
        DbgLog("DrainAllPendingTasks: enter, thread_id=%lu, main_thread_id=%lu",
               (unsigned long)::GetCurrentThreadId(), (unsigned long)g_mainThreadId);
        int drained = 0;
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lk(g_queueMutex);
                if (g_queue.empty()) {
                    DbgLog("DrainAllPendingTasks: exit, drained %d", drained);
                    return;
                }
                task = std::move(g_queue.front());
                g_queue.pop();
            }
            DbgLog("DrainAllPendingTasks: running task #%d", drained);
            try { task(); } catch (...) { /* swallow */ }
            ++drained;
            DbgLog("DrainAllPendingTasks: task #%d returned", drained - 1);
        }
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_VWMCP_DRAIN) {
            DbgLog("WndProc: WM_VWMCP_DRAIN received, thread_id=%lu",
                   (unsigned long)::GetCurrentThreadId());
            DrainAllPendingTasks();
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
#endif // _WIN32

} // anonymous namespace

bool MainThreadDispatch::Initialize()
{
#ifdef _WIN32
    if (g_hwnd != nullptr) return true;

    // CRITICAL: pin this DLL into the process so its code pages — specifically
    // the WndProc we're about to register — cannot be unmapped. VW loads
    // plug-in DLLs for module_main calls and may unload them between calls; an
    // un-pinned WndProc address becomes a stale jump target as soon as the DLL
    // is gone, and the next message to our window SEH-crashes VW.
    //
    // GetModuleHandleEx with GET_MODULE_HANDLE_EX_FLAG_PIN keeps this DLL
    // mapped for the lifetime of the process. The cost is one un-released
    // module ref — a non-issue since the plug-in is meant to live as long as
    // VW does anyway.
    {
        HMODULE hSelf = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&WndProc),
            &hSelf);
    }

    g_mainThreadId = ::GetCurrentThreadId();
    DbgLog("Initialize: main_thread_id=%lu", (unsigned long)g_mainThreadId);

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);

    if (!g_classRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = &WndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = kClassName;
        ATOM atom    = ::RegisterClassW(&wc);
        DWORD lastErr = ::GetLastError();   // capture BEFORE any other call
        if (atom == 0 && lastErr != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        g_classRegistered = true;
    }

    g_hwnd = ::CreateWindowExW(
        /*dwExStyle*/   0,
        /*lpClassName*/ kClassName,
        /*lpWindowName*/L"vwmcp dispatch",
        /*dwStyle*/     0,
        /*x,y,w,h*/     0, 0, 0, 0,
        /*hWndParent*/  HWND_MESSAGE,
        /*hMenu*/       nullptr,
        /*hInstance*/   hInst,
        /*lpParam*/     nullptr);
    return (g_hwnd != nullptr);
#else
    return true;
#endif
}

void MainThreadDispatch::Shutdown()
{
#ifdef _WIN32
    if (g_hwnd) {
        ::DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_classRegistered) {
        ::UnregisterClassW(kClassName, ::GetModuleHandleW(nullptr));
        g_classRegistered = false;
    }
#endif
}

bool MainThreadDispatch::IsReady()
{
#ifdef _WIN32
    return g_hwnd != nullptr;
#else
    return true;
#endif
}

void MainThreadDispatch::EnqueueErased(std::function<void()> task)
{
#ifdef _WIN32
    DbgLog("EnqueueErased: thread_id=%lu, g_hwnd=%p",
           (unsigned long)::GetCurrentThreadId(), (void*)g_hwnd);
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        g_queue.push(std::move(task));
    }
    if (g_hwnd) {
        // PostMessage is asynchronous: the call returns immediately on the
        // worker thread; VW's main message pump dispatches WM_VWMCP_DRAIN to
        // our WndProc when it next services its queue.
        BOOL ok = ::PostMessageW(g_hwnd, WM_VWMCP_DRAIN, 0, 0);
        DbgLog("EnqueueErased: PostMessageW ok=%d lastErr=%lu",
               (int)ok, (unsigned long)::GetLastError());
    } else {
        // Dispatcher wasn't initialized — fall back to direct invocation.
        // This preserves prior (buggy) behavior rather than silently dropping
        // the task. Hits only if SocketBridge starts before plugin_module_main
        // has run, which the auto-start defer-to-call-4 guard already prevents.
        std::function<void()> immediate;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (!g_queue.empty()) {
                immediate = std::move(g_queue.front());
                g_queue.pop();
            }
        }
        if (immediate) {
            try { immediate(); } catch (...) {}
        }
    }
#else
    // Non-Windows fallback: run inline on the calling thread. Wrong on Mac
    // (this is the bug we're fixing on Win), but lets the code compile until
    // a libdispatch implementation lands.
    try { task(); } catch (...) {}
#endif
}

} // namespace vwmcp
