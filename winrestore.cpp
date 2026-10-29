// winrestore: puts the debuggee's window (console or GUI) back where you want it when you
// restart debugging.
//
// Where the window goes, first match wins:
//   1. "Restore default (this session)" is on for this exe -> the plugin leaves it alone
//   2. a profile is assigned to this exe                     -> always opens there, moves are not saved
//   3. a global profile is set                               -> always opens there, moves are not saved
//   4. otherwise                                             -> opens where it was last, moves are saved
//
// Positions are stored relative to a monitor's work area (the part without the taskbar),
// together with that monitor's device name and rectangle. On restore the same monitor is
// looked up again (by rectangle, then name, then nearest), and the position is scaled if its
// resolution changed, so it keeps working when monitors are added, removed or rearranged.
//
// Everything is stored in winrestore.ini next to this DLL.

#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cstdio>
#include "_plugins.h"
#include "resource.h"

#define PLUGIN_NAME "winrestore"

#define SEC_GENERAL "general"   // enabled, global
#define SEC_LAST "last"         // exe path -> last position (mode 4)
#define SEC_PROFILES "profiles" // profile name -> position
#define SEC_ASSIGN "assign"     // exe path -> profile name

enum MenuEntry
{
    MENU_ENABLED,
    MENU_PROFILES,
    MENU_BYPASS,
    MENU_FORGET_CURRENT,
    MENU_FORGET_ALL,
    MENU_ABOUT,
};

static HINSTANCE g_inst;
static int g_handle;
static std::string g_ini;
static std::mutex g_iniLock;
static std::mutex g_consoleLock;

static std::thread g_worker;
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_reapply{false}; // set when profiles change, worker re-resolves
static std::atomic<bool> g_enabled{true};

static std::mutex g_stateLock;
static std::string g_currentExe; // exe path of the current/last debuggee
static DWORD g_currentPid;
static bool g_debugging;
static std::set<std::string> g_bypass; // exes restored to default until x64dbg closes

// ---------------------------------------------------------------- ini

static std::string iniGet(const char* sec, const std::string& key)
{
    std::lock_guard<std::mutex> lock(g_iniLock);
    char buf[2048] = "";
    GetPrivateProfileStringA(sec, key.c_str(), "", buf, sizeof(buf), g_ini.c_str());
    return buf;
}

// value == nullptr deletes the key
static void iniSet(const char* sec, const std::string& key, const char* value)
{
    std::lock_guard<std::mutex> lock(g_iniLock);
    WritePrivateProfileStringA(sec, key.c_str(), value, g_ini.c_str());
}

static void iniClearSection(const char* sec)
{
    std::lock_guard<std::mutex> lock(g_iniLock);
    WritePrivateProfileStringA(sec, nullptr, nullptr, g_ini.c_str());
}

static std::vector<std::string> iniKeys(const char* sec)
{
    std::lock_guard<std::mutex> lock(g_iniLock);
    std::vector<char> buf(1 << 16);
    DWORD n = GetPrivateProfileStringA(sec, nullptr, "", buf.data(), (DWORD)buf.size(), g_ini.c_str());
    std::vector<std::string> keys;
    for(const char* p = buf.data(); p < buf.data() + n && *p; p += strlen(p) + 1)
        keys.push_back(p);
    return keys;
}

// ---------------------------------------------------------------- state helpers

static std::string fileName(const std::string& path)
{
    auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static bool isBypassed(const std::string& exe)
{
    std::lock_guard<std::mutex> lock(g_stateLock);
    return g_bypass.count(exe) != 0;
}

static void currentTarget(std::string& exe, DWORD& pid, bool& debugging)
{
    std::lock_guard<std::mutex> lock(g_stateLock);
    exe = g_currentExe;
    pid = g_currentPid;
    debugging = g_debugging;
}

// ---------------------------------------------------------------- monitors and placements

struct Monitor
{
    HMONITOR h;
    std::string device; // \\.\DISPLAYn
    RECT rc;            // whole monitor, virtual screen coordinates
    RECT work;          // monitor minus taskbar
    bool primary;
};

struct Placement
{
    std::string device;
    RECT monitor;     // monitor rect when it was saved
    int workW, workH; // work area size when it was saved
    RECT rc;          // window rect relative to the work area's top-left
    bool maximized;
};

static BOOL CALLBACK monitorProc(HMONITOR h, HDC, LPRECT, LPARAM lp)
{
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    if(GetMonitorInfoA(h, &mi))
        ((std::vector<Monitor>*)lp)->push_back({h, mi.szDevice, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

static std::vector<Monitor> monitors()
{
    std::vector<Monitor> v;
    EnumDisplayMonitors(nullptr, nullptr, monitorProc, (LPARAM)&v);
    return v;
}

static std::string monitorLabel(int i, const Monitor& m)
{
    char buf[256];
    sprintf_s(buf, "Monitor %d: %ldx%ld%s (%s)", i + 1, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
              m.primary ? ", primary" : "", m.device.c_str());
    return buf;
}

// The monitor a placement belongs to on the current setup. Same rectangle first (same spot
// in the layout at the same resolution), then same device name, then whatever is nearest.
static int pickMonitor(const std::vector<Monitor>& mons, const Placement& p)
{
    for(size_t i = 0; i < mons.size(); i++)
        if(EqualRect(&mons[i].rc, &p.monitor) && mons[i].device == p.device)
            return (int)i;
    for(size_t i = 0; i < mons.size(); i++)
        if(EqualRect(&mons[i].rc, &p.monitor))
            return (int)i;
    for(size_t i = 0; i < mons.size(); i++)
        if(mons[i].device == p.device)
            return (int)i;
    POINT center{(p.monitor.left + p.monitor.right) / 2, (p.monitor.top + p.monitor.bottom) / 2};
    HMONITOR nearest = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    for(size_t i = 0; i < mons.size(); i++)
        if(mons[i].h == nearest)
            return (int)i;
    for(size_t i = 0; i < mons.size(); i++)
        if(mons[i].primary)
            return (int)i;
    return 0;
}

// Screen rect for a placement on monitor m: scaled if the work area size changed, then
// shrunk/moved so it is fully inside the work area.
static RECT toScreen(const Placement& p, const Monitor& m)
{
    const RECT& work = m.work;
    LONG W = work.right - work.left, H = work.bottom - work.top;
    RECT r = p.rc;
    if(p.workW > 0 && p.workH > 0 && (p.workW != W || p.workH != H))
    {
        r.left = MulDiv(r.left, W, p.workW);
        r.right = MulDiv(r.right, W, p.workW);
        r.top = MulDiv(r.top, H, p.workH);
        r.bottom = MulDiv(r.bottom, H, p.workH);
    }
    LONG w = min(max(r.right - r.left, 100L), W);
    LONG h = min(max(r.bottom - r.top, 50L), H);
    LONG x = min(max(work.left + r.left, work.left), work.right - w);
    LONG y = min(max(work.top + r.top, work.top), work.bottom - h);
    return {x, y, x + w, y + h};
}

static Placement fromScreen(const RECT& r, const Monitor& m, bool maximized)
{
    Placement p;
    p.device = m.device;
    p.monitor = m.rc;
    p.workW = m.work.right - m.work.left;
    p.workH = m.work.bottom - m.work.top;
    p.rc = {r.left - m.work.left, r.top - m.work.top, r.right - m.work.left, r.bottom - m.work.top};
    p.maximized = maximized;
    return p;
}

static std::string serialize(const Placement& p)
{
    char buf[512];
    sprintf_s(buf, "%s;%ld,%ld,%ld,%ld;%d,%d;%ld,%ld,%ld,%ld;%d", p.device.c_str(),
              p.monitor.left, p.monitor.top, p.monitor.right, p.monitor.bottom, p.workW, p.workH,
              p.rc.left, p.rc.top, p.rc.right, p.rc.bottom, p.maximized ? 1 : 0);
    return buf;
}

static bool parse(const std::string& s, Placement& p)
{
    auto semi = s.find(';');
    if(semi == std::string::npos)
        return false;
    p.device = s.substr(0, semi);
    int maximized = 0;
    if(sscanf_s(s.c_str() + semi + 1, "%ld,%ld,%ld,%ld;%d,%d;%ld,%ld,%ld,%ld;%d",
                &p.monitor.left, &p.monitor.top, &p.monitor.right, &p.monitor.bottom, &p.workW, &p.workH,
                &p.rc.left, &p.rc.top, &p.rc.right, &p.rc.bottom, &maximized) != 11)
        return false;
    p.maximized = maximized != 0;
    return p.rc.right > p.rc.left && p.rc.bottom > p.rc.top;
}

static bool loadProfile(const std::string& name, Placement& p)
{
    return !name.empty() && parse(iniGet(SEC_PROFILES, name), p);
}

// Positions must be in real pixels on every monitor, whatever DPI awareness x64dbg runs with.
struct DpiScope
{
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope()
    {
        if(old)
            SetThreadDpiAwarenessContext(old);
    }
};

// ---------------------------------------------------------------- windows

struct FindCtx
{
    DWORD pid;
    HWND found;
};

static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp)
{
    auto ctx = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if(pid == ctx->pid && IsWindowVisible(hwnd))
    {
        ctx->found = hwnd;
        if(!GetWindow(hwnd, GW_OWNER))
            return FALSE;
    }
    return TRUE;
}

// Top-level window of the debuggee. conhost reports the client PID for console windows,
// so this also finds the console. Under Windows Terminal the debuggee only owns a
// PseudoConsoleWindow whose owner is the real terminal window, so walk up to the root owner.
// Fallback: attach to its console and ask for the window.
static HWND findWindow(DWORD pid)
{
    FindCtx ctx{pid, nullptr};
    EnumWindows(enumProc, (LPARAM)&ctx);
    HWND hwnd = ctx.found;
    if(!hwnd)
    {
        std::lock_guard<std::mutex> lock(g_consoleLock);
        if(AttachConsole(pid))
        {
            hwnd = GetConsoleWindow();
            FreeConsole();
        }
    }
    if(!hwnd)
        return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
    if(root)
        hwnd = root;
    return IsWindowVisible(hwnd) ? hwnd : nullptr;
}

static bool capture(HWND hwnd, Placement& p)
{
    if(!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;
    auto mons = monitors();
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    const Monitor* m = nullptr;
    for(auto& x : mons)
        if(x.h == hmon)
            m = &x;
    if(!m)
        return false;
    RECT r;
    bool maximized = IsZoomed(hwnd) != FALSE;
    if(maximized)
    {
        // rcNormalPosition is in workspace coordinates, so only keep its size and center it
        WINDOWPLACEMENT wp{sizeof(wp)};
        GetWindowPlacement(hwnd, &wp);
        LONG w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        LONG h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        r.left = m->work.left + (m->work.right - m->work.left - w) / 2;
        r.top = m->work.top + (m->work.bottom - m->work.top - h) / 2;
        r.right = r.left + w;
        r.bottom = r.top + h;
    }
    else if(!GetWindowRect(hwnd, &r))
        return false;
    p = fromScreen(r, *m, maximized);
    return true;
}

static void apply(HWND hwnd, const Placement& p)
{
    auto mons = monitors();
    if(mons.empty())
        return;
    RECT r = toScreen(p, mons[pickMonitor(mons, p)]);
    if(IsIconic(hwnd) || IsZoomed(hwnd))
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if(p.maximized)
        ShowWindow(hwnd, SW_MAXIMIZE); // maximizes on the monitor it was just moved to
}

// Close enough counts: terminals snap their size to whole character cells.
static bool matches(HWND hwnd, const Placement& p)
{
    if(IsIconic(hwnd))
        return false;
    auto mons = monitors();
    if(mons.empty())
        return true;
    const Monitor& m = mons[pickMonitor(mons, p)];
    if(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != m.h)
        return false;
    if(p.maximized)
        return IsZoomed(hwnd) != FALSE;
    RECT want = toScreen(p, m), cur;
    GetWindowRect(hwnd, &cur);
    return !IsZoomed(hwnd) && abs(cur.left - want.left) <= 8 && abs(cur.top - want.top) <= 8 &&
           abs((cur.right - cur.left) - (want.right - want.left)) <= 40 &&
           abs((cur.bottom - cur.top) - (want.bottom - want.top)) <= 40;
}

// ---------------------------------------------------------------- worker

enum class Mode
{
    Off,      // restored to default for this session
    Fixed,    // profile, moves are not saved
    Remember, // last position, moves are saved
};

struct Target
{
    Mode mode = Mode::Off;
    bool have = false;
    Placement p{};
    std::string why;
};

static Target resolve(const std::string& exe)
{
    Target t;
    if(isBypassed(exe))
        return t;
    std::string name = iniGet(SEC_ASSIGN, exe);
    if(loadProfile(name, t.p))
    {
        t.mode = Mode::Fixed;
        t.have = true;
        t.why = "profile \"" + name + "\"";
        return t;
    }
    name = iniGet(SEC_GENERAL, "global");
    if(loadProfile(name, t.p))
    {
        t.mode = Mode::Fixed;
        t.have = true;
        t.why = "global profile \"" + name + "\"";
        return t;
    }
    t.mode = Mode::Remember;
    t.have = parse(iniGet(SEC_LAST, exe), t.p);
    t.why = "last position";
    return t;
}

static bool sleepOrStop(DWORD ms)
{
    for(DWORD t = 0; t < ms; t += 25)
    {
        if(g_stop)
            return true;
        Sleep(25);
    }
    return g_stop;
}

static void worker(DWORD pid, std::string exe)
{
    DpiScope dpi;

    // wait for the window to show up (console is created with the process, GUI windows later)
    HWND hwnd = nullptr;
    for(int i = 0; i < 600 && !hwnd; i++) // ~30s
    {
        if(sleepOrStop(50))
            return;
        hwnd = findWindow(pid);
    }
    if(!hwnd)
        return;

    Target t;
    std::string lastSaved;
    g_reapply = true;
    while(!g_stop && IsWindow(hwnd))
    {
        if(g_reapply.exchange(false))
        {
            t = resolve(exe);
            if(t.mode != Mode::Off && t.have)
            {
                // Windows/the terminal sometimes moves or minimizes the window right after
                // creation, so keep re-applying for a short while until it sticks.
                for(int i = 0; i < 10 && !g_stop && IsWindow(hwnd); i++)
                {
                    if(!matches(hwnd, t.p))
                        apply(hwnd, t.p);
                    sleepOrStop(100);
                }
                _plugin_logprintf("[" PLUGIN_NAME "] %s: moved to %s\n", fileName(exe).c_str(), t.why.c_str());
            }
            lastSaved = t.mode == Mode::Remember && t.have ? serialize(t.p) : "";
        }
        Placement cur;
        if(t.mode == Mode::Remember && capture(hwnd, cur))
        {
            auto s = serialize(cur);
            if(s != lastSaved)
            {
                iniSet(SEC_LAST, exe, s.c_str());
                lastSaved = s;
            }
        }
        if(sleepOrStop(250))
            break;
    }
}

static void stopWorker()
{
    g_stop = true;
    if(g_worker.joinable())
        g_worker.join();
    g_stop = false;
}

static void startWorker()
{
    stopWorker();
    std::string exe;
    DWORD pid;
    bool debugging;
    currentTarget(exe, pid, debugging);
    if(g_enabled && debugging && !exe.empty())
        g_worker = std::thread(worker, pid, exe);
}

static void setBypass(bool on)
{
    std::string exe;
    DWORD pid;
    bool debugging;
    currentTarget(exe, pid, debugging);
    if(exe.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(g_stateLock);
        if(on)
            g_bypass.insert(exe);
        else
            g_bypass.erase(exe);
    }
    _plugin_menuentrysetchecked(g_handle, MENU_BYPASS, on);
    g_reapply = true;
    _plugin_logprintf("[" PLUGIN_NAME "] %s: %s\n", fileName(exe).c_str(),
                      on ? "restored to default until x64dbg closes (profiles are kept)" : "profiles/last position active again");
}

// ---------------------------------------------------------------- profile editor dialog

struct EditCtx
{
    std::string origName; // empty for a new profile
    std::string startName;
    Placement p;
    std::vector<Monitor> mons;
    std::string exe;
    DWORD pid;
    bool debugging;
};

static void showPlacement(HWND dlg, const EditCtx* ctx)
{
    int i = pickMonitor(ctx->mons, ctx->p);
    SendDlgItemMessageA(dlg, IDC_MONITOR, CB_SETCURSEL, i, 0);
    RECT r = toScreen(ctx->p, ctx->mons[i]);
    const RECT& work = ctx->mons[i].work;
    SetDlgItemInt(dlg, IDC_X, r.left - work.left, FALSE);
    SetDlgItemInt(dlg, IDC_Y, r.top - work.top, FALSE);
    SetDlgItemInt(dlg, IDC_W, r.right - r.left, FALSE);
    SetDlgItemInt(dlg, IDC_H, r.bottom - r.top, FALSE);
    CheckDlgButton(dlg, IDC_MAX, ctx->p.maximized ? BST_CHECKED : BST_UNCHECKED);
}

static void renameProfile(const std::string& from, const std::string& to)
{
    std::string value = iniGet(SEC_PROFILES, from);
    iniSet(SEC_PROFILES, from, nullptr);
    iniSet(SEC_PROFILES, to, value.c_str());
    for(auto& exe : iniKeys(SEC_ASSIGN))
        if(iniGet(SEC_ASSIGN, exe) == from)
            iniSet(SEC_ASSIGN, exe, to.c_str());
    if(iniGet(SEC_GENERAL, "global") == from)
        iniSet(SEC_GENERAL, "global", to.c_str());
}

static bool saveEdit(HWND dlg, EditCtx* ctx)
{
    char buf[256] = "";
    GetDlgItemTextA(dlg, IDC_NAME, buf, sizeof(buf));
    std::string name = buf;
    if(name.empty() || name.find_first_of(";=[]") != std::string::npos || name.front() == ' ' || name.back() == ' ')
    {
        MessageBoxA(dlg, "The name can't be empty, start or end with a space, or contain ; = [ ]", PLUGIN_NAME, MB_ICONWARNING);
        return false;
    }
    if(name != ctx->origName && !iniGet(SEC_PROFILES, name).empty())
    {
        MessageBoxA(dlg, "A profile with that name already exists.", PLUGIN_NAME, MB_ICONWARNING);
        return false;
    }
    BOOL okX, okY, okW, okH;
    int x = GetDlgItemInt(dlg, IDC_X, &okX, FALSE);
    int y = GetDlgItemInt(dlg, IDC_Y, &okY, FALSE);
    int w = GetDlgItemInt(dlg, IDC_W, &okW, FALSE);
    int h = GetDlgItemInt(dlg, IDC_H, &okH, FALSE);
    if(!okX || !okY || !okW || !okH || w <= 0 || h <= 0)
    {
        MessageBoxA(dlg, "X, Y, width and height must be numbers, and width/height above 0.", PLUGIN_NAME, MB_ICONWARNING);
        return false;
    }
    int mi = (int)SendDlgItemMessageA(dlg, IDC_MONITOR, CB_GETCURSEL, 0, 0);
    if(mi < 0 || mi >= (int)ctx->mons.size())
        mi = 0;
    const Monitor& m = ctx->mons[mi];
    RECT r{m.work.left + x, m.work.top + y, m.work.left + x + w, m.work.top + y + h};
    Placement p = fromScreen(r, m, IsDlgButtonChecked(dlg, IDC_MAX) == BST_CHECKED);

    if(!ctx->origName.empty() && name != ctx->origName)
        renameProfile(ctx->origName, name);
    iniSet(SEC_PROFILES, name, serialize(p).c_str());

    if(IsDlgButtonChecked(dlg, IDC_GLOBAL) == BST_CHECKED)
        iniSet(SEC_GENERAL, "global", name.c_str()); // only one global, this replaces the old one
    else if(iniGet(SEC_GENERAL, "global") == name)
        iniSet(SEC_GENERAL, "global", nullptr);

    if(!ctx->exe.empty())
    {
        if(IsDlgButtonChecked(dlg, IDC_ASSIGN) == BST_CHECKED)
            iniSet(SEC_ASSIGN, ctx->exe, name.c_str());
        else if(iniGet(SEC_ASSIGN, ctx->exe) == name)
            iniSet(SEC_ASSIGN, ctx->exe, nullptr);
    }
    g_reapply = true;
    return true;
}

static INT_PTR CALLBACK editProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    auto ctx = (EditCtx*)GetWindowLongPtrA(dlg, DWLP_USER);
    switch(msg)
    {
    case WM_INITDIALOG:
    {
        ctx = (EditCtx*)lp;
        SetWindowLongPtrA(dlg, DWLP_USER, (LONG_PTR)ctx);
        SetWindowTextA(dlg, ctx->origName.empty() ? "New profile" : ("Profile: " + ctx->origName).c_str());
        SetDlgItemTextA(dlg, IDC_NAME, ctx->startName.c_str());
        for(size_t i = 0; i < ctx->mons.size(); i++)
            SendDlgItemMessageA(dlg, IDC_MONITOR, CB_ADDSTRING, 0, (LPARAM)monitorLabel((int)i, ctx->mons[i]).c_str());
        showPlacement(dlg, ctx);
        bool isGlobal = !ctx->origName.empty() && iniGet(SEC_GENERAL, "global") == ctx->origName;
        CheckDlgButton(dlg, IDC_GLOBAL, isGlobal ? BST_CHECKED : BST_UNCHECKED);
        if(ctx->exe.empty())
        {
            SetDlgItemTextA(dlg, IDC_ASSIGN, "Use for this process (no target loaded yet)");
            EnableWindow(GetDlgItem(dlg, IDC_ASSIGN), FALSE);
        }
        else
        {
            SetDlgItemTextA(dlg, IDC_ASSIGN, ("Use for this process (" + fileName(ctx->exe) + ")").c_str());
            bool assigned = !ctx->origName.empty() && iniGet(SEC_ASSIGN, ctx->exe) == ctx->origName;
            CheckDlgButton(dlg, IDC_ASSIGN, assigned ? BST_CHECKED : BST_UNCHECKED);
        }
        EnableWindow(GetDlgItem(dlg, IDC_CAPTURE), ctx->debugging);
        return TRUE;
    }

    case WM_COMMAND:
        switch(LOWORD(wp))
        {
        case IDC_CAPTURE:
        {
            Placement p;
            if(capture(findWindow(ctx->pid), p))
            {
                ctx->mons = monitors();
                ctx->p = p;
                showPlacement(dlg, ctx);
            }
            else
                MessageBoxA(dlg, "Couldn't find a visible, non-minimized window for the debuggee.", PLUGIN_NAME, MB_ICONWARNING);
            return TRUE;
        }
        case IDOK:
            if(saveEdit(dlg, ctx))
                EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static bool editProfile(HWND parent, const std::string& name)
{
    EditCtx ctx;
    ctx.mons = monitors();
    if(ctx.mons.empty())
        return false;
    currentTarget(ctx.exe, ctx.pid, ctx.debugging);
    ctx.origName = name;

    if(!name.empty())
    {
        ctx.startName = name;
        if(!loadProfile(name, ctx.p))
            ctx.p = fromScreen(ctx.mons[0].work, ctx.mons[0], false);
    }
    else
    {
        // new: start from the debuggee's window, else 60% of the primary monitor, centered
        int n = 1;
        do
            ctx.startName = "Profile " + std::to_string(n++);
        while(!iniGet(SEC_PROFILES, ctx.startName).empty());
        if(!ctx.debugging || !capture(findWindow(ctx.pid), ctx.p))
        {
            const Monitor* primary = &ctx.mons[0];
            for(auto& x : ctx.mons)
                if(x.primary)
                    primary = &x;
            LONG W = primary->work.right - primary->work.left, H = primary->work.bottom - primary->work.top;
            RECT r{primary->work.left + W / 5, primary->work.top + H / 5, primary->work.left + W * 4 / 5, primary->work.top + H * 4 / 5};
            ctx.p = fromScreen(r, *primary, false);
        }
    }
    return DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_EDIT), parent, editProc, (LPARAM)&ctx) == IDOK;
}

// ---------------------------------------------------------------- profiles dialog

static void setCell(HWND list, int row, int col, const std::string& text)
{
    ListView_SetItemText(list, row, col, const_cast<char*>(text.c_str()));
}

static std::string selectedProfile(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_LIST);
    int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if(i < 0)
        return "";
    char buf[256] = "";
    ListView_GetItemText(list, i, 0, buf, sizeof(buf));
    return buf;
}

static void refreshProfiles(HWND dlg)
{
    std::string exe;
    DWORD pid;
    bool debugging;
    currentTarget(exe, pid, debugging);

    std::string target = exe.empty() ? "Current target: none loaded yet" : "Current target: " + exe;
    SetDlgItemTextA(dlg, IDC_TARGET, target.c_str());
    EnableWindow(GetDlgItem(dlg, IDC_BYPASS), !exe.empty());
    SetDlgItemTextA(dlg, IDC_BYPASS, isBypassed(exe) ? "Turn this process's profile back on" : "Restore this process to default (this session)");

    HWND list = GetDlgItem(dlg, IDC_LIST);
    ListView_DeleteAllItems(list);
    auto mons = monitors();
    std::string global = iniGet(SEC_GENERAL, "global");
    std::vector<std::pair<std::string, std::string>> assigns;
    for(auto& key : iniKeys(SEC_ASSIGN))
        assigns.push_back({key, iniGet(SEC_ASSIGN, key)});

    int row = 0;
    for(auto& name : iniKeys(SEC_PROFILES))
    {
        LVITEMA item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<char*>(name.c_str());
        ListView_InsertItem(list, &item);

        Placement p;
        if(loadProfile(name, p) && !mons.empty())
        {
            int mi = pickMonitor(mons, p);
            RECT r = toScreen(p, mons[mi]);
            char buf[128];
            sprintf_s(buf, "Monitor %d", mi + 1);
            setCell(list, row, 1, buf);
            if(p.maximized)
                strcpy_s(buf, "maximized");
            else
                sprintf_s(buf, "%ldx%ld at %ld,%ld", r.right - r.left, r.bottom - r.top, r.left - mons[mi].work.left, r.top - mons[mi].work.top);
            setCell(list, row, 2, buf);
        }
        else
            setCell(list, row, 1, "(invalid)");
        setCell(list, row, 3, name == global ? "yes" : "");
        std::string procs;
        for(auto& a : assigns)
            if(a.second == name)
                procs += (procs.empty() ? "" : ", ") + fileName(a.first);
        setCell(list, row, 4, procs);
        row++;
    }
}

static INT_PTR CALLBACK profilesProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch(msg)
    {
    case WM_INITDIALOG:
    {
        HWND list = GetDlgItem(dlg, IDC_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        const char* titles[] = {"Name", "Monitor", "Position", "Global", "Used by processes"};
        int widths[] = {120, 75, 140, 50, 220};
        for(int i = 0; i < 5; i++)
        {
            LVCOLUMNA col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<char*>(titles[i]);
            col.cx = MulDiv(widths[i], GetDpiForWindow(dlg), 96);
            ListView_InsertColumn(list, i, &col);
        }
        refreshProfiles(dlg);
        return TRUE;
    }

    case WM_NOTIFY:
    {
        auto hdr = (LPNMHDR)lp;
        if(hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK && ((LPNMITEMACTIVATE)lp)->iItem >= 0)
        {
            if(editProfile(dlg, selectedProfile(dlg)))
                refreshProfiles(dlg);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        switch(LOWORD(wp))
        {
        case IDC_NEW:
            if(editProfile(dlg, ""))
                refreshProfiles(dlg);
            return TRUE;

        case IDC_EDITPROF:
        {
            auto name = selectedProfile(dlg);
            if(name.empty())
                MessageBoxA(dlg, "Select a profile first.", PLUGIN_NAME, MB_ICONINFORMATION);
            else if(editProfile(dlg, name))
                refreshProfiles(dlg);
            return TRUE;
        }

        case IDC_DELETE:
        {
            auto name = selectedProfile(dlg);
            if(name.empty())
                return TRUE;
            if(MessageBoxA(dlg, ("Delete profile \"" + name + "\"?").c_str(), PLUGIN_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return TRUE;
            iniSet(SEC_PROFILES, name, nullptr);
            for(auto& exe : iniKeys(SEC_ASSIGN))
                if(iniGet(SEC_ASSIGN, exe) == name)
                    iniSet(SEC_ASSIGN, exe, nullptr);
            if(iniGet(SEC_GENERAL, "global") == name)
                iniSet(SEC_GENERAL, "global", nullptr);
            g_reapply = true;
            refreshProfiles(dlg);
            return TRUE;
        }

        case IDC_BYPASS:
        {
            std::string exe;
            DWORD pid;
            bool debugging;
            currentTarget(exe, pid, debugging);
            setBypass(!isBypassed(exe));
            refreshProfiles(dlg);
            return TRUE;
        }

        case IDOK:
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- x64dbg callbacks

static void cbCreateProcess(CBTYPE, void* info)
{
    auto cp = (PLUG_CB_CREATEPROCESS*)info;
    if(!cp->fdProcessInfo || !cp->DebugFileName)
        return;
    {
        std::lock_guard<std::mutex> lock(g_stateLock);
        g_currentExe = cp->DebugFileName;
        g_currentPid = cp->fdProcessInfo->dwProcessId;
        g_debugging = true;
    }
    _plugin_menuentrysetchecked(g_handle, MENU_BYPASS, isBypassed(cp->DebugFileName));
    startWorker();
}

static void cbStopDebug(CBTYPE, void*)
{
    stopWorker();
    std::lock_guard<std::mutex> lock(g_stateLock);
    g_debugging = false;
}

static void cbMenuEntry(CBTYPE, void* info)
{
    switch(((PLUG_CB_MENUENTRY*)info)->hEntry)
    {
    case MENU_ENABLED:
        g_enabled = !g_enabled;
        _plugin_menuentrysetchecked(g_handle, MENU_ENABLED, g_enabled);
        iniSet(SEC_GENERAL, "enabled", g_enabled ? "1" : "0");
        if(g_enabled)
            startWorker();
        else
            stopWorker();
        _plugin_logprintf("[" PLUGIN_NAME "] %s\n", g_enabled ? "enabled" : "disabled");
        break;

    case MENU_PROFILES:
    {
        DpiScope dpi;
        DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_PROFILES), GuiGetWindowHandle(), profilesProc, 0);
        break;
    }

    case MENU_BYPASS:
    {
        std::string exe;
        DWORD pid;
        bool debugging;
        currentTarget(exe, pid, debugging);
        if(exe.empty())
        {
            _plugin_menuentrysetchecked(g_handle, MENU_BYPASS, false);
            _plugin_logputs("[" PLUGIN_NAME "] no target loaded yet");
            break;
        }
        setBypass(!isBypassed(exe));
        break;
    }

    case MENU_FORGET_CURRENT:
    {
        std::string exe;
        DWORD pid;
        bool debugging;
        currentTarget(exe, pid, debugging);
        if(exe.empty())
        {
            _plugin_logputs("[" PLUGIN_NAME "] no target loaded yet, nothing to forget");
            break;
        }
        iniSet(SEC_LAST, exe, nullptr);
        _plugin_logprintf("[" PLUGIN_NAME "] forgot last position for %s\n", exe.c_str());
        break;
    }

    case MENU_FORGET_ALL:
        if(MessageBoxA(GuiGetWindowHandle(), "Forget the last position of every target?\n(Profiles are kept.)", PLUGIN_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES)
            break;
        iniClearSection(SEC_LAST);
        _plugin_logputs("[" PLUGIN_NAME "] forgot all last positions");
        break;

    case MENU_ABOUT:
        MessageBoxA(GuiGetWindowHandle(),
                    "Puts the debuggee's window (console or GUI) back where you want it when you restart debugging.\n\n"
                    "Which position wins:\n"
                    "1. Restored to default for this session: the plugin leaves the window alone.\n"
                    "2. The process's own profile: always opens there, moves are not saved.\n"
                    "3. The global profile: every process without its own profile opens there.\n"
                    "4. Otherwise: opens where it was last.\n\n"
                    "Settings are in winrestore.ini next to the plugin.",
                    PLUGIN_NAME, MB_OK | MB_ICONINFORMATION);
        break;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if(reason == DLL_PROCESS_ATTACH)
        g_inst = inst;
    return TRUE;
}

extern "C" __declspec(dllexport) bool pluginit(PLUG_INITSTRUCT* init)
{
    init->pluginVersion = 2;
    init->sdkVersion = PLUG_SDKVERSION;
    strcpy_s(init->pluginName, PLUGIN_NAME);
    g_handle = init->pluginHandle;

    char path[MAX_PATH] = "";
    GetModuleFileNameA(g_inst, path, MAX_PATH);
    g_ini = path;
    g_ini = g_ini.substr(0, g_ini.find_last_of('.')) + ".ini";
    g_enabled = iniGet(SEC_GENERAL, "enabled") != "0";

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    BridgeSettingSet("WinRestore", nullptr, nullptr); // leftovers from version 1, which used x64dbg.ini

    _plugin_registercallback(g_handle, CB_CREATEPROCESS, cbCreateProcess);
    _plugin_registercallback(g_handle, CB_STOPDEBUG, cbStopDebug);
    _plugin_registercallback(g_handle, CB_MENUENTRY, cbMenuEntry);
    return true;
}

extern "C" __declspec(dllexport) bool plugstop()
{
    stopWorker();
    return true;
}

extern "C" __declspec(dllexport) void plugsetup(PLUG_SETUPSTRUCT* setup)
{
    int menu = setup->hMenu;
    _plugin_menuaddentry(menu, MENU_ENABLED, "Enabled");
    _plugin_menuentrysetchecked(g_handle, MENU_ENABLED, g_enabled);
    _plugin_menuaddseparator(menu);
    _plugin_menuaddentry(menu, MENU_PROFILES, "Profiles...");
    _plugin_menuaddentry(menu, MENU_BYPASS, "Restore this process to default (this session)");
    _plugin_menuaddseparator(menu);
    _plugin_menuaddentry(menu, MENU_FORGET_CURRENT, "Forget last position for current target");
    _plugin_menuaddentry(menu, MENU_FORGET_ALL, "Forget all last positions");
    _plugin_menuaddseparator(menu);
    _plugin_menuaddentry(menu, MENU_ABOUT, "About");
}
