# LOKDocView → GTK4 port: prep notes

## Layout
- `reference/lo-core/` — sparse checkout of LibreOffice core (read-only reference, upstream commit 91f5f11c, 2026-10-15)
  - `libreofficekit/source/gtk/lokdocview.cxx` (~4050 lines) — the GTK3 widget
  - `libreofficekit/source/gtk/tilebuffer.{cxx,hxx}` — tile cache (cairo surfaces)
  - `include/LibreOfficeKit/LibreOfficeKitGtk.h` — public widget API (32 functions)
  - `libreofficekit/qa/gtktiledviewer/` — GTK3 demo app (good test-harness template)
- `include/LibreOfficeKit/` — vendored LOK headers (header-only; `LibreOfficeKitInit.h` dlopens `libmergedlo.so` / `libsofficeapp.so` at runtime via `lok_init_2(path, profileURL)`)

All upstream files are MPL-2.0; ported files must stay MPL-2.0.

## How the GTK3 widget works
- Subclasses `GtkDrawingArea`; overrides `draw`, `button_press/release_event`, `motion_notify_event`, `key_press/release_event`, `destroy`.
- LOK calls run on a `GThreadPool` (single worker) wrapped in `GTask`s (~16 task types: paint tile, post key/mouse, set part, set zoom, …).
- LOK callbacks arrive on LO's thread → marshalled to main loop with `gdk_threads_add_idle` → parse JSON payload → update state (cursor, selections, invalidations) → emit GObject signals / `queue_draw`.
- Tiles: 256px tiles rendered by `pDocument->pClass->paintTile` into cairo image surfaces, cached in `TileBuffer`, painted in `draw`.
- Overlays (cursor, selection rects, handles, other views' cursors) drawn with plain cairo.

## GTK3 → GTK4 mapping (the actual work)
| GTK3 | GTK4 |
|---|---|
| `draw` vfunc + `cairo_t` | `snapshot` vfunc; tiles → `GdkMemoryTexture` + `gtk_snapshot_append_texture`; overlays via `gtk_snapshot_append_color` or `gtk_snapshot_append_cairo` |
| `button_*_event`, `motion_notify_event` | `GtkGestureClick`, `GtkGestureDrag`, `GtkEventControllerMotion` |
| `key_*_event`, `GdkEventKey` | `GtkEventControllerKey` + `GtkIMMulticontext` (proper text input) |
| `gtk_widget_add_events` | gone |
| `gdk_window_set_cursor` | `gtk_widget_set_cursor` |
| `gdk_window_get_visible_region` / clip rect | implement `GtkScrollable` (h/v `GtkAdjustment`) so the widget knows its viewport |
| `gtk_widget_get_toplevel` | `gtk_widget_get_root` |
| `gtk_widget_destroy` | `dispose` / `g_object_unref` |
| `gdk_threads_add_idle` | `g_idle_add` / `g_main_context_invoke` |
| `GtkClipboard` (copy/paste) | `GdkClipboard` (async, `GdkContentProvider`) |
| `GdkPixbuf` | `GdkTexture` |

Decision to consider: GTK4 discourages subclassing `GtkDrawingArea`; subclass `GtkWidget` directly and implement `GtkScrollable` so it drops into a `GtkScrolledWindow`.

## Runtime requirement
LibreOffice on this machine is **Flatpak-only** (26.8.1) — unsuitable for dlopen from a host binary.
Install native: `sudo pacman -S libreoffice-fresh` → program dir `/usr/lib/libreoffice/program`.
The stock GTK3 `liblibreofficekitgtk.so` in that package is a useful behaviour reference.

## Build
```
meson setup build        # packagers: --prefix=/usr, and -Dlo_path=... if LO isn't in <prefix>/<libdir>/libreoffice/program
ninja -C build
./build/lokdocview4-demo some.odt
```
Deps: gtk4 ≥ 4.12, json-glib-1.0, meson, a C++20 compiler; native LibreOffice at runtime.

### Finding LibreOffice
Build time never probes the host. `lo_path` (default `<prefix>/<libdir>/libreoffice/program`,
mirroring LO's own `$(LIBDIR)/libreoffice/program`) lands in generated `config.h` as `LOK_PATH`.

At runtime, when the caller passes a NULL path, `getDefaultLOPath()` tries, first hit wins:
1. `$LOK_PATH` env var
2. build-time `LOK_PATH`, if it contains `libmergedlo.so`/`libsofficeapp.so`
3. `soffice` / `libreoffice` on `PATH`, `realpath`'d (same trick as LO's launcher script)
4. `/usr/lib64/libreoffice/program`, `/usr/lib/libreoffice/program`
5. newest `/opt/libreoffice*/program`

Flatpak LibreOffice is never matched (its export is a wrapper script, not a program dir).

## Status (scaffold)
Done:
- `include/LibreOfficeKit/LibreOfficeKitGtk.h` — same function API; structs now derive from `GtkWidget`.
- `src/lokdocview.cxx` — all upstream properties + signals, `GInitable`, `GtkScrollable`
  (centers doc when smaller than viewport), async load (`g_task_run_in_thread`),
  tiles via `paintTile` → `GdkMemoryTexture` → `gtk_snapshot_append_texture`,
  `GtkGestureDrag` mouse, `GtkEventControllerKey` keys, LOK callbacks marshalled
  via `g_idle_add` with a `GWeakRef`, simple signal-forwarding callbacks, whole public API.
- `src/tilebuffer.*` — texture cache with rect invalidation.
- `demo/main.c` — plain-C viewer (zoom, edit toggle, progress, status).
- Upstream's vcl/awt/boost deps replaced: local `MOUSE_*`/`KEY_*`/`AwtKey` constants, json-glib.

Deliberate deviations from GTK3 widget:
- `edit-changed` emits the new value (upstream emitted the old one, contrary to its docs).
- Constructors no longer set halign/valign=center; centering is done inside the widget.
- LOK calls are synchronous on the main thread under `g_aLOKMutex` (except load).

## Next steps
1. Build & smoke test in toolbox; fix compile errors.
2. Async tile rendering on a single-worker `GThreadPool` (upstream `LOK_PAINT_TILE`), placeholder while pending.
3. Overlays: visible cursor (+blink), text selection, handles, graphic selection, cell cursor, other views.
4. IM context (`GtkIMMulticontext`), mouse pointer shape (`LOK_CALLBACK_MOUSE_POINTER`), `setClientVisibleArea`.
5. Zoom anchoring, fractional scaling, part-aware invalidation.
6. Clipboard (`GdkClipboard`), comments, multi-view (`new_from_widget`), dialogs; port gtktiledviewer as test app.
