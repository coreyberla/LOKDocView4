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
  `GtkGestureDrag` mouse, `GtkEventControllerKey` keys, LOK callbacks marshalled
  via `g_idle_add` with a `GWeakRef`, simple signal-forwarding callbacks, whole public API.
- Tile rendering off the main thread:
  - `snapshot` never paints; it draws cached textures and queues missing/stale tiles
    on a single-worker `GThreadPool` (`paintTileInThread` → `paintTile` → `GdkMemoryTexture`),
    results come back via `g_idle_add` (`paintTileDone`).
  - Invalidated tiles keep showing their old texture until the repaint lands (no flicker);
    a per-tile serial discards results painted before the latest invalidation.
  - Zoom/scale changes bump an atomic generation: queued requests for the old one are
    skipped, old tiles are drawn scaled as a placeholder until the view is complete.
  - Order (`requestTiles()`): the paint queue is sorted by priority — visible tiles by distance
    from the viewport center, then a one-tile prefetch ring. Snapshot publishes that "wanted"
    area on the `ViewHandle`; the worker skips queued requests that left it (scrolled away), and
    such tiles become requestable again.
  - The cache is bounded to ~3× the wanted area (min 64 tiles), evicting the tiles farthest from
    the viewport center (not those with a paint in flight).
  - Part-aware invalidation (Calc/Impress/Draw; Writer has no parts here): invalidations for
    other parts are skipped. A `LOK_CALLBACK_SET_PART` that changes the part resets the tiles,
    since the new part's invalidation may have arrived first and been skipped.
- `src/tilebuffer.*` — texture cache with valid/pending/serial state per tile.
- Overlays (`appendOverlays()`): own text cursor (blinks per `gtk-cursor-blink*` settings, only
  while focused and in edit mode), text selection, graphic selection frame + 8 handles, Calc cell
  cursor. `cursor-changed` reports twips like the GTK3 widget (its docs say pixels).
- Mouse pointer shape: hover posts button-less mouse moves (`onMotion()`/`flushHover()`, try-lock so
  the UI never waits behind a tile paint; latest position retried), LOK answers with
  `LOK_CALLBACK_MOUSE_POINTER` CSS names → `gtk_widget_set_cursor_from_name()`. Also in view-only
  mode, unlike the GTK3 widget.
- Input method: `GtkIMMulticontext` on the key controller, attached only while editing and focused.
  Single-character commits outside a composition go through `postKeyEvent` (same as typing, keeps
  autocorrect); preedit and other commits through `postWindowExtTextInputEvent` (window 0 =
  document; `LOK_EXT_TEXTINPUT` sets the whole composition, `_END` finishes it).
  `delete-surrounding` → `removeTextContext`. Candidate window follows the cursor
  (`set_cursor_location` from snapshot). KEYUP only for keys whose press was posted, matched by
  hardware keycode.
- Fractional scaling: tiles are rendered at `round(256 × gdk_surface_get_scale())` device pixels
  (followed via the surface's `notify::scale` from realize to unrealize; `notify::scale-factor`
  still covers X11), and the document origin is snapped to device pixels in surface coordinates,
  since the widget itself can sit at a fractional device position. Exact for the usual scales
  (1.25, 1.5, 1.75, 2); others (e.g. 4/3) scale tiles very slightly.
- System clipboard (`GdkClipboard`); the GTK3 widget only had LOK's internal per-view clipboard.
  - Copy/cut: shortcuts and `post_command` run `.uno:Copy`/`.uno:Cut` with bNotifyWhenFinished;
    on its `LOK_CALLBACK_UNO_COMMAND_RESULT` the LOK clipboard is fetched eagerly via
    `getClipboard()` as LibreOffice's native embed-source XML, HTML, RTF (+`application/rtf`), PNG
    and text, and published as a `GdkContentProvider` union. Results of commands we only posted
    for ourselves aren't emitted as `command-result`. Copy also works in view-only mode.
  - `LOK_CALLBACK_CLIPBOARD_CHANGED` (empty payload, to all views) is handled too, by the view that
    last had input (`g_pLastActiveHandle`; each view has its own LOK clipboard), but observed not
    to fire for copies in practice (LibreOffice 26.8, Calc): the listener sits on the LOK clipboard
    of whatever view was current when the controller was created, presumably not the one copies
    write to. Hence the command-result route.
  - Paste (Ctrl+V, Shift+Insert, Paste key, `.uno:Paste`/`.uno:PasteUnformattedText` via
    `post_command`): our own content → `.uno:Paste` from LOK's clipboard; otherwise the richest
    available format, read async (bytes, or GDK conversions to texture → PNG / text), → `paste()`.
  - `paste()` itself triggers `CLIPBOARD_CHANGED`; it's recognised by matching the pasted bytes
    and not echoed back to the system clipboard. It also replaces LOK's clipboard, so afterwards
    our own exported content is pasted from the system clipboard, not via `.uno:Paste`.
  - Primary selection: selected text is published (debounced 250ms, `getTextSelection`) to
    `gtk_widget_get_primary_clipboard()`; deselecting leaves it. Middle click isn't passed to LOK
    (LOK's primary selection is the same per-view LOK clipboard as Ctrl+C): in edit mode it
    left-clicks to place the cursor, then pastes the primary's text. LOK handles mouse events
    asynchronously, so cursor-move-before-paste is likely, not guaranteed.
- Zoom keeps a document point fixed (`setZoom()`): the view center for `lok_doc_view_set_zoom()`,
  the pointer for Ctrl+scroll, the gesture center for pinch (`GtkGestureZoom`). The scroll
  position is applied in `size_allocate`, once the adjustments know the new size; zoom steps
  before that continue from the pending target. `setClientZoom` is coalesced into a low-priority
  idle (pinch would otherwise wait on the LOK mutex behind tile paints per step).
- `demo/main.c` — plain-C test viewer: zoom, edit toggle, Writer boundaries, bold/italic/underline
  (`post_command` + state via `command-changed`), sheet/slide selector (`get_parts`/`get_part_name`/
  `set_part`/`part-changed`), find bar (Ctrl+F; `find_next`/`find_prev`/`highlight_all`,
  `search-not-found`, `search-result-count`), Calc cell address + formula (`address-changed`,
  `formula-changed`). Covers the commonly used parts of gtktiledviewer.
- Upstream's vcl/awt/boost deps replaced: local `MOUSE_*`/`KEY_*`/`AwtKey` constants, json-glib.

Deliberate deviations from GTK3 widget:
- `edit-changed` emits the new value (upstream emitted the old one, contrary to its docs).
- Constructors no longer set halign/valign=center; centering is done inside the widget.
- `lok_doc_view_post_command()` lets display-only commands (`isViewOnlyCommand()`: `.uno:ShowBoundaries`
  and its deprecated per-type predecessors) through in view-only mode; upstream dropped all commands.

Lifetime (GTK4 has no `destroy` that tears down every child regardless of refs):
- LibreOffice must be `destroy()`ed before the process exits, or its static destructors crash/hang
  at exit. Dispose does that, so the view must actually be disposed when its window closes.
- Async work (LOK callbacks, tile paints) therefore holds a never-freed `ViewHandle` (weak ref +
  tile generation), never a strong ref on the view.
- The one exception is the load task (`documentLoad()` can't be interrupted); while it runs the
  default `GApplication` is held, so the app can't exit mid-load.
- Apps must not keep their own ref on the view past window close; the demo's signal handlers use
  `g_signal_connect_object()` against the toplevel since the view can outlive it during a load.
- Even after a clean `destroy()`, LibreOffice 26.8 crashes in its static destructors at exit:
  a Writer static asks for the system clipboard → `LOKClipboardFactory::getClipboardForCurView()`
  (`desktop/source/lib/lokclipboard.cxx`) dereferences `gClipboards.get()`, which is null after
  `DeleteOnDeinit` teardown (`releaseClipboardForView()` checks it, this doesn't). Upstream bug.
  Workaround (as Collabora Online's kit does): apps `fflush(NULL); _exit(status);` after their main
  loop instead of returning from `main`. The demo does this.

Rendering notes (LibreOffice behaviour, not ours):
- Writer's `initializeForRendering` (`sw/source/uibase/uno/unotxdoc.cxx`) force-disables field
  shadings in tiled mode, so form-field grey boxes never show — same in GTK3 widget / Collabora Online.
- Other view options (table/text boundaries, …) come from the LibreOffice user profile in use.
  The demo uses its own profile (`$XDG_CONFIG_HOME/lokdocview4`) and sets `.uno:ShowBoundaries` explicitly.
- Tiles paint on a worker; all other LOK calls (keys, mouse, commands) run on the main
  thread under `g_aLOKMutex`, so they can wait for at most one in-flight tile paint.

## Next steps
1. Overlays still missing: other views' cursors/selections, content controls, touch selection
   handles (upstream PNGs), dragging graphic handles via `setGraphicSelection()`.
2. `setClientVisibleArea`; IM `retrieve-surrounding` (needs text around the cursor from LOK).
3. Impress invalidation mode (normal/notes/master) isn't checked yet, only the part.
4. Comments, multi-view (`new_from_widget`, plus a "new view" button in the demo), dialogs.
5. Clipboard leftover: lazy export (only convert the formats another app actually requests).

## TODO upstream (LibreOffice)
- Exit-time crash: null-check `gClipboards.get()` in `LOKClipboardFactory::getClipboardForCurView()`
  and in `releaseClipboardForView()`'s clear-all branch (`desktop/source/lib/lokclipboard.cxx`).
  Submit patch + bug report; once released, drop the demo's `_exit()` workaround and return from `main`.
