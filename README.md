# SumatraLister — Total Commander Lister plugin powered by SumatraPDF

Displays PDF, CHM, DjVu, EPUB, FB2, MOBI, PRC, XPS/OXPS, and comic-archive
(CB7, CBR, CBT, CBZ) files inside Total Commander's Lister (F3) by embedding
SumatraPDF.exe directly into the Lister pane.

## API correctness

An earlier version of this plugin's `ListSendCommand` implementation, its
`ListSearchText`/`ListSearchTextW` handling, its "Auto Night Mode" feature,
and `ListPrint`'s exact signature were all based on **unverified** guesses
at the Total Commander Lister Plugin (WLX) API rather than the real,
documented one. This has since been corrected by fetching and reading the
official SDK directly (https://github.com/ghisler/WLX-SDK,
https://ghisler.github.io/WLX-SDK/). Concretely, what was wrong:

- **`ListSendCommand` used entirely invented command constants** (things
  named `LCS_RESIZE`, `LCS_NEXT`, `LCS_PREV`, `LCS_FIND`, `LCS_BACKGND`,
  `LCS_SETPAGE`, etc.) that Total Commander never actually sends. The real,
  complete command set is four values: `LC_COPY`, `LC_NEWPARAMS`,
  `LC_SELECTALL`, `LC_SETPERCENT`. Several of the old (fake) commands were
  "solving" things that either aren't real TC behavior at all, or already
  work for free through this plugin's own architecture — resizing happens
  via `WM_SIZE` sent directly to the host window, and page navigation/zoom
  reach Sumatra directly through normal keyboard input once its window has
  focus, with no explicit forwarding needed either way.
- **"Auto Night Mode" could never have worked.** It read a command called
  `LCS_BACKGND` carrying a background colour, which does not exist in the
  real API — Total Commander would never have sent it, so the feature was
  silently inert regardless of the `AutoNightMode=` setting. The real
  mechanism is completely different: dark-mode state arrives via
  `LC_NEWPARAMS` with the `LCP_DARKMODE`/`LCP_DARKMODENATIVE` flags. This is
  now implemented correctly.
- **`ListPrint`/`ListPrintW`'s `Margins` parameter was declared as `RECT`
  passed by value; the real signature is `RECT*` (a pointer).** Under the
  `__stdcall` convention TC uses, passing a 16-byte struct where the caller
  pushes an 8-byte pointer is a genuine stack-argument-size mismatch — a
  real crash/corruption risk on an actual call, not just a style issue.
  Fixed to the correct pointer signature.
- **`ListSearchText`/`ListSearchTextW` ignored `SearchParameter` entirely**,
  discarding the real `LCS_FINDFIRST`/`LCS_MATCHCASE`/`LCS_WHOLEWORDS`/
  `LCS_BACKWARDS` flags TC passes (these flags *are* real — the confusion
  was assuming they belonged to `ListSendCommand` instead). Now decoded and
  used, at minimum for search direction (F3 vs Shift+F3).
- Two `ListLoad` `ShowFlags` constants (`lcs_show_buttons`,
  `lcs_show_browser`) were also invented and, on inspection, were never
  actually used anywhere in the plugin's logic — removed.

Also added while correcting this: `ListSearchDialog`, a real, optional WLX
export that lets a plugin show its own find UI instead of routing through
TC's generic search dialog — a better fit here than the clipboard-paste
bridge `ListSearchTextW` has to use, since it forwards straight to
Sumatra's own find bar. And a real, verified `ITM_FOCUS` notification
mechanism so Total Commander's Quick View Panel (Ctrl+Q) correctly
highlights this pane when it gains focus.

**Deliberately not implemented:** `ListNotificationReceived`. This is a
real, optional WLX export, but the SDK's own documentation says plainly not
to implement it unless a plugin uses owner-drawn child controls or needs
`WM_MEASUREITEM`/`WM_DRAWITEM`/`WM_NOTIFY` routing — this plugin does
neither (it just hosts Sumatra's own reparented window), so implementing it
would be dead weight against the SDK's own guidance, not a gap.

Every constant and signature referenced above and used in `SumatraLister.cpp`
was checked against the official SDK pages directly; see the comments at
each site in the source for specifics.

## Features

- **Auto-detection** of SumatraPDF via registry App Paths, install/uninstall
  keys, common install folders, the plugin's own folder, then `PATH`.
- **`SumatraLister.ini`** (optional, same folder as the plugin) lets you:
  - override the auto-detected path (`SumatraPath=`)
  - pass arbitrary extra launch arguments (`ExtraArgs=`)
  - launch in sandboxed `-restrict` mode for untrusted files (`RestrictMode=1`)
  - launch in inverted-colour / night reading mode (`NightMode=1`)
  - set a default zoom / view mode (`DefaultZoom=`, `DefaultView=`)
  - set a page-surround background colour (`BackgroundColor=`)
  - trim which extensions the plugin claims (`EnabledExtensions=`)
  - override zoom/view/night-mode per file type, e.g. comics vs. PDFs (an
    `[EXT]` section such as `[CBZ]`)
  - enable a troubleshooting log, `SumatraLister.log` (`DebugLog=1`)

  See `SumatraLister.ini` in this package for the full, commented template —
  copy it next to the `.wlx`/`.wlx64` file and edit as needed.
- **Deep links**: append parameters to the file path passed into
  `ListLoad`/`ListLoadNext` after a `#`, e.g. `C:\book.pdf#page=42`. A real
  file that happens to be named that way on disk always takes precedence
  over the heuristic. Supported parameters (combine with `&`, e.g.
  `#page=5&search=foo`):
  - `page=N` — open at page N
  - `dest=NAME` — open at a named destination / table-of-contents entry (`-named-dest`)
  - `search=TERM` — open with TERM already searched and highlighted (`-search`)
  - `forwardsearch=SOURCE:LINE` — SyncTeX/PdfSync jump from a LaTeX source
    location, e.g. `#forwardsearch=C%3A%5Csrc%5Cmain.tex:123` (`-forward-search`)
  - `pwd=PASSWORD` — open a password-protected file (`-pwd`); only ever read
    from a link supplied like this, never stored anywhere by this plugin
  - Values are percent-decoded (`%20` → space). `page`/`dest`/`forwardsearch`
    are mutually exclusive "where to open" commands; `search`/`pwd` combine
    with any of them.
- **Find / Copy / Select All / scroll-to-percent**: the real
  `ListSendCommand` set (`LC_COPY`, `LC_SELECTALL`, `LC_SETPERCENT`) and
  `ListSearchText[W]`/`ListSearchDialog` (honoring the real
  `LCS_FINDFIRST`/`LCS_BACKWARDS` flags for search direction) are forwarded
  into Sumatra's own find bar, clipboard copy, and select-all — done with
  synthesized keystrokes (`SendInput`) targeted at the embedded window,
  since Sumatra has no message-based remote-control API. Page navigation
  and zoom need no forwarding at all: they reach Sumatra directly through
  normal keyboard input once its window has focus.
- **Process reuse** across `ListLoadNext` (Lister's next/previous file
  navigation reuses the same host window instead of recreating it).
- **File-list thumbnails**: implements `ListGetPreviewBitmap[W]` using the
  Windows Shell thumbnail pipeline (`IShellItemImageFactory`), so Total
  Commander's Thumbnails view shows real first-page previews for these file
  types — no extra Sumatra process spawned per thumbnail. Toggle with
  `ThumbnailsEnabled=` in the INI.
- **Printing**: implements `ListPrint[W]`. When the file being printed is
  already open in a live embedded pane, shows Sumatra's own native print
  dialog (full printer/page-range/copies control) via Ctrl+P. Otherwise
  (the less common case — no live pane to reuse), `InteractivePrintFallback=`
  decides what happens: by default (`1`), it still shows Sumatra's own
  interactive print dialog (`-print-dialog -exit-when-done`) and returns
  immediately rather than waiting for you to finish with it; set it to `0`
  for the old silent, non-interactive behaviour (`-print-to[-default]
  -silent`, briefly blocking until the job finishes) if you need
  scripted/automated printing that can't have a dialog pop up unattended.
  Either mode honours `PrintSettings=` from the INI (e.g. `fit`, `duplex`,
  `landscape`).
- **Auto night mode**: with `AutoNightMode=1`, the plugin reads Total
  Commander's real dark-mode notification (`LC_NEWPARAMS` with the
  `LCP_DARKMODE`/`LCP_DARKMODENATIVE` flags) and relaunches Sumatra
  with/without `-invertcolors` to match TC's theme, instead of relying on a
  fixed `NightMode=` value.
- **Quick View Panel integration**: notifies Total Commander
  (`WM_COMMAND`/`ITM_FOCUS`) when the embedded pane gains focus, so the
  Quick View Panel (Ctrl+Q) highlights its header correctly.
- **Crash detection**: if an embedded Sumatra process exits unexpectedly —
  crashed, killed externally, an internal Sumatra error — the pane switches
  to a clear "closed unexpectedly" message (and, if `FallbackToShellOpen=1`,
  tries opening the file in its default handler) instead of staying a dead,
  unresponsive blank window. Uses a Windows thread-pool wait on the process
  handle (`RegisterWaitForSingleObject`), so detection is immediate and
  doesn't poll.
- **Pop-out to full Sumatra** (`EnablePopOut=1`, off by default): registers
  Ctrl+Alt+O to reopen the current file in a normal, full SumatraPDF window
  with its menu/toolbar, for anything the embedded view doesn't expose
  (Save As, annotating, rotating, bookmarks). Off by default because the
  hotkey is active system-wide for as long as any Lister pane using this
  plugin is open, not just while that pane has focus — see Limitations.
- **Diagnostic snapshot** (`EnableDiagnosticHotkey=1`, off by default, same
  system-wide scoping caveat as pop-out): Ctrl+Alt+D copies plugin version,
  detected Sumatra path, effective settings, and a recent log tail (if
  `DebugLog=1`) to the clipboard — handy for pasting into a bug report.
  Never includes a deep link's `pwd`/`search` value.
- **Fallback to default app** (`FallbackToShellOpen=1`): if Sumatra can't be
  found or fails to embed, opens the file with its default Windows handler
  in a separate window instead of just showing an error message.
- **Configurable embed timeout** (`EmbedTimeoutMs=`, default 4000): how long
  to wait for Sumatra to reparent itself before giving up; the wait itself
  uses `WaitForInputIdle` rather than fixed polling, so it typically resolves
  much faster than the timeout in the normal case.
- **Friendly fallback** message inside the pane if Sumatra can't be found or
  embedded, instead of a blank/crashed view.

## Verified builds

This isn't just "should compile" — the source in this package has actually
been compiled and linked (via MinGW-w64, both `x86_64-w64-mingw32-g++` and
`i686-w64-mingw32-g++`) into real, loadable `PE32`/`PE32+` DLLs with all 14
expected entry points present under their correct undecorated names
(`ListLoad`, `ListLoadW`, `ListGetPreviewBitmap`, `ListPrintW`, etc. — verified
via `objdump -p`'s export table). It's also been checked with `cppcheck` and,
separately, `clang-tidy` (an independent static-analysis engine from a
different vendor than GCC, run against both `bugprone-*`/`cert-*` and
`performance-*` checks) — plus GCC with a much stricter warning set than a
typical build uses (`-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast`
and friends) on both architectures. Every finding from all of these was
individually reviewed: some were real bugs (fixed, see below), some were
verified-safe Windows/cross-architecture idioms a generic linter can't have
context for (documented in-source rather than "fixed" in a way that would
actually break something), and none were left unexamined.

**Bugs found and fixed by this process** (beyond what's described inline at
each fix site in the source):
- The `ListSendCommand`/`ListSearchText`/`ListPrint`/"Auto Night Mode"
  correctness issues described in "API correctness" above — found by
  fetching and reading the official WLX SDK directly rather than relying on
  memory, after which several previously-"working" features turned out to
  have never been reachable through real Total Commander behavior at all.
- `ForwardFindToSumatra` (Lister's Find dialog) was silently overwriting the
  system clipboard with the search text and never restoring what was there
  before — every search clobbered whatever the user had last copied. Now
  saves and restores it.
- `ListPrint`'s 60-second wait for Sumatra to finish printing discarded the
  wait's actual result: a timeout was silently reported as success, and the
  still-running process was never terminated, just abandoned. Now a timeout
  is reported as failure and the runaway process is terminated.
- An early draft of the pop-out feature subclassed Sumatra's own window with
  a `WNDPROC` from this DLL — which doesn't work, since that window belongs
  to a different process and a code pointer from one process is meaningless
  in another. Replaced with `RegisterHotKey`, the mechanism actually designed
  for this.
- A host window left showing the "Sumatra not found" error pane by one
  failed launch attempt stayed stuck in that state even after a later
  attempt succeeded (e.g. Sumatra gets installed partway through a session),
  silently breaking resize/focus/hotkey handling for that pane from then on.
- Registry-view probing during Sumatra detection tried 3 `KEY_WOW64_*`
  variations per key when at most 2 are ever meaningfully different for a
  given build — the third was always a wasted duplicate.
- The diagnostic-snapshot hotkey (Ctrl+Alt+D) showed its confirmation
  message box while still holding the same per-pane lock every other
  operation on that pane needs — closing the pane, navigating to the next
  file, a find/copy command, even pressing the hotkey again would all have
  blocked for however long the user left that confirmation dialog open,
  since it's a modal call with no bound on how long that takes. Fixed by
  moving the message box outside the lock, keeping only the actual
  data-gathering (which does need it) protected.
- `CMakeLists.txt` was missing `ole32` and `gdi32` from its linked
  libraries, even though the source genuinely uses both (COM for the
  thumbnail feature, GDI for the error pane's rendering) — every one of
  this project's own MinGW verification builds throughout development
  explicitly included `-lole32 -lgdi32`, but the CMakeLists.txt itself was
  never updated to match. In practice this gap turned out to be masked for
  most real build paths: MinGW-w64's own linker driver automatically
  appends a standard set of Windows default libraries (including both of
  these) to every DLL link regardless of what's explicitly requested, and
  a CMake-generated Visual Studio project file typically inherits a
  similar default library list from VS's own project template — which is
  almost certainly why an actual MSVC build got as far as a `LNK4070`
  warning rather than failing outright. It's still a real correctness gap
  worth fixing, though: a CMake generator that doesn't go through a
  `.vcxproj` (e.g. Ninja or NMake makefiles targeting the MSVC toolset
  directly) has no such safety net, and a `CMakeLists.txt` shouldn't rely
  on another tool's implicit default-library behavior to begin with. Fixed
  by adding both explicitly; verified with a real CMake-driven MinGW build
  (configure + compile + link, not just a manual compiler invocation).

## How it works

SumatraPDF ships a "plugin" embedding mode used by its browser plugin/ActiveX
control:

```
SumatraPDF.exe -plugin <decimal HWND> "<file>"
```

Given a window handle, Sumatra reparents its main window underneath it and
hides its menu/toolbar/statusbar, leaving a clean reader view. This plugin:

1. Locates `SumatraPDF.exe` (registry App Paths, install/uninstall keys,
   common Program Files / AppData locations, the plugin's own folder, then
   `PATH`).
2. On `ListLoad`, creates a small host child window inside the HWND Total
   Commander supplies, and launches Sumatra with `-plugin <hostHwnd> <file>`.
3. Waits for Sumatra's window to appear as a child of the host window and
   resizes it to fill the Lister pane; forwards `WM_SIZE` on every resize.
4. On `ListLoadNext` (Lister's "next file" navigation), tears down the old
   Sumatra process and starts a fresh one for the new file in the same host
   window.
5. On `ListCloseWindow`, asks Sumatra to close gracefully (falls back to
   `TerminateProcess` after a short timeout) and destroys the host window.

If Sumatra cannot be found or fails to embed (e.g. an old version without
`-plugin` support), the pane shows a short explanatory message instead of a
crash or blank screen.

## Thread safety

Total Commander can call into a Lister plugin from more than one thread at
once — most notably, it generates Thumbnails-view previews
(`ListGetPreviewBitmap`) on a background thread, concurrently with normal
UI-thread calls (`ListLoad`, `ListSendCommand`, ...) for any open Lister
window. This plugin accounts for that:

- Each open Lister pane's state is held via `shared_ptr`, so the underlying
  object stays alive for as long as any thread is still using it, even if
  `ListCloseWindow` removes it from the instance map on another thread at
  the same moment.
- Each pane has its own lock, separate from the map's lock, so a slow
  operation (relaunching Sumatra, waiting on synthesized keystrokes) on one
  pane never blocks calls for a different pane or a thumbnail request.
- Config loading and Sumatra-path detection are one-time, lock-protected
  initializations (`std::call_once`), safe regardless of which thread
  triggers them first.

## Requirements

- Windows 7 or later
- SumatraPDF installed (any recent build — `-plugin` has been supported for
  many years). Get it from https://www.sumatrapdfreader.org/
- Total Commander 7.5+ (Unicode build recommended; the plugin also exports
  ANSI entry points for older versions)

## Building

### Visual Studio (recommended)

Open a "x64 Native Tools" or "x86 Native Tools" command prompt in this
folder, then:

```bat
:: 64-bit build -> SumatraLister.wlx64
cmake -B build64 -A x64
cmake --build build64 --config Release

:: 32-bit build -> SumatraLister.wlx
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

Build **both** if your Total Commander install could be either bitness —
TC auto-selects the matching file by extension (`.wlx` for 32-bit TC,
`.wlx64` for 64-bit TC).

**About `warning LNK4070`**: if you see something like `/OUT:SumatraLister.dll
directive in .EXP differs from output filename '...SumatraLister.wlx64';
ignoring directive`, that's expected and harmless — it's MSVC noticing that
`SumatraLister.def` doesn't hardcode an output name (deliberately, since
this same `.def` file is shared by both the 32-bit and 64-bit builds, which
produce different filenames) and just falling back to the real one you
asked for. It only affects programs that implicitly/statically link
against the generated `.lib`; Total Commander loads this plugin via
`LoadLibrary`+`GetProcAddress` instead, which never touches that metadata.
See the comment in `SumatraLister.def` for the full explanation.

### MinGW-w64

```bash
# 64-bit
x86_64-w64-mingw32-g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ \
    SumatraLister.cpp SumatraLister.def \
    -lshlwapi -luser32 -ladvapi32 -lshell32 -lole32 -lgdi32 \
    -o SumatraLister.wlx64

# 32-bit
i686-w64-mingw32-g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ \
    SumatraLister.cpp SumatraLister.def \
    -lshlwapi -luser32 -ladvapi32 -lshell32 -lole32 -lgdi32 \
    -o SumatraLister.wlx
```

## Installing into Total Commander

1. Copy `SumatraLister.wlx` (32-bit TC) or `SumatraLister.wlx64` (64-bit TC)
   anywhere, e.g. `%COMMANDER_PATH%\Plugins\wlx\SumatraLister\`.
2. In Total Commander: **Configuration → Options → Plugins → Lister plugins
   → Add/configure**, browse to the `.wlx`/`.wlx64` file, and confirm.
3. Total Commander will read `ListGetDetectString` automatically and offer
   the plugin for the listed extensions; you can also force it for a single
   file via Lister's "Plugins" menu.

No INI file or extra configuration is required — Sumatra is located
automatically at runtime.

## Supported extensions

PDF, CHM, DJVU, EPUB, FB2, FB2Z, MOBI, PRC, XPS, OXPS, CB7, CBR, CBT, CBZ
(exactly what SumatraPDF itself can open).

## Notes / limitations

- This wraps Sumatra's window rather than re-implementing a renderer.
  Find/copy/select-all forwarding uses synthesized keystrokes (`SendInput`)
  aimed at the embedded window, which requires the Lister window to be the
  foreground window — this matches normal usage but won't work if something
  else has stolen focus at the exact moment a search is triggered. Page
  navigation and zoom don't need this at all: they reach Sumatra directly
  through normal keyboard input once its window has focus.
- Crash detection (`RegisterWaitForSingleObject` on the embedded process
  handle) degrades gracefully if registration itself fails, which the
  Windows documentation allows for but doesn't specify a concrete cause for
  in practice — the pane simply behaves as it did before this feature
  existed (an unexpected exit would leave a blank, unresponsive pane until
  the user navigates away) rather than erroring out. This is logged when
  `DebugLog=1`. The detection is intentionally ordered so that this
  plugin's own intentional closes (navigating to the next file, relaunching
  for a page jump or theme change, closing the Lister window) can never be
  mistaken for a crash — see the comment block above `ProcessExitCallback`
  in the source for the exact reasoning.
- `LCS_MATCHCASE`/`LCS_WHOLEWORDS` (case-sensitive / whole-word search
  flags from `ListSearchText`) are not translated into Sumatra's find bar.
  This plugin doesn't have confirmed knowledge of Sumatra's exact find-bar
  hotkeys for toggling those specific options, and guessing risked silently
  toggling the wrong control instead of the intended one. `LCS_BACKWARDS`
  (search direction) is honored via F3/Shift+F3.
- `LC_SETPERCENT` (Total Commander's "scroll to X% of document") and the
  `#page=N` deep link are handled differently. The deep link opens exactly
  at the requested page by relaunching the embedded Sumatra process with
  `-page N` (there's no way to tell an already-running embedded instance to
  jump pages without restarting it — fast, but does cause a brief reload).
  `LC_SETPERCENT` only has real page-count information TC's side, which
  Sumatra has no message-based API to query from this plugin's side — so
  only the two unambiguous boundaries (0%→first page, 100%→last page) are
  handled precisely, via Sumatra's own Ctrl+Home/Ctrl+End; intermediate
  percentages are a documented no-op rather than a guess at the wrong page.
  A real file that happens to be named literally like a deep link (e.g.
  `Report#page=2.pdf`) is always opened as-is — the filesystem is checked
  before the `#page=` heuristic is applied.
- `EnablePopOut`'s Ctrl+Alt+O hotkey is registered via Win32's
  `RegisterHotKey`, which is process- and thread-scoped but **not** scoped to
  the embedded pane having focus — it's live anywhere in the system for as
  long as any Lister pane using this plugin is open, and could shadow the
  same combo in another running application. This is why it defaults to off;
  turn it on deliberately if you don't have another use for Ctrl+Alt+O.
- Closing/reopening files rapidly (e.g. holding the cursor key in TC's file
  list with Lister's "auto-update" preview on) will start/stop a Sumatra
  process per file; this is inherent to the embedding approach and matches
  how Sumatra's own browser plugin behaves.
- `RestrictMode`, `NightMode`, `DefaultZoom`, `DefaultView`, `PrintSettings`,
  and `ExtraArgs` all map to real SumatraPDF command-line switches
  (`-restrict`, `-invertcolors`, `-zoom`, `-view`, `-print-settings`); verify
  exact accepted values for your installed version with `SumatraPDF.exe -h`,
  since these have evolved slightly across releases. (These are genuine
  SumatraPDF CLI switches, independent of the WLX API corrections described
  above — SumatraPDF's own command-line reference was not the source of the
  earlier errors.)
- Tested conceptually against Sumatra's documented `-plugin` switch; verify
  against your installed Sumatra build's command-line help if you use a very
  old version.
- Thumbnails rely on a registered Windows Shell thumbnail handler for the
  given extension (SumatraPDF's installer registers one for its supported
  formats; PDF also has a built-in Windows handler on modern versions). If a
  given extension has no handler registered, `ListGetPreviewBitmap` simply
  returns no bitmap and TC falls back to its default icon — it won't crash.
- `ListPrint`'s fallback path (no live pane to reuse) defaults to showing
  Sumatra's own interactive print dialog and returning immediately
  (`InteractivePrintFallback=1`) — see Features. The old 60-second-timeout,
  wait-for-completion behavior only applies in the legacy silent mode
  (`InteractivePrintFallback=0`); very large documents on a slow printer
  could still exceed that timeout in that mode, in which case the plugin
  reports failure and terminates the still-running Sumatra process rather
  than leaving it orphaned. This wait happens on whichever thread Total
  Commander calls `ListPrint` on. The interactive mode is deliberately
  fire-and-forget instead of also waiting, since blocking on arbitrary,
  user-paced dialog interaction would make Total Commander's own UI appear
  frozen for however long that takes.
- `ListPrint` does not forward custom print margins from Total Commander
  (the `Margins` parameter, a `RECT*` in units of MM_LOMETRIC — tenths of a
  millimeter — per the SDK docs). Sumatra manages its own print margins via
  `-print-settings` rather than accepting page-margin hints from the caller,
  and translating an arbitrary rectangle into Sumatra's own margin syntax
  without live cross-version testing risked silently wrong (clipped) output
  — worse than the current, explicit no-op. Use `PrintSettings=` in the INI
  if you need specific margins.
- The `forwardsearch=` deep-link parameter's exact argument shape to
  Sumatra's `-forward-search` switch has one point of genuine ambiguity:
  Sumatra's own documentation shows this switch both with and without an
  explicit third `<pdfpath>` argument (the other two being the source file
  and line number) in different examples. This plugin always supplies the
  PDF via the ordinary trailing file argument — the same convention
  `-page`/`-zoom`/`-view` already rely on here — and does not pass a third
  argument to `-forward-search` itself. This is expected to work based on
  that convention, but hasn't been confirmed against a real forward-search
  workflow; if it doesn't work as expected for you, that argument shape is
  the first thing to check with `SumatraPDF.exe -h` against your version.
- Deep-link values (destination names, search terms, LaTeX source paths,
  passwords) go through a properly-escaping command-line quoting routine
  (matching `CommandLineToArgvW`'s parsing rules) rather than naive
  `"value"` wrapping, since these are more free-form than the strictly
  digit-validated page numbers the original `#page=N` link used — an
  unescaped embedded quote could otherwise break out of its argument and
  inject additional command-line arguments. Verified with a set of
  known-tricky round-trip tests (embedded quotes, trailing backslashes,
  an explicit injection attempt) during development, not just reasoned
  through.
- `EnableDiagnosticHotkey`'s Ctrl+Alt+D has the same system-wide scoping
  caveat as `EnablePopOut`'s Ctrl+Alt+O — see the note on that above. Both
  default to off for the same reason and can be enabled independently.
