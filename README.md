# Playlist Tabs 64 (foo_vtabs64)

A playlist tab strip for foobar2000 (64-bit and 32-bit), vertical or horizontal, with sorting.
Original implementation against the public foobar2000 SDK. It is not a port of
any existing component and contains no third-party component code.

## Features

- Switch layout any time: right-click > **Orientation** > Vertical, Horizontal, or Automatic
  (Automatic picks horizontal when the panel is wider than tall). The choice is saved with your layout.
- Vertical: playlists listed top to bottom. Horizontal: a tab strip left to right whose tabs
  size to their text, with a horizontal scrollbar and wheel scrolling when they overflow
- Item counts on each tab (toggle in the right-click menu)
- Click to switch, Up/Down/Left/Right/Home/End to move between playlists
- Drag a tab to reorder (drop line shown, auto-scrolls near the edges)
- Sort playlists: name A-Z (natural order, so "Mix 2" < "Mix 10"), name Z-A, size largest/smallest first
- Rename: double-click or F2 (inline editor, Enter = apply, Esc = cancel)
- New: Ctrl+N, or double-click empty space. Delete: Del or middle-click (locked playlists are skipped)
- The playing playlist is shown in bold and in the highlight colour
- Smooth wheel scrolling and a normal scrollbar
- Uses the host's colours and tab font, so it follows Default UI dark mode

Add it in Default UI via: right-click > Layout editing mode > replace an element >
**Utility > Playlist Tabs 64**.

## Not included

- Dropping tracks from a playlist onto a tab (needs OLE drag and drop)
- Tab icons, multi-select of tabs

## Building

Status: the source type-checks cleanly against the real foobar2000 SDK headers, but
it has NOT been compiled with MSVC or loaded into foobar2000 yet. Expect to fix
small things on the first real build.

### Option A: GitHub Actions (no local setup)

1. Create a GitHub repo and push this folder to it.
2. Open the Actions tab and run the `build` workflow (it also runs on every push).
3. Download the `foo_vtabs64` artifact. It holds `foo_vtabs64.fb2k-component`
   (x86 and x64 inside) plus the raw DLLs.
4. In foobar2000: Preferences > Components > Install... > pick the `.fb2k-component`.

### Option B: local build

1. Install Visual Studio 2022 with the "Desktop development with C++" workload.
2. Get the SDK sources into `deps\`:
   ```
   git clone --depth 1 https://github.com/reupen/foobar2000-sdk-modified.git deps\foobar2000
   git clone --depth 1 https://github.com/reupen/pfc-modified.git deps\pfc
   ```
   Or point at the official SDK from foobar2000.org: `build.cmd C:\path\to\sdk\`
   (the folder that contains `foobar2000\` and `pfc\`).
3. Open "x64 Native Tools Command Prompt for VS 2022" in this folder and run `build.cmd`.
4. Output: `build\out\x64\foo_vtabs64.dll`. Easiest is to zip it into a `.fb2k-component` (DLL inside an `x64\` folder) and install it from Preferences > Components. Manual placement for 64-bit foobar2000 v2 should be
   `%APPDATA%\foobar2000-v2\user-components-x64\foo_vtabs64\foo_vtabs64.dll` (check your profile folder if yours differs).

If your Visual Studio is newer and `v143` is missing, change `PlatformToolset`
in `build.cmd` and in the workflow.

## Layout of the source

Everything is in `foo_vtabs64.cpp` (~800 lines, plain Win32/GDI): settings,
the `ui_element_instance` window, playlist/playback callbacks, and registration.
