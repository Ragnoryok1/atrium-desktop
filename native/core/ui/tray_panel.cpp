#include "ui/tray_panel.h"

#include "clock.h"
#include "localization.h"
#include "log.h"
#include "motion.h"
#include "shell/keyboard_hook.h"

#include <commctrl.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace atrium {

namespace {

constexpr float kCell = 40, kIcon = 20, kAir = (kCell - kIcon) / 2;
constexpr float kSplit = 14, kFooterH = 24;
constexpr double kStageMs = 240, kStaggerMs = 14, kRippleMs = 520;
constexpr UINT_PTR kRefreshTimer = 0x5452415A, kTipTimer = 0x5A15, kHoverTimer = 0x5A17, kClickTimer = 0x5A11, kSettleTimer = 0x5A13, kMenuTimer = 0x5A19;
constexpr UINT kHoverMs = 50, kSettleMs = 150, kMenuPollMs = 120, kOutsideClickMsg = WM_APP + 0x5A;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::optional<size_t> IconIndex(std::wstring_view id)
{
    if (!id.starts_with(L"icon:") || id.size() == 5)
        return std::nullopt;
    size_t value = 0;
    for (wchar_t ch : id.substr(5)) {
        if (ch < L'0' || ch > L'9' || value > (SIZE_MAX - static_cast<size_t>(ch - L'0')) / 10)
            return std::nullopt;
        value = value * 10 + static_cast<size_t>(ch - L'0');
    }
    return value;
}

std::optional<size_t> FindTargetIndex(const std::vector<TrayIcon>& icons, const tray::TargetKey& key)
{
    if (!key.shell.Valid())
        return std::nullopt;
    std::optional<size_t> found;
    for (size_t i = 0; i < icons.size(); ++i) {
        const auto cand = icons[i].Target();
        if (cand.shell != key.shell ||
            (!key.identity.empty() && !cand.identity.empty() ? key.identity != cand.identity : key.runtime.empty() || key.runtime != cand.runtime))
            continue;
        if (found)
            return std::nullopt;
        found = i;
    }
    return found;
}

bool ProcessMatchesExe(DWORD pid, const std::wstring& exe)
{
    if (!pid || exe.empty())
        return false;
    bool same = false;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(process, 0, buf, &size))
            same = CompareStringOrdinal(buf, int(size), exe.c_str(), -1, TRUE) == CSTR_EQUAL;
        CloseHandle(process);
    }
    return same;
}

bool IsCustomMenuStyle(HWND hwnd, const wchar_t* cls)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) && !(ex & WS_EX_APPWINDOW) &&
           _wcsicmp(cls, L"tooltips_class32") != 0;
}

struct MenuFollow {
    HWINEVENTHOOK hook = nullptr;
    POINT at{}, placed{};
    std::wstring exe;
    UINT_PTR timer = 0;
    DWORD pid = 0, clickInput = 0;
    ULONGLONG until = 0;
    HWND menu = nullptr;
    int moves = 0;
} g_follow;

void StopFollow()
{
    if (g_follow.hook)
        UnhookWinEvent(g_follow.hook);
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.hook = nullptr, g_follow.menu = nullptr, g_follow.timer = 0, g_follow.pid = 0;
}

void CALLBACK FollowExpired(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer == g_follow.timer && GetTickCount64() >= g_follow.until)
        StopFollow();
}

POINT MenuPlace(POINT at, int w, int h)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    return tray::PlaceMenu(at, w, h, mi.rcWork);
}

bool FollowStillValid()
{
    if (GetTickCount64() > g_follow.until)
        return StopFollow(), false;
    constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_RETURN, VK_ESCAPE, VK_SPACE};
    const bool keyDown = std::ranges::any_of(kKeys, [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; });
    LASTINPUTINFO input{sizeof(input)};
    POINT cursor{};
    const SIZE dblClick{GetSystemMetrics(SM_CXDOUBLECLK), GetSystemMetrics(SM_CYDOUBLECLK)};
    if (!GetLastInputInfo(&input) || !GetCursorPos(&cursor) ||
        TrayFollowCancelled(input.dwTime, g_follow.clickInput, TrayModel::LastSyntheticInput(), TrayModel::SyntheticPointerInFlight(), g_follow.at, cursor,
                            dblClick, keyDown)) {
        StopFollow();
        return false;
    }
    return true;
}

bool FollowProcessAllowed(DWORD pid, std::wstring& path)
{
    if (g_follow.exe.empty())
        return false;
    bool elevated = true;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(process, 0, buf, &size))
            path.assign(buf, size);
        HANDLE token = nullptr;
        if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD got = 0;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got))
                elevated = e.TokenIsElevated != 0;
            CloseHandle(token);
        }
        CloseHandle(process);
    }
    if (elevated || path.empty() || CompareStringOrdinal(path.c_str(), -1, g_follow.exe.c_str(), -1, TRUE) != CSTR_EQUAL)
        return false;
    return g_follow.menu || g_follow.pid == 0 || pid == g_follow.pid;
}

void AdjustMovedMenu(HWND hwnd)
{
    RECT r{};
    if (hwnd != g_follow.menu || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    if ((std::abs(r.left - g_follow.placed.x) > 2 || std::abs(r.top - g_follow.placed.y) > 2) && g_follow.moves < 3) {
        const POINT p = MenuPlace(g_follow.at, r.right - r.left, r.bottom - r.top);
        g_follow.placed = p, ++g_follow.moves;
        SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Log(L"tray: the menu went to %ld,%ld again: back to %ld,%ld", r.left, r.top, p.x, p.y);
    }
}

void PlaceInitialFollowedMenu(HWND hwnd, DWORD pid, const wchar_t* name, const wchar_t* cls, bool nativeMenu)
{
    RECT r{};
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(g_follow.at, MONITOR_DEFAULTTONEAREST), &mi);
    if (w < 40 || h < 20 || w > (mi.rcWork.right - mi.rcWork.left) * 3 / 5 || h > (mi.rcWork.bottom - mi.rcWork.top) * 19 / 20)
        return;
    if (!tray::ShouldMoveFollowedMenu(r, g_follow.at, mi.rcMonitor, mi.rcWork, nativeMenu)) {
        StopFollow();
        return;
    }
    const POINT p = MenuPlace(g_follow.at, w, h);
    if (!SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        Log(L"tray: %s's menu (%s) at %ld,%ld could not be moved (%lu)", name, cls, r.left, r.top, GetLastError());
        StopFollow();
        return;
    }
    Log(L"tray: %s's menu (%s) moved from %ld,%ld to %ld,%ld", name, cls, r.left, r.top, p.x, p.y);
    g_follow.menu = hwnd, g_follow.pid = pid, g_follow.placed = p, g_follow.moves = 0, g_follow.until = GetTickCount64() + 800;
    if (g_follow.timer)
        KillTimer(nullptr, g_follow.timer);
    g_follow.timer = SetTimer(nullptr, 0, 800, FollowExpired);
    if (!g_follow.timer)
        StopFollow();
}

void CALLBACK MenuShown(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if ((event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE) || !FollowStillValid())
        return;
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || (!g_follow.menu && event != EVENT_OBJECT_SHOW))
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring path;
    if (!pid || (g_follow.menu && (hwnd != g_follow.menu || pid != g_follow.pid)) || !FollowProcessAllowed(pid, path))
        return;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64))
        return;
    const bool nativeMenu = wcscmp(cls, L"#32768") == 0;
    if (!nativeMenu && !IsCustomMenuStyle(hwnd, cls))
        return;
    g_follow.menu ? AdjustMovedMenu(hwnd) : PlaceInitialFollowedMenu(hwnd, pid, PathFindFileNameW(path.c_str()), cls, nativeMenu);
}

void FollowMenu(POINT at, const std::wstring& exe, DWORD preferredPid)
{
    StopFollow();
    if (exe.empty() || _wcsicmp(PathFindFileNameW(exe.c_str()), L"explorer.exe") == 0)
        return;
    LASTINPUTINFO input{sizeof(input)};
    if (!GetLastInputInfo(&input))
        return;
    g_follow.clickInput = input.dwTime, g_follow.at = at, g_follow.exe = exe, g_follow.pid = preferredPid, g_follow.until = GetTickCount64() + 3000;
    g_follow.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, MenuShown, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (g_follow.hook)
        g_follow.timer = SetTimer(nullptr, 0, 3000, FollowExpired);
    if (!g_follow.timer)
        StopFollow();
}

bool IsMenuPopupWindow(HWND root, DWORD menuPid, const std::wstring& menuExe)
{
    if (!root || !IsWindowVisible(root))
        return false;
    if (g_follow.menu && root == g_follow.menu)
        return true;
    wchar_t cls[64]{};
    if (!GetClassNameW(root, cls, 64))
        return false;
    if (wcscmp(cls, L"#32768") == 0)
        return true;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(root, &pid);
    if (!pid || ((!menuPid || pid != menuPid) && !ProcessMatchesExe(pid, menuExe)))
        return false;
    GUITHREADINFO gui{sizeof(gui)};
    return (GetGUIThreadInfo(tid, &gui) && (gui.flags & GUI_INMENUMODE)) || IsCustomMenuStyle(root, cls);
}

} // namespace

TrayPanel::TrayPanel(HINSTANCE instance, TrayModel& model) : surface_(instance, L"Atrium Tray", true), model_(model)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
}

TrayPanel::~TrayPanel()
{
    EndMenuSession();
    StopFollow();
}

float TrayPanel::Margin() const { return std::clamp(radius_ * 0.65f, 14.f, 22.f); }

int TrayPanel::Cols() const
{
    if (columns_)
        return columns_;
    const int n = int(icons_.size()), rows = std::max(1, (n + 5) / 6);
    return std::clamp((n + rows - 1) / rows, 4, 6);
}

D2D1_SIZE_F TrayPanel::Size() const
{
    int cols = Cols(), rows = std::max(1, int((icons_.size() + size_t(cols) - 1) / size_t(cols)));
    const float m = Margin();
    return D2D1::SizeF(2 * (m - kAir) + float(cols) * kCell, (m - kAir) + float(rows) * kCell - kAir + 2 * kSplit + kFooterH + m);
}

D2D1_RECT_F TrayPanel::CellRect(size_t i) const
{
    const size_t cols = size_t(Cols()), row = i / cols;
    const float pad = Margin() - kAir;
    return D2D1::RectF(pad + float(i % cols) * kCell, pad + float(row) * kCell, pad + float(i % cols + 1) * kCell, pad + float(row + 1) * kCell);
}

void TrayPanel::Place()
{
    RECT r = PopupRect(bar_, Size(), PopupAlign::Right);
    if (surface_.Visible())
        surface_.Move(r, menuSession_); // do not raise the panel over an open app context menu
    else
        surface_.Show(r, true, PopupOrigin(bar_, r, anchor_));
}

void TrayPanel::Open(RECT anchor, RECT bar, HWND previous)
{
    EndMenuSession();
    previous_ = previous, anchor_ = anchor, bar_ = bar;
    icons_ = model_.Icons(), columns_ = 0, columns_ = Cols();
    hover_.clear(), pressed_.clear(), hoverAnim_.clear(), openedMs_ = ClockMs();
    model_.Refresh();
    model_.SetHostPlaces(anchor);
    SetTimer(surface_.Hwnd(), kRefreshTimer, 1500, nullptr);
    Place();
    Log(L"tray: panel opened (%zu icons)", icons_.size());
}

void TrayPanel::Close()
{
    EndMenuSession();
    model_.Hover(nullptr);
    HideTip();
    for (UINT_PTR id : {kRefreshTimer, kClickTimer, kSettleTimer, kHoverTimer})
        KillTimer(surface_.Hwnd(), id);
    lastClick_.reset();
    if (!surface_.Visible())
        return;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

void TrayPanel::StartMenuSession(const TrayIcon& icon)
{
    EndMenuSession();
    menuSession_ = true, menuObserved_ = false, menuSessionAt_ = GetTickCount64(), menuPid_ = icon.pid, menuExe_ = icon.exe;
    if (HWND hwnd = surface_.Hwnd()) {
        mouseWatcherToken_ = KeyboardHook::Instance().AddMouseDownWatcher(hwnd, kOutsideClickMsg);
        SetTimer(hwnd, kMenuTimer, kMenuPollMs, nullptr);
    }
}

void TrayPanel::EndMenuSession()
{
    if (mouseWatcherToken_ > 0) {
        KeyboardHook::Instance().RemoveMouseDownWatcher(mouseWatcherToken_);
        mouseWatcherToken_ = 0;
    }
    if (HWND hwnd = surface_.Hwnd())
        KillTimer(hwnd, kMenuTimer);
    menuSession_ = false, menuObserved_ = false, menuSessionAt_ = 0, panelReenterUntil_ = 0, menuPid_ = 0, menuExe_.clear();
}

bool TrayPanel::IsPointInActiveMenu(POINT screenPt) const
{
    HWND under = WindowFromPoint(screenPt);
    HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
    return root && (root == surface_.Hwnd() || (tip_ && root == tip_) || IsMenuPopupWindow(root, menuPid_, menuExe_));
}

bool TrayPanel::IsActiveMenuOpen() const
{
    if (g_follow.menu && IsWindowVisible(g_follow.menu))
        return true;
    struct Ctx {
        DWORD pid;
        const std::wstring& exe;
        bool open = false;
    } ctx{menuPid_, menuExe_};
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(param);
            return IsMenuPopupWindow(h, c.pid, c.exe) ? (c.open = true, FALSE) : TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.open;
}

void TrayPanel::Refresh()
{
    if (!surface_.Visible() || !pressed_.empty())
        return;
    const bool hadIconHover = hover_.starts_with(L"icon:"), hadTip = !tipFor_.empty();
    const auto hoverIdx = IconIndex(hover_), tipIdx = IconIndex(tipFor_);
    const auto hoverTarget = hoverIdx && *hoverIdx < icons_.size() ? std::optional(icons_[*hoverIdx].Target()) : std::nullopt;
    const auto tipTarget = tipIdx && *tipIdx < icons_.size() ? std::optional(icons_[*tipIdx].Target()) : std::nullopt;
    auto fresh = model_.Icons();
    std::vector<tray::TargetKey> oldKeys, freshKeys;
    for (const auto& icon : icons_)
        oldKeys.push_back(icon.Target());
    for (const auto& icon : fresh)
        freshKeys.push_back(icon.Target());
    icons_.clear();
    for (size_t i : tray::StableOrder(oldKeys, freshKeys))
        icons_.push_back(std::move(fresh[i]));
    if (const auto newHoverIdx = hoverTarget ? FindTargetIndex(icons_, *hoverTarget) : std::nullopt)
        hover_ = L"icon:" + std::to_wstring(*newHoverIdx);
    else if (hadIconHover)
        hover_.clear(), KillTimer(surface_.Hwnd(), kHoverTimer), model_.Hover(nullptr);
    Place(), surface_.Invalidate();
    if (hadTip) {
        if (const auto newTipIdx = tipTarget ? FindTargetIndex(icons_, *tipTarget) : std::nullopt) {
            tipFor_ = L"icon:" + std::to_wstring(*newTipIdx);
            if (tipFor_ == hover_ && tip_ && IsWindowVisible(tip_))
                ShowTip();
        } else
            HideTip();
    }
}

void TrayPanel::ArmTip()
{
    HideTip();
    const std::optional<size_t> idx = IconIndex(hover_);
    if (!idx || *idx >= icons_.size() || private_ || icons_[*idx].appTooltip || icons_[*idx].tip.empty())
        return;
    tipFor_ = hover_;
    UINT delay = tip_ ? UINT(SendMessageW(tip_, TTM_GETDELAYTIME, TTDT_INITIAL, 0)) : 500;
    SetTimer(surface_.Hwnd(), kTipTimer, delay ? delay : 500, nullptr);
}

RECT TrayPanel::CellScreenRect(size_t i) const
{
    const RECT win = surface_.Rect();
    const float s = surface_.Scale();
    const D2D1_RECT_F cell = CellRect(i);
    return {win.left + LONG(cell.left * s), win.top + LONG(cell.top * s), win.left + LONG(cell.right * s), win.top + LONG(cell.bottom * s)};
}

void TrayPanel::ShowTip()
{
    const std::optional<size_t> idx = IconIndex(tipFor_);
    if (tipFor_ != hover_ || !idx || *idx >= icons_.size() || !surface_.Visible())
        return;
    const size_t i = *idx;
    const RECT win = surface_.Rect(), cellPx = CellScreenRect(i);
    const float s = surface_.Scale();
    const D2D1_RECT_F cell = CellRect(i);
    const std::wstring& text = icons_[i].tip;
    if (tip_ && IsWindowVisible(tip_) && text == tipText_ && EqualRect(&cellPx, &tipCell_))
        return;
    TTTOOLINFOW ti{sizeof(ti), TTF_TRACK | TTF_ABSOLUTE, surface_.Hwnd(), 1};
    if (!tip_) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
        tip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, surface_.Hwnd(), nullptr, nullptr, nullptr);
        if (!tip_)
            return;
        ti.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    ti.lpszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, LPARAM(400 * s));
    SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    if (!IsWindowVisible(tip_)) {
        SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(win.left, win.top));
        SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    }
    const LRESULT size = SendMessageW(tip_, TTM_GETBUBBLESIZE, 0, reinterpret_cast<LPARAM>(&ti));
    const int w = LOWORD(size), h = HIWORD(size);
    POINT at{win.left + LONG((cell.left + cell.right) / 2 * s) - w / 2, win.top + LONG(cell.top * s) - h - LONG(4 * s)};
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(surface_.Hwnd(), MONITOR_DEFAULTTONEAREST), &mi)) {
        at.x = std::clamp(at.x, mi.rcWork.left, std::max(mi.rcWork.left, mi.rcWork.right - w));
        if (at.y < mi.rcWork.top)
            at.y = win.top + LONG(cell.bottom * s) + LONG(4 * s);
    }
    SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(at.x, at.y));
    tipText_ = text, tipCell_ = cellPx;
}

void TrayPanel::HideTip()
{
    KillTimer(surface_.Hwnd(), kTipTimer), tipFor_.clear(), tipText_.clear(), tipCell_ = {};
    if (!tip_)
        return;
    TTTOOLINFOW ti{sizeof(ti), 0, surface_.Hwnd(), 1};
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
}

void TrayPanel::HoverAt(POINT pt)
{
    const std::wstring h = HitAt(pt);
    if (h == hover_)
        return;
    hover_ = h;
    const std::optional<size_t> idx = IconIndex(h);
    const bool hosted = idx && *idx < icons_.size() && !icons_[*idx].hostKey.empty();
    model_.Hover(hosted ? &icons_[*idx] : nullptr, hosted ? CellScreenRect(*idx) : RECT{});
    ArmTip(), surface_.Invalidate();
}

void TrayPanel::LeaveHover()
{
    const std::optional<size_t> idx = IconIndex(hover_);
    const size_t i = idx.value_or(icons_.size());
    if (i < icons_.size() && surface_.Visible()) {
        POINT cur{};
        HWND under = GetCursorPos(&cur) ? WindowFromPoint(cur) : nullptr;
        if (under == surface_.Hwnd() && HitAt(surface_.ScreenToContent(cur)) == hover_) {
            KillTimer(surface_.Hwnd(), kHoverTimer);
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
                tracking_ = TrackMouseEvent(&tme) != FALSE;
            }
            return;
        }
        DWORD pid = 0;
        if (under && GetWindowThreadProcessId(under, &pid) && pid == icons_[i].pid && pid != GetCurrentProcessId()) {
            SetTimer(surface_.Hwnd(), kHoverTimer, kHoverMs, nullptr);
            return;
        }
    }
    KillTimer(surface_.Hwnd(), kHoverTimer), model_.Hover(nullptr), hover_.clear(), surface_.Invalidate();
}

void TrayPanel::SetAttention(const std::vector<TaskItem>& tasks)
{
    std::vector<std::wstring> next;
    std::vector<DWORD> nextPids;
    for (const auto& task : tasks) {
        if (!task.attention)
            continue;
        next.push_back(task.key);
        for (DWORD pid = 0; HWND w : task.windows)
            if (GetWindowThreadProcessId(w, &pid) && pid && std::ranges::find(nextPids, pid) == nextPids.end())
                nextPids.push_back(pid);
    }
    if (next != attention_ || nextPids != attentionPids_)
        attention_ = std::move(next), attentionPids_ = std::move(nextPids), surface_.Invalidate();
}

bool TrayPanel::HasAttention(const TrayIcon& icon) const
{
    if (icon.flashing)
        return true;
    return icon.pid ? std::ranges::find(attentionPids_, icon.pid) != attentionPids_.end() :
                      (!icon.exe.empty() && std::ranges::find(attention_, icon.exe) != attention_.end());
}

float TrayPanel::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& f = hoverAnim_[id];
    float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void TrayPanel::PaintIconCell(Canvas& c, size_t i, float plateR, int iconPx)
{
    const Theme& t = c.theme;
    const TrayIcon& icon = icons_[i];
    const D2D1_RECT_F cell = CellRect(i);
    const std::wstring id = L"icon:" + std::to_wstring(i);
    const float e = motion::EaseOut(float((c.now - openedMs_ - kStaggerMs * double(std::min<size_t>(i, 18))) / kStageMs));
    if (e < 1)
        c.Continue();
    c.BeginGroup(e, 1, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2, 0, 6 * (1 - e));
    const float hv = Hover(c, id, hover_ == id);
    const D2D1_RECT_F plate = D2D1::RectF(cell.left + 2, cell.top + 2, cell.right - 2, cell.bottom - 2);
    if (hv > 0.001f)
        c.Fill(plate, plateR, Alpha(t.hover, hv));
    if (pressed_ == id)
        c.Fill(plate, plateR, t.press);
    if (rippleId_ == id && c.now - rippleMs_ < kRippleMs) {
        c.Ripple(plate, plateR, rippleX_, rippleY_, float((c.now - rippleMs_) / kRippleMs),
                 t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    const float cx = (cell.left + cell.right) / 2, cy = (cell.top + cell.bottom) / 2;
    const D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, cy - kIcon / 2, cx + kIcon / 2, cy + kIcon / 2);
    c.BeginGroup(1, pressed_ == id ? 0.9f : 1 + 0.1f * hv, cx, cy);
    if (icon.image) {
        const float dark = icon.inked ? c.DarkAt(ir) : 0.f, inked = !icon.inked ? 0.f : icon.inkedOnLight ? dark : 1 - dark;
        wchar_t key[64];
        if (inked < 0.999f)
            swprintf_s(key, L"tray|%llu|normal|%d", icon.imageVersion, iconPx), c.Image(key, *icon.image, ir, 1 - inked);
        if (inked > 0.001f)
            swprintf_s(key, L"tray|%llu|inked|%d", icon.imageVersion, iconPx), c.Image(key, *icon.inked, ir, inked);
    } else {
        const std::wstring title = TrayModel::Title(icon.tip);
        c.Fill(ir, kIcon / 2, Alpha(t.hover, 1.4f));
        c.Text(title.empty() ? L"?" : title.substr(0, 1), ir, 10, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    c.EndGroup(), c.EndGroup();
    if (HasAttention(icon)) {
        const float pulse = 0.72f + 0.28f * float(std::sin((c.now - openedMs_) * 0.006));
        c.Fill(D2D1::RectF(ir.right - 3, ir.top - 3, ir.right + 4, ir.top + 4), 3.5f, D2D1::ColorF(0.96f, 0.22f, 0.25f, pulse));
        c.Continue();
    }
    hits_.push_back({cell, id});
}

void TrayPanel::PaintFooter(Canvas& c, float w, float footTop, float divider)
{
    const Theme& t = c.theme;
    const float m = Margin();
    c.Line(m, divider, w - m, divider, t.divider);
    std::wstring label;
    if (const auto idx = IconIndex(hover_); idx && *idx < icons_.size())
        label = private_ ? Tr(L"Tray icon") : TrayModel::Title(icons_[*idx].tip);
    if (label.empty())
        label = icons_.empty() ? Tr(L"Tray") : TrFormat(L"%d icons", int(icons_.size()));
    const std::wstring sys = Tr(L"System tray");
    const D2D1_RECT_F b = D2D1::RectF(w - m - (c.TextWidth(sys, 11.5f) + 20), footTop, w - m, footTop + kFooterH);
    c.Text(label, D2D1::RectF(m, footTop, b.left - 10, footTop + kFooterH), 12, hover_.starts_with(L"icon:") ? t.text : t.subtext);
    const float hv = Hover(c, L"system", hover_ == L"system");
    c.Fill(b, (b.bottom - b.top) / 2, Alpha(pressed_ == L"system" ? t.press : t.hover, 0.45f + 0.55f * hv));
    c.Text(sys, b, 11.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    hits_.push_back({b, L"system"});
}

void TrayPanel::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const D2D1_SIZE_F size = Size();
    const float w = size.width, h = size.height, m = Margin(), gridTop = m - kAir, footTop = h - m - kFooterH, divider = footTop - kSplit;
    hits_.clear();
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), radius_, t.panel);
    if (icons_.empty()) {
        const std::wstring msg = !model_.Loaded() ? Tr(L"Reading…") : model_.Available() ? Tr(L"No tray icons") : Tr(L"The system tray is unavailable");
        c.Text(msg, D2D1::RectF(m, gridTop, w - m, gridTop + kCell), 12.5f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    const float plateR = std::clamp(radius_ - m - 2, 6.f, 12.f);
    const int iconPx = int(std::lround(kIcon * c.scale));
    for (size_t i = 0; i < icons_.size(); ++i)
        PaintIconCell(c, i, plateR, iconPx);
    PaintFooter(c, w, footTop, divider);
}

std::wstring TrayPanel::HitAt(POINT pt) const
{
    const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void TrayPanel::Activate(const std::wstring& id, bool right)
{
    if (id == L"system") {
        if (!right)
            Close(), onSystemTray ? onSystemTray() : void();
        return;
    }
    const std::optional<size_t> idx = IconIndex(id);
    if (!idx || *idx >= icons_.size())
        return;
    const TrayIcon selected = pressedIcon_.value_or(icons_[*idx]);
    const RECT cell = CellScreenRect(*idx);
    POINT at{};
    GetCursorPos(&at);
    if (right) {
        FollowMenu(at, selected.exe, selected.pid), lastClick_.reset();
        KillTimer(surface_.Hwnd(), kClickTimer), KillTimer(surface_.Hwnd(), kSettleTimer);
        StartMenuSession(selected), model_.Click(selected, true, cell);
        return;
    }
    EndMenuSession();
    const bool second = lastClick_ && tray::SameGestureTarget(lastClick_->Target(), selected.Target()) &&
                        GetTickCount64() - lastWhen_ <= GetDoubleClickTime() && std::abs(at.x - lastAt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                        std::abs(at.y - lastAt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    if (second) {
        model_.DoubleClick(selected, true, cell), lastClick_.reset(), Close();
    } else {
        model_.Click(selected, false, cell);
        lastClick_ = selected, lastAt_ = at, lastWhen_ = GetTickCount64();
        SetTimer(surface_.Hwnd(), kClickTimer, GetDoubleClickTime(), nullptr);
    }
}

bool TrayPanel::OnTimerMessage(WPARAM w)
{
    if (w == kTipTimer)
        return KillTimer(surface_.Hwnd(), kTipTimer), ShowTip(), true;
    if (w == kSettleTimer) {
        KillTimer(surface_.Hwnd(), kSettleTimer);
        HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        if (fg)
            GetClassNameW(fg, cls, 64);
        const bool shell = !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
        if (!menuSession_ && fg != surface_.Hwnd() && !shell)
            Close();
        return true;
    }
    if (w == kClickTimer)
        return KillTimer(surface_.Hwnd(), kClickTimer), lastClick_.reset(), !menuSession_ ? Close() : void(), true;
    if (w == kRefreshTimer)
        return surface_.Visible() ? model_.Refresh() : void(), true;
    if (w == kHoverTimer)
        return LeaveHover(), true;
    if (w == kMenuTimer && menuSession_) {
        if (IsActiveMenuOpen())
            return menuObserved_ = true, true;
        if (menuObserved_ || GetTickCount64() - menuSessionAt_ >= 3500) {
            if (GetForegroundWindow() == surface_.Hwnd()) {
                EndMenuSession();
            } else if (menuObserved_ && GetTickCount64() < panelReenterUntil_) {
                SetForegroundWindow(surface_.Hwnd());
                EndMenuSession();
            } else {
                Close();
            }
        }
        return true;
    }
    return false;
}

void TrayPanel::OnMouseDownMessage(POINT pt)
{
    HideTip();
    pressed_ = HitAt(pt), pressedIcon_.reset();
    if (const auto idx = IconIndex(pressed_); idx && *idx < icons_.size())
        pressedIcon_ = icons_[*idx];
    if (!pressed_.empty()) {
        const float s = surface_.Scale();
        rippleId_ = pressed_, rippleX_ = float(pt.x) / s, rippleY_ = float(pt.y) / s, rippleMs_ = ClockMs();
        surface_.Ripple(rippleX_, rippleY_);
    }
    surface_.Invalidate();
}

void TrayPanel::OnMouseUpMessage(UINT m, POINT pt)
{
    const std::wstring h = HitAt(pt);
    const bool same = !h.empty() && h == pressed_;
    pressed_.clear(), surface_.Invalidate();
    if (same)
        Activate(h, m == WM_RBUTTONUP);
    pressedIcon_.reset();
}

void TrayPanel::OnOutsideMouseDown(LPARAM l)
{
    if (!surface_.Visible() || !menuSession_)
        return;
    const POINT screenPt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const RECT panelRect = surface_.Rect();
    if (PtInRect(&panelRect, screenPt)) {
        panelReenterUntil_ = GetTickCount64() + 300;
        return;
    }
    if (!IsPointInActiveMenu(screenPt))
        Close();
}

bool TrayPanel::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    if (m == kOutsideClickMsg)
        return OnOutsideMouseDown(l), true;
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_TIMER: return OnTimerMessage(w);
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && surface_.Visible()) {
            if (!menuSession_) {
                if (lastClick_ && GetTickCount64() - lastWhen_ <= GetDoubleClickTime())
                    SetTimer(surface_.Hwnd(), kSettleTimer, kSettleMs, nullptr);
                else
                    Close();
            }
        } else if (LOWORD(w) != WA_INACTIVE && menuSession_) {
            EndMenuSession();
        }
        return false;
    case WM_MOUSEMOVE:
        KillTimer(surface_.Hwnd(), kHoverTimer);
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        return HoverAt(pt), true;
    case WM_MBUTTONUP:
        if (const auto idx = IconIndex(HitAt(pt)); idx && *idx < icons_.size() && !icons_[*idx].hostKey.empty())
            model_.MiddleClick(icons_[*idx], CellScreenRect(*idx));
        return true;
    case WM_MOUSELEAVE:
        tracking_ = false, HideTip();
        if (!pressed_.empty())
            pressed_.clear(), surface_.Invalidate();
        return LeaveHover(), true;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: return OnMouseDownMessage(pt), true;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: return OnMouseUpMessage(m, pt), true;
    case WM_KEYDOWN: return w == VK_ESCAPE ? (Close(), true) : false;
    default: break;
    }
    return false;
}

} // namespace atrium
