// Picks which top-level desktop window holds the visible wallpaper for widget
// glass capture:
//  - On Windows 11 24H2+ (Raised Desktop) or an unsplit desktop, Progman owns
//    SHELLDLL_DefView and any live-wallpaper WorkerW child inside itself.
//  - On Windows 10 and Windows 11 23H2, tools such as Wallpaper Engine split
//    the desktop into sibling top-level WorkerW windows: one holds
//    SHELLDLL_DefView and another directly behind it in Z-order hosts the
//    wallpaper window, while Progman retains only the static wallpaper.
#pragma once

#include <windows.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace atrium {

template<typename Fn> inline bool SubmitThreadpoolWork(Fn fn)
{
    static std::mutex jobLock;
    static std::unordered_map<void*, std::shared_ptr<Fn>> jobs;
    auto job = std::make_shared<Fn>(std::move(fn));
    {
        std::lock_guard g(jobLock);
        jobs.emplace(job.get(), job);
    }
    auto callback = [](PTP_CALLBACK_INSTANCE, PVOID ctx) {
        std::shared_ptr<Fn> local;
        {
            std::lock_guard g(jobLock);
            if (auto it = jobs.find(ctx); it != jobs.end()) {
                local = std::move(it->second);
                jobs.erase(it);
            }
        }
        if (local)
            (*local)();
    };
    if (!TrySubmitThreadpoolCallback(callback, job.get(), nullptr)) {
        std::lock_guard g(jobLock);
        jobs.erase(job.get());
        return false;
    }
    return true;
}

// Waits at most `budgetMs` for every handle in `handles`; returns true if all signaled or empty.
inline bool WaitBoundedHandles(const std::vector<HANDLE>& handles, DWORD budgetMs)
{
    if (handles.empty())
        return true;
    const DWORD count = static_cast<DWORD>(std::min<size_t>(handles.size(), MAXIMUM_WAIT_OBJECTS));
    return WaitForMultipleObjects(count, handles.data(), TRUE, budgetMs) == WAIT_OBJECT_0;
}

struct DesktopHostCandidate {
    HWND hwnd = nullptr;
    bool isProgman = false;
    bool hasDefView = false;
    bool visible = false;
    bool cloaked = false;
    bool hasVisibleChild = false; // visible child window >= 64x64 (excluding SHELLDLL_DefView)
    RECT rect{};
};

struct DesktopBackdropTarget {
    HWND hwnd = nullptr;
    bool isProgman = true;
};

inline DesktopBackdropTarget SelectDesktopBackdropTarget(const std::vector<DesktopHostCandidate>& windows, HWND skipWorker = nullptr)
{
    const DesktopHostCandidate* progman = nullptr;
    size_t defViewIdx = windows.size();
    for (size_t i = 0; i < windows.size(); ++i) {
        const DesktopHostCandidate& w = windows[i];
        if (!w.hwnd)
            continue;
        if (w.isProgman && !progman)
            progman = &w;
        if (!w.isProgman && w.hasDefView && defViewIdx == windows.size())
            defViewIdx = i;
    }
    if (progman && progman->hasDefView)
        return {progman->hwnd, true};

    if (defViewIdx < windows.size()) {
        for (size_t i = defViewIdx + 1; i < windows.size(); ++i) {
            const DesktopHostCandidate& w = windows[i];
            if (w.isProgman || !w.hwnd || w.hwnd == skipWorker || !w.visible || w.cloaked || !w.hasVisibleChild)
                continue;
            const LONG width = w.rect.right - w.rect.left;
            const LONG height = w.rect.bottom - w.rect.top;
            if (width >= 320 && height >= 200)
                return {w.hwnd, false};
        }
    }

    if (progman)
        return {progman->hwnd, true};
    return {};
}

} // namespace atrium
