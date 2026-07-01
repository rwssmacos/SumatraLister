# SumatraLister — Total Commander Lister plugin powered by SumatraPDF

Displays PDF, CHM, DjVu, EPUB, FB2, MOBI, PRC, XPS/OXPS, and comic-archive
(CB7, CBR, CBT, CBZ) files inside Total Commander's Lister (F3) by embedding
SumatraPDF.exe directly into the Lister pane.

## Features

- **Auto-detection** of SumatraPDF via registry App Paths, install/uninstall
  keys, common install folders, the plugin's own folder, then `PATH`.
- **`SumatraLister.ini`** (optional, same folder as the plugin) lets you:
  - override the auto-detected path (`SumatraPath=`)
  - pass arbitrary extra launch arguments (`ExtraArgs=`)
  - launch in sandboxed `-restrict` mode for untrusted files (`RestrictMode=1`)
  - launch in inverted-colour / night reading mode (`NightMode=1`)
  - set a default zoom / view mode (`DefaultZoom=`, `DefaultView=`)
  - enable a troubleshooting log, `SumatraLister.log` (`DebugLog=1`)

  See `SumatraLister.ini` in this package for the full, commented template —
  copy it next to the `.wlx`/`.wlx64` file and edit as needed.
- **Deep-link page jumping**: append `#page=N` to the file path passed into
  `ListLoad`/`ListLoadNext` (e.g. `C:\book.pdf#page=42`) to open directly at
  page 42. Lister's own page-navigation toolbar (`LCS_SETPAGE`) uses the same
  mechanism internally.
- **Find / Copy / zoom forwarding**: Lister's Find dialog (`ListSearchTextW`),
  and the toolbar's Find/Copy/Zoom buttons (`ListSendCommand`), are forwarded
  into Sumatra's own find bar, clipboard copy, and Ctrl+/Ctrl- zoom — this is
  done with synthesized keystrokes (`SendInput`) targeted at the embedded
  window, since Sumatra has no message-based remote-control API.
- **Process reuse** across `ListLoadNext` (Lister's next/previous file
  navigation reuses the same host window instead of recreating it).
- **File-list thumbnails**: implements `ListGetPreviewBitmap[W]` using the
  Windows Shell thumbnail pipeline (`IShellItemImageFactory`), so Total
  Commander's Thumbnails view shows real first-page previews for these file
  types — no extra Sumatra process spawned per thumbnail. Toggle with
  `ThumbnailsEnabled=` in the INI.
- **Printing**: implements `ListPrint[W]`, so Total Commander's File > Print
  on a selected file launches Sumatra's own non-interactive command-line
  printing (`-print-to[-default]`), optionally with `-print-settings` from
  the INI (`PrintSettings=`, e.g. `fit`, `duplex`, `landscape`).
- **Auto night mode**: with `AutoNightMode=1`, the plugin reads Total
  Commander's own Lister background colour (`LCS_BACKGND`) and relaunches
  Sumatra with/without `-invertcolors` to match TC's light/dark theme,
  instead of relying on a fixed `NightMode=` value.
- **Pop-out to full Sumatra** (`EnablePopOut=1`, off by default): registers
  Ctrl+Alt+O to reopen the current file in a normal, full SumatraPDF window
  with its menu/toolbar, for anything the embedded view doesn't expose
  (Save As, annotating, rotating, bookmarks). Off by default because the
  hotkey is active system-wide for as long as any Lister pane using this
  plugin is open, not just while that pane has focus — see Limitations.
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
via `objdump -p`'s export table), and separately checked with `cppcheck`
for logic issues beyond what the compiler catches. A few real bugs were
caught and fixed this way during development — see Notes / limitations below
for the ones worth knowing about even now that they're fixed, and the
in-source comments at each fix site for the specifics.

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
  Find/copy/zoom/page-navigation forwarding uses synthesized keystrokes
  (`SendInput`) aimed at the embedded window, which requires the Lister
  window to be the foreground window — this matches normal usage but won't
  work if something else has stolen focus at the exact moment a toolbar
  button is pressed.
- `LCS_SETPAGE` (jump to page) and the `#page=N` deep link both work by
  relaunching the embedded Sumatra process with `-page N`, since there's no
  way to tell an already-running embedded instance to jump pages without
  restarting it. This is fast but does cause a brief reload. A real file
  that happens to be named literally like a deep link (e.g.
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
  since these have evolved slightly across releases.
- Tested conceptually against Sumatra's documented `-plugin` switch; verify
  against your installed Sumatra build's command-line help if you use a very
  old version.
- Thumbnails rely on a registered Windows Shell thumbnail handler for the
  given extension (SumatraPDF's installer registers one for its supported
  formats; PDF also has a built-in Windows handler on modern versions). If a
  given extension has no handler registered, `ListGetPreviewBitmap` simply
  returns no bitmap and TC falls back to its default icon — it won't crash.
- `ListPrint` waits for Sumatra's print job to finish and exit before
  returning, up to a 60-second timeout; very large documents on a slow
  printer could exceed it. This wait happens on whichever thread Total
  Commander calls `ListPrint` on.
