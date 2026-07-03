// ============================================================================
//  SumatraLister.wlx / SumatraLister.wlx64
//
//  Total Commander "Lister" (WLX) plugin that displays PDF, CHM, DjVu, EPUB,
//  FB2, MOBI, PRC, XPS/OXPS and comic archive (CB7/CBR/CBT/CBZ) files by
//  embedding SumatraPDF.exe directly into the Lister window.
//
//  How it works
//  ------------
//  SumatraPDF has a built-in "plugin" mode (the same mechanism it uses for
//  the Firefox/Chrome browser plugin and the IE ActiveX control):
//
//      SumatraPDF.exe -plugin <decimal HWND> "<path to file>"
//
//  When started this way, Sumatra creates its main window as a CHILD of the
//  HWND you pass in, hides its toolbar/menu, and tracks the size of that
//  parent window. We create a small "host" child window inside the HWND
//  that Total Commander gives us in ListLoad(), and pass *that* window's
//  handle to SumatraPDF. We then forward WM_SIZE so Sumatra's view always
//  fills the Lister pane, and we terminate the Sumatra process when the
//  Lister window is closed.
//
//  Features
//  --------
//   - Auto-detects SumatraPDF (registry / common paths / PATH), with an
//     optional manual override via SumatraLister.ini.
//   - INI-configurable launch options: default zoom, default view mode,
//     night/inverted-colour mode, restricted (sandboxed) mode, embed timeout,
//     and free-form extra command-line arguments.
//   - Deep-link page jumping: append "#page=N" to the path passed to
//     ListLoad/ListLoadNext (e.g. by another plugin or a custom TC command)
//     to open directly at page N. A real file that happens to be named that
//     way on disk always takes precedence over the heuristic.
//   - Implements the real ListSendCommand set (LC_COPY, LC_NEWPARAMS,
//     LC_SELECTALL, LC_SETPERCENT), ListSearchText[W] with its real
//     LCS_FINDFIRST/LCS_MATCHCASE/LCS_WHOLEWORDS/LCS_BACKWARDS flags, and
//     ListSearchDialog -- forwarding find/copy/select-all/scroll into
//     Sumatra's own UI. Page navigation, zoom, and general keyboard input
//     reach Sumatra directly once its window has focus and need no explicit
//     forwarding.
//   - File-list thumbnails (ListGetPreviewBitmap[W]) via the Windows Shell
//     thumbnail pipeline -- no extra Sumatra process spawned per thumbnail.
//   - Printing (ListPrint[W]): shows Sumatra's own native print dialog when
//     reusing a live embedded pane for the file being printed, falling back
//     to non-interactive CLI printing (-print-to/-print-to-default)
//     otherwise.
//   - Auto night mode: with AutoNightMode=1, follows Total Commander's own
//     Lister dark-mode state (via the real LC_NEWPARAMS/LCP_DARKMODE
//     mechanism) instead of a fixed setting.
//   - Quick View Panel (Ctrl+Q) focus notification (ITM_FOCUS), so its
//     header highlights correctly when the embedded pane gains focus.
//   - Crash detection: if an embedded Sumatra process exits unexpectedly
//     (crash, killed externally) rather than via this plugin's own
//     intentional close, the pane switches to an accurate message instead
//     of staying a dead, unresponsive blank window. Uses a thread-pool wait
//     on the process handle (RegisterWaitForSingleObject), not polling.
//   - Optional pop-out hotkey (EnablePopOut=1, off by default): Ctrl+Alt+O
//     reopens the current file in a normal, full SumatraPDF window with its
//     menu/toolbar, for actions the embedded view doesn't expose (Save As,
//     annotate, rotate...).
//   - Optional fallback (FallbackToShellOpen=1): if Sumatra can't be found
//     or embedded, opens the file with its default Windows handler instead
//     of just showing an error message.
//   - Reuses one embedded Sumatra process across ListLoadNext (Lister's
//     "next file" navigation) instead of recreating the host window.
//   - Optional debug log (SumatraLister.log next to the DLL, auto-rotated
//     past ~2MB) for troubleshooting detection/launch problems.
//
//  API correctness
//  ----------------
//  The WLX constants and struct layouts used in this file (LC_*, LCP_*,
//  LCS_*, ITM_FOCUS, ListDefaultParamStruct, and every exported function's
//  exact signature including ListPrint's RECT* Margins) have been verified
//  against the official Total Commander Lister Plugin SDK
//  (https://github.com/ghisler/WLX-SDK). See README.md's "API correctness"
//  section for the full account of what an earlier, unverified version of
//  this file got wrong and what changed as a result.
//
//  Thread safety
//  -------------
//  Total Commander can call into a Lister plugin from more than one thread
//  at once -- most notably, ListGetPreviewBitmap for background thumbnail
//  generation runs concurrently with the UI thread's ListLoad/ListSendCommand
//  calls for an open Lister window. Instances are held as shared_ptr (so an
//  object always outlives every thread still using it, even if
//  ListCloseWindow removes it from the map mid-operation on another thread)
//  and each instance has its own opLock, separate from the map's g_mutex, so
//  a slow relaunch on one Lister window never blocks operations on another.
//
//  Build: see README.md in this package for MSVC / MinGW instructions.
//  Target: Win32 (SumatraLister.wlx) and x64 (SumatraLister.wlx64).
// ============================================================================

// UNICODE/_UNICODE: this plugin is Unicode-first (all Win32 calls use the
// explicit *W form already, e.g. LoadCursorW), but generic SDK macros like
// IDC_ARROW resolve through MAKEINTRESOURCE, which itself picks its ANSI or
// wide form based on whether UNICODE is defined -- without this, IDC_ARROW
// silently becomes an LPSTR and fails to bind to the *W functions we call it
// with. NOMINMAX avoids windef.h's min/max macros shadowing std::min/std::max.
#define UNICODE
#define _UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>      // SHCreateItemFromParsingName / IShellItemImageFactory (thumbnails)
#include <shellapi.h>    // ShellExecuteW (FallbackToShellOpen)
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <algorithm>     // std::min / std::max
#include <cstdint>       // std::uint8_t
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

// ---------------------------------------------------------------------------
//  WLX API constants -- verified against the official Total Commander
//  Lister Plugin SDK (https://github.com/ghisler/WLX-SDK,
//  https://ghisler.github.io/WLX-SDK/listplug.h.htm), fetched and
//  cross-checked against an independent mirror during development. These
//  replace an earlier, unverified set of constants that did not match the
//  real API; see README.md's "API correctness" section for what changed
//  and why.
// ---------------------------------------------------------------------------

#define LISTPLUGIN_VERSION 0x1A0

// ListSendCommand commands (TC calls this "when the user changes some
// options in Lister's menu" -- these four are the complete, real set).
#define LC_COPY       1  // copy current selection to the clipboard
#define LC_NEWPARAMS  2  // display options changed; Parameter is a combination of LCP_* flags below
#define LC_SELECTALL  3  // select the whole contents
#define LC_SETPERCENT 4  // scroll to a new position in the document, as a percent (Parameter = 0-100)

// LC_NEWPARAMS flags (the Parameter passed alongside LC_NEWPARAMS).
// This plugin only acts on LCP_DARKMODE/LCP_DARKMODENATIVE (for
// AutoNightMode); the others describe TC's own built-in text/hex/image
// viewer options, which don't apply to an embedded SumatraPDF view, and
// per the SDK docs "you may ignore these parameters if they don't apply
// to your document type."
#define LCP_WRAPTEXT       1
#define LCP_FITTOWINDOW    2
#define LCP_ANSI           4
#define LCP_ASCII          8
#define LCP_VARIABLE       12
#define LCP_FORCESHOW      16
#define LCP_FITLARGERONLY  32
#define LCP_CENTER         64
#define LCP_DARKMODE       128  // added in WLX SDK 2.12 / TC 10.0 (2022)
#define LCP_DARKMODENATIVE 256  // added in WLX SDK 2.12 / TC 10.0 (2022)

// ListSearchText / ListSearchTextW SearchParameter flags.
#define LCS_FINDFIRST  1  // fresh search from the top; unset means "find next" from current position
#define LCS_MATCHCASE  2  // case-sensitive search (not currently translated -- see README)
#define LCS_WHOLEWORDS 4  // whole-words-only search (not currently translated -- see README)
#define LCS_BACKWARDS  8  // search backwards (upward) through the document

// WM_COMMAND values the PLUGIN sends TO Total Commander's Lister/Quick-View
// parent window (not the reverse). Confirmed via the WLX SDK changelog and
// Total Commander's own "List of changes": sent as
// WM_COMMAND(MAKEWPARAM(0, ITM_FOCUS), (LPARAM)pluginWindow) to notify the
// Quick View Panel (Ctrl+Q) that this pane gained focus, so its header
// highlights correctly.
#define ITM_FOCUS 0xFFF8

// Custom message (private to this plugin, not part of the WLX API): posted
// to a host window by ProcessExitCallback when its embedded Sumatra process
// exits. See RegisterCrashDetection.
#define WM_SUMATRA_PROCESS_EXITED (WM_APP + 1)

// GWLP_USERDATA values for ErrorWndProc, set alongside the WNDPROC swap at
// each call site that switches a pane into its error/fallback display.
#define ERRSTATE_NOT_FOUND        0  // Sumatra couldn't be found/started at all
#define ERRSTATE_OPENED_EXTERNAL  1  // FallbackToShellOpen kicked in instead
#define ERRSTATE_CRASHED          2  // was embedded and working, then the process exited unexpectedly

// return codes
#define LISTPLUGIN_OK       0
#define LISTPLUGIN_ERROR    1

// Struct TC passes to ListSetDefaultParams, verified against the official
// SDK (ghisler.github.io/WLX-SDK/listplug.h.htm). DefaultIniName is always
// narrow (char[]), even in the Unicode/W-suffixed build.
struct ListDefaultParamStruct {
    int   size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char  DefaultIniName[MAX_PATH];
};

// ---------------------------------------------------------------------------
//  Config (SumatraLister.ini, same folder as the DLL)
// ---------------------------------------------------------------------------
//
//  [Settings]
//  SumatraPath=C:\Tools\SumatraPDF\SumatraPDF.exe   ; manual override, optional
//  ExtraArgs=-esc-to-exit                            ; appended verbatim
//  RestrictMode=0                                    ; 1 = launch with -restrict
//  NightMode=0                                       ; 1 = launch with -invertcolors
//  AutoNightMode=0                                   ; 1 = follow TC's Lister light/dark theme instead
//  DefaultZoom=                                      ; e.g. "fit page", "100", "fit width"
//  DefaultView=                                      ; e.g. "continuous", "facing", "book view"
//  PrintSettings=                                    ; forwarded to -print-settings for ListPrint
//  ThumbnailsEnabled=1                               ; 1 = supply file-list thumbnails via Shell API
//  EmbedTimeoutMs=4000                               ; how long to wait for Sumatra to embed before giving up
//  FallbackToShellOpen=0                             ; 1 = open the file with its default Windows app if
//                                                     ;     Sumatra can't be found/embedded, instead of just
//                                                     ;     showing an error message in the Lister pane
//  EnablePopOut=0                                     ; 1 = register Ctrl+Alt+O as a system-wide hotkey,
//                                                     ;     for as long as a Lister pane using this plugin
//                                                     ;     is open, to reopen the current file in a normal,
//                                                     ;     full SumatraPDF window (menu/toolbar included,
//                                                     ;     for printing/annotating/saving). OFF by default
//                                                     ;     since the hotkey isn't scoped to just this pane
//                                                     ;     having focus -- see README for details.
//  DebugLog=0                                        ; 1 = write SumatraLister.log next to DLL
//
struct PluginConfig {
    std::wstring sumatraPathOverride;
    std::wstring extraArgs;
    bool         restrictMode        = false;
    bool         nightMode           = false;
    bool         autoNightMode       = false;
    std::wstring defaultZoom;
    std::wstring defaultView;
    std::wstring printSettings;
    bool         thumbnailsEnabled   = true;
    DWORD        embedTimeoutMs      = 4000;
    bool         fallbackShellOpen   = false;
    bool         enablePopOut        = false;
    bool         debugLog            = false;
};

static PluginConfig   g_config;
static std::once_flag g_configOnceFlag; // guards lazy config load against concurrent threads
                                          // (Total Commander can call into this plugin from a
                                          // background thumbnail-generation thread at the same
                                          // time the UI thread is opening a Lister window)
static HINSTANCE      g_hInst = nullptr;

static std::wstring GetModuleDir()
{
    wchar_t modulePath[MAX_PATH] = {0};
    GetModuleFileNameW(g_hInst, modulePath, MAX_PATH);
    PathRemoveFileSpecW(modulePath);
    return modulePath;
}

static std::wstring GetIniPath()
{
    return GetModuleDir() + L"\\SumatraLister.ini";
}

static std::wstring GetLogPath()
{
    // Computed once (thread-safe static local init) rather than re-deriving
    // the module's own path via GetModuleFileNameW on every LogF call --
    // the DLL's location can't change during its own lifetime, so repeating
    // that lookup per log line was pure avoidable overhead on a path that
    // can be called many times during an active troubleshooting session.
    static const std::wstring path = GetModuleDir() + L"\\SumatraLister.log";
    return path;
}

// Lightweight debug logger; no-op unless DebugLog=1 in the INI. Guarded by
// its own mutex (separate from g_mutex/opLock) since log calls can now come
// from a background thumbnail thread interleaved with the UI thread; without
// this, concurrent appends could interleave mid-line in the file.
static std::mutex g_logMutex;

static void LogF(const wchar_t* fmt, ...)
{
    if (!g_config.debugLog) return;

    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_logMutex);

    const std::wstring& path = GetLogPath();

    // Simple rotation: once the log passes ~2MB, start fresh rather than
    // growing forever across a long-running Total Commander session.
    //
    // Size is tracked in-memory after an initial seed rather than re-stat'ing
    // the file via GetFileAttributesExW on every call: every byte ever
    // written to this log goes through this same function under this same
    // mutex, so once we know the starting size (from one real stat, lazily
    // done on the first log call of this DLL's lifetime), we can just keep a
    // running total ourselves instead of asking the filesystem again each time.
    static bool      sizeSeeded    = false;
    static ULONGLONG approxSizeBytes = 0;

    if (!sizeSeeded) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            approxSizeBytes = (((ULONGLONG)fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        sizeSeeded = true;
    }

    if (approxSizeBytes > 2ull * 1024 * 1024) {
        DeleteFileW(path.c_str());
        approxSizeBytes = 0;
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        int written = fwprintf(f, L"[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
        if (written > 0)
            approxSizeBytes += (ULONGLONG)written * sizeof(wchar_t); // approximate; exact byte count isn't worth another stat
        (void)fclose(f); // best-effort log write; nothing actionable to do if the close itself fails here
    }
}

static void LoadConfigImpl()
{
    std::wstring ini = GetIniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // no INI present -> defaults apply, that's fine

    wchar_t buf[1024];

    GetPrivateProfileStringW(L"Settings", L"SumatraPath", L"", buf, _countof(buf), ini.c_str());
    g_config.sumatraPathOverride = buf;

    GetPrivateProfileStringW(L"Settings", L"ExtraArgs", L"", buf, _countof(buf), ini.c_str());
    g_config.extraArgs = buf;

    g_config.restrictMode      = GetPrivateProfileIntW(L"Settings", L"RestrictMode", 0, ini.c_str()) != 0;
    g_config.nightMode         = GetPrivateProfileIntW(L"Settings", L"NightMode", 0, ini.c_str()) != 0;
    g_config.autoNightMode     = GetPrivateProfileIntW(L"Settings", L"AutoNightMode", 0, ini.c_str()) != 0;
    g_config.thumbnailsEnabled = GetPrivateProfileIntW(L"Settings", L"ThumbnailsEnabled", 1, ini.c_str()) != 0;
    g_config.fallbackShellOpen = GetPrivateProfileIntW(L"Settings", L"FallbackToShellOpen", 0, ini.c_str()) != 0;
    g_config.enablePopOut      = GetPrivateProfileIntW(L"Settings", L"EnablePopOut", 0, ini.c_str()) != 0;
    g_config.debugLog          = GetPrivateProfileIntW(L"Settings", L"DebugLog", 0, ini.c_str()) != 0;

    UINT timeoutRaw = GetPrivateProfileIntW(L"Settings", L"EmbedTimeoutMs", 4000, ini.c_str());
    // Clamped entirely in unsigned space (matching GetPrivateProfileIntW's
    // actual UINT return type) rather than round-tripping through a signed
    // int first: a malformed/negative INI value can make GetPrivateProfileInt
    // return a huge UINT (e.g. "-1" -> UINT_MAX), and converting that to int
    // before clamping would rely on implementation-defined signed/unsigned
    // conversion behavior instead of just clamping the unsigned value directly.
    g_config.embedTimeoutMs = (timeoutRaw < 500) ? 500 : (timeoutRaw > 30000 ? 30000 : timeoutRaw);

    GetPrivateProfileStringW(L"Settings", L"DefaultZoom", L"", buf, _countof(buf), ini.c_str());
    g_config.defaultZoom = buf;

    GetPrivateProfileStringW(L"Settings", L"DefaultView", L"", buf, _countof(buf), ini.c_str());
    g_config.defaultView = buf;

    GetPrivateProfileStringW(L"Settings", L"PrintSettings", L"", buf, _countof(buf), ini.c_str());
    g_config.printSettings = buf;

    LogF(L"Config loaded: override='%s' extra='%s' restrict=%d night=%d auto-night=%d "
         L"zoom='%s' view='%s' timeout=%lu fallback=%d",
         g_config.sumatraPathOverride.c_str(), g_config.extraArgs.c_str(),
         g_config.restrictMode, g_config.nightMode, g_config.autoNightMode,
         g_config.defaultZoom.c_str(), g_config.defaultView.c_str(),
         g_config.embedTimeoutMs, g_config.fallbackShellOpen);
}

// Thread-safe lazy config load: exactly one caller (whichever thread gets
// here first -- UI thread on first ListLoad, or a thumbnail worker thread)
// actually runs LoadConfigImpl(); everyone else blocks briefly until it's
// done, then proceeds with a fully-populated g_config.
static void LoadConfig()
{
    std::call_once(g_configOnceFlag, LoadConfigImpl);
}

// ---------------------------------------------------------------------------
//  Internal state: one record per open Lister instance (per host window)
// ---------------------------------------------------------------------------

struct ListerInstance {
    HWND               hostWnd     = nullptr;  // child window we create inside TC's ParentWin
    HWND               parentWin   = nullptr;  // window handle TC gave us
    HWND               sumatraWnd  = nullptr;  // top-level Sumatra window (child of hostWnd)
    PROCESS_INFORMATION pi         = {};
    std::wstring       filePath;
    bool               currentInvert = false;  // effective -invertcolors state of the running process
    bool               closed        = false;  // true once TeardownInstance has run; guards reuse-after-close
    int                hotkeyId      = 0;       // RegisterHotKey id for pop-out, 0 = not registered
    HANDLE             processWaitHandle = nullptr; // RegisterWaitForSingleObject handle; see ProcessExitCallback
    std::mutex         opLock;                 // serializes launch/relaunch/close for *this* instance,
                                                // held independently of g_mutex (which only protects the
                                                // map itself) so a slow relaunch on one Lister window never
                                                // blocks operations on other windows or thumbnail requests
};

// Instances are kept alive via shared_ptr, not raw pointer. TC can call into
// this plugin from more than one thread (the documented case is background
// thumbnail generation for ListGetPreviewBitmap, but ListSendCommand /
// ListCloseWindow could in principle race a relaunch too) -- with a raw
// pointer, a thread that looked up `inst` from the map could end up calling
// inst->opLock.lock() on memory that ListCloseWindow already deleted on
// another thread. Handing out shared_ptr copies under g_mutex means the
// object stays alive for as long as *any* thread is still using it, even
// after ListCloseWindow erases the map entry; opLock below still serializes
// the actual teardown vs. in-flight operations so nothing operates on a
// half-closed instance.
using ListerInstancePtr = std::shared_ptr<ListerInstance>;

static std::mutex                            g_mutex;
static std::map<HWND, ListerInstancePtr>     g_instances; // keyed by hostWnd (== "ListWin" we return)
static const wchar_t* HOST_CLASS_NAME        = L"SumatraListerHostWnd";

// Thread-safe lookup helper: copies the shared_ptr out under g_mutex and
// returns immediately, so callers never hold g_mutex while doing slow work
// (launching/closing a process, sleeping between synthesized keystrokes).
static ListerInstancePtr FindInstance(HWND listWin)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(listWin);
    return it != g_instances.end() ? it->second : nullptr;
}

// ---------------------------------------------------------------------------
//  ANSI/Unicode interop for the legacy ANSI-suffixed entry points
// ---------------------------------------------------------------------------

// Converts an ANSI (CP_ACP) C-string to UTF-16. Used by the ANSI-suffixed
// entry points (ListLoad, ListLoadNext, ListSearchText, ListPrint) that
// pre-Unicode builds of Total Commander call instead of the *W versions.
// Safe against null/empty input AND conversion failure: MultiByteToWideChar
// returns 0 on a malformed byte sequence for the given code page, and
// naively constructing a std::wstring from a then-possibly-null buffer
// pointer would be undefined behavior rather than just an empty result.
static std::wstring AnsiToWide(const char* s)
{
    if (!s || !*s)
        return std::wstring();

    int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (len <= 0)
        return std::wstring(); // malformed for this code page; fail safe rather than crash

    std::vector<wchar_t> buf(static_cast<size_t>(len));
    MultiByteToWideChar(CP_ACP, 0, s, -1, buf.data(), len);
    return std::wstring(buf.data());
}

// ---------------------------------------------------------------------------
//  Locate SumatraPDF.exe on this machine
// ---------------------------------------------------------------------------

static bool FileExistsW(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ReadRegString(HKEY root, const std::wstring& subKey, const std::wstring& valueName,
                           std::wstring& out)
{
    // Only two views are ever meaningfully distinct for a given build: the
    // default (0, native to this process) and whichever WOW64 flag isn't a
    // no-op for that bitness. A 64-bit process's default view already IS
    // the 64-bit view (KEY_WOW64_64KEY would just repeat it); a 32-bit
    // process is already WOW64-redirected to the 32-bit view by default
    // (KEY_WOW64_32KEY would just repeat it). Trying all three unconditionally
    // meant one RegOpenKeyExW call per subkey was always a wasted duplicate.
#ifdef _WIN64
    const REGSAM views[] = { 0, KEY_WOW64_32KEY };
#else
    const REGSAM views[] = { 0, KEY_WOW64_64KEY };
#endif
    for (REGSAM extra : views) {
        HKEY hKey;
        if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ | extra, &hKey) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH * 2] = {0};
            DWORD   cb   = sizeof(buf);
            DWORD   type = 0;
            LONG    rc   = RegQueryValueExW(hKey, valueName.empty() ? nullptr : valueName.c_str(),
                                             nullptr, &type, (LPBYTE)buf, &cb);
            RegCloseKey(hKey);
            if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                wchar_t expanded[MAX_PATH * 2] = {0};
                if (type == REG_EXPAND_SZ) {
                    ExpandEnvironmentStringsW(buf, expanded, MAX_PATH * 2);
                    out = expanded;
                } else {
                    out = buf;
                }
                if (!out.empty())
                    return true;
            }
        }
    }
    return false;
}

static std::wstring StripQuotesAndArgs(std::wstring s)
{
    if (!s.empty() && s.front() == L'"') {
        size_t end = s.find(L'"', 1);
        if (end != std::wstring::npos)
            return s.substr(1, end - 1);
    }
    return s;
}

static std::wstring FindSumatraPDF()
{
    // 0) Manual override from SumatraLister.ini wins over everything.
    if (!g_config.sumatraPathOverride.empty()) {
        if (FileExistsW(g_config.sumatraPathOverride)) {
            LogF(L"Using SumatraPath override: %s", g_config.sumatraPathOverride.c_str());
            return g_config.sumatraPathOverride;
        }
        LogF(L"SumatraPath override set but file not found: %s", g_config.sumatraPathOverride.c_str());
    }

    std::wstring path;

    // 1) App Paths (set by the Sumatra installer for both per-user / per-machine installs)
    if (ReadRegString(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\SumatraPDF.exe",
                       L"", path)) {
        path = StripQuotesAndArgs(path);
        if (FileExistsW(path))
            return path;
    }

    if (ReadRegString(HKEY_LOCAL_MACHINE,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\SumatraPDF.exe",
                       L"", path)) {
        path = StripQuotesAndArgs(path);
        if (FileExistsW(path))
            return path;
    }

    // 2) Sumatra's own uninstall / install-dir registry keys
    const wchar_t* uninstallKeys[] = {
        L"Software\\SumatraPDF",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SumatraPDF",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{0A5EBB13-4D6F-4674-A8C6-13E1107E0BE2}_is1", // legacy installer GUID
    };
    for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        for (auto key : uninstallKeys) {
            if (ReadRegString(root, key, L"InstallLocation", path)) {
                std::wstring exe = path;
                if (!exe.empty() && exe.back() != L'\\') exe += L'\\';
                exe += L"SumatraPDF.exe";
                if (FileExistsW(exe)) return exe;
            }
            if (ReadRegString(root, key, L"DisplayIcon", path)) {
                path = StripQuotesAndArgs(path);
                if (FileExistsW(path)) return path;
            }
        }
    }

    // 3) Common install locations (covers portable-installed-to-Program-Files setups too)
    wchar_t programFiles[MAX_PATH]    = {0};
    wchar_t programFilesX86[MAX_PATH] = {0};
    wchar_t localAppData[MAX_PATH]    = {0};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES,    nullptr, 0, programFiles);
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, programFilesX86);
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA,    nullptr, 0, localAppData);

    std::vector<std::wstring> candidates = {
        std::wstring(programFiles)    + L"\\SumatraPDF\\SumatraPDF.exe",
        std::wstring(programFilesX86) + L"\\SumatraPDF\\SumatraPDF.exe",
        std::wstring(localAppData)    + L"\\SumatraPDF\\SumatraPDF.exe",
        std::wstring(localAppData)    + L"\\Programs\\SumatraPDF\\SumatraPDF.exe",
    };
    for (auto& c : candidates)
        if (!c.empty() && FileExistsW(c))
            return c;

    // 4) Same directory as this plugin DLL (user may have dropped a portable copy there)
    {
        std::wstring local = GetModuleDir() + L"\\SumatraPDF.exe";
        if (FileExistsW(local)) return local;
        local = GetModuleDir() + L"\\SumatraPDF\\SumatraPDF.exe";
        if (FileExistsW(local)) return local;
    }

    // 5) Finally, fall back to whatever is on PATH
    wchar_t resolved[MAX_PATH] = {0};
    if (SearchPathW(nullptr, L"SumatraPDF.exe", nullptr, MAX_PATH, resolved, nullptr) > 0)
        return resolved;

    return L""; // not found
}

static std::wstring   g_sumatraPath;
static std::once_flag g_sumatraPathOnceFlag; // see g_configOnceFlag: same concurrent-caller concern

static void FindSumatraPDFImpl()
{
    g_sumatraPath = FindSumatraPDF();
    LogF(L"Resolved SumatraPDF.exe: %s", g_sumatraPath.empty() ? L"<not found>" : g_sumatraPath.c_str());
}

static const std::wstring& GetSumatraPath()
{
    std::call_once(g_sumatraPathOnceFlag, FindSumatraPDFImpl);
    return g_sumatraPath;
}

// ---------------------------------------------------------------------------
//  Deep-link page suffix: "C:\book.pdf#page=42" -> path="C:\book.pdf", page=42
// ---------------------------------------------------------------------------

static int ExtractPageSuffix(std::wstring& path)
{
    // If a literal file already exists at this exact path -- including any
    // "#page=N"-looking tail -- trust the filesystem over the heuristic.
    // This is what keeps a real file legitimately named e.g.
    // "Report#page=2.pdf" from being silently misread as a page-42 deep
    // link and opened under the wrong (non-existent) trimmed path.
    if (FileExistsW(path))
        return -1;

    const std::wstring marker = L"#page=";
    size_t pos = path.rfind(marker);
    if (pos == std::wstring::npos)
        return -1;

    std::wstring numPart = path.substr(pos + marker.size());
    if (numPart.empty() || numPart.find_first_not_of(L"0123456789") != std::wstring::npos)
        return -1; // not a clean number, leave the path alone

    int page = _wtoi(numPart.c_str());
    if (page <= 0)
        return -1;

    path.resize(pos); // truncate to the prefix before "#page=N"; equivalent to path.substr(0,pos) without the temporary
    return page;
}

// ---------------------------------------------------------------------------
//  Host child window: a plain window that lives inside TC's ParentWin and
//  hosts Sumatra's reparented window. Resizing it resizes Sumatra to match.
// ---------------------------------------------------------------------------

// Forward declarations: defined later in the file, called here from
// WM_HOTKEY and WM_SUMATRA_PROCESS_EXITED respectively.
static void LaunchSumatraStandalone(const ListerInstance* inst);
static bool TryFallbackShellOpen(const std::wstring& filePath);
static LRESULT CALLBACK ErrorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void UnregisterCrashDetection(ListerInstance* inst); // defined just above LaunchSumatraEmbedded

static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_SIZE: {
            ListerInstancePtr inst = FindInstance(hwnd);
            if (inst && inst->sumatraWnd && IsWindow(inst->sumatraWnd)) {
                RECT rc; GetClientRect(hwnd, &rc);
                MoveWindow(inst->sumatraWnd, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
            }
            return 0;
        }
        case WM_SETFOCUS: {
            ListerInstancePtr inst = FindInstance(hwnd);
            if (inst) {
                if (inst->sumatraWnd && IsWindow(inst->sumatraWnd))
                    SetFocus(inst->sumatraWnd);
                // Notify TC's Quick View Panel (Ctrl+Q) that this pane
                // gained focus, so its header highlights correctly -- see
                // ITM_FOCUS's definition near the top of this file for the
                // verified source of this mechanism. Harmless no-op when
                // this Lister window isn't inside a Quick View Panel.
                if (inst->parentWin && IsWindow(inst->parentWin))
                    PostMessageW(inst->parentWin, WM_COMMAND, MAKEWPARAM(0, ITM_FOCUS), (LPARAM)hwnd);
            }
            return 0;
        }
        case WM_HOTKEY: {
            // Fired by RegisterPopOutHotkey's Ctrl+Alt+O registration.
            // opLock here matches every other reader of filePath/currentInvert
            // (ListLoadNextW, ListSendCommand) so a hotkey press can't read
            // a half-updated file path mid-relaunch; `closed` skips a stale
            // instance that's mid-teardown.
            ListerInstancePtr inst = FindInstance(hwnd);
            if (inst) {
                std::lock_guard<std::mutex> opLock(inst->opLock);
                if (!inst->closed)
                    LaunchSumatraStandalone(inst.get());
            }
            return 0;
        }
        case WM_SUMATRA_PROCESS_EXITED: {
            // Posted by ProcessExitCallback when an embedded Sumatra
            // process exits on its own. processWaitHandle being still set
            // is the staleness guard described at RegisterCrashDetection/
            // UnregisterCrashDetection above -- if it's already null, our
            // own intentional close got there first and this notification
            // is obsolete.
            ListerInstancePtr inst = FindInstance(hwnd);
            if (inst) {
                std::lock_guard<std::mutex> opLock(inst->opLock);
                if (!inst->closed && inst->processWaitHandle && inst->pi.hProcess) {
                    LogF(L"Detected unexpected Sumatra process exit for %s", inst->filePath.c_str());
                    inst->processWaitHandle = nullptr; // already fired; nothing left to unregister
                    CloseHandle(inst->pi.hThread);
                    CloseHandle(inst->pi.hProcess);
                    inst->pi = {};
                    inst->sumatraWnd = nullptr;

                    bool opened = TryFallbackShellOpen(inst->filePath);
                    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)ErrorWndProc);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, opened ? ERRSTATE_OPENED_EXTERNAL : ERRSTATE_CRASHED);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // avoid flicker; Sumatra's child window covers the whole client area
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static std::once_flag g_hostClassOnceFlag;

static void EnsureHostClassRegistered()
{
    std::call_once(g_hostClassOnceFlag, []() {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = HostWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = HOST_CLASS_NAME;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
    });
}

static HWND WaitForSumatraChild(HWND hostWnd, HANDLE hProcess, DWORD processId, DWORD timeoutMs)
{
    DWORD start = GetTickCount();

    // Most of the wait is normally "process hasn't created its message
    // queue / main window yet". WaitForInputIdle blocks efficiently (no
    // busy polling) until Sumatra's main thread is ready to pump messages,
    // which is almost always already true by the time it reparents itself.
    // This typically collapses the wait from several 30ms poll iterations
    // down to one OS wait call.
    DWORD idleBudget = timeoutMs > 1000 ? timeoutMs - 500 : timeoutMs / 2;
    WaitForInputIdle(hProcess, idleBudget);

    HWND  found = nullptr;
    DWORD pollIntervalMs = 15; // start fast; the window usually appears right after input-idle
    while (GetTickCount() - start < timeoutMs) {
        HWND child = GetWindow(hostWnd, GW_CHILD);
        while (child) {
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (pid == processId) { found = child; break; }
            child = GetWindow(child, GW_HWNDNEXT);
        }
        if (found) break;
        Sleep(pollIntervalMs);
        if (pollIntervalMs < 50) pollIntervalMs = std::min(pollIntervalMs + 10, 50ul); // hard-cap at 50ms
    }
    return found;
}

// ---------------------------------------------------------------------------
//  Crash detection: notice when an embedded Sumatra process exits on its
//  own (crash, killed externally, internal error) rather than via our own
//  intentional close, and show an accurate message instead of leaving a
//  dead, unresponsive blank pane.
//
//  Uses RegisterWaitForSingleObject (a thread-pool wait, not polling) on
//  the process handle. The callback runs on an arbitrary thread-pool thread
//  and must do minimal, thread-safe work only -- it just posts a message
//  back to the host window, which does the real handling on the UI thread.
//
//  Race safety: CloseRunningSumatraProcess (our own intentional-close path)
//  always calls UnregisterWaitEx BEFORE taking any action that could cause
//  the process to actually exit (WM_CLOSE / TerminateProcess). Since
//  UnregisterWaitEx cancels the registration and blocks until any
//  in-progress callback finishes, and the process handle cannot yet be
//  signaled at that point (we haven't asked it to close yet), an
//  intentional close can never itself trigger this notification. The
//  WM_SUMATRA_PROCESS_EXITED handler additionally checks that
//  processWaitHandle is still set before acting, as a defense-in-depth
//  guard against any notification that outlives its instance.
// ---------------------------------------------------------------------------

static void CALLBACK ProcessExitCallback(void* lpParameter, BOOLEAN /*timedOut*/)
{
    // Minimal work only: this runs on an OS thread-pool thread, not ours.
    // PostMessageW is documented safe to call from any thread.
    HWND hostWnd = (HWND)lpParameter;
    PostMessageW(hostWnd, WM_SUMATRA_PROCESS_EXITED, 0, 0);
}

// Registers crash detection for inst's currently-running process. Must be
// called with inst->opLock held, right after a successful embed.
static void RegisterCrashDetection(ListerInstance* inst)
{
    if (!inst->pi.hProcess || inst->processWaitHandle)
        return; // nothing to watch, or already watching

    if (!RegisterWaitForSingleObject(&inst->processWaitHandle, inst->pi.hProcess,
                                      ProcessExitCallback, (void*)inst->hostWnd,
                                      INFINITE, WT_EXECUTEONLYONCE)) {
        LogF(L"RegisterWaitForSingleObject failed, error %lu (crash detection unavailable for this instance)",
             GetLastError());
        inst->processWaitHandle = nullptr;
    }
}

// Cancels crash detection. Must be called with inst->opLock held, and
// BEFORE any action that could cause inst->pi.hProcess to actually exit --
// see the race-safety note above.
static void UnregisterCrashDetection(ListerInstance* inst)
{
    if (inst->processWaitHandle) {
        // INVALID_HANDLE_VALUE here means "block until any in-progress
        // callback completes," not "pass an invalid handle" -- this is the
        // documented Win32 idiom for a synchronous, race-free unregister.
        UnregisterWaitEx(inst->processWaitHandle, INVALID_HANDLE_VALUE);
        inst->processWaitHandle = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Launch SumatraPDF.exe in plugin mode, embedded into hostWnd
// ---------------------------------------------------------------------------

static void AppendArg(std::wstring& cmd, const std::wstring& arg)
{
    cmd += L' ';
    cmd += arg;
}

static void AppendQuotedArg(std::wstring& cmd, const std::wstring& arg)
{
    cmd += L" \"";
    cmd += arg;
    cmd += L'"';
}

// Converts a pointer-sized handle (HWND, HINSTANCE, ...) to its decimal
// string form, e.g. for the "-plugin <hwnd>" command line and for logging.
// The cast through `long long` looks redundant on a 64-bit-only build --
// GCC's -Wuseless-cast flags it there -- but this same source file also
// builds a 32-bit target (SumatraLister.wlx), where intptr_t is only 32
// bits. Passing a 32-bit vararg to a %lld format specifier (which always
// reads 64 bits) is a real stack-argument-width mismatch, not a style nit:
// verified empirically against GCC's own -Wformat on a 32-bit build, which
// flags precisely this shape of call, and the actual bytes read past the
// pushed argument are unspecified stack contents -- correct-looking output
// in testing would be luck, not a guarantee. Widening through `long long`
// here keeps the vararg's actual width matching the format specifier on
// both targets, which is why this one warning is deliberately left in a
// strict x64-only build rather than "fixed" by dropping the cast.
static long long HandleToInt64(void* handle)
{
    return static_cast<long long>(reinterpret_cast<intptr_t>(handle));
}

// ---------------------------------------------------------------------------
//  "Pop out" to a normal, full SumatraPDF window
//  ---------------------------------------------------------------------
//  Embedded -plugin mode deliberately hides Sumatra's menu/toolbar, so
//  there's no built-in way for the user to reach things like Save As,
//  rotate, annotate, or bookmarks from inside the Lister pane. EnablePopOut
//  registers a hotkey (Ctrl+Alt+O) that reopens the *same file* (preserving
//  the current page) in an ordinary, separate Sumatra window with its full
//  UI, without disturbing the embedded preview.
//
//  This is implemented with RegisterHotKey rather than by subclassing the
//  embedded Sumatra window. Subclassing won't work here: `sumatraWnd`
//  belongs to SumatraPDF.exe's own process, and a WNDPROC is just a code
//  pointer -- one from our DLL is meaningless (and would crash the other
//  process) when that process's message loop tries to call through it in
//  its own address space. RegisterHotKey is the correct, documented
//  mechanism for "deliver a message to my own window on a specific key
//  combo, regardless of which window currently has keyboard focus," and
//  the resulting WM_HOTKEY is delivered to hostWnd, which we DO own, in
//  our own process.
//
//  The real tradeoff, and why EnablePopOut defaults to OFF, is that the
//  hotkey claim is genuinely system-wide for as long as any Lister pane
//  using this plugin is open -- not scoped to just this preview having
//  focus -- so it can shadow the same combo in other applications.
// ---------------------------------------------------------------------------

static const UINT  POPOUT_HOTKEY_MOD = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
static const UINT  POPOUT_HOTKEY_VK  = 'O';
static int         g_nextHotkeyId    = 1; // see RegisterPopOutHotkey: protected by g_mutex

static void LaunchSumatraStandalone(const ListerInstance* inst)
{
    const std::wstring& exe = GetSumatraPath();
    if (exe.empty() || inst->filePath.empty())
        return;

    std::wstring filePath = inst->filePath;
    int page = ExtractPageSuffix(filePath);

    std::wstring cmdLine = L"\"" + exe + L"\"";
    if (page > 0) {
        AppendArg(cmdLine, L"-page");
        AppendArg(cmdLine, std::to_wstring(page));
    }
    if (inst->currentInvert)
        AppendArg(cmdLine, L"-invertcolors");
    AppendQuotedArg(cmdLine, filePath);

    LogF(L"Pop-out: launching standalone Sumatra: %s", cmdLine.c_str());

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Fire-and-forget: this standalone window's lifetime is intentionally
    // independent of the embedded instance, so we don't track its handles.
    if (CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LogF(L"Pop-out CreateProcess failed, error %lu", GetLastError());
    }
}

// Registers the pop-out hotkey for this instance's hostWnd. Idempotent
// (skips re-registering on every relaunch) and tolerant of failure (e.g.
// another application already owns Ctrl+Alt+O system-wide) -- pop-out
// simply stays unavailable for that instance rather than erroring out.
// Must be called on the same thread that created inst->hostWnd, which is
// always true here: every caller of LaunchSumatraEmbedded runs on Total
// Commander's UI thread, same as DoListLoad's CreateWindowExW for hostWnd.
static void RegisterPopOutHotkey(ListerInstance* inst)
{
    if (!g_config.enablePopOut || inst->hotkeyId != 0 || !inst->hostWnd)
        return;

    int id;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        id = g_nextHotkeyId++;
    }

    if (RegisterHotKey(inst->hostWnd, id, POPOUT_HOTKEY_MOD, POPOUT_HOTKEY_VK)) {
        inst->hotkeyId = id;
    } else {
        LogF(L"RegisterHotKey for pop-out failed, error %lu "
             L"(another application may already use Ctrl+Alt+O)", GetLastError());
    }
}

static void UnregisterPopOutHotkey(ListerInstance* inst)
{
    if (inst->hotkeyId != 0 && inst->hostWnd) {
        UnregisterHotKey(inst->hostWnd, inst->hotkeyId);
        inst->hotkeyId = 0;
    }
}

static bool LaunchSumatraEmbedded(ListerInstance* inst)
{
    const std::wstring& exe = GetSumatraPath();
    if (exe.empty()) {
        LogF(L"Cannot launch: SumatraPDF.exe not found");
        return false;
    }

    std::wstring filePath = inst->filePath;
    int page = ExtractPageSuffix(filePath); // strips "#page=N" if present

    wchar_t hwndBuf[32];
    swprintf_s(hwndBuf, L"%lld", HandleToInt64(inst->hostWnd));

    std::wstring cmdLine = L"\"" + exe + L"\"";
    AppendArg(cmdLine, L"-plugin");
    AppendArg(cmdLine, hwndBuf);

    if (page > 0) {
        AppendArg(cmdLine, L"-page");
        AppendArg(cmdLine, std::to_wstring(page));
    }
    if (!g_config.defaultZoom.empty()) {
        AppendArg(cmdLine, L"-zoom");
        AppendQuotedArg(cmdLine, g_config.defaultZoom);
    }
    if (!g_config.defaultView.empty()) {
        AppendArg(cmdLine, L"-view");
        AppendQuotedArg(cmdLine, g_config.defaultView);
    }
    if (inst->currentInvert)
        AppendArg(cmdLine, L"-invertcolors");
    if (g_config.restrictMode)
        AppendArg(cmdLine, L"-restrict");
    if (!g_config.extraArgs.empty())
        AppendArg(cmdLine, g_config.extraArgs);

    AppendQuotedArg(cmdLine, filePath);

    LogF(L"Launching: %s", cmdLine.c_str());

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        exe.c_str(),
        cmdBuf.data(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        LogF(L"CreateProcess failed, error %lu", GetLastError());
        return false;
    }

    inst->pi = pi;

    HWND child = WaitForSumatraChild(inst->hostWnd, pi.hProcess, pi.dwProcessId, g_config.embedTimeoutMs);
    if (!child) {
        LogF(L"Sumatra did not reparent within %lu ms (no -plugin support / crashed?)", g_config.embedTimeoutMs);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        inst->pi = {};
        return false;
    }

    inst->sumatraWnd = child;
    RegisterPopOutHotkey(inst);
    RegisterCrashDetection(inst);

    RECT rc; GetClientRect(inst->hostWnd, &rc);
    MoveWindow(child, 0, 0, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    ShowWindow(child, SW_SHOW);

    return true;
}

// Shared by TeardownInstance, RelaunchSumatra, and ListLoadNextW: closes the
// currently-running embedded Sumatra process gracefully (WM_CLOSE), falling
// back to TerminateProcess if it doesn't exit promptly. Caller must hold
// inst->opLock. Leaves inst->pi / inst->sumatraWnd zeroed on return.
static void CloseRunningSumatraProcess(ListerInstance* inst, DWORD graceMs = 1200)
{
    if (!inst->pi.hProcess)
        return;

    // Must happen before WM_CLOSE/TerminateProcess below -- see the
    // race-safety note at RegisterCrashDetection/UnregisterCrashDetection.
    UnregisterCrashDetection(inst);

    if (inst->sumatraWnd && IsWindow(inst->sumatraWnd))
        PostMessageW(inst->sumatraWnd, WM_CLOSE, 0, 0);

    if (WaitForSingleObject(inst->pi.hProcess, graceMs) == WAIT_TIMEOUT)
        TerminateProcess(inst->pi.hProcess, 0);

    CloseHandle(inst->pi.hThread);
    CloseHandle(inst->pi.hProcess);
    inst->pi = {};
    inst->sumatraWnd = nullptr;
}

// Tears the instance all the way down: closes the Sumatra process and
// destroys the host window. MUST be called with inst->opLock held, and is
// idempotent (the `closed` flag makes a second call a no-op) so it's safe
// even if some other thread is mid-operation and only gets opLock afterward.
static void TeardownInstance(ListerInstance* inst)
{
    if (!inst || inst->closed)
        return;
    inst->closed = true;

    CloseRunningSumatraProcess(inst, 1500);
    UnregisterPopOutHotkey(inst);

    if (inst->hostWnd && IsWindow(inst->hostWnd))
        DestroyWindow(inst->hostWnd);
    inst->hostWnd = nullptr;
}

// Last-resort fallback when Sumatra can't be found/embedded: open the file
// with whatever the user's default Windows handler for it is. This can't be
// embedded into the Lister pane (a foreign app's top-level window can't be
// safely reparented the way Sumatra's -plugin mode supports), so it opens
// as a separate window; the Lister pane still shows an explanatory message.
static bool TryFallbackShellOpen(const std::wstring& filePath)
{
    if (!g_config.fallbackShellOpen || filePath.empty())
        return false;

    LogF(L"FallbackToShellOpen: launching default handler for %s", filePath.c_str());
    HINSTANCE result = ShellExecuteW(nullptr, L"open", filePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool ok = (intptr_t)result > 32; // per ShellExecute convention
    if (!ok)
    LogF(L"FallbackToShellOpen failed, ShellExecute returned %lld", HandleToInt64(result));
    return ok;
}

// hwnd's GWLP_USERDATA doubles as a flag: 1 once a fallback shell-open
// actually succeeded for this pane, so WM_PAINT can show the right message.
// GWLP_USERDATA values for ErrorWndProc, set alongside the WNDPROC swap at
// each call site (defined near the top of this file, alongside the other
// message/state constants).

static LRESULT CALLBACK ErrorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);

        LONG_PTR state = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        const wchar_t* msg1 = L"SumatraPDF could not be found or started.";
        const wchar_t* msg2 = L"Install it from https://www.sumatrapdfreader.org/ "
                               L"or place SumatraPDF.exe next to this plugin. "
                               L"Enable DebugLog=1 in SumatraLister.ini for details.";
        if (state == ERRSTATE_OPENED_EXTERNAL) {
            msg1 = L"SumatraPDF could not be embedded here, so the file was opened in its default application instead.";
            msg2 = L"Install SumatraPDF from https://www.sumatrapdfreader.org/ for an embedded preview here.";
        } else if (state == ERRSTATE_CRASHED) {
            msg1 = L"SumatraPDF closed unexpectedly while viewing this file.";
            msg2 = L"This usually means the file is corrupted or triggered a rendering "
                   L"issue in SumatraPDF itself. Try a different file, or re-open this "
                   L"one to try again. Enable DebugLog=1 in SumatraLister.ini for details.";
        }

        InflateRect(&rc, -10, -10);
        DrawTextW(dc, msg1, -1, &rc, DT_TOP | DT_LEFT | DT_WORDBREAK);
        rc.top += 36;
        DrawTextW(dc, msg2, -1, &rc, DT_TOP | DT_LEFT | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static std::once_flag g_errorClassOnceFlag;

static void EnsureErrorClassRegistered()
{
    std::call_once(g_errorClassOnceFlag, []() {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = ErrorWndProc;
        wc.hInstance     = g_hInst;
        wc.lpszClassName = L"SumatraListerErrorWnd";
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
    });
}

// ---------------------------------------------------------------------------
//  Best-effort UI forwarding into Sumatra (find / copy / next-page / etc.)
//  Sumatra has no public message-based remote-control API, so these use
//  synthetic input (SendInput) targeted at its focused window, the same way
//  a user's own keypresses would reach it. This requires the embedding
//  window (i.e. Total Commander's Lister) to be the foreground window.
// ---------------------------------------------------------------------------

// Keyboard modifier for SendKeyCombo. A plain enum (not enum class) so the
// call sites read naturally, e.g. SendKeyCombo(ModCtrl, 'F'); fixed to a
// 1-byte underlying type since it only ever needs 3 values.
enum KeyModifier : std::uint8_t { ModNone, ModCtrl, ModShift };

// Synthesizes a key-down sequence (optionally with Ctrl or Shift held)
// followed by a matching key-up sequence, e.g. SendKeyCombo(ModCtrl, 'F')
// == Ctrl+F, SendKeyCombo(ModShift, VK_F3) == Shift+F3.
static void SendKeyCombo(KeyModifier mod, WORD vk)
{
    WORD modVk = (mod == ModCtrl) ? VK_CONTROL : (mod == ModShift) ? VK_SHIFT : 0;

    INPUT down[2] = {};
    int   n = 0;
    if (modVk) { down[n].type = INPUT_KEYBOARD; down[n].ki.wVk = modVk; n++; }
    down[n].type = INPUT_KEYBOARD; down[n].ki.wVk = vk; n++;
    SendInput(static_cast<UINT>(n), down, sizeof(INPUT)); // n is 1-2, bounded by the array size above

    INPUT up[2] = {};
    n = 0;
    up[n].type = INPUT_KEYBOARD; up[n].ki.wVk = vk; up[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (modVk) { up[n].type = INPUT_KEYBOARD; up[n].ki.wVk = modVk; up[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(static_cast<UINT>(n), up, sizeof(INPUT)); // n is 1-2, bounded by the array size above
}

static bool SetClipboardTextW(const std::wstring& text)
{
    if (!OpenClipboard(nullptr))
        return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        memcpy(dst, text.c_str(), bytes);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
    return hMem != nullptr;
}

// Reads the clipboard's current CF_UNICODETEXT content, if any. Used to
// save/restore the user's clipboard around ForwardFindToSumatra's
// paste-into-find-box trick, which would otherwise silently overwrite
// whatever the user last copied (potentially something they still meant to
// paste elsewhere) every time they used Lister's Find dialog.
static bool GetClipboardTextW(std::wstring& out)
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return false;
    if (!OpenClipboard(nullptr))
        return false;

    bool ok = false;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hData);
        if (src) {
            out = src;
            ok = true;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return ok;
}

// Opens Sumatra's find bar and executes a search for `text`, honoring the
// real LCS_BACKWARDS flag from SearchParameter for direction (F3/Shift+F3).
// Always (re)pastes `text` into the find box before searching, even on a
// "find next" call (LCS_FINDFIRST unset) -- this keeps Sumatra's find box
// guaranteed in sync with whatever Total Commander currently considers the
// search text to be, rather than assuming it's still there unchanged from
// an earlier call, at the cost of a harmless redundant paste on repeats.
static void ForwardFindToSumatra(ListerInstance* inst, const std::wstring& text, int searchFlags)
{
    if (!inst->sumatraWnd || !IsWindow(inst->sumatraWnd) || text.empty())
        return;

    bool backwards = (searchFlags & LCS_BACKWARDS) != 0;
    // LCS_MATCHCASE / LCS_WHOLEWORDS are intentionally not translated here:
    // this plugin doesn't have confirmed knowledge of Sumatra's exact find-
    // bar hotkeys for toggling those specific options, and guessing risks
    // silently toggling the wrong control instead. See README.

    std::wstring savedClipboard;
    bool hadClipboardText = GetClipboardTextW(savedClipboard);

    SetFocus(inst->sumatraWnd);
    SendKeyCombo(ModCtrl, 'F'); // Ctrl+F -> open/focus Sumatra's find toolbar
    Sleep(120);

    SendKeyCombo(ModCtrl, 'A'); // select any existing text in the find box
    Sleep(30);
    if (SetClipboardTextW(text)) {
        SendKeyCombo(ModCtrl, 'V'); // paste search text
        Sleep(30);
        SendKeyCombo(backwards ? ModShift : ModNone, VK_F3); // execute search in the requested direction

        // Give Sumatra a moment to actually process the paste and the
        // search before we touch the clipboard again -- restoring too
        // early could overwrite our search text before Sumatra's own
        // thread gets around to reading it, pasting stale content instead.
        Sleep(50);
        if (hadClipboardText)
            SetClipboardTextW(savedClipboard);
        // If there was nothing on the clipboard before (or it held a
        // non-text format we don't track), we leave the search text in
        // place rather than actively clearing it -- we only restore
        // content we know we clobbered.
    }
}

static void ForwardCopyToSumatra(ListerInstance* inst)
{
    if (!inst->sumatraWnd || !IsWindow(inst->sumatraWnd))
        return;
    SetFocus(inst->sumatraWnd);
    SendKeyCombo(ModCtrl, 'C'); // Ctrl+C -> copies current selection to clipboard
}

// Real LC_SELECTALL command: select the whole document contents.
static void ForwardSelectAllToSumatra(ListerInstance* inst)
{
    if (!inst->sumatraWnd || !IsWindow(inst->sumatraWnd))
        return;
    SetFocus(inst->sumatraWnd);
    SendKeyCombo(ModCtrl, 'A');
}

// Real LC_SETPERCENT command: scroll to a position in the document, given
// as a percent (0-100). Sumatra has no message-based API to query total
// page count, so translating an arbitrary percentage into an exact target
// page isn't possible without risking a silently wrong jump. The two
// unambiguous boundary cases are handled precisely via Sumatra's own
// Ctrl+Home/Ctrl+End; everything in between is a documented no-op rather
// than a guess.
static void ForwardSetPercentToSumatra(ListerInstance* inst, int percent)
{
    if (!inst->sumatraWnd || !IsWindow(inst->sumatraWnd))
        return;
    SetFocus(inst->sumatraWnd);
    if (percent <= 1)
        SendKeyCombo(ModCtrl, VK_HOME); // beginning of document
    else if (percent >= 99)
        SendKeyCombo(ModCtrl, VK_END);  // end of document
}

// Opens Sumatra's own native print dialog (full printer/page-range/copies
// control) in an already-embedded instance, used by ListPrint when reusing
// a live pane for the exact file being printed -- see ListPrintW.
static void ForwardPrintDialogToSumatra(ListerInstance* inst)
{
    if (!inst->sumatraWnd || !IsWindow(inst->sumatraWnd))
        return;
    SetFocus(inst->sumatraWnd);
    SendKeyCombo(ModCtrl, 'P');
}

// ---------------------------------------------------------------------------
//  File-list thumbnails (Total Commander's Thumbnail view / Quick View use
//  ListGetPreviewBitmap, separately from the Lister window above). Rather
//  than spawning Sumatra per thumbnail, this uses the Windows Shell thumbnail
//  pipeline (IShellItemImageFactory) -- the SumatraPDF installer registers a
//  shell thumbnail handler for the formats it supports, and modern Windows
//  also has its own built-in PDF thumbnail handler, so this works whether or
//  not the request even came from a Sumatra-associated file type.
// ---------------------------------------------------------------------------

static HBITMAP GetShellThumbnail(const std::wstring& path, int width, int height)
{
    if (!g_config.thumbnailsEnabled || path.empty())
        return nullptr;

    HBITMAP hbmp = nullptr;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool comInitHere = SUCCEEDED(hr);

    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        IShellItemImageFactory* factory = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory)))) {
            SIZE sz = { width, height };
            // SIIGBF_RESIZETOFIT keeps aspect ratio within the requested box;
            // SIIGBF_BIGGERSIZEOK lets the cache return a larger cached image
            // if a smaller exact match isn't available (faster, still scales
            // down fine for a file-list thumbnail).
            if (FAILED(factory->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp)))
                hbmp = nullptr;
            factory->Release();
        }
        item->Release();
    }

    if (comInitHere) CoUninitialize();
    return hbmp;
}

// ---------------------------------------------------------------------------
//  Printing (ListPrint). SumatraPDF supports fully non-interactive command-
//  line printing, so this launches a separate, hidden, synchronous Sumatra
//  process distinct from any embedded Lister instance.
// ---------------------------------------------------------------------------

static int DoListPrint(const std::wstring& fileToPrint, const std::wstring& printerName)
{
    const std::wstring& exe = GetSumatraPath();
    if (exe.empty() || fileToPrint.empty())
        return LISTPLUGIN_ERROR;

    std::wstring cmdLine = L"\"" + exe + L"\"";

    if (printerName.empty()) {
        AppendArg(cmdLine, L"-print-to-default");
    } else {
        AppendArg(cmdLine, L"-print-to");
        AppendQuotedArg(cmdLine, printerName);
    }

    if (!g_config.printSettings.empty()) {
        AppendArg(cmdLine, L"-print-settings");
        AppendQuotedArg(cmdLine, g_config.printSettings);
    }
    if (g_config.restrictMode)
        AppendArg(cmdLine, L"-restrict");

    AppendQuotedArg(cmdLine, fileToPrint);

    LogF(L"Printing: %s", cmdLine.c_str());

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        LogF(L"Print CreateProcess failed, error %lu", GetLastError());
        return LISTPLUGIN_ERROR;
    }

    // -print-to[-default] makes Sumatra print and exit on its own; wait
    // (bounded) for that exit so TC's print dialog/status reflects completion.
    DWORD waitResult = WaitForSingleObject(pi.hProcess, 60000);

    if (waitResult == WAIT_TIMEOUT) {
        // Sumatra didn't exit in time -- don't silently report success for
        // a print job we never actually confirmed finished. Terminate the
        // runaway process rather than just closing our handle to it, which
        // would leave it running indefinitely with nothing tracking it.
        // (This can't guarantee the print spooler/printer itself stops --
        // only that Sumatra's own process does.)
        LogF(L"Print job did not finish within 60s, terminating");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return LISTPLUGIN_ERROR;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (waitResult != WAIT_OBJECT_0) {
        LogF(L"WaitForSingleObject on print process failed, error %lu", GetLastError());
        return LISTPLUGIN_ERROR;
    }

    return LISTPLUGIN_OK;
}

// ===========================================================================
//  Exported WLX API
// ===========================================================================

extern "C" {

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        LoadConfig();
    } else if (reason == DLL_PROCESS_DETACH) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& kv : g_instances) {
            std::lock_guard<std::mutex> opLock(kv.second->opLock);
            TeardownInstance(kv.second.get());
        }
        g_instances.clear(); // drops our shared_ptr refs; frees each ListerInstance once unreferenced
    }
    return TRUE;
}

// --- ListGetDetectString --------------------------------------------------
__declspec(dllexport) void __stdcall ListGetDetectString(char* DetectString, int maxlen)
{
    static const char* detect =
        "EXT=\"PDF\" | EXT=\"CHM\" | EXT=\"DJVU\" | EXT=\"EPUB\" | "
        "EXT=\"FB2\" | EXT=\"FB2Z\" | EXT=\"MOBI\" | EXT=\"PRC\" | "
        "EXT=\"XPS\" | EXT=\"OXPS\" | "
        "EXT=\"CB7\" | EXT=\"CBR\" | EXT=\"CBT\" | EXT=\"CBZ\"";
    lstrcpynA(DetectString, detect, maxlen);
}

// --- core loader, shared by ANSI and Unicode entry points ----------------
static HWND DoListLoad(HWND ParentWin, const std::wstring& fileName)
{
    LoadConfig();
    EnsureHostClassRegistered();
    EnsureErrorClassRegistered();

    RECT rc; GetClientRect(ParentWin, &rc);

    HWND host = CreateWindowExW(
        0, HOST_CLASS_NAME, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        ParentWin, nullptr, g_hInst, nullptr);

    if (!host)
        return nullptr;

    auto inst = std::make_shared<ListerInstance>();
    inst->hostWnd       = host;
    inst->parentWin     = ParentWin;
    inst->filePath      = fileName;
    inst->currentInvert = g_config.autoNightMode ? false : g_config.nightMode; // AutoNightMode decides later via LC_NEWPARAMS

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_instances[host] = inst;
    }

    if (!LaunchSumatraEmbedded(inst.get())) {
        bool opened = TryFallbackShellOpen(fileName);
        SetWindowLongPtrW(host, GWLP_WNDPROC, (LONG_PTR)ErrorWndProc);
        SetWindowLongPtrW(host, GWLP_USERDATA, opened ? ERRSTATE_OPENED_EXTERNAL : ERRSTATE_NOT_FOUND);
        InvalidateRect(host, nullptr, TRUE);
    }

    return host;
}

// --- ListLoadW (Unicode, preferred by TC) ---------------------------------
// ShowFlags (e.g. lcs_alt_open, set when the file was opened via Alt+F3 /
// Ctrl+PgDn rather than F3) is accepted per the real signature but
// intentionally unused: this plugin embeds Sumatra the same way regardless
// of how the file was opened, so there's nothing to branch on.
__declspec(dllexport) HWND __stdcall ListLoadW(HWND ParentWin, WCHAR* FileToLoad, int /*ShowFlags*/)
{
    return DoListLoad(ParentWin, FileToLoad ? FileToLoad : L"");
}

// --- ListLoad (ANSI fallback for older Total Commander builds) -----------
__declspec(dllexport) HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int /*ShowFlags*/)
{
    return DoListLoad(ParentWin, AnsiToWide(FileToLoad));
}

// --- ListLoadNextW / ListLoadNext: reuse same window for next/prev file --
__declspec(dllexport) int __stdcall ListLoadNextW(HWND /*ParentWin*/, HWND ListWin, WCHAR* FileToLoad, int /*ShowFlags*/)
{
    ListerInstancePtr inst = FindInstance(ListWin);
    if (!inst)
        return LISTPLUGIN_ERROR;

    // inst->opLock (not g_mutex) covers the actual relaunch, so a slow
    // embed here doesn't block ListSendCommand/thumbnail calls for other
    // open Lister windows. Holding a shared_ptr (rather than the raw
    // pointer the map used to store) keeps the object alive even if
    // ListCloseWindow erases it from the map on another thread right now;
    // the `closed` check below then catches that race cleanly instead of
    // operating on a half-torn-down instance.
    std::lock_guard<std::mutex> opLock(inst->opLock);
    if (inst->closed)
        return LISTPLUGIN_ERROR;

    CloseRunningSumatraProcess(inst.get());

    inst->filePath = FileToLoad ? FileToLoad : L"";

    if (!LaunchSumatraEmbedded(inst.get())) {
        bool opened = TryFallbackShellOpen(inst->filePath);
        SetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC, (LONG_PTR)ErrorWndProc);
        SetWindowLongPtrW(inst->hostWnd, GWLP_USERDATA, opened ? ERRSTATE_OPENED_EXTERNAL : ERRSTATE_NOT_FOUND);
        InvalidateRect(inst->hostWnd, nullptr, TRUE);
        return LISTPLUGIN_ERROR;
    }

    // This host window may have been left showing ErrorWndProc by an
    // earlier failed attempt (e.g. Sumatra wasn't installed yet the first
    // time, and this is a later ListLoadNextW call after it was). Restore
    // normal operation now that embedding has actually succeeded, or
    // WM_SIZE/WM_SETFOCUS/WM_HOTKEY would silently stay broken for the
    // rest of this pane's life even though a document is now embedded.
    if ((WNDPROC)GetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC) != HostWndProc) {
        SetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC, (LONG_PTR)HostWndProc);
        SetWindowLongPtrW(inst->hostWnd, GWLP_USERDATA, 0);
    }
    return LISTPLUGIN_OK;
}

__declspec(dllexport) int __stdcall ListLoadNext(HWND ParentWin, HWND ListWin, char* FileToLoad, int ShowFlags)
{
    std::wstring wpath = AnsiToWide(FileToLoad);
    return ListLoadNextW(ParentWin, ListWin, wpath.empty() ? nullptr : &wpath[0], ShowFlags);
}

// --- ListCloseWindow -------------------------------------------------------
__declspec(dllexport) void __stdcall ListCloseWindow(HWND ListWin)
{
    ListerInstancePtr inst;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(ListWin);
        if (it == g_instances.end())
            return;
        inst = it->second;       // take our own ref before erasing
        g_instances.erase(it);   // new lookups (FindInstance) won't see this instance from here on
    }

    // Acquire opLock before tearing down: if some other thread is mid-way
    // through a relaunch (ListLoadNextW / ListSendCommand) it's holding
    // this lock right now, so we simply wait our turn. Either way,
    // TeardownInstance's `closed` guard makes this safe even if we somehow
    // raced ahead of it.
    std::lock_guard<std::mutex> opLock(inst->opLock);
    TeardownInstance(inst.get());
    // `inst` (and the ListerInstance it points to) is freed automatically
    // once this shared_ptr -- and any other thread's copy still in scope --
    // goes out of scope; no explicit delete needed.
}

// Tears down the currently-running embedded Sumatra process (if any) and
// starts a fresh one for inst->filePath / inst->currentInvert. Currently
// used only by AutoNightMode's LC_NEWPARAMS handling in ListSendCommand,
// since there's no live way to toggle -invertcolors on an already-running
// embedded instance without restarting it. Caller must hold inst->opLock.
// Returns false (after switching the pane to the same error/fallback UI
// every other launch path uses) if the relaunch didn't succeed -- e.g.
// Sumatra was closed/crashed externally between the original embed and
// this relaunch.
static bool RelaunchSumatra(ListerInstance* inst)
{
    CloseRunningSumatraProcess(inst);

    if (LaunchSumatraEmbedded(inst)) {
        // As in ListLoadNextW: clear any stale error state from a previous
        // failed attempt on this same reused host window.
        if ((WNDPROC)GetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC) != HostWndProc) {
            SetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC, (LONG_PTR)HostWndProc);
            SetWindowLongPtrW(inst->hostWnd, GWLP_USERDATA, 0);
        }
        return true;
    }

    bool opened = TryFallbackShellOpen(inst->filePath);
    SetWindowLongPtrW(inst->hostWnd, GWLP_WNDPROC, (LONG_PTR)ErrorWndProc);
    SetWindowLongPtrW(inst->hostWnd, GWLP_USERDATA, opened ? ERRSTATE_OPENED_EXTERNAL : ERRSTATE_NOT_FOUND);
    InvalidateRect(inst->hostWnd, nullptr, TRUE);
    return false;
}

// --- ListSendCommand --------------------------------------------------------
// Real command set verified against the official WLX SDK (see the
// constants block near the top of this file). TC calls this "when the
// user changes some options in Lister's menu" -- resizing, page
// navigation, zoom, and find/print all reach this plugin through other,
// more direct paths (WM_SIZE on hostWnd; normal keyboard focus once
// Sumatra's window has it; ListSearchDialog/ListSearchTextW; ListPrint) and
// were never real ListSendCommand traffic despite an earlier, unverified
// implementation assuming otherwise.
__declspec(dllexport) int __stdcall ListSendCommand(HWND ListWin, int Command, int Parameter)
{
    ListerInstancePtr instPtr = FindInstance(ListWin);
    if (!instPtr)
        return LISTPLUGIN_ERROR;

    std::lock_guard<std::mutex> opLock(instPtr->opLock);
    if (instPtr->closed)
        return LISTPLUGIN_ERROR;

    ListerInstance* inst = instPtr.get();

    switch (Command) {
        case LC_COPY:
            ForwardCopyToSumatra(inst);
            return LISTPLUGIN_OK;

        case LC_SELECTALL:
            ForwardSelectAllToSumatra(inst);
            return LISTPLUGIN_OK;

        case LC_SETPERCENT:
            ForwardSetPercentToSumatra(inst, Parameter);
            return LISTPLUGIN_OK;

        case LC_NEWPARAMS: {
            // Parameter is a combination of LCP_* flags. This plugin only
            // acts on the dark-mode ones (for AutoNightMode); the rest
            // (wrap/fit/ansi-ascii/center/...) describe TC's own built-in
            // text/hex/image viewer options and don't apply to an embedded
            // PDF/ebook view -- "you may ignore these parameters if they
            // don't apply to your document type" per the SDK docs.
            if (!g_config.autoNightMode)
                return LISTPLUGIN_OK;

            bool wantInvert = (Parameter & (LCP_DARKMODE | LCP_DARKMODENATIVE)) != 0;
            if (wantInvert != inst->currentInvert) {
                inst->currentInvert = wantInvert;
                LogF(L"AutoNightMode: LC_NEWPARAMS dark-mode flag %s -> invert=%d, relaunching",
                     wantInvert ? L"set" : L"unset", wantInvert);
                // This fires from TC's own theme-change notifications rather
                // than a direct user action, so a failed relaunch is
                // surfaced via the pane's error UI (RelaunchSumatra always
                // applies that) rather than through this command's return code.
                RelaunchSumatra(inst);
            }
            return LISTPLUGIN_OK;
        }

        default:
            return LISTPLUGIN_OK; // unhandled/unrecognized commands are simply ignored
    }
}

// --- ListSearchText / ListSearchTextW: Lister's own "find" dialog ---------
__declspec(dllexport) int __stdcall ListSearchTextW(HWND ListWin, WCHAR* SearchString, int SearchParameter)
{
    if (!SearchString)
        return LISTPLUGIN_ERROR;

    ListerInstancePtr inst = FindInstance(ListWin);
    if (!inst)
        return LISTPLUGIN_ERROR;

    std::lock_guard<std::mutex> opLock(inst->opLock);
    if (inst->closed)
        return LISTPLUGIN_ERROR;

    std::wstring text = SearchString;
    ForwardFindToSumatra(inst.get(), text, SearchParameter);
    return LISTPLUGIN_OK;
}

__declspec(dllexport) int __stdcall ListSearchText(HWND ListWin, char* SearchString, int SearchParameter)
{
    // &wtext[0] is well-defined even when wtext is empty (guaranteed to
    // reference the null terminator per the standard), so this safely
    // forwards through to ListSearchTextW's own null check either way.
    std::wstring wtext = AnsiToWide(SearchString);
    return ListSearchTextW(ListWin, &wtext[0], SearchParameter);
}

// --- ListSearchDialog: plugin-native find UI, preferred over ListSearchTextW ---
// Real, optional WLX export (SDK-verified): called instead of TC showing its
// own generic search dialog. Forwarding straight to Sumatra's own find bar
// gives the user its full find UI (result highlighting, its own match-case/
// whole-word controls) rather than the clipboard-paste bridge ListSearchTextW
// has to use for TC's generic dialog. Both are implemented and coexist
// fine per the SDK docs; TC prefers this one when both are exported.
__declspec(dllexport) int __stdcall ListSearchDialog(HWND ListWin, int FindNext)
{
    ListerInstancePtr inst = FindInstance(ListWin);
    if (!inst)
        return LISTPLUGIN_ERROR; // no live pane for this window; let TC fall back to its own dialog

    std::lock_guard<std::mutex> opLock(inst->opLock);
    if (inst->closed || !inst->sumatraWnd || !IsWindow(inst->sumatraWnd))
        return LISTPLUGIN_ERROR;

    SetFocus(inst->sumatraWnd);
    if (FindNext)
        SendKeyCombo(ModNone, VK_F3);  // continue the previous search forward
    else
        SendKeyCombo(ModCtrl, 'F');    // open Sumatra's own find bar fresh

    // Per the SDK docs: never return LISTPLUGIN_ERROR here just because we
    // can't confirm whether a match was found -- ERROR specifically means
    // "please show your own dialog instead," which isn't what happened;
    // we successfully showed Sumatra's native one.
    return LISTPLUGIN_OK;
}

// --- ListGetPreviewBitmapW / ListGetPreviewBitmap: file-list thumbnails ---
__declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmapW(
    WCHAR* FileToLoad, int width, int height, char* /*contentbuf*/, int /*contentbuflen*/)
{
    LoadConfig();
    if (!FileToLoad) return nullptr;
    return GetShellThumbnail(FileToLoad, width, height);
}

__declspec(dllexport) HBITMAP __stdcall ListGetPreviewBitmap(
    char* FileToLoad, int width, int height, char* contentbuf, int contentbuflen)
{
    if (!FileToLoad) return nullptr;
    std::wstring wpath = AnsiToWide(FileToLoad);
    if (wpath.empty()) return nullptr;
    return ListGetPreviewBitmapW(&wpath[0], width, height, contentbuf, contentbuflen);
}

// --- ListPrintW / ListPrint --------------------------------------------
// Note: `Margins` (custom print margins, in MM_LOMETRIC units -- i.e.
// tenths of a millimeter, per the SDK docs) is intentionally not forwarded
// to Sumatra. Sumatra manages its own print margins via -print-settings
// rather than accepting page-margin hints from the caller, and translating
// an arbitrary RECT into Sumatra's own margin syntax without live testing
// across versions risks silently WRONG margins (clipped content) rather
// than the current, honest no-op. Set PrintSettings= in the INI if you
// need specific margins. Margins may legitimately be null; the SDK docs
// say it "may be ignored," which this plugin does either way.
__declspec(dllexport) int __stdcall ListPrintW(
    HWND ListWin, WCHAR* FileToPrint, WCHAR* DefPrinter, int /*PrintFlags*/, RECT* /*Margins*/)
{
    LoadConfig();
    if (!FileToPrint) return LISTPLUGIN_ERROR;

    // Prefer Sumatra's own native print dialog (full printer/page-range/
    // copies control) by reusing a live embedded instance for the exact
    // file being printed, if one is open in this Lister window -- this
    // matches the SDK's documented expectation that ListPrint "show a
    // print dialog, in which the user can choose what to print, and
    // select a different printer" far more faithfully than silent CLI
    // printing does. Falls back to the CLI (-print-to/-print-to-default)
    // approach when there's no live matching instance to reuse.
    ListerInstancePtr inst = FindInstance(ListWin);
    if (inst) {
        std::lock_guard<std::mutex> opLock(inst->opLock);
        if (!inst->closed && inst->sumatraWnd && IsWindow(inst->sumatraWnd)) {
            std::wstring currentFile = inst->filePath;
            ExtractPageSuffix(currentFile); // strip any "#page=N" so the comparison matches the real path
            if (_wcsicmp(currentFile.c_str(), FileToPrint) == 0) {
                ForwardPrintDialogToSumatra(inst.get());
                return LISTPLUGIN_OK;
            }
        }
    }

    std::wstring printer = DefPrinter ? DefPrinter : L"";
    return DoListPrint(FileToPrint, printer);
}

__declspec(dllexport) int __stdcall ListPrint(
    HWND ListWin, char* FileToPrint, char* DefPrinter, int PrintFlags, RECT* Margins)
{
    if (!FileToPrint) return LISTPLUGIN_ERROR;

    std::wstring wFile    = AnsiToWide(FileToPrint);
    std::wstring wPrinter = AnsiToWide(DefPrinter);
    if (wFile.empty()) return LISTPLUGIN_ERROR;
    return ListPrintW(ListWin, &wFile[0], wPrinter.empty() ? nullptr : &wPrinter[0], PrintFlags, Margins);
}

// --- Optional: TC calls this once at startup with persisted plugin params --
__declspec(dllexport) void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps)
{
    LoadConfig();
    // Settings live in SumatraLister.ini (next to the DLL) rather than the
    // location dps->DefaultIniName suggests -- the SDK docs explicitly
    // sanction storing plugin-specific settings "directly in that file, or
    // in that directory under a different name," so this is a supported
    // alternative, not a deviation. Logged only for diagnostics.
    if (dps)
        LogF(L"ListSetDefaultParams: interface version %lu/%lu, TC-suggested ini: %hs",
             dps->PluginInterfaceVersionHi, dps->PluginInterfaceVersionLow, dps->DefaultIniName);
}

} // extern "C"
