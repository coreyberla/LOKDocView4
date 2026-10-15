/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * GTK4 port of libreofficekit/source/gtk/lokdocview.cxx.
 *
 * Status: scaffold. Implemented: GObject properties/signals, GInitable,
 * GtkScrollable, async document load, tiled rendering via GtkSnapshot,
 * basic mouse/keyboard input, LOK callback dispatch for the simple
 * signal-forwarding callbacks, and the public C API.
 *
 * Not yet ported (see PORTING.md): async tile rendering on a worker thread,
 * cursor/selection/handle overlays, graphic selection, other views'
 * cursors, IM context, clipboard, cell cursor, comments, dialogs.
 */

#include "config.h"

#define LOK_USE_UNSTABLE_API
#define LOK_DOC_VIEW_IMPLEMENTATION

#include <LibreOfficeKit/LibreOfficeKit.h>
#include <LibreOfficeKit/LibreOfficeKitInit.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>
#include <LibreOfficeKit/LibreOfficeKitGtk.h>

#include <json-glib/json-glib.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tilebuffer.hxx"

// Maximum Zoom allowed
#define MAX_ZOOM 5.0f
// Minimum Zoom allowed
#define MIN_ZOOM 0.25f

namespace
{
// VCL constants (vcl/event.hxx, vcl/keycodes.hxx). Not public API, but this
// is what LOK's postMouseEvent()/postKeyEvent() expect.
constexpr int MOUSE_LEFT = 0x0001;
constexpr int MOUSE_MIDDLE = 0x0002;
constexpr int MOUSE_RIGHT = 0x0004;
constexpr int KEY_SHIFT = 0x1000;
constexpr int KEY_MOD1 = 0x2000;
constexpr int KEY_MOD2 = 0x4000;

// css::awt::Key constants (offapi/com/sun/star/awt/Key.idl).
namespace AwtKey
{
constexpr int NUM0 = 256;
constexpr int A = 512;
constexpr int F1 = 768;
constexpr int DOWN = 1024;
constexpr int UP = 1025;
constexpr int LEFT = 1026;
constexpr int RIGHT = 1027;
constexpr int HOME = 1028;
constexpr int END = 1029;
constexpr int PAGEUP = 1030;
constexpr int PAGEDOWN = 1031;
constexpr int RETURN = 1280;
constexpr int ESCAPE = 1281;
constexpr int TAB = 1282;
constexpr int BACKSPACE = 1283;
constexpr int INSERT = 1285;
constexpr int DELETE = 1286;
}

/// This is expected to be locked during setView(), doSomethingElse() LOK calls.
std::mutex g_aLOKMutex;

/**
 * What asynchronous work (LOK callbacks, tile paints, their main-loop
 * continuations) holds on to instead of the view itself.
 *
 * Holding a strong ref there would keep the widget alive after its window is
 * closed, so dispose (which shuts LOK down) wouldn't run before the app exits,
 * and LibreOffice's static destructors then crash or hang at exit. Instead,
 * pending work re-acquires the view through the weak ref and drops out if
 * it's gone.
 *
 * Deliberately never freed: LOK may still be inside a callback on its own
 * thread while the view is disposed, so we only clear the weak reference and
 * late work becomes a no-op.
 */
struct ViewHandle
{
    GWeakRef m_aView;
    /// Bumped whenever the view's tile buffer is replaced (and on dispose).
    /// Read by the paint worker, so stale queued requests are skipped and
    /// stale results dropped.
    std::atomic<unsigned> m_nTileGeneration{ 0 };

    /// Tiles worth painting, [start, end) in tile units: the visible ones plus
    /// a prefetch margin, published by snapshot. Queued requests outside it
    /// (e.g. scrolled away meanwhile) are skipped by the paint worker.
    std::mutex m_aWantedMutex;
    int m_nWantedColStart = 0;
    int m_nWantedRowStart = 0;
    int m_nWantedColEnd = INT_MAX;
    int m_nWantedRowEnd = INT_MAX;
};

/**
 * Views with a document, main thread only. LOK doesn't reliably tell the
 * other views when a view is destroyed, but within this process every view
 * is a LOKDocView: a disposed view tells its siblings directly.
 */
std::vector<LOKDocView*> g_aLiveViews;

/// What another view of the same document shows us of itself (twips).
/// Each item carries the part it is on: only drawn on the current one
/// (except in Writer).
struct OtherView
{
    GdkRectangle m_aCursor = {};
    int m_nCursorPart = 0;
    bool m_bCursorVisible = true;
    std::vector<GdkRectangle> m_aSelection;
    int m_nSelectionPart = 0;
    GdkRectangle m_aCellCursor = {};
    int m_nCellCursorPart = 0;
    GdkRectangle m_aGraphicSelection = {};
    int m_nGraphicSelectionPart = 0;
};

/// A LOK callback marshalled to the main loop.
struct CallbackEvent
{
    ViewHandle* m_pHandle;
    int m_nType;
    std::string m_aPayload;
};
}

/// Private struct used by this GObject type
struct LOKDocViewPrivateImpl
{
    std::string m_aLOPath;
    bool m_bUnipoll = false;
    std::string m_aUserProfileURL;
    std::string m_aDocPath;
    std::string m_aRenderingArguments;
    LibreOfficeKit* m_pOffice = nullptr;
    LibreOfficeKitDocument* m_pDocument = nullptr;
    int m_nViewId = 0;
    int m_eTileMode = LOK_TILEMODE_BGRA;
    int m_eDocumentType = LOK_DOCTYPE_OTHER;

    long m_nDocumentWidthTwips = 0;
    long m_nDocumentHeightTwips = 0;
    float m_fZoom = 1.0f;
    /// Device pixels per logical pixel: the surface's scale, which may be
    /// fractional (e.g. 1.25), unlike gtk_widget_get_scale_factor().
    double m_fScale = 1.0;
    /// Surface whose notify::scale we follow while realized.
    GdkSurface* m_pScaleSurface = nullptr;
    gulong m_nScaleHandlerId = 0;

    /// Tiles for the current zoom level and scale factor.
    std::unique_ptr<TileBuffer> m_pTileBuffer = std::make_unique<TileBuffer>();
    /// Tiles of the previous zoom level, drawn scaled where current tiles are
    /// still missing; dropped once the visible area is fully rendered.
    std::unique_ptr<TileBuffer> m_pPreviousTiles;
    float m_fPreviousZoom = 1.0f;
    /// Single worker thread painting tiles (LOK can't paint in parallel).
    GThreadPool* m_pPaintPool = nullptr;

    bool m_bEdit = false;
    bool m_bCanZoomIn = true;
    bool m_bCanZoomOut = true;
    bool m_bIsLoading = false;
    bool m_bInitialized = false;
    bool m_bDisposed = false;
    double m_fLoadProgress = 0.0;
    bool m_bDocPassword = false;
    bool m_bDocPasswordToModify = false;
    bool m_bTiledAnnotations = true;
    unsigned long long m_nLOKFeatures = 0;

    int m_nParts = 0;
    int m_nPartId = 0;

    /// Visible document area in twips, as last computed in snapshot (or set
    /// via lok_doc_view_set_visible_area()), and as last sent to LOK.
    GdkRectangle m_aVisibleArea = {};
    GdkRectangle m_aSentVisibleArea = {};

    /// Cursors and selections of the document's other views, by view id.
    std::map<int, OtherView> m_aOtherViews;

    GtkAdjustment* m_pHAdjustment = nullptr;
    GtkAdjustment* m_pVAdjustment = nullptr;
    GtkScrollablePolicy m_eHScrollPolicy = GTK_SCROLL_MINIMUM;
    GtkScrollablePolicy m_eVScrollPolicy = GTK_SCROLL_MINIMUM;

    /// Buttons currently held down (VCL MOUSE_* bits) and click-count state.
    int m_nMouseButtons = 0;
    int m_nClickCount = 0;
    guint32 m_nLastPressTime = 0;
    guint m_nLastPressButton = 0;

    /// Zoom anchor waiting for the next size_allocate (see setZoom()): the
    /// widget point to keep fixed, and the document pixel (at the new zoom)
    /// that must end up under it.
    bool m_bZoomAnchorPending = false;
    double m_fAnchorX = 0;
    double m_fAnchorY = 0;
    double m_fAnchorDocX = 0;
    double m_fAnchorDocY = 0;
    /// Coalesced setClientZoom()/setClientVisibleArea(), see scheduleClientViewUpdate().
    guint m_nClientViewIdleId = 0;
    bool m_bClientZoomDirty = false;
    float m_fPinchStartZoom = 1.0f;
    /// Last pointer position over the widget, for Ctrl+scroll zoom.
    bool m_bPointerKnown = false;
    double m_fPointerX = 0;
    double m_fPointerY = 0;

    /// Latest hover position not yet sent to LOK (see onMotion()).
    bool m_bHoverPending = false;
    double m_fHoverX = 0;
    double m_fHoverY = 0;
    int m_nHoverModifier = 0;
    guint m_nHoverRetryId = 0;
    /// Last LOK_CALLBACK_MOUSE_POINTER shape, applied only while the pointer
    /// is over the document.
    std::string m_aPointerName = "default";
    bool m_bPointerInDocument = false;

    /// Input method (dead keys, compose, CJK, ...). Attached to the key
    /// controller only while editing and focused, see updateIMContext().
    GtkIMContext* m_pIMContext = nullptr;
    /// Owned by the widget.
    GtkEventControllerKey* m_pKeyController = nullptr;
    bool m_bIMActive = false;
    /// Whether LOK currently has a composition (LOK_EXT_TEXTINPUT) open.
    bool m_bPreediting = false;
    /// Last cursor location told to the IM (widget coordinates).
    GdkRectangle m_aIMCursorLocation = {};
    /// Keys whose press was posted to LOK, by hardware keycode, with the
    /// keyval/state of the press: only those get a KEYUP, and with the same
    /// keyval even if Shift was released first.
    std::map<guint, std::pair<guint, GdkModifierType>> m_aKeysDown;

    /// What we last put on the system clipboard (owned ref), to recognise
    /// our own content when pasting.
    GdkContentProvider* m_pExportedContent = nullptr;
    /// What we last paste()d into LOK: LOK reports that as a clipboard change
    /// too, which must not be echoed back to the system clipboard.
    std::string m_aLastPasteMimeType;
    GBytes* m_pLastPasteData = nullptr;
    /// Cancels pending system clipboard reads on dispose.
    GCancellable* m_pClipboardCancellable = nullptr;
    /// Commands we asked a result for only for our own use; their
    /// LOK_CALLBACK_UNO_COMMAND_RESULT isn't emitted as command-result.
    std::multiset<std::string> m_aInternalCommandResults;
    /// Debounce for publishing the primary selection.
    guint m_nPrimaryTimeoutId = 0;
    /// The current press is a middle click, handled as primary paste.
    bool m_bMiddleClick = false;

    // Overlay state, in document twips (LOK draws none of this into tiles).
    /// Own text cursor (LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR); width may be 0.
    GdkRectangle m_aVisibleCursor = {};
    /// LOK_CALLBACK_CURSOR_VISIBLE. Starts true: LOK doesn't necessarily send
    /// it on load, only when the cursor gets hidden/shown later.
    bool m_bCursorVisible = true;
    /// Blink phase; the cursor is drawn only while this is set.
    bool m_bCursorBlinkOn = true;
    guint m_nBlinkTimeoutId = 0;
    /// Owned by the widget.
    GtkEventControllerFocus* m_pFocusController = nullptr;
    gint64 m_nBlinkStartTime = 0;
    std::vector<GdkRectangle> m_aTextSelectionRectangles;
    /// Selected shape/image, empty if none.
    GdkRectangle m_aGraphicSelection = {};
    /// Calc cell cursor, empty if none.
    GdkRectangle m_aCellCursor = {};

    ViewHandle* m_pHandle = nullptr;
    /// Application held while a document loads, see lok_doc_view_open_document().
    GApplication* m_pHeldApplication = nullptr;
};

struct LOKDocViewPrivateStruct
{
    LOKDocViewPrivateImpl* m_pImpl;

    LOKDocViewPrivateImpl* operator->() { return m_pImpl; }
};

enum
{
    LOAD_CHANGED,
    EDIT_CHANGED,
    COMMAND_CHANGED,
    SEARCH_NOT_FOUND,
    PART_CHANGED,
    SIZE_CHANGED,
    HYPERLINK_CLICKED,
    CURSOR_CHANGED,
    SEARCH_RESULT_COUNT,
    COMMAND_RESULT,
    ADDRESS_CHANGED,
    FORMULA_CHANGED,
    TEXT_SELECTION,
    CONTENT_CONTROL,
    PASSWORD_REQUIRED,
    COMMENT,
    RULER,
    WINDOW,
    INVALIDATE_HEADER,

    LAST_SIGNAL
};

enum
{
    PROP_0,

    PROP_LO_PATH,
    PROP_LO_UNIPOLL,
    PROP_LO_POINTER,
    PROP_USER_PROFILE_URL,
    PROP_DOC_PATH,
    PROP_DOC_POINTER,
    PROP_EDITABLE,
    PROP_LOAD_PROGRESS,
    PROP_ZOOM,
    PROP_IS_LOADING,
    PROP_IS_INITIALIZED,
    PROP_DOC_WIDTH,
    PROP_DOC_HEIGHT,
    PROP_CAN_ZOOM_IN,
    PROP_CAN_ZOOM_OUT,
    PROP_DOC_PASSWORD,
    PROP_DOC_PASSWORD_TO_MODIFY,
    PROP_TILED_ANNOTATIONS,

    PROP_LAST,

    // GtkScrollable, overridden rather than installed.
    PROP_HADJUSTMENT = PROP_LAST,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY
};

static guint doc_view_signals[LAST_SIGNAL] = { 0 };
static GParamSpec *properties[PROP_LAST] = { nullptr };

static void lok_doc_view_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (LOKDocView, lok_doc_view, GTK_TYPE_WIDGET,
                         G_ADD_PRIVATE (LOKDocView)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, lok_doc_view_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, nullptr));

static LOKDocViewPrivate& getPrivate(LOKDocView* pDocView)
{
    LOKDocViewPrivate* priv = static_cast<LOKDocViewPrivate*>(lok_doc_view_get_instance_private(pDocView));
    return *priv;
}

/// Helper function to set the view of the document; must be called with g_aLOKMutex held.
static void setDocumentView(LibreOfficeKitDocument* pDoc, int viewId)
{
    if (pDoc->pClass->getView(pDoc) != viewId)
        pDoc->pClass->setView(pDoc, viewId);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/// Document size in logical (unscaled) pixels at the current zoom.
static double getDocumentWidthPixels(LOKDocViewPrivate& priv)
{
    return twipToPixel(priv->m_nDocumentWidthTwips, priv->m_fZoom);
}

static double getDocumentHeightPixels(LOKDocViewPrivate& priv)
{
    return twipToPixel(priv->m_nDocumentHeightTwips, priv->m_fZoom);
}

/// Integer tile size in twips; the tile grid LOK paints on and is told about via setClientZoom().
static int getTileSizeTwips(LOKDocViewPrivate& priv)
{
    return static_cast<int>(pixelToTwip(nTileSizePixels, priv->m_fZoom));
}

/**
 * Size of a tile in device pixels. A whole number of device pixels per
 * 256-pixel tile for the usual scales (1.25, 1.5, 1.75, 2, ...); for others
 * (e.g. 4/3) tiles get scaled very slightly when drawn.
 */
static int getTilePixels(LOKDocViewPrivate& priv)
{
    return static_cast<int>(std::max(1L, std::lround(nTileSizePixels * priv->m_fScale)));
}

/// Position of the widget's origin in its surface's coordinates.
static void getSurfaceOffset(GtkWidget* pWidget, double& rX, double& rY)
{
    rX = rY = 0;
    GtkNative* pNative = gtk_widget_get_native(pWidget);
    if (!pNative)
        return;
    graphene_point_t aOrigin = GRAPHENE_POINT_INIT(0, 0);
    graphene_point_t aPoint;
    if (!gtk_widget_compute_point(pWidget, GTK_WIDGET(pNative), &aOrigin, &aPoint))
        return;
    double fTransformX, fTransformY;
    gtk_native_get_surface_transform(pNative, &fTransformX, &fTransformY);
    rX = aPoint.x + fTransformX;
    rY = aPoint.y + fTransformY;
}

/**
 * Widget coordinates of the document's top-left corner: centered when the
 * document is narrower/shorter than the viewport, otherwise scrolled.
 * Snapped to device pixels so tiles stay crisp.
 */
static void getDocumentOrigin(LOKDocView* pDocView, double& rX, double& rY)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    int nWidth = gtk_widget_get_width(pWidget);
    int nHeight = gtk_widget_get_height(pWidget);
    double fDocWidth = getDocumentWidthPixels(priv);
    double fDocHeight = getDocumentHeightPixels(priv);

    rX = fDocWidth < nWidth ? (nWidth - fDocWidth) / 2 : -gtk_adjustment_get_value(priv->m_pHAdjustment);
    rY = fDocHeight < nHeight ? (nHeight - fDocHeight) / 2 : -gtk_adjustment_get_value(priv->m_pVAdjustment);

    // Snap in surface coordinates: at fractional scales the widget itself
    // may sit at a fractional device pixel position.
    double fScale = priv->m_fScale;
    double fOffsetX, fOffsetY;
    getSurfaceOffset(pWidget, fOffsetX, fOffsetY);
    rX = std::round((rX + fOffsetX) * fScale) / fScale - fOffsetX;
    rY = std::round((rY + fOffsetY) * fScale) / fScale - fOffsetY;
}

/// Converts widget coordinates to document twips.
static void widgetToTwip(LOKDocView* pDocView, double fX, double fY, int& rX, int& rY)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    double fOriginX, fOriginY;
    getDocumentOrigin(pDocView, fOriginX, fOriginY);
    rX = pixelToTwip(fX - fOriginX, priv->m_fZoom);
    rY = pixelToTwip(fY - fOriginY, priv->m_fZoom);
}

static void configureAdjustment(GtkAdjustment* pAdjustment, double fDocSize, int nViewSize)
{
    double fUpper = std::max(fDocSize, static_cast<double>(nViewSize));
    double fValue = std::clamp(gtk_adjustment_get_value(pAdjustment), 0.0, fUpper - nViewSize);
    gtk_adjustment_configure(pAdjustment, fValue, 0, fUpper,
                             nViewSize * 0.1, nViewSize * 0.9, nViewSize);
}

static void configureAdjustments(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    configureAdjustment(priv->m_pHAdjustment, getDocumentWidthPixels(priv), gtk_widget_get_width(pWidget));
    configureAdjustment(priv->m_pVAdjustment, getDocumentHeightPixels(priv), gtk_widget_get_height(pWidget));
}

static void onAdjustmentValueChanged(GtkAdjustment*, gpointer pData)
{
    // TODO: also tell LOK via setClientVisibleArea() so other views' cursors are reported.
    gtk_widget_queue_draw(GTK_WIDGET(pData));
}

static void setAdjustment(LOKDocView* pDocView, GtkAdjustment*& rpSlot, GtkAdjustment* pAdjustment)
{
    if (pAdjustment && rpSlot == pAdjustment)
        return;

    if (rpSlot)
    {
        g_signal_handlers_disconnect_by_func(rpSlot, reinterpret_cast<gpointer>(onAdjustmentValueChanged), pDocView);
        g_object_unref(rpSlot);
    }
    if (!pAdjustment)
        pAdjustment = gtk_adjustment_new(0, 0, 0, 0, 0, 0);

    rpSlot = GTK_ADJUSTMENT(g_object_ref_sink(pAdjustment));
    g_signal_connect(rpSlot, "value-changed", G_CALLBACK(onAdjustmentValueChanged), pDocView);
    gtk_widget_queue_allocate(GTK_WIDGET(pDocView));
}

// ---------------------------------------------------------------------------
// LOK state helpers (call on the main thread)
// ---------------------------------------------------------------------------

/// Tells LOK about our tile grid so its invalidations line up with it.
static void updateClientZoom(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    int nTilePixels = getTilePixels(priv);
    int nTileTwips = getTileSizeTwips(priv);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->setClientZoom(priv->m_pDocument, nTilePixels, nTilePixels, nTileTwips, nTileTwips);
}

static void refreshSize(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->getDocumentSize(priv->m_pDocument,
                                                   &priv->m_nDocumentWidthTwips,
                                                   &priv->m_nDocumentHeightTwips);
    }
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_DOC_WIDTH]);
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_DOC_HEIGHT]);
    gtk_widget_queue_resize(GTK_WIDGET(pDocView));
}

static void applyOptionalFeatures(LOKDocViewPrivate& priv)
{
    if (!priv->m_pOffice)
        return;

    unsigned long long nFeatures = LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK
                                   | LOK_FEATURE_VIEWID_IN_VISCURSOR_INVALIDATION_CALLBACK;
    if (priv->m_bDocPassword)
        nFeatures |= LOK_FEATURE_DOCUMENT_PASSWORD;
    if (priv->m_bDocPasswordToModify)
        nFeatures |= LOK_FEATURE_DOCUMENT_PASSWORD_TO_MODIFY;
    if (!priv->m_bTiledAnnotations)
        nFeatures |= LOK_FEATURE_NO_TILED_ANNOTATIONS;

    if (nFeatures == priv->m_nLOKFeatures)
        return;
    priv->m_nLOKFeatures = nFeatures;
    priv->m_pOffice->pClass->setOptionalFeatures(priv->m_pOffice, nFeatures);
}

static void setLoadProgress(LOKDocView* pDocView, double fProgress)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_fLoadProgress = fProgress;
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_LOAD_PROGRESS]);
    g_signal_emit(pDocView, doc_view_signals[LOAD_CHANGED], 0, fProgress);
}

static void setIsLoading(LOKDocView* pDocView, bool bIsLoading)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (priv->m_bIsLoading == bIsLoading)
        return;
    priv->m_bIsLoading = bIsLoading;
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_IS_LOADING]);
}

// ---------------------------------------------------------------------------
// System clipboard
// ---------------------------------------------------------------------------

namespace
{
/// LibreOffice's native clipboard format, as desktop LibreOffice offers it.
constexpr char aEmbedSourceMimeType[] = "application/x-openoffice-embed-source-xml;windows_formatname=\"Star Embed Source (XML)\"";

/// Formats published to the system clipboard after a copy in LOK.
const char* aExportMimeTypes[] = { aEmbedSourceMimeType, "text/html", "text/rtf", "image/png",
                                   "text/plain;charset=utf-8", nullptr };

/// Formats taken from the system clipboard on paste, best first. Images in
/// other formats and plain text are handled through GDK's conversions.
const char* const aPasteMimeTypes[] = { aEmbedSourceMimeType, "text/html", "text/rtf", "application/rtf", "image/png" };

/**
 * View that last had input. LOK notifies every view about a clipboard
 * change, but each view has its own LOK clipboard: only the view the copy
 * happened in may publish it, others would publish stale content.
 */
ViewHandle* g_pLastActiveHandle = nullptr;

/// A pending system clipboard read for a paste.
struct ClipboardRead
{
    ViewHandle* m_pHandle;
    /// Mime type to hand to LOK.
    std::string m_aMimeType;
    GCancellable* m_pCancellable; // ref
    GOutputStream* m_pBuffer = nullptr;

    ~ClipboardRead()
    {
        g_clear_object(&m_pBuffer);
        g_clear_object(&m_pCancellable);
    }
};
}

static void setActiveView(LOKDocView* pDocView)
{
    g_pLastActiveHandle = getPrivate(pDocView)->m_pHandle;
}

/// LOK_CALLBACK_CLIPBOARD_CHANGED: publish this view's LOK clipboard to the
/// system clipboard. The data is fetched right away, so it stays pasteable
/// elsewhere after the document is closed.
static void exportClipboard(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument || (g_pLastActiveHandle && g_pLastActiveHandle != priv->m_pHandle))
        return;

    size_t nCount = 0;
    char** ppMimeTypes = nullptr;
    size_t* pSizes = nullptr;
    char** ppStreams = nullptr;
    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        if (!priv->m_pDocument->pClass->getClipboard(priv->m_pDocument, aExportMimeTypes, &nCount, &ppMimeTypes,
                                                     &pSizes, &ppStreams))
            return;
    }

    std::vector<GdkContentProvider*> aProviders;
    bool bEcho = false;
    for (size_t i = 0; i < nCount; ++i)
    {
        if (pSizes[i] && ppStreams[i])
        {
            std::string_view aMimeType(ppMimeTypes[i]);
            g_autoptr(GBytes) pBytes = g_bytes_new(ppStreams[i], pSizes[i]);
            if (priv->m_pLastPasteData && aMimeType == priv->m_aLastPasteMimeType
                && g_bytes_equal(pBytes, priv->m_pLastPasteData))
                bEcho = true;

            if (aMimeType == "text/plain;charset=utf-8")
            {
                // As a string, so GDK offers all the usual text targets.
                std::string aText(ppStreams[i], pSizes[i]);
                aProviders.push_back(gdk_content_provider_new_typed(G_TYPE_STRING, aText.c_str()));
            }
            else
            {
                aProviders.push_back(gdk_content_provider_new_for_bytes(ppMimeTypes[i], pBytes));
                if (aMimeType == "text/rtf")
                    aProviders.push_back(gdk_content_provider_new_for_bytes("application/rtf", pBytes));
            }
        }
        free(ppMimeTypes[i]);
        free(ppStreams[i]);
    }
    free(ppMimeTypes);
    free(pSizes);
    free(ppStreams);

    // Our own paste() changed LOK's clipboard: nothing new to publish.
    if (bEcho || aProviders.empty())
    {
        for (GdkContentProvider* pProvider : aProviders)
            g_object_unref(pProvider);
        return;
    }

    g_clear_object(&priv->m_pExportedContent);
    priv->m_pExportedContent = gdk_content_provider_new_union(aProviders.data(), aProviders.size());
    gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(pDocView)), priv->m_pExportedContent);
    g_clear_pointer(&priv->m_pLastPasteData, g_bytes_unref);
}

/// Pastes data via LOK (setClipboard + .uno:Paste), remembering it so the
/// resulting clipboard change isn't echoed back.
static bool pasteData(LOKDocView* pDocView, const char* pMimeType, const char* pData, gsize nSize)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument || !priv->m_bEdit || !pData)
        return false;

    priv->m_aLastPasteMimeType = pMimeType;
    g_clear_pointer(&priv->m_pLastPasteData, g_bytes_unref);
    priv->m_pLastPasteData = g_bytes_new(pData, nSize);
    // paste() replaces LOK's clipboard, so it no longer holds what we
    // exported: from now on paste our own content from the system clipboard
    // too (its native format keeps the fidelity), not LOK's.
    g_clear_object(&priv->m_pExportedContent);

    g_info("lok::Document::paste('%s', %" G_GSIZE_FORMAT " bytes)", pMimeType, nSize);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->paste(priv->m_pDocument, pMimeType, pData, nSize);
}

/// Runs pasteData() for a finished read, if the view is still there.
static void finishClipboardRead(ClipboardRead* pRead, const char* pData, gsize nSize)
{
    if (gpointer pView = g_weak_ref_get(&pRead->m_pHandle->m_aView))
    {
        pasteData(LOK_DOC_VIEW(pView), pRead->m_aMimeType.c_str(), pData, nSize);
        g_object_unref(pView);
    }
}

static void warnClipboardError(GError* pError)
{
    if (!g_error_matches(pError, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("Reading the clipboard failed: %s", pError->message);
    g_error_free(pError);
}

static void onPasteSpliced(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    std::unique_ptr<ClipboardRead> pRead(static_cast<ClipboardRead*>(pData));
    GError* pError = nullptr;
    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(pSource), pResult, &pError) < 0)
        return warnClipboardError(pError);

    g_autoptr(GBytes) pBytes = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(pRead->m_pBuffer));
    gsize nSize = 0;
    auto* pBuffer = static_cast<const char*>(g_bytes_get_data(pBytes, &nSize));
    finishClipboardRead(pRead.get(), pBuffer ? pBuffer : "", nSize);
}

static void onPasteStreamReady(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    std::unique_ptr<ClipboardRead> pRead(static_cast<ClipboardRead*>(pData));
    GError* pError = nullptr;
    g_autoptr(GInputStream) pStream = gdk_clipboard_read_finish(GDK_CLIPBOARD(pSource), pResult, nullptr, &pError);
    if (!pStream)
        return warnClipboardError(pError);

    pRead->m_pBuffer = g_memory_output_stream_new_resizable();
    GOutputStream* pBuffer = pRead->m_pBuffer;
    GCancellable* pCancellable = pRead->m_pCancellable;
    g_output_stream_splice_async(pBuffer, pStream,
                                 static_cast<GOutputStreamSpliceFlags>(G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE
                                                                       | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET),
                                 G_PRIORITY_DEFAULT, pCancellable, onPasteSpliced, pRead.release());
}

static void onPasteText(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    std::unique_ptr<ClipboardRead> pRead(static_cast<ClipboardRead*>(pData));
    GError* pError = nullptr;
    g_autofree char* pText = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(pSource), pResult, &pError);
    if (!pText)
        return pError ? warnClipboardError(pError) : void();
    finishClipboardRead(pRead.get(), pText, strlen(pText));
}

static void onPasteTexture(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    std::unique_ptr<ClipboardRead> pRead(static_cast<ClipboardRead*>(pData));
    GError* pError = nullptr;
    g_autoptr(GdkTexture) pTexture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(pSource), pResult, &pError);
    if (!pTexture)
        return pError ? warnClipboardError(pError) : void();
    g_autoptr(GBytes) pPng = gdk_texture_save_to_png_bytes(pTexture);
    gsize nSize = 0;
    auto* pBuffer = static_cast<const char*>(g_bytes_get_data(pPng, &nSize));
    finishClipboardRead(pRead.get(), pBuffer, nSize);
}

/**
 * Pastes the system clipboard: our own copy straight from LOK's clipboard
 * (all formats, full fidelity), otherwise the richest format available,
 * read asynchronously. bTextOnly: plain text only (unformatted paste).
 */
static void pasteFromClipboard(LOKDocView* pDocView, bool bTextOnly, GdkClipboard* pClipboard = nullptr)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument || !priv->m_bEdit)
        return;

    if (!pClipboard)
        pClipboard = gtk_widget_get_clipboard(GTK_WIDGET(pDocView));
    if (!bTextOnly && priv->m_pExportedContent && gdk_clipboard_get_content(pClipboard) == priv->m_pExportedContent)
    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->postUnoCommand(priv->m_pDocument, ".uno:Paste", nullptr, false);
        return;
    }

    auto* pRead = new ClipboardRead{ priv->m_pHandle, "", G_CANCELLABLE(g_object_ref(priv->m_pClipboardCancellable)) };
    GdkContentFormats* pFormats = gdk_clipboard_get_formats(pClipboard);
    if (!bTextOnly)
    {
        for (const char* pMimeType : aPasteMimeTypes)
        {
            if (!gdk_content_formats_contain_mime_type(pFormats, pMimeType))
                continue;
            // LibreOffice knows RTF as text/rtf.
            pRead->m_aMimeType = g_str_equal(pMimeType, "application/rtf") ? "text/rtf" : pMimeType;
            const char* aMimeTypes[] = { pMimeType, nullptr };
            gdk_clipboard_read_async(pClipboard, aMimeTypes, G_PRIORITY_DEFAULT, pRead->m_pCancellable,
                                     onPasteStreamReady, pRead);
            return;
        }
    }

    // Whatever GDK can convert: other image formats to a texture, text to UTF-8.
    g_autoptr(GdkContentFormats) pTypes = gdk_content_formats_union_deserialize_gtypes(gdk_content_formats_ref(pFormats));
    if (!bTextOnly && gdk_content_formats_contain_gtype(pTypes, GDK_TYPE_TEXTURE))
    {
        pRead->m_aMimeType = "image/png";
        gdk_clipboard_read_texture_async(pClipboard, pRead->m_pCancellable, onPasteTexture, pRead);
    }
    else if (gdk_content_formats_contain_gtype(pTypes, G_TYPE_STRING))
    {
        pRead->m_aMimeType = "text/plain;charset=utf-8";
        gdk_clipboard_read_text_async(pClipboard, pRead->m_pCancellable, onPasteText, pRead);
    }
    else
        delete pRead; // nothing we can paste
}

/**
 * Posts .uno:Copy / .uno:Cut with a result notification: the system
 * clipboard is filled when LOK reports the command done (see
 * handleCallback()). LOK_CALLBACK_CLIPBOARD_CHANGED alone isn't reliable for
 * this: the LOK clipboard the listener sits on isn't necessarily the one a
 * copy writes to. bInternal: the caller didn't ask for command-result.
 */
static void postClipboardCommand(LOKDocView* pDocView, const char* pCommand, bool bInternal)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;
    if (bInternal)
        priv->m_aInternalCommandResults.insert(pCommand);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postUnoCommand(priv->m_pDocument, pCommand, nullptr, true);
}

/// Puts the selected text on the primary selection (middle-click paste).
static gboolean publishPrimarySelection(gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_nPrimaryTimeoutId = 0;
    if (!priv->m_pDocument || priv->m_aTextSelectionRectangles.empty())
        return G_SOURCE_REMOVE;

    char* pUsedMimeType = nullptr;
    char* pText;
    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        pText = priv->m_pDocument->pClass->getTextSelection(priv->m_pDocument, "text/plain;charset=utf-8", &pUsedMimeType);
    }
    if (pText && *pText)
        gdk_clipboard_set_text(gtk_widget_get_primary_clipboard(GTK_WIDGET(pDocView)), pText);
    free(pText);
    free(pUsedMimeType);
    return G_SOURCE_REMOVE;
}

/**
 * Publishes the primary selection once the selection settles: while
 * drag-selecting it changes many times a second, and fetching the text each
 * time would be a LOK call per change.
 */
static void schedulePrimarySelection(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    g_clear_handle_id(&priv->m_nPrimaryTimeoutId, g_source_remove);
    priv->m_nPrimaryTimeoutId = g_timeout_add(250, publishPrimarySelection, pDocView);
}

/// Clipboard shortcuts handled here rather than by LOK; true if consumed.
static bool handleClipboardShortcut(LOKDocView* pDocView, guint nKeyval, GdkModifierType eState)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return false;

    GdkModifierType eMods = static_cast<GdkModifierType>(eState & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK));
    bool bPaste = ((nKeyval == GDK_KEY_v || nKeyval == GDK_KEY_V) && eMods == GDK_CONTROL_MASK)
                  || (nKeyval == GDK_KEY_Insert && eMods == GDK_SHIFT_MASK) || nKeyval == GDK_KEY_Paste;
    bool bCopy = ((nKeyval == GDK_KEY_c || nKeyval == GDK_KEY_C) && eMods == GDK_CONTROL_MASK)
                 || (nKeyval == GDK_KEY_Insert && eMods == GDK_CONTROL_MASK) || nKeyval == GDK_KEY_Copy;
    bool bCut = ((nKeyval == GDK_KEY_x || nKeyval == GDK_KEY_X) && eMods == GDK_CONTROL_MASK)
                || (nKeyval == GDK_KEY_Delete && eMods == GDK_SHIFT_MASK) || nKeyval == GDK_KEY_Cut;

    if (bPaste && priv->m_bEdit)
    {
        pasteFromClipboard(pDocView, false);
        return true;
    }
    // Copy also in view-only mode, where keys aren't posted to LOK.
    if (bCopy)
    {
        postClipboardCommand(pDocView, ".uno:Copy", true);
        return true;
    }
    if (bCut && priv->m_bEdit)
    {
        postClipboardCommand(pDocView, ".uno:Cut", true);
        return true;
    }
    return false;
}

/// LOK_CALLBACK_UNO_COMMAND_RESULT: {"commandName": ..., "success": ...}.
static void handleCommandResult(LOKDocView* pDocView, const std::string& rPayload)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    std::string aCommand;
    bool bFailed = false;

    g_autoptr(JsonParser) pParser = json_parser_new();
    if (json_parser_load_from_data(pParser, rPayload.c_str(), -1, nullptr)
        && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(pParser)))
    {
        JsonObject* pObject = json_node_get_object(json_parser_get_root(pParser));
        aCommand = json_object_get_string_member_with_default(pObject, "commandName", "");
        // "success" is missing when LOK doesn't know; only skip on a definite failure.
        bFailed = json_object_has_member(pObject, "success")
                  && !json_object_get_boolean_member(pObject, "success");
    }

    if ((aCommand == ".uno:Copy" || aCommand == ".uno:Cut") && !bFailed)
        exportClipboard(pDocView);

    auto it = priv->m_aInternalCommandResults.find(aCommand);
    if (it != priv->m_aInternalCommandResults.end())
        priv->m_aInternalCommandResults.erase(it);
    else
        g_signal_emit(pDocView, doc_view_signals[COMMAND_RESULT], 0, rPayload.c_str());
}

// ---------------------------------------------------------------------------
// LOK callbacks
// ---------------------------------------------------------------------------

/// Parses the leading "x, y, width, height" of a payload; LOK appends extra fields to some.
/// Parses a leading list of comma-separated integers; stops at anything else.
static std::vector<long> parseNumbers(const char* p)
{
    std::vector<long> aNumbers;
    for (;;)
    {
        while (*p == ',' || *p == ' ')
            ++p;
        char* pEnd = nullptr;
        long nValue = std::strtol(p, &pEnd, 10);
        if (pEnd == p)
            break;
        aNumbers.push_back(nValue);
        p = pEnd;
    }
    return aNumbers;
}

static GdkRectangle toRectangle(const std::vector<long>& rNumbers)
{
    auto toInt = [](long n) { return static_cast<int>(std::clamp<long>(n, INT_MIN, INT_MAX)); };
    return { toInt(rNumbers[0]), toInt(rNumbers[1]), toInt(rNumbers[2]), toInt(rNumbers[3]) };
}

/// Parses the leading "x, y, width, height" of a payload; LOK appends extra fields to some.
static bool parseRectangle(const char* p, GdkRectangle& rRect)
{
    std::vector<long> aNumbers = parseNumbers(p);
    if (aNumbers.size() < 4)
        return false;
    rRect = toRectangle(aNumbers);
    return true;
}

/**
 * Parses "x, y, width, height[, part[, mode]]" (twips) or
 * "EMPTY[, part[, mode]]". rnPart is -1 if the payload has none.
 */
static bool parseInvalidation(const std::string& rPayload, GdkRectangle& rRect, bool& rbAll, int& rnPart)
{
    rbAll = rPayload.compare(0, 5, "EMPTY") == 0;
    std::vector<long> aNumbers = parseNumbers(rPayload.c_str() + (rbAll ? 5 : 0));
    size_t nPartIndex = rbAll ? 0 : 4;
    if (!rbAll)
    {
        if (aNumbers.size() < 4)
            return false;
        rRect = toRectangle(aNumbers);
    }
    rnPart = aNumbers.size() > nPartIndex ? static_cast<int>(aNumbers[nPartIndex]) : -1;
    return true;
}

static bool isEmptyRectangle(const GdkRectangle& rRect)
{
    // A text cursor legitimately has width 0, so only "no height" counts as empty.
    return rRect.height <= 0;
}

/// Parses an overlay rectangle (twips), clamped to the document; empty for
/// "EMPTY" or anything unparsable.
static GdkRectangle payloadToRectangle(LOKDocViewPrivate& priv, const char* pPayload)
{
    GdkRectangle aRect = {};
    if (g_str_has_prefix(pPayload, "EMPTY") || !parseRectangle(pPayload, aRect))
        return {};

    aRect.x = std::max(aRect.x, 0);
    aRect.y = std::max(aRect.y, 0);
    aRect.width = static_cast<int>(std::min<long>(aRect.width, priv->m_nDocumentWidthTwips - aRect.x));
    aRect.height = static_cast<int>(std::min<long>(aRect.height, priv->m_nDocumentHeightTwips - aRect.y));
    return aRect;
}

/// Parses "rect; rect; ..." or "EMPTY".
static std::vector<GdkRectangle> payloadToRectangles(LOKDocViewPrivate& priv, const char* pPayload)
{
    std::vector<GdkRectangle> aRectangles;
    if (g_str_has_prefix(pPayload, "EMPTY"))
        return aRectangles;

    g_auto(GStrv) ppRectangles = g_strsplit(pPayload, "; ", 0);
    for (gchar** ppRectangle = ppRectangles; *ppRectangle; ++ppRectangle)
    {
        GdkRectangle aRect = payloadToRectangle(priv, *ppRectangle);
        if (!isEmptyRectangle(aRect))
            aRectangles.push_back(aRect);
    }
    return aRectangles;
}

/// Whether the own text cursor should be drawn at all (ignoring the blink phase).
static bool isCursorShown(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    // Ask the focus controller rather than gtk_widget_has_focus(): it is
    // already up to date when its enter/leave signals run.
    return priv->m_bEdit && priv->m_bCursorVisible && !isEmptyRectangle(priv->m_aVisibleCursor)
           && priv->m_pFocusController && gtk_event_controller_focus_is_focus(priv->m_pFocusController);
}

static gboolean onCursorBlink(gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    // Like GTK's text widgets: stop blinking (cursor shown) after
    // gtk-cursor-blink-timeout seconds without cursor movement.
    int nBlinkTimeoutSeconds = 10;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(pDocView)), "gtk-cursor-blink-timeout", &nBlinkTimeoutSeconds, nullptr);
    if (g_get_monotonic_time() - priv->m_nBlinkStartTime > gint64(nBlinkTimeoutSeconds) * G_USEC_PER_SEC)
    {
        priv->m_bCursorBlinkOn = true;
        priv->m_nBlinkTimeoutId = 0;
        gtk_widget_queue_draw(GTK_WIDGET(pDocView));
        return G_SOURCE_REMOVE;
    }

    priv->m_bCursorBlinkOn = !priv->m_bCursorBlinkOn;
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
    return G_SOURCE_CONTINUE;
}

/**
 * Shows the cursor solid and (re)starts blinking: call when the cursor moves,
 * focus or edit mode changes. Honours gtk-cursor-blink / -blink-time.
 */
static void restartCursorBlink(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_bCursorBlinkOn = true;
    g_clear_handle_id(&priv->m_nBlinkTimeoutId, g_source_remove);
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));

    if (!isCursorShown(pDocView))
        return;

    gboolean bBlink = TRUE;
    int nBlinkTime = 1200;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(pDocView)),
                 "gtk-cursor-blink", &bBlink, "gtk-cursor-blink-time", &nBlinkTime, nullptr);
    if (!bBlink || nBlinkTime <= 0)
        return;

    priv->m_nBlinkStartTime = g_get_monotonic_time();
    // gtk-cursor-blink-time is a full on/off cycle.
    priv->m_nBlinkTimeoutId = g_timeout_add(nBlinkTime / 2, onCursorBlink, pDocView);
}

/// LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR: JSON {"viewId", "rectangle", ...},
/// or a bare rectangle from older LibreOffice versions.
static void setVisibleCursor(LOKDocView* pDocView, const std::string& rPayload)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    std::string aRectangle = rPayload;

    if (!rPayload.empty() && rPayload[0] == '{')
    {
        g_autoptr(JsonParser) pParser = json_parser_new();
        if (!json_parser_load_from_data(pParser, rPayload.c_str(), -1, nullptr))
            return;
        JsonNode* pRoot = json_parser_get_root(pParser);
        if (!JSON_NODE_HOLDS_OBJECT(pRoot))
            return;
        JsonObject* pObject = json_node_get_object(pRoot);
        // viewId is a string in the payload; other views' cursors come via
        // LOK_CALLBACK_INVALIDATE_VIEW_CURSOR, so this should always be ours.
        if (const char* pViewId = json_object_get_string_member_with_default(pObject, "viewId", nullptr))
            if (std::atoi(pViewId) != priv->m_nViewId)
                return;
        aRectangle = json_object_get_string_member_with_default(pObject, "rectangle", "");
    }

    priv->m_aVisibleCursor = payloadToRectangle(priv, aRectangle.c_str());
    // Twips, like the GTK3 widget (its docs say pixels).
    g_signal_emit(pDocView, doc_view_signals[CURSOR_CHANGED], 0,
                  priv->m_aVisibleCursor.x, priv->m_aVisibleCursor.y,
                  priv->m_aVisibleCursor.width, priv->m_aVisibleCursor.height);
    restartCursorBlink(pDocView);
}

static void invalidateTiles(LOKDocView* pDocView, const std::string& rPayload)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GdkRectangle aTwips;
    bool bAll;
    int nPart;
    if (!parseInvalidation(rPayload, aTwips, bAll, nPart))
    {
        g_warning("invalidateTiles: unexpected payload '%s'", rPayload.c_str());
        return;
    }

    // With LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK, skip changes on other
    // sheets/slides: our tiles only show the current one. Writer has no
    // parts in this sense. (A negative part means all parts.)
    // TODO: Impress also sends a mode (normal/notes/master), not checked yet.
    if (nPart >= 0 && priv->m_eDocumentType != LOK_DOCTYPE_TEXT && nPart != priv->m_nPartId)
        return;

    if (bAll)
        priv->m_pTileBuffer->invalidateAll();
    else
    {
        // Twips -> pixels, rounded outwards; clamp to avoid overflow on INT_MAX extents.
        auto toPixels = [&priv](double fTwips) {
            return std::min<double>(INT_MAX / 2, twipToPixel(fTwips, priv->m_fZoom));
        };
        double fX = toPixels(aTwips.x), fY = toPixels(aTwips.y);
        GdkRectangle aPixels = { static_cast<int>(std::floor(fX)),
                                 static_cast<int>(std::floor(fY)),
                                 static_cast<int>(std::ceil(toPixels(aTwips.width))) + 2,
                                 static_cast<int>(std::ceil(toPixels(aTwips.height))) + 2 };
        priv->m_pTileBuffer->invalidate(aPixels);
    }
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

static int countSearchResults(const std::string& rPayload)
{
    g_autoptr(JsonParser) pParser = json_parser_new();
    if (!json_parser_load_from_data(pParser, rPayload.c_str(), -1, nullptr))
        return 0;
    JsonNode* pRoot = json_parser_get_root(pParser);
    if (!JSON_NODE_HOLDS_OBJECT(pRoot))
        return 0;
    JsonArray* pResults = json_object_get_array_member(json_node_get_object(pRoot), "searchResultSelection");
    return pResults ? json_array_get_length(pResults) : 0;
}

static void startNewTileGeneration(LOKDocView* pDocView, bool bKeepAsPlaceholder);

/// JSON member as int; LOK sends ids as strings ("viewId": "1") or numbers.
static int getIntMember(JsonObject* pObject, const char* pName, int nDefault)
{
    JsonNode* pNode = json_object_get_member(pObject, pName);
    if (!pNode || !JSON_NODE_HOLDS_VALUE(pNode))
        return nDefault;
    if (json_node_get_value_type(pNode) == G_TYPE_STRING)
        return std::atoi(json_node_get_string(pNode));
    return static_cast<int>(json_node_get_int(pNode));
}

/// JSON member as bool; LOK sends "true"/"false" strings or booleans.
static bool getBoolMember(JsonObject* pObject, const char* pName, bool bDefault)
{
    JsonNode* pNode = json_object_get_member(pObject, pName);
    if (!pNode || !JSON_NODE_HOLDS_VALUE(pNode))
        return bDefault;
    if (json_node_get_value_type(pNode) == G_TYPE_STRING)
        return g_strcmp0(json_node_get_string(pNode), "true") == 0;
    return json_node_get_boolean(pNode);
}

/**
 * Another view's cursor/selection callbacks: {"viewId": ..., "part": ...,
 * "rectangle"/"selection"/"visible": ...}. LOK doesn't reliably tell us when
 * a view goes away; see dropOtherView().
 */
static void handleOtherViewCallback(LOKDocView* pDocView, int nType, const std::string& rPayload)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    g_autoptr(JsonParser) pParser = json_parser_new();
    if (!json_parser_load_from_data(pParser, rPayload.c_str(), -1, nullptr)
        || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(pParser)))
        return;
    JsonObject* pObject = json_node_get_object(json_parser_get_root(pParser));
    int nViewId = getIntMember(pObject, "viewId", -1);
    if (nViewId < 0 || nViewId == priv->m_nViewId)
        return;
    // Ignore late callbacks about a view that is gone already (it was
    // dropped from m_aOtherViews on dispose and must not come back).
    bool bLive = std::any_of(g_aLiveViews.begin(), g_aLiveViews.end(), [&priv, nViewId](LOKDocView* pView) {
        LOKDocViewPrivate& rView = getPrivate(pView);
        return rView->m_pDocument == priv->m_pDocument && rView->m_nViewId == nViewId;
    });
    if (!bLive)
        return;

    OtherView& rView = priv->m_aOtherViews[nViewId];
    int nPart = getIntMember(pObject, "part", 0);
    const char* pRectangle = json_object_get_string_member_with_default(pObject, "rectangle", "EMPTY");
    const char* pSelection = json_object_get_string_member_with_default(pObject, "selection", "EMPTY");
    switch (nType)
    {
    case LOK_CALLBACK_INVALIDATE_VIEW_CURSOR:
        rView.m_aCursor = payloadToRectangle(priv, pRectangle);
        rView.m_nCursorPart = nPart;
        break;
    case LOK_CALLBACK_VIEW_CURSOR_VISIBLE:
        rView.m_bCursorVisible = getBoolMember(pObject, "visible", true);
        break;
    case LOK_CALLBACK_TEXT_VIEW_SELECTION:
        rView.m_aSelection = payloadToRectangles(priv, pSelection);
        rView.m_nSelectionPart = nPart;
        break;
    case LOK_CALLBACK_CELL_VIEW_CURSOR:
        rView.m_aCellCursor = payloadToRectangle(priv, pRectangle);
        rView.m_nCellCursorPart = nPart;
        break;
    case LOK_CALLBACK_GRAPHIC_VIEW_SELECTION:
        rView.m_aGraphicSelection = payloadToRectangle(priv, pSelection);
        rView.m_nGraphicSelectionPart = nPart;
        break;
    }
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

static void handleCallback(LOKDocView* pDocView, CallbackEvent* pEvent)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    const std::string& rPayload = pEvent->m_aPayload;
    const char* pPayload = rPayload.c_str();

    switch (pEvent->m_nType)
    {
    case LOK_CALLBACK_INVALIDATE_TILES:
        invalidateTiles(pDocView, rPayload);
        break;
    case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
        if (priv->m_pDocument)
        {
            refreshSize(pDocView);
            g_signal_emit(pDocView, doc_view_signals[SIZE_CHANGED], 0, 0);
        }
        break;
    case LOK_CALLBACK_STATUS_INDICATOR_START:
        setLoadProgress(pDocView, 0.0);
        break;
    case LOK_CALLBACK_STATUS_INDICATOR_SET_VALUE:
        setLoadProgress(pDocView, std::atoi(pPayload) / 100.0);
        break;
    case LOK_CALLBACK_STATUS_INDICATOR_FINISH:
        setLoadProgress(pDocView, 1.0);
        break;
    case LOK_CALLBACK_STATE_CHANGED:
        g_signal_emit(pDocView, doc_view_signals[COMMAND_CHANGED], 0, pPayload);
        break;
    case LOK_CALLBACK_HYPERLINK_CLICKED:
        g_signal_emit(pDocView, doc_view_signals[HYPERLINK_CLICKED], 0, pPayload);
        break;
    case LOK_CALLBACK_SEARCH_NOT_FOUND:
        g_signal_emit(pDocView, doc_view_signals[SEARCH_NOT_FOUND], 0, pPayload);
        break;
    case LOK_CALLBACK_SEARCH_RESULT_SELECTION:
        g_signal_emit(pDocView, doc_view_signals[SEARCH_RESULT_COUNT], 0,
                      std::to_string(countSearchResults(rPayload)).c_str());
        break;
    case LOK_CALLBACK_SET_PART:
    {
        int nPart = std::atoi(pPayload);
        // LOK switched part itself (e.g. Ctrl+PageDown in Calc): the tiles
        // show the old one. Don't rely on an invalidation for the new part,
        // it may have arrived before this and been skipped as "other part".
        if (nPart != priv->m_nPartId && priv->m_eDocumentType != LOK_DOCTYPE_TEXT)
            startNewTileGeneration(pDocView, false);
        priv->m_nPartId = nPart;
        g_signal_emit(pDocView, doc_view_signals[PART_CHANGED], 0, priv->m_nPartId);
        break;
    }
    case LOK_CALLBACK_UNO_COMMAND_RESULT:
        handleCommandResult(pDocView, rPayload);
        break;
    case LOK_CALLBACK_CELL_ADDRESS:
        g_signal_emit(pDocView, doc_view_signals[ADDRESS_CHANGED], 0, pPayload);
        break;
    case LOK_CALLBACK_CELL_FORMULA:
        g_signal_emit(pDocView, doc_view_signals[FORMULA_CHANGED], 0, pPayload);
        break;
    case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:
        setVisibleCursor(pDocView, rPayload);
        break;
    case LOK_CALLBACK_CURSOR_VISIBLE:
        priv->m_bCursorVisible = rPayload == "true";
        restartCursorBlink(pDocView);
        break;
    case LOK_CALLBACK_TEXT_SELECTION:
        priv->m_aTextSelectionRectangles = payloadToRectangles(priv, pPayload);
        g_signal_emit(pDocView, doc_view_signals[TEXT_SELECTION], 0, gboolean(!priv->m_aTextSelectionRectangles.empty()));
        // Like other X11/Wayland apps, a deselect leaves the primary selection as is.
        if (!priv->m_aTextSelectionRectangles.empty())
            schedulePrimarySelection(pDocView);
        gtk_widget_queue_draw(GTK_WIDGET(pDocView));
        break;
    case LOK_CALLBACK_TEXT_SELECTION_START:
    case LOK_CALLBACK_TEXT_SELECTION_END:
        // Only needed for touch selection handles. TODO: port those (upstream
        // draws PNGs from share/libreofficekit/, often not packaged).
        break;
    case LOK_CALLBACK_GRAPHIC_SELECTION:
        // Payload may carry extra fields (angle, handles JSON); only the rectangle is used.
        // TODO: dragging handles via setGraphicSelection() like upstream, unless
        // plain mouse events on the handle positions turn out to be enough.
        priv->m_aGraphicSelection = payloadToRectangle(priv, pPayload);
        gtk_widget_queue_draw(GTK_WIDGET(pDocView));
        break;
    case LOK_CALLBACK_CELL_CURSOR:
        priv->m_aCellCursor = payloadToRectangle(priv, pPayload);
        gtk_widget_queue_draw(GTK_WIDGET(pDocView));
        break;
    case LOK_CALLBACK_CLIPBOARD_CHANGED:
        exportClipboard(pDocView);
        break;
    case LOK_CALLBACK_INVALIDATE_VIEW_CURSOR:
    case LOK_CALLBACK_VIEW_CURSOR_VISIBLE:
    case LOK_CALLBACK_TEXT_VIEW_SELECTION:
    case LOK_CALLBACK_CELL_VIEW_CURSOR:
    case LOK_CALLBACK_GRAPHIC_VIEW_SELECTION:
        handleOtherViewCallback(pDocView, pEvent->m_nType, rPayload);
        break;
    case LOK_CALLBACK_MOUSE_POINTER:
        // CSS cursor names ("text", "pointer", "move", ...), as GTK4 takes them;
        // unknown names fall back to the default pointer. Unlike the GTK3
        // widget, also in view-only mode: selecting and links work there too.
        priv->m_aPointerName = rPayload;
        if (priv->m_bPointerInDocument)
            gtk_widget_set_cursor_from_name(GTK_WIDGET(pDocView), pPayload);
        break;
    case LOK_CALLBACK_CONTENT_CONTROL:
        g_signal_emit(pDocView, doc_view_signals[CONTENT_CONTROL], 0, pPayload);
        break;
    case LOK_CALLBACK_DOCUMENT_PASSWORD:
    case LOK_CALLBACK_DOCUMENT_PASSWORD_TO_MODIFY:
        g_signal_emit(pDocView, doc_view_signals[PASSWORD_REQUIRED], 0, pPayload,
                      gboolean(pEvent->m_nType == LOK_CALLBACK_DOCUMENT_PASSWORD_TO_MODIFY));
        break;
    case LOK_CALLBACK_COMMENT:
        g_signal_emit(pDocView, doc_view_signals[COMMENT], 0, pPayload);
        break;
    case LOK_CALLBACK_RULER_UPDATE:
        g_signal_emit(pDocView, doc_view_signals[RULER], 0, pPayload);
        break;
    case LOK_CALLBACK_WINDOW:
        g_signal_emit(pDocView, doc_view_signals[WINDOW], 0, pPayload);
        break;
    case LOK_CALLBACK_INVALIDATE_HEADER:
        g_signal_emit(pDocView, doc_view_signals[INVALIDATE_HEADER], 0, pPayload);
        break;
    default:
        // TODO: visible cursor, cursor visibility, cell cursor, graphic
        // selection, view cursors, mouse pointer, ... (upstream callbackWorker).
        g_debug("unhandled LOK callback %d: %s", pEvent->m_nType, pPayload);
        break;
    }
}

static gboolean callbackIdle(gpointer pData)
{
    std::unique_ptr<CallbackEvent> pEvent(static_cast<CallbackEvent*>(pData));
    // Re-acquire the view; it may have been disposed since LOK called us.
    if (gpointer pView = g_weak_ref_get(&pEvent->m_pHandle->m_aView))
    {
        LOKDocView* pDocView = LOK_DOC_VIEW(pView);
        if (!getPrivate(pDocView)->m_bDisposed)
            handleCallback(pDocView, pEvent.get());
        g_object_unref(pDocView);
    }
    return G_SOURCE_REMOVE;
}

/**
 * Called by LOK, possibly on its own thread and possibly synchronously from
 * inside a LOK call we made while holding g_aLOKMutex. So: never touch LOK
 * here, always defer to the main loop. Also never take a strong ref here: if
 * it were the last one, unreffing it would dispose the view on LOK's thread.
 */
static void lokCallback(int nType, const char* pPayload, void* pData)
{
    auto* pEvent = new CallbackEvent{ static_cast<ViewHandle*>(pData), nType, pPayload ? pPayload : "" };
    g_idle_add_full(G_PRIORITY_DEFAULT, callbackIdle, pEvent, nullptr);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

namespace
{
/// One tile to paint on the worker thread. Everything the worker needs is
/// copied in, so it never reads view state except the handle's generation.
struct PaintRequest
{
    ViewHandle* m_pHandle;
    LibreOfficeKitDocument* m_pDocument;
    int m_nViewId;
    int m_eTileMode;
    unsigned m_nGeneration;
    unsigned m_nSerial;
    int m_nColumn;
    int m_nRow;
    int m_nTilePixels;
    int m_nTileTwips;
    /// Queue order: lower first (see comparePaintRequests()).
    unsigned m_nPriority;
    guint64 m_nSequence;
    GdkTexture* m_pTexture = nullptr; // result
};

/// Request order tiebreaker, main thread only.
guint64 g_nPaintSequence = 0;
}

/// Paint queue order: by priority (distance from the viewport center,
/// prefetch after visible tiles), then first come first served.
static gint comparePaintRequests(gconstpointer pA, gconstpointer pB, gpointer)
{
    auto* pRequestA = static_cast<const PaintRequest*>(pA);
    auto* pRequestB = static_cast<const PaintRequest*>(pB);
    if (pRequestA->m_nPriority != pRequestB->m_nPriority)
        return pRequestA->m_nPriority < pRequestB->m_nPriority ? -1 : 1;
    return pRequestA->m_nSequence < pRequestB->m_nSequence ? -1 : pRequestA->m_nSequence > pRequestB->m_nSequence;
}

/// Worker thread: whether the tile is still in the area snapshot last asked for.
static bool isTileWanted(const PaintRequest* pRequest)
{
    ViewHandle* pHandle = pRequest->m_pHandle;
    std::scoped_lock<std::mutex> aGuard(pHandle->m_aWantedMutex);
    return pRequest->m_nColumn >= pHandle->m_nWantedColStart && pRequest->m_nColumn < pHandle->m_nWantedColEnd
           && pRequest->m_nRow >= pHandle->m_nWantedRowStart && pRequest->m_nRow < pHandle->m_nWantedRowEnd;
}

static void freePaintRequest(gpointer pData)
{
    auto* pRequest = static_cast<PaintRequest*>(pData);
    g_clear_object(&pRequest->m_pTexture);
    delete pRequest;
}

/// Main loop: store a finished tile, unless it was superseded meanwhile.
static gboolean paintTileDone(gpointer pData)
{
    auto* pRequest = static_cast<PaintRequest*>(pData);

    // Generation check first: also covers dispose, which bumps it.
    if (pRequest->m_nGeneration == pRequest->m_pHandle->m_nTileGeneration)
    {
        if (gpointer pView = g_weak_ref_get(&pRequest->m_pHandle->m_aView))
        {
            LOKDocView* pDocView = LOK_DOC_VIEW(pView);
            TileBuffer& rTiles = *getPrivate(pDocView)->m_pTileBuffer;
            if (pRequest->m_pTexture)
            {
                rTiles.setTile(pRequest->m_nColumn, pRequest->m_nRow, std::exchange(pRequest->m_pTexture, nullptr),
                               pRequest->m_nSerial);
                gtk_widget_queue_draw(GTK_WIDGET(pDocView));
            }
            else // skipped as no longer wanted: request again if it comes back into view
                rTiles.clearPending(pRequest->m_nColumn, pRequest->m_nRow);
            g_object_unref(pDocView);
        }
    }

    freePaintRequest(pRequest);
    return G_SOURCE_REMOVE;
}

/// Worker thread: paint one tile into a texture.
static void paintTileInThread(gpointer pData, gpointer /*pUserData*/)
{
    auto* pRequest = static_cast<PaintRequest*>(pData);

    // Skip requests superseded while queued (zoom change, part switch, dispose)
    // or scrolled out of the wanted area. Dispose waits for a running paint
    // before destroying the document.
    if (pRequest->m_nGeneration == pRequest->m_pHandle->m_nTileGeneration && isTileWanted(pRequest))
    {
        int nTilePixels = pRequest->m_nTilePixels;
        int nTileTwips = pRequest->m_nTileTwips;
        gsize nStride = static_cast<gsize>(nTilePixels) * 4;
        gsize nSize = nStride * nTilePixels;
        // Must start zeroed (transparent): LOK leaves areas without content, e.g.
        // Writer's margin around the pages, untouched.
        auto* pBuffer = static_cast<guchar*>(g_malloc0(nSize));

        {
            std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
            setDocumentView(pRequest->m_pDocument, pRequest->m_nViewId);
            pRequest->m_pDocument->pClass->paintTile(pRequest->m_pDocument, pBuffer, nTilePixels, nTilePixels,
                                                     pRequest->m_nColumn * nTileTwips, pRequest->m_nRow * nTileTwips,
                                                     nTileTwips, nTileTwips);
        }

        GdkMemoryFormat eFormat = pRequest->m_eTileMode == LOK_TILEMODE_RGBA ? GDK_MEMORY_R8G8B8A8_PREMULTIPLIED
                                                                             : GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
        GBytes* pBytes = g_bytes_new_take(pBuffer, nSize);
        // GdkTexture is immutable and may be created off the main thread.
        pRequest->m_pTexture = gdk_memory_texture_new(nTilePixels, nTilePixels, eFormat, pBytes, nStride);
        g_bytes_unref(pBytes);
    }

    g_idle_add_full(G_PRIORITY_DEFAULT, paintTileDone, pRequest, nullptr);
}

static void requestTile(LOKDocView* pDocView, int nColumn, int nRow, Tile& rTile, unsigned nPriority)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    rTile.m_bPending = true;

    auto* pRequest = new PaintRequest{ priv->m_pHandle,
                                       priv->m_pDocument,
                                       priv->m_nViewId,
                                       priv->m_eTileMode,
                                       priv->m_pHandle->m_nTileGeneration,
                                       rTile.m_nSerial,
                                       nColumn,
                                       nRow,
                                       getTilePixels(priv),
                                       getTileSizeTwips(priv),
                                       nPriority,
                                       g_nPaintSequence++ };
    g_thread_pool_push(priv->m_pPaintPool, pRequest, nullptr);
}

/**
 * Starts over with an empty tile buffer, e.g. on zoom or part change.
 *
 * With bKeepAsPlaceholder, the current tiles are kept (with the zoom they were
 * rendered at, so call this before changing m_fZoom) and drawn scaled until
 * the new ones arrive. If the current buffer is still empty, an older
 * placeholder is kept instead, so rapid zoom steps don't blank the view.
 */
static void startNewTileGeneration(LOKDocView* pDocView, bool bKeepAsPlaceholder)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    ++priv->m_pHandle->m_nTileGeneration;

    if (!bKeepAsPlaceholder)
        priv->m_pPreviousTiles.reset();
    else if (priv->m_pTileBuffer->hasTextures())
    {
        priv->m_pPreviousTiles = std::move(priv->m_pTileBuffer);
        priv->m_fPreviousZoom = priv->m_fZoom;
    }
    priv->m_pTileBuffer = std::make_unique<TileBuffer>();
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

/// Draws the textures of rTiles that intersect rRegion (current document pixels).
/// fScale converts rTiles' pixel grid to the current one.
static void appendTiles(GtkSnapshot* pSnapshot, const TileBuffer& rTiles, float fScale, const graphene_rect_t& rRegion)
{
    float fTile = nTileSizePixels * fScale;
    int nColStart = static_cast<int>(rRegion.origin.x / fTile);
    int nRowStart = static_cast<int>(rRegion.origin.y / fTile);
    int nColEnd = static_cast<int>(std::ceil((rRegion.origin.x + rRegion.size.width) / fTile));
    int nRowEnd = static_cast<int>(std::ceil((rRegion.origin.y + rRegion.size.height) / fTile));

    for (int nRow = nRowStart; nRow < nRowEnd; ++nRow)
    {
        for (int nColumn = nColStart; nColumn < nColEnd; ++nColumn)
        {
            const Tile* pTile = rTiles.findTile(nColumn, nRow);
            if (!pTile || !pTile->m_pTexture)
                continue;
            graphene_rect_t aRect = GRAPHENE_RECT_INIT(nColumn * fTile, nRow * fTile, fTile, fTile);
            gtk_snapshot_append_texture(pSnapshot, pTile->m_pTexture, &aRect);
        }
    }
}

static void updateIMCursorLocation(LOKDocView* pDocView);
static void scheduleClientViewUpdate(LOKDocView* pDocView);

/**
 * Queues paints for missing or stale tiles, given the visible tile range
 * [start, end) and the viewport center (tile units):
 * - visible tiles nearest the center first (the queue is sorted by priority),
 * - then a one-tile prefetch ring around them, so scrolling a little shows
 *   painted tiles instead of blanks.
 * Publishes that area to the worker, which skips queued requests that left
 * it (e.g. after scrolling on) instead of painting them first. Also bounds
 * the tile cache to a few times that area.
 */
static void requestTiles(LOKDocView* pDocView, int nColStart, int nRowStart, int nColEnd, int nRowEnd,
                         double fCenterColumn, double fCenterRow)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    constexpr int nPrefetch = 1;
    int nDocColumns = static_cast<int>(std::ceil(getDocumentWidthPixels(priv) / nTileSizePixels));
    int nDocRows = static_cast<int>(std::ceil(getDocumentHeightPixels(priv) / nTileSizePixels));
    int nWantedColStart = std::max(0, nColStart - nPrefetch);
    int nWantedRowStart = std::max(0, nRowStart - nPrefetch);
    int nWantedColEnd = std::min(nDocColumns, nColEnd + nPrefetch);
    int nWantedRowEnd = std::min(nDocRows, nRowEnd + nPrefetch);
    {
        ViewHandle* pHandle = priv->m_pHandle;
        std::scoped_lock<std::mutex> aGuard(pHandle->m_aWantedMutex);
        pHandle->m_nWantedColStart = nWantedColStart;
        pHandle->m_nWantedRowStart = nWantedRowStart;
        pHandle->m_nWantedColEnd = nWantedColEnd;
        pHandle->m_nWantedRowEnd = nWantedRowEnd;
    }

    for (int nRow = nWantedRowStart; nRow < nWantedRowEnd; ++nRow)
    {
        for (int nColumn = nWantedColStart; nColumn < nWantedColEnd; ++nColumn)
        {
            Tile& rTile = priv->m_pTileBuffer->getTile(nColumn, nRow);
            if (rTile.m_bValid || rTile.m_bPending)
                continue;

            bool bVisible = nColumn >= nColStart && nColumn < nColEnd && nRow >= nRowStart && nRow < nRowEnd;
            double fDX = nColumn + 0.5 - fCenterColumn;
            double fDY = nRow + 0.5 - fCenterRow;
            // Squared distance in 1/16 tiles; prefetch always after visible tiles.
            unsigned nPriority = static_cast<unsigned>(std::min(fDX * fDX + fDY * fDY, 1e6) * 16)
                                 + (bVisible ? 0 : 1u << 28);
            requestTile(pDocView, nColumn, nRow, rTile, nPriority);
        }
    }

    size_t nWanted = static_cast<size_t>(nWantedColEnd - nWantedColStart) * (nWantedRowEnd - nWantedRowStart);
    priv->m_pTileBuffer->evictFarthest(fCenterColumn, fCenterRow, std::max<size_t>(64, 3 * nWanted));
}

static void appendRectangleOutline(GtkSnapshot* pSnapshot, const graphene_rect_t& rRect, float fWidth, const GdkRGBA& rColor)
{
    GskRoundedRect aRounded;
    gsk_rounded_rect_init_from_rect(&aRounded, &rRect, 0);
    const float aWidths[4] = { fWidth, fWidth, fWidth, fWidth };
    const GdkRGBA aColors[4] = { rColor, rColor, rColor, rColor };
    gtk_snapshot_append_border(pSnapshot, &aRounded, aWidths, aColors);
}

/// Thin frame plus 8 resize handles (9px, not zoomed).
static void appendGraphicSelection(GtkSnapshot* pSnapshot, const graphene_rect_t& rSelection, const GdkRGBA& rColor)
{
    const GdkRGBA aWhite = { 1, 1, 1, 1 };
    appendRectangleOutline(pSnapshot, rSelection, 1, rColor);

    const float fHandle = 9;
    for (float fX : { 0.0f, 0.5f, 1.0f })
    {
        for (float fY : { 0.0f, 0.5f, 1.0f })
        {
            if (fX == 0.5f && fY == 0.5f)
                continue;
            graphene_rect_t aHandle = GRAPHENE_RECT_INIT(
                rSelection.origin.x + rSelection.size.width * fX - fHandle / 2,
                rSelection.origin.y + rSelection.size.height * fY - fHandle / 2, fHandle, fHandle);
            gtk_snapshot_append_color(pSnapshot, &aWhite, &aHandle);
            appendRectangleOutline(pSnapshot, aHandle, 1, rColor);
        }
    }
}

/// Per-view colour, by view id: modelled on LibreOffice's dark author
/// colours, minus the blues, which looked like our own selection.
static GdkRGBA getViewColor(int nViewId, float fAlpha)
{
    static const guint8 aColors[][3] = { { 198, 146, 0 },  { 87, 157, 28 }, { 105, 43, 157 },
                                         { 197, 0, 11 },   { 0, 128, 128 }, { 140, 132, 0 },
                                         { 209, 118, 0 } };
    const guint8* pColor = aColors[static_cast<unsigned>(nViewId) % G_N_ELEMENTS(aColors)];
    return { pColor[0] / 255.0f, pColor[1] / 255.0f, pColor[2] / 255.0f, fAlpha };
}

/**
 * Draws what LOK leaves out of the tiles: text cursor, text selection,
 * graphic selection and Calc cell cursor (upstream renderOverlay()).
 * pSnapshot is in document pixels.
 *
 * Also other views' cursors and selections, in per-view colours.
 *
 * TODO: content controls, touch handles.
 */
static void appendOverlays(LOKDocView* pDocView, GtkSnapshot* pSnapshot)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    const float fZoom = priv->m_fZoom;
    auto toPixels = [fZoom](const GdkRectangle& rTwips) {
        return GRAPHENE_RECT_INIT(twipToPixel(rTwips.x, fZoom), twipToPixel(rTwips.y, fZoom),
                                  twipToPixel(rTwips.width, fZoom), twipToPixel(rTwips.height, fZoom));
    };

    // Other views first, so our own selection and cursor stay on top. Each
    // item only on the part it is on, except in Writer.
    auto isOnCurrentPart = [&priv](int nPart) {
        return priv->m_eDocumentType == LOK_DOCTYPE_TEXT || nPart == priv->m_nPartId;
    };
    for (const auto& [nViewId, rView] : priv->m_aOtherViews)
    {
        const GdkRGBA aColor = getViewColor(nViewId, 1);
        if (isOnCurrentPart(rView.m_nSelectionPart))
        {
            const GdkRGBA aTranslucent = getViewColor(nViewId, 0.25f);
            for (const GdkRectangle& rRectangle : rView.m_aSelection)
            {
                graphene_rect_t aRect = toPixels(rRectangle);
                gtk_snapshot_append_color(pSnapshot, &aTranslucent, &aRect);
            }
        }
        if (!isEmptyRectangle(rView.m_aCellCursor) && isOnCurrentPart(rView.m_nCellCursorPart))
            appendRectangleOutline(pSnapshot, toPixels(rView.m_aCellCursor), 2, aColor);
        if (!isEmptyRectangle(rView.m_aGraphicSelection) && isOnCurrentPart(rView.m_nGraphicSelectionPart))
            appendGraphicSelection(pSnapshot, toPixels(rView.m_aGraphicSelection), aColor);
        // Other views' cursors don't blink (upstream), and show in view-only mode too.
        if (rView.m_bCursorVisible && !isEmptyRectangle(rView.m_aCursor) && isOnCurrentPart(rView.m_nCursorPart))
        {
            GdkRectangle aCursor = rView.m_aCursor;
            aCursor.width = std::max(aCursor.width, 30);
            graphene_rect_t aRect = toPixels(aCursor);
            gtk_snapshot_append_color(pSnapshot, &aColor, &aRect);
        }
    }

    // Text selection: blue, 75% transparent (upstream colour).
    const GdkRGBA aSelectionColor = { 0x43 / 255.0f, 0xac / 255.0f, 0xe8 / 255.0f, 0.25f };
    for (const GdkRectangle& rRectangle : priv->m_aTextSelectionRectangles)
    {
        graphene_rect_t aRect = toPixels(rRectangle);
        gtk_snapshot_append_color(pSnapshot, &aSelectionColor, &aRect);
    }

    const GdkRGBA aBlack = { 0, 0, 0, 1 };

    // Calc cell cursor: 2px frame.
    if (!isEmptyRectangle(priv->m_aCellCursor))
        appendRectangleOutline(pSnapshot, toPixels(priv->m_aCellCursor), 2, aBlack);

    if (!isEmptyRectangle(priv->m_aGraphicSelection))
        appendGraphicSelection(pSnapshot, toPixels(priv->m_aGraphicSelection), aBlack);

    // Own text cursor; LOK reports width 0, give it a minimum like upstream (30 twips).
    if (isCursorShown(pDocView) && priv->m_bCursorBlinkOn)
    {
        GdkRectangle aCursor = priv->m_aVisibleCursor;
        aCursor.width = std::max(aCursor.width, 30);
        graphene_rect_t aRect = toPixels(aCursor);
        gtk_snapshot_append_color(pSnapshot, &aBlack, &aRect);
    }
}

static void lok_doc_view_snapshot(GtkWidget* pWidget, GtkSnapshot* pSnapshot)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pWidget);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    int nWidth = gtk_widget_get_width(pWidget);
    int nHeight = gtk_widget_get_height(pWidget);
    double fDocWidth = getDocumentWidthPixels(priv);
    double fDocHeight = getDocumentHeightPixels(priv);
    double fOriginX, fOriginY;
    getDocumentOrigin(pDocView, fOriginX, fOriginY);

    // Everything below is in document pixels.
    gtk_snapshot_save(pSnapshot);
    graphene_point_t aOrigin = GRAPHENE_POINT_INIT(float(fOriginX), float(fOriginY));
    gtk_snapshot_translate(pSnapshot, &aOrigin);
    graphene_rect_t aDocRect = GRAPHENE_RECT_INIT(0, 0, float(fDocWidth), float(fDocHeight));
    gtk_snapshot_push_clip(pSnapshot, &aDocRect);

    // Visible part of the document.
    double fVisLeft = std::max(0.0, -fOriginX);
    double fVisTop = std::max(0.0, -fOriginY);
    double fVisRight = std::min(fDocWidth, nWidth - fOriginX);
    double fVisBottom = std::min(fDocHeight, nHeight - fOriginY);

    int nColStart = static_cast<int>(fVisLeft / nTileSizePixels);
    int nRowStart = static_cast<int>(fVisTop / nTileSizePixels);
    int nColEnd = static_cast<int>(std::ceil(fVisRight / nTileSizePixels));
    int nRowEnd = static_cast<int>(std::ceil(fVisBottom / nTileSizePixels));

    bool bComplete = true;
    for (int nRow = nRowStart; nRow < nRowEnd; ++nRow)
    {
        for (int nColumn = nColStart; nColumn < nColEnd; ++nColumn)
        {
            Tile& rTile = priv->m_pTileBuffer->getTile(nColumn, nRow);
            graphene_rect_t aTileRect = GRAPHENE_RECT_INIT(float(nColumn * nTileSizePixels), float(nRow * nTileSizePixels),
                                                           float(nTileSizePixels), float(nTileSizePixels));
            if (rTile.m_pTexture)
            {
                // Possibly stale: shown until its replacement arrives, to avoid flicker.
                gtk_snapshot_append_texture(pSnapshot, rTile.m_pTexture, &aTileRect);
            }
            else
            {
                bComplete = false;
                // Not rendered yet: fill with the previous zoom level's tiles, scaled.
                // Clipped to this tile, so it can't show through transparent
                // areas of tiles that are already there.
                if (priv->m_pPreviousTiles)
                {
                    gtk_snapshot_push_clip(pSnapshot, &aTileRect);
                    appendTiles(pSnapshot, *priv->m_pPreviousTiles, priv->m_fZoom / priv->m_fPreviousZoom, aTileRect);
                    gtk_snapshot_pop(pSnapshot);
                }
            }
        }
    }
    if (bComplete)
        priv->m_pPreviousTiles.reset();

    // Keep LOK informed of what this view shows (twips): it decides e.g.
    // which other views' cursor invalidations to send based on it.
    GdkRectangle aVisibleArea = { static_cast<int>(pixelToTwip(fVisLeft, priv->m_fZoom)),
                                  static_cast<int>(pixelToTwip(fVisTop, priv->m_fZoom)),
                                  static_cast<int>(pixelToTwip(fVisRight - fVisLeft, priv->m_fZoom)),
                                  static_cast<int>(pixelToTwip(fVisBottom - fVisTop, priv->m_fZoom)) };
    if (!gdk_rectangle_equal(&aVisibleArea, &priv->m_aVisibleArea))
    {
        priv->m_aVisibleArea = aVisibleArea;
        scheduleClientViewUpdate(pDocView);
    }

    if (nColStart < nColEnd && nRowStart < nRowEnd)
        requestTiles(pDocView, nColStart, nRowStart, nColEnd, nRowEnd,
                     (fVisLeft + fVisRight) / 2 / nTileSizePixels, (fVisTop + fVisBottom) / 2 / nTileSizePixels);

    appendOverlays(pDocView, pSnapshot);

    gtk_snapshot_pop(pSnapshot);
    gtk_snapshot_restore(pSnapshot);

    updateIMCursorLocation(pDocView);
}

static void lok_doc_view_measure(GtkWidget* pWidget, GtkOrientation eOrientation, int /*nForSize*/,
                                 int* pMinimum, int* pNatural, int* pMinimumBaseline, int* pNaturalBaseline)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pWidget));
    double fSize = eOrientation == GTK_ORIENTATION_HORIZONTAL ? getDocumentWidthPixels(priv)
                                                              : getDocumentHeightPixels(priv);
    *pMinimum = 0;
    *pNatural = static_cast<int>(std::ceil(fSize));
    *pMinimumBaseline = -1;
    *pNaturalBaseline = -1;
}

static void lok_doc_view_size_allocate(GtkWidget* pWidget, int /*nWidth*/, int /*nHeight*/, int /*nBaseline*/)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pWidget);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    configureAdjustments(pDocView);

    // Apply a zoom anchor only now: before the adjustments know the new
    // document size, the scroll value would be clamped to the old one.
    if (priv->m_bZoomAnchorPending)
    {
        priv->m_bZoomAnchorPending = false;
        gtk_adjustment_set_value(priv->m_pHAdjustment, priv->m_fAnchorDocX - priv->m_fAnchorX);
        gtk_adjustment_set_value(priv->m_pVAdjustment, priv->m_fAnchorDocY - priv->m_fAnchorY);
    }
}

/// Re-reads the (possibly fractional) scale and re-renders at the new resolution.
static void updateScale(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    double fScale = priv->m_pScaleSurface ? gdk_surface_get_scale(priv->m_pScaleSurface)
                                          : gtk_widget_get_scale_factor(GTK_WIDGET(pDocView));
    if (fScale <= 0 || fScale == priv->m_fScale)
        return;

    g_debug("scale %.3f -> %.3f (tiles %d px)", priv->m_fScale, fScale,
            static_cast<int>(std::lround(nTileSizePixels * fScale)));
    priv->m_fScale = fScale;
    // Same layout, new resolution: old tiles are a fine placeholder.
    startNewTileGeneration(pDocView, true);
    updateClientZoom(pDocView);
}

static void onScaleChanged(GObject*, GParamSpec*, gpointer pData)
{
    updateScale(LOK_DOC_VIEW(pData));
}

/// Follow the surface's scale: notify::scale-factor only reports integer changes.
static void lok_doc_view_realize(GtkWidget* pWidget)
{
    GTK_WIDGET_CLASS(lok_doc_view_parent_class)->realize(pWidget);

    LOKDocView* pDocView = LOK_DOC_VIEW(pWidget);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (GtkNative* pNative = gtk_widget_get_native(pWidget))
    {
        if (GdkSurface* pSurface = gtk_native_get_surface(pNative))
        {
            priv->m_pScaleSurface = GDK_SURFACE(g_object_ref(pSurface));
            priv->m_nScaleHandlerId = g_signal_connect(pSurface, "notify::scale", G_CALLBACK(onScaleChanged), pDocView);
        }
    }
    updateScale(pDocView);
}

static void lok_doc_view_unrealize(GtkWidget* pWidget)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pWidget));
    if (priv->m_pScaleSurface)
    {
        g_clear_signal_handler(&priv->m_nScaleHandlerId, priv->m_pScaleSurface);
        g_clear_object(&priv->m_pScaleSurface);
    }
    GTK_WIDGET_CLASS(lok_doc_view_parent_class)->unrealize(pWidget);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static int toLOKModifier(GdkModifierType eState)
{
    int nModifier = 0;
    if (eState & GDK_SHIFT_MASK)
        nModifier |= KEY_SHIFT;
    if (eState & GDK_CONTROL_MASK)
        nModifier |= KEY_MOD1;
    if (eState & GDK_ALT_MASK)
        nModifier |= KEY_MOD2;
    return nModifier;
}

static int toLOKButton(guint nButton)
{
    switch (nButton)
    {
    case GDK_BUTTON_PRIMARY:
        return MOUSE_LEFT;
    case GDK_BUTTON_MIDDLE:
        return MOUSE_MIDDLE;
    case GDK_BUTTON_SECONDARY:
        return MOUSE_RIGHT;
    default:
        return 0;
    }
}

static void postMouseEvent(LOKDocView* pDocView, int nType, double fX, double fY, int nCount, int nButtons, int nModifier)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    int nTwipX, nTwipY;
    widgetToTwip(pDocView, fX, fY, nTwipX, nTwipY);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postMouseEvent(priv->m_pDocument, nType, nTwipX, nTwipY, nCount, nButtons, nModifier);
}

static gboolean flushHover(gpointer pData);

/**
 * Sends the pending hover position as a button-less mouse move, which is what
 * makes LOK report the pointer shape (LOK_CALLBACK_MOUSE_POINTER) and hover
 * effects. Only try-locks: if the paint worker holds the LOK mutex, keep the
 * latest position and retry shortly instead of blocking the UI behind a tile
 * paint. Intermediate positions don't matter for hover.
 */
static gboolean flushHover(gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_nHoverRetryId = 0;
    if (!priv->m_bHoverPending || !priv->m_pDocument)
        return G_SOURCE_REMOVE;

    int nTwipX, nTwipY;
    widgetToTwip(pDocView, priv->m_fHoverX, priv->m_fHoverY, nTwipX, nTwipY);
    std::unique_lock<std::mutex> aGuard(g_aLOKMutex, std::try_to_lock);
    if (!aGuard.owns_lock())
    {
        priv->m_nHoverRetryId = g_timeout_add(16, flushHover, pDocView);
        return G_SOURCE_REMOVE;
    }
    priv->m_bHoverPending = false;
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postMouseEvent(priv->m_pDocument, LOK_MOUSEEVENT_MOUSEMOVE, nTwipX, nTwipY,
                                              1, 0, priv->m_nHoverModifier);
    return G_SOURCE_REMOVE;
}

static void onMotion(GtkEventControllerMotion* pController, double fX, double fY, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_bPointerKnown = true;
    priv->m_fPointerX = fX;
    priv->m_fPointerY = fY;
    // While a button is down the drag gesture sends ordered moves itself.
    if (priv->m_nMouseButtons)
        return;

    // Outside the document (e.g. beside a centered page) nothing belongs to
    // LOK: show the default pointer and don't ask LOK about it. Restore LOK's
    // last shape on the way back in, as it only reports changes.
    double fOriginX, fOriginY;
    getDocumentOrigin(pDocView, fOriginX, fOriginY);
    bool bInDocument = fX >= fOriginX && fX < fOriginX + getDocumentWidthPixels(priv)
                       && fY >= fOriginY && fY < fOriginY + getDocumentHeightPixels(priv);
    if (bInDocument != priv->m_bPointerInDocument)
    {
        priv->m_bPointerInDocument = bInDocument;
        gtk_widget_set_cursor_from_name(GTK_WIDGET(pDocView), bInDocument ? priv->m_aPointerName.c_str() : nullptr);
    }
    if (!bInDocument)
        return;

    priv->m_fHoverX = fX;
    priv->m_fHoverY = fY;
    priv->m_nHoverModifier = toLOKModifier(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(pController)));
    priv->m_bHoverPending = true;
    if (!priv->m_nHoverRetryId) // otherwise a retry is already scheduled
        flushHover(pDocView);
}

static void onPointerLeave(GtkEventControllerMotion*, gpointer pData)
{
    getPrivate(LOK_DOC_VIEW(pData))->m_bPointerKnown = false;
}

static void setZoom(LOKDocView* pDocView, float fZoom, double fAnchorX, double fAnchorY);

/// Ctrl+scroll zooms around the pointer; plain scrolling is left to the
/// parent GtkScrolledWindow (its controller runs after ours).
static gboolean onScroll(GtkEventControllerScroll* pController, double /*fDx*/, double fDy, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GdkModifierType eState = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(pController));
    if (!(eState & GDK_CONTROL_MASK) || !priv->m_pDocument || fDy == 0)
        return FALSE;

    // A wheel notch (fDy = ±1) zooms by 10%. Touchpads report smooth deltas
    // in surface pixels: scale those down to a comparable rate.
    if (gtk_event_controller_scroll_get_unit(pController) == GDK_SCROLL_UNIT_SURFACE)
        fDy /= 20;
    float fZoom = priv->m_fZoom * static_cast<float>(std::pow(1.1, -fDy));

    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    double fX = priv->m_bPointerKnown ? priv->m_fPointerX : gtk_widget_get_width(pWidget) / 2.0;
    double fY = priv->m_bPointerKnown ? priv->m_fPointerY : gtk_widget_get_height(pWidget) / 2.0;
    setZoom(pDocView, fZoom, fX, fY);
    return TRUE;
}

/// Touchpad/touchscreen pinch: zoom around the gesture center.
static void onPinchBegin(GtkGesture*, GdkEventSequence*, gpointer pData)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pData));
    priv->m_fPinchStartZoom = priv->m_fZoom;
}

static void onPinchScaleChanged(GtkGestureZoom* pGesture, double fScale, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    double fX, fY;
    if (!gtk_gesture_get_bounding_box_center(GTK_GESTURE(pGesture), &fX, &fY))
        return;
    setZoom(pDocView, getPrivate(pDocView)->m_fPinchStartZoom * static_cast<float>(fScale), fX, fY);
}

// GtkGestureDrag gives us press (drag-begin), motion (drag-update) and release
// (drag-end) for the same sequence, even without movement. Click counting is
// done by hand, like upstream.
static void onDragBegin(GtkGestureDrag* pGesture, double fX, double fY, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GtkEventController* pController = GTK_EVENT_CONTROLLER(pGesture);
    guint nButton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(pGesture));
    guint32 nTime = gtk_event_controller_get_current_event_time(pController);

    gtk_widget_grab_focus(GTK_WIDGET(pDocView));
    setActiveView(pDocView);

    // Middle click pastes the primary selection at the click, as elsewhere on
    // X11/Wayland. Not passed to LOK: its "primary selection" is the same
    // per-view LOK clipboard as Ctrl+C, so it would paste the last copy.
    priv->m_bMiddleClick = nButton == GDK_BUTTON_MIDDLE;
    if (priv->m_bMiddleClick)
    {
        if (priv->m_bEdit)
        {
            // Move the cursor there first. LOK processes mouse events
            // asynchronously; the primary read normally takes longer, but
            // the order isn't guaranteed.
            postMouseEvent(pDocView, LOK_MOUSEEVENT_MOUSEBUTTONDOWN, fX, fY, 1, MOUSE_LEFT, 0);
            postMouseEvent(pDocView, LOK_MOUSEEVENT_MOUSEBUTTONUP, fX, fY, 1, MOUSE_LEFT, 0);
            pasteFromClipboard(pDocView, true, gtk_widget_get_primary_clipboard(GTK_WIDGET(pDocView)));
        }
        return;
    }

    int nDoubleClickTime = 400;
    g_object_get(gtk_widget_get_settings(GTK_WIDGET(pDocView)), "gtk-double-click-time", &nDoubleClickTime, nullptr);
    if (nButton == priv->m_nLastPressButton && nTime - priv->m_nLastPressTime < guint32(nDoubleClickTime))
        priv->m_nClickCount = std::min(priv->m_nClickCount + 1, 3);
    else
        priv->m_nClickCount = 1;
    priv->m_nLastPressTime = nTime;
    priv->m_nLastPressButton = nButton;

    priv->m_nMouseButtons |= toLOKButton(nButton);
    // TODO: hit-test selection/graphic handles first (upstream handleTextSelectionOnButtonPress etc.).
    postMouseEvent(pDocView, LOK_MOUSEEVENT_MOUSEBUTTONDOWN, fX, fY, priv->m_nClickCount,
                   toLOKButton(nButton), toLOKModifier(gtk_event_controller_get_current_event_state(pController)));
}

static void onDragUpdate(GtkGestureDrag* pGesture, double fOffsetX, double fOffsetY, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (priv->m_bMiddleClick)
        return;
    double fStartX, fStartY;
    gtk_gesture_drag_get_start_point(pGesture, &fStartX, &fStartY);
    postMouseEvent(pDocView, LOK_MOUSEEVENT_MOUSEMOVE, fStartX + fOffsetX, fStartY + fOffsetY, 1,
                   priv->m_nMouseButtons,
                   toLOKModifier(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(pGesture))));
}

static void onDragEnd(GtkGestureDrag* pGesture, double fOffsetX, double fOffsetY, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    guint nButton = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(pGesture));
    if (std::exchange(priv->m_bMiddleClick, false))
        return;
    double fStartX, fStartY;
    gtk_gesture_drag_get_start_point(pGesture, &fStartX, &fStartY);

    priv->m_nMouseButtons &= ~toLOKButton(nButton);
    postMouseEvent(pDocView, LOK_MOUSEEVENT_MOUSEBUTTONUP, fStartX + fOffsetX, fStartY + fOffsetY,
                   priv->m_nClickCount, toLOKButton(nButton),
                   toLOKModifier(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(pGesture))));
}

static bool postKey(LOKDocView* pDocView, int nType, guint nKeyval, GdkModifierType eState)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return false;
    if (!priv->m_bEdit)
    {
        g_info("postKey: not in edit mode, ignore");
        return false;
    }

    int nCharCode = 0;
    int nKeyCode = 0;
    switch (nKeyval)
    {
    case GDK_KEY_BackSpace:
        nKeyCode = AwtKey::BACKSPACE;
        break;
    case GDK_KEY_Delete:
        nKeyCode = AwtKey::DELETE;
        break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        nKeyCode = AwtKey::RETURN;
        break;
    case GDK_KEY_Escape:
        nKeyCode = AwtKey::ESCAPE;
        break;
    case GDK_KEY_Tab:
        nKeyCode = AwtKey::TAB;
        break;
    case GDK_KEY_Down:
        nKeyCode = AwtKey::DOWN;
        break;
    case GDK_KEY_Up:
        nKeyCode = AwtKey::UP;
        break;
    case GDK_KEY_Left:
        nKeyCode = AwtKey::LEFT;
        break;
    case GDK_KEY_Right:
        nKeyCode = AwtKey::RIGHT;
        break;
    case GDK_KEY_Home:
        nKeyCode = AwtKey::HOME;
        break;
    case GDK_KEY_End:
        nKeyCode = AwtKey::END;
        break;
    case GDK_KEY_Page_Down:
        nKeyCode = AwtKey::PAGEDOWN;
        break;
    case GDK_KEY_Page_Up:
        nKeyCode = AwtKey::PAGEUP;
        break;
    case GDK_KEY_Insert:
        nKeyCode = AwtKey::INSERT;
        break;
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
        // Modifiers are taken from the event state instead of being tracked.
        return false;
    default:
        if (nKeyval >= GDK_KEY_F1 && nKeyval <= GDK_KEY_F26)
            nKeyCode = AwtKey::F1 + (nKeyval - GDK_KEY_F1);
        else
            nCharCode = gdk_keyval_to_unicode(nKeyval);
    }

    int nModifier = toLOKModifier(eState);
    nKeyCode |= nModifier;
    if (nModifier)
    {
        if (nKeyval >= GDK_KEY_a && nKeyval <= GDK_KEY_z)
            nKeyCode |= AwtKey::A + (nKeyval - GDK_KEY_a);
        else if (nKeyval >= GDK_KEY_A && nKeyval <= GDK_KEY_Z)
            nKeyCode |= AwtKey::A + (nKeyval - GDK_KEY_A);
        else if (nKeyval >= GDK_KEY_0 && nKeyval <= GDK_KEY_9)
            nKeyCode |= AwtKey::NUM0 + (nKeyval - GDK_KEY_0);
    }

    if (!nCharCode && !(nKeyCode & 0x0fff))
        return false;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postKeyEvent(priv->m_pDocument, nType, nCharCode, nKeyCode);
    return true;
}

// With an IM attached, the key controller only emits key-pressed for keys the
// IM didn't consume: navigation, editing and shortcut keys. Text arrives via
// the IM's commit/preedit signals below.
static gboolean onKeyPressed(GtkEventControllerKey*, guint nKeyval, guint nKeycode, GdkModifierType eState, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    setActiveView(pDocView);
    if (handleClipboardShortcut(pDocView, nKeyval, eState))
        return TRUE;
    if (!postKey(pDocView, LOK_KEYEVENT_KEYINPUT, nKeyval, eState))
        return FALSE;
    getPrivate(pDocView)->m_aKeysDown[nKeycode] = { nKeyval, eState };
    return TRUE;
}

static void onKeyReleased(GtkEventControllerKey*, guint /*nKeyval*/, guint nKeycode, GdkModifierType, gpointer pData)
{
    // Only keys whose press we posted; text typed through the IM got its
    // KEYUP together with the commit.
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    auto it = priv->m_aKeysDown.find(nKeycode);
    if (it == priv->m_aKeysDown.end())
        return;
    auto [nKeyval, eState] = it->second;
    priv->m_aKeysDown.erase(it);
    postKey(pDocView, LOK_KEYEVENT_KEYUP, nKeyval, eState);
}

/// LOK's IME path (postWindowExtTextInputEvent, window 0 = the document):
/// LOK_EXT_TEXTINPUT sets the whole composition text, LOK_EXT_TEXTINPUT_END
/// finishes it.
static void postExtTextInput(LOKDocViewPrivate& priv, int nType, const char* pText)
{
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postWindowExtTextInputEvent(priv->m_pDocument, 0, nType, pText);
}

static void onIMCommit(GtkIMContext*, const char* pText, gpointer pData)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pData));
    if (!priv->m_pDocument || !priv->m_bEdit || !pText || !*pText)
        return;

    // A single character outside a composition (plain typing, compose
    // results, most dead keys): post it as a key press, so it behaves exactly
    // like typing did before, e.g. for Writer's autocorrect.
    if (!priv->m_bPreediting && g_utf8_strlen(pText, -1) == 1)
    {
        gunichar nChar = g_utf8_get_char(pText);
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->postKeyEvent(priv->m_pDocument, LOK_KEYEVENT_KEYINPUT, nChar, 0);
        priv->m_pDocument->pClass->postKeyEvent(priv->m_pDocument, LOK_KEYEVENT_KEYUP, nChar, 0);
        return;
    }

    // Replace the composition (if any) with the committed text and finish it.
    postExtTextInput(priv, LOK_EXT_TEXTINPUT, pText);
    postExtTextInput(priv, LOK_EXT_TEXTINPUT_END, "");
    priv->m_bPreediting = false;
}

static void onIMPreeditChanged(GtkIMContext* pContext, gpointer pData)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pData));
    if (!priv->m_pDocument || !priv->m_bEdit)
        return;

    // LOK draws the composition (underlined) itself, so the attributes and
    // the preedit cursor position aren't needed.
    g_autofree char* pPreedit = nullptr;
    gtk_im_context_get_preedit_string(pContext, &pPreedit, nullptr, nullptr);
    if (pPreedit && *pPreedit)
    {
        postExtTextInput(priv, LOK_EXT_TEXTINPUT, pPreedit);
        priv->m_bPreediting = true;
    }
    else if (priv->m_bPreediting)
    {
        // Composition cancelled (a commit, if any, re-inserts its own text).
        postExtTextInput(priv, LOK_EXT_TEXTINPUT, "");
        postExtTextInput(priv, LOK_EXT_TEXTINPUT_END, "");
        priv->m_bPreediting = false;
    }
}

static gboolean onIMDeleteSurrounding(GtkIMContext*, int nOffset, int nChars, gpointer pData)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pData));
    // The range is [nOffset, nOffset + nChars) relative to the cursor; LOK
    // can only delete a range touching the cursor.
    if (!priv->m_pDocument || !priv->m_bEdit || nOffset > 0 || nOffset + nChars < 0)
        return FALSE;

    int nBefore = std::min(-nOffset, nChars);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->removeTextContext(priv->m_pDocument, 0, nBefore, nChars - nBefore);
    return TRUE;
}
// TODO: retrieve-surrounding (LOK has no cheap way to get the text around
// the cursor), so IMs relying on surrounding text get less context.

/// The IM is attached and focused only while editing with focus, so input
/// methods don't pop up candidate windows on a read-only view.
static void updateIMContext(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pIMContext || !priv->m_pKeyController)
        return;

    bool bActive = priv->m_bEdit && priv->m_pFocusController
                   && gtk_event_controller_focus_is_focus(priv->m_pFocusController);
    if (bActive == priv->m_bIMActive)
        return;
    priv->m_bIMActive = bActive;

    if (bActive)
    {
        gtk_event_controller_key_set_im_context(priv->m_pKeyController, priv->m_pIMContext);
        gtk_im_context_focus_in(priv->m_pIMContext);
        priv->m_aIMCursorLocation = {}; // re-send on next draw
        gtk_widget_queue_draw(GTK_WIDGET(pDocView));
    }
    else
    {
        // Drop an unfinished composition rather than leave it dangling in the document.
        if (priv->m_bPreediting && priv->m_pDocument)
        {
            postExtTextInput(priv, LOK_EXT_TEXTINPUT, "");
            postExtTextInput(priv, LOK_EXT_TEXTINPUT_END, "");
        }
        priv->m_bPreediting = false;
        gtk_im_context_reset(priv->m_pIMContext);
        gtk_im_context_focus_out(priv->m_pIMContext);
        gtk_event_controller_key_set_im_context(priv->m_pKeyController, nullptr);
    }
}

/// Tells the IM where the text cursor is (for candidate window placement).
/// Called from snapshot, which runs after anything that moves it: cursor
/// moves, scrolling, zooming.
static void updateIMCursorLocation(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_bIMActive || isEmptyRectangle(priv->m_aVisibleCursor))
        return;

    double fOriginX, fOriginY;
    getDocumentOrigin(pDocView, fOriginX, fOriginY);
    const GdkRectangle& rCursor = priv->m_aVisibleCursor;
    GdkRectangle aLocation = { static_cast<int>(fOriginX + twipToPixel(rCursor.x, priv->m_fZoom)),
                               static_cast<int>(fOriginY + twipToPixel(rCursor.y, priv->m_fZoom)),
                               std::max(1, static_cast<int>(twipToPixel(rCursor.width, priv->m_fZoom))),
                               std::max(1, static_cast<int>(twipToPixel(rCursor.height, priv->m_fZoom))) };
    if (gdk_rectangle_equal(&aLocation, &priv->m_aIMCursorLocation))
        return;
    priv->m_aIMCursorLocation = aLocation;
    gtk_im_context_set_cursor_location(priv->m_pIMContext, &aLocation);
}

static void onFocusChanged(LOKDocView* pDocView)
{
    restartCursorBlink(pDocView);
    updateIMContext(pDocView);
}

// ---------------------------------------------------------------------------
// Document loading
// ---------------------------------------------------------------------------

/**
 * Main-thread setup once documentLoad() succeeded, or a view was created
 * (upstream postDocumentLoad()). nViewId: the view this widget is for, or
 * -1 after documentLoad() (the document's only view).
 */
static void postDocumentLoad(LOKDocView* pDocView, int nViewId)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    LibreOfficeKitDocument* pDocument = priv->m_pDocument;

    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        // initializeForRendering() and registerCallback() act on the current
        // view, which is global and may have changed since the view was
        // created (e.g. another view's tile worker painting in between):
        // select ours explicitly, under the same lock.
        if (nViewId < 0 && pDocument->pClass->getViewsCount(pDocument) == 1)
            pDocument->pClass->getViewIds(pDocument, &nViewId, 1);
        if (nViewId >= 0)
            pDocument->pClass->setView(pDocument, nViewId);
        pDocument->pClass->initializeForRendering(pDocument, priv->m_aRenderingArguments.c_str());
        // initializeForRendering() may change the view id in Impress, so query it afterwards.
        priv->m_nViewId = pDocument->pClass->getView(pDocument);
        pDocument->pClass->registerCallback(pDocument, lokCallback, priv->m_pHandle);
        priv->m_eTileMode = pDocument->pClass->getTileMode(pDocument);
        priv->m_eDocumentType = pDocument->pClass->getDocumentType(pDocument);
        priv->m_nParts = pDocument->pClass->getParts(pDocument);
        priv->m_nPartId = pDocument->pClass->getPart(pDocument);
    }

    startNewTileGeneration(pDocView, false);
    refreshSize(pDocView);
    updateClientZoom(pDocView);
    g_aLiveViews.push_back(pDocView);

    setIsLoading(pDocView, false);
    priv->m_bInitialized = true;
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_IS_INITIALIZED]);
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

static void openDocumentInThread(GTask* pTask, gpointer pSource, gpointer, GCancellable*)
{
    LOKDocViewPrivate& priv = getPrivate(LOK_DOC_VIEW(pSource));

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    LibreOfficeKitDocument* pDocument
        = priv->m_pOffice->pClass->documentLoadWithOptions(priv->m_pOffice, priv->m_aDocPath.c_str(), "");
    if (!pDocument)
    {
        char* pError = priv->m_pOffice->pClass->getError(priv->m_pOffice);
        g_task_return_new_error(pTask, g_quark_from_static_string("LOK error"), 0, "%s",
                                pError ? pError : "documentLoad() failed");
        free(pError);
        return;
    }
    g_task_return_pointer(pTask, pDocument, nullptr);
}

static void openDocumentDone(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pSource);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GTask* pOuterTask = G_TASK(pData);
    GError* pError = nullptr;

    if (priv->m_pHeldApplication)
    {
        g_debug("load finished, releasing application");
        g_application_release(priv->m_pHeldApplication);
        g_clear_object(&priv->m_pHeldApplication);
    }

    auto* pDocument = static_cast<LibreOfficeKitDocument*>(g_task_propagate_pointer(G_TASK(pResult), &pError));
    if (!pDocument || priv->m_bDisposed)
    {
        if (pDocument)
        {
            // Explicitly disposed mid-load: dispose had no document to close yet.
            std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
            pDocument->pClass->destroy(pDocument);
            if (priv->m_bDisposed && priv->m_pOffice && priv->m_pOffice->pClass->getDocsCount(priv->m_pOffice) == 0)
            {
                priv->m_pOffice->pClass->destroy(priv->m_pOffice);
                priv->m_pOffice = nullptr;
            }
        }
        setIsLoading(pDocView, false);
        if (pError)
            g_task_return_error(pOuterTask, pError);
        else
            g_task_return_new_error(pOuterTask, G_IO_ERROR, G_IO_ERROR_CANCELLED, "view disposed during load");
        g_object_unref(pOuterTask);
        return;
    }

    priv->m_pDocument = pDocument;
    postDocumentLoad(pDocView, -1);
    g_task_return_boolean(pOuterTask, TRUE);
    g_object_unref(pOuterTask);
}

// ---------------------------------------------------------------------------
// GObject / GtkWidget
// ---------------------------------------------------------------------------

static void lok_doc_view_init (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv.m_pImpl = new LOKDocViewPrivateImpl();
    priv->m_pHandle = new ViewHandle;
    g_weak_ref_init(&priv->m_pHandle->m_aView, pDocView);
    priv->m_pClipboardCancellable = g_cancellable_new();
    priv->m_pPaintPool = g_thread_pool_new_full(paintTileInThread, nullptr, freePaintRequest,
                                                /*max_threads=*/1, /*exclusive=*/FALSE, nullptr);
    g_thread_pool_set_sort_function(priv->m_pPaintPool, comparePaintRequests, nullptr);

    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    gtk_widget_set_focusable(pWidget, TRUE);
    gtk_widget_set_overflow(pWidget, GTK_OVERFLOW_HIDDEN);
    priv->m_fScale = gtk_widget_get_scale_factor(pWidget); // until realized, see lok_doc_view_realize()

    setAdjustment(pDocView, priv->m_pHAdjustment, nullptr);
    setAdjustment(pDocView, priv->m_pVAdjustment, nullptr);

    GtkGesture* pDrag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pDrag), 0);
    g_signal_connect(pDrag, "drag-begin", G_CALLBACK(onDragBegin), pDocView);
    g_signal_connect(pDrag, "drag-update", G_CALLBACK(onDragUpdate), pDocView);
    g_signal_connect(pDrag, "drag-end", G_CALLBACK(onDragEnd), pDocView);
    gtk_widget_add_controller(pWidget, GTK_EVENT_CONTROLLER(pDrag));

    GtkEventController* pMotion = gtk_event_controller_motion_new();
    g_signal_connect(pMotion, "motion", G_CALLBACK(onMotion), pDocView);
    g_signal_connect(pMotion, "leave", G_CALLBACK(onPointerLeave), pDocView);
    gtk_widget_add_controller(pWidget, pMotion);

    GtkEventController* pScroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(pScroll, "scroll", G_CALLBACK(onScroll), pDocView);
    gtk_widget_add_controller(pWidget, pScroll);

    GtkGesture* pPinch = gtk_gesture_zoom_new();
    g_signal_connect(pPinch, "begin", G_CALLBACK(onPinchBegin), pDocView);
    g_signal_connect(pPinch, "scale-changed", G_CALLBACK(onPinchScaleChanged), pDocView);
    gtk_widget_add_controller(pWidget, GTK_EVENT_CONTROLLER(pPinch));

    GtkEventController* pKey = gtk_event_controller_key_new();
    priv->m_pKeyController = GTK_EVENT_CONTROLLER_KEY(pKey);
    g_signal_connect(pKey, "key-pressed", G_CALLBACK(onKeyPressed), pDocView);
    g_signal_connect(pKey, "key-released", G_CALLBACK(onKeyReleased), pDocView);
    gtk_widget_add_controller(pWidget, pKey);

    priv->m_pIMContext = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(priv->m_pIMContext, pWidget);
    g_signal_connect(priv->m_pIMContext, "commit", G_CALLBACK(onIMCommit), pDocView);
    g_signal_connect(priv->m_pIMContext, "preedit-changed", G_CALLBACK(onIMPreeditChanged), pDocView);
    g_signal_connect(priv->m_pIMContext, "delete-surrounding", G_CALLBACK(onIMDeleteSurrounding), pDocView);

    // The cursor is only shown (and blinks), and the IM only active, while focused.
    GtkEventController* pFocus = gtk_event_controller_focus_new();
    priv->m_pFocusController = GTK_EVENT_CONTROLLER_FOCUS(pFocus);
    g_signal_connect_swapped(pFocus, "enter", G_CALLBACK(onFocusChanged), pDocView);
    g_signal_connect_swapped(pFocus, "leave", G_CALLBACK(onFocusChanged), pDocView);
    gtk_widget_add_controller(pWidget, pFocus);

    // Covers backends without a fractional surface scale (X11).
    g_signal_connect(pDocView, "notify::scale-factor", G_CALLBACK(onScaleChanged), pDocView);
}

static void lok_doc_view_set_property (GObject* object, guint propId, const GValue *value, GParamSpec *pspec)
{
    LOKDocView* pDocView = LOK_DOC_VIEW (object);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    switch (propId)
    {
    case PROP_LO_PATH:
        priv->m_aLOPath = g_value_get_string (value) ? g_value_get_string (value) : "";
        break;
    case PROP_LO_UNIPOLL:
        priv->m_bUnipoll = g_value_get_boolean (value);
        break;
    case PROP_LO_POINTER:
        priv->m_pOffice = static_cast<LibreOfficeKit*>(g_value_get_pointer(value));
        break;
    case PROP_USER_PROFILE_URL:
        if (const gchar* pUserProfile = g_value_get_string(value))
            priv->m_aUserProfileURL = pUserProfile;
        break;
    case PROP_DOC_PATH:
        priv->m_aDocPath = g_value_get_string (value) ? g_value_get_string (value) : "";
        break;
    case PROP_DOC_POINTER:
        priv->m_pDocument = static_cast<LibreOfficeKitDocument*>(g_value_get_pointer(value));
        break;
    case PROP_EDITABLE:
        lok_doc_view_set_edit (pDocView, g_value_get_boolean (value));
        break;
    case PROP_ZOOM:
        lok_doc_view_set_zoom (pDocView, g_value_get_float (value));
        break;
    case PROP_DOC_WIDTH:
        priv->m_nDocumentWidthTwips = g_value_get_long (value);
        break;
    case PROP_DOC_HEIGHT:
        priv->m_nDocumentHeightTwips = g_value_get_long (value);
        break;
    case PROP_DOC_PASSWORD:
        priv->m_bDocPassword = g_value_get_boolean (value);
        applyOptionalFeatures(priv);
        break;
    case PROP_DOC_PASSWORD_TO_MODIFY:
        priv->m_bDocPasswordToModify = g_value_get_boolean (value);
        applyOptionalFeatures(priv);
        break;
    case PROP_TILED_ANNOTATIONS:
        priv->m_bTiledAnnotations = g_value_get_boolean (value);
        applyOptionalFeatures(priv);
        break;
    case PROP_HADJUSTMENT:
        setAdjustment(pDocView, priv->m_pHAdjustment, GTK_ADJUSTMENT(g_value_get_object(value)));
        break;
    case PROP_VADJUSTMENT:
        setAdjustment(pDocView, priv->m_pVAdjustment, GTK_ADJUSTMENT(g_value_get_object(value)));
        break;
    case PROP_HSCROLL_POLICY:
        priv->m_eHScrollPolicy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
        gtk_widget_queue_resize(GTK_WIDGET(pDocView));
        break;
    case PROP_VSCROLL_POLICY:
        priv->m_eVScrollPolicy = static_cast<GtkScrollablePolicy>(g_value_get_enum(value));
        gtk_widget_queue_resize(GTK_WIDGET(pDocView));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propId, pspec);
    }
}

static void lok_doc_view_get_property (GObject* object, guint propId, GValue *value, GParamSpec *pspec)
{
    LOKDocView* pDocView = LOK_DOC_VIEW (object);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    switch (propId)
    {
    case PROP_LO_PATH:
        g_value_set_string (value, priv->m_aLOPath.c_str());
        break;
    case PROP_LO_UNIPOLL:
        g_value_set_boolean (value, priv->m_bUnipoll);
        break;
    case PROP_LO_POINTER:
        g_value_set_pointer(value, priv->m_pOffice);
        break;
    case PROP_USER_PROFILE_URL:
        g_value_set_string(value, priv->m_aUserProfileURL.c_str());
        break;
    case PROP_DOC_PATH:
        g_value_set_string (value, priv->m_aDocPath.c_str());
        break;
    case PROP_DOC_POINTER:
        g_value_set_pointer(value, priv->m_pDocument);
        break;
    case PROP_EDITABLE:
        g_value_set_boolean (value, priv->m_bEdit);
        break;
    case PROP_LOAD_PROGRESS:
        g_value_set_double (value, priv->m_fLoadProgress);
        break;
    case PROP_ZOOM:
        g_value_set_float (value, priv->m_fZoom);
        break;
    case PROP_IS_LOADING:
        g_value_set_boolean (value, priv->m_bIsLoading);
        break;
    case PROP_IS_INITIALIZED:
        g_value_set_boolean (value, priv->m_bInitialized);
        break;
    case PROP_DOC_WIDTH:
        g_value_set_long (value, priv->m_nDocumentWidthTwips);
        break;
    case PROP_DOC_HEIGHT:
        g_value_set_long (value, priv->m_nDocumentHeightTwips);
        break;
    case PROP_CAN_ZOOM_IN:
        g_value_set_boolean (value, priv->m_bCanZoomIn);
        break;
    case PROP_CAN_ZOOM_OUT:
        g_value_set_boolean (value, priv->m_bCanZoomOut);
        break;
    case PROP_DOC_PASSWORD:
        g_value_set_boolean (value, priv->m_bDocPassword);
        break;
    case PROP_DOC_PASSWORD_TO_MODIFY:
        g_value_set_boolean (value, priv->m_bDocPasswordToModify);
        break;
    case PROP_TILED_ANNOTATIONS:
        g_value_set_boolean (value, priv->m_bTiledAnnotations);
        break;
    case PROP_HADJUSTMENT:
        g_value_set_object(value, priv->m_pHAdjustment);
        break;
    case PROP_VADJUSTMENT:
        g_value_set_object(value, priv->m_pVAdjustment);
        break;
    case PROP_HSCROLL_POLICY:
        g_value_set_enum(value, priv->m_eHScrollPolicy);
        break;
    case PROP_VSCROLL_POLICY:
        g_value_set_enum(value, priv->m_eVScrollPolicy);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propId, pspec);
    }
}

// GTK4 has no GtkWidget::destroy; close the document in dispose, which runs
// promptly when the widget is removed (finalize may be delayed by bindings'
// GC, see rhbz#1444437).
static void lok_doc_view_dispose (GObject* object)
{
    LOKDocView* pDocView = LOK_DOC_VIEW (object);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    if (!priv->m_bDisposed)
    {
        g_debug("dispose: start");
        priv->m_bDisposed = true;
        g_weak_ref_set(&priv->m_pHandle->m_aView, nullptr);
        g_clear_handle_id(&priv->m_nBlinkTimeoutId, g_source_remove);
        g_clear_handle_id(&priv->m_nHoverRetryId, g_source_remove);
        g_clear_handle_id(&priv->m_nClientViewIdleId, g_source_remove);
        g_clear_handle_id(&priv->m_nPrimaryTimeoutId, g_source_remove);
        // Before the document goes: the IM handlers post to it.
        if (priv->m_pIMContext)
        {
            g_signal_handlers_disconnect_by_data(priv->m_pIMContext, pDocView);
            gtk_im_context_set_client_widget(priv->m_pIMContext, nullptr);
            g_clear_object(&priv->m_pIMContext);
        }
        // GtkWidget's dispose frees the controllers.
        priv->m_pFocusController = nullptr;
        priv->m_pKeyController = nullptr;

        // Pending clipboard reads find the view gone via the handle; cancel
        // them anyway. What we published stays on the system clipboard.
        g_cancellable_cancel(priv->m_pClipboardCancellable);
        if (g_pLastActiveHandle == priv->m_pHandle)
            g_pLastActiveHandle = nullptr;

        // Stop painting before the document goes away: drop queued requests
        // and wait for the running one. Must not hold g_aLOKMutex here, the
        // worker needs it to finish.
        ++priv->m_pHandle->m_nTileGeneration;
        if (priv->m_pPaintPool)
        {
            g_debug("dispose: stopping paint worker");
            g_thread_pool_free(priv->m_pPaintPool, /*immediate=*/TRUE, /*wait=*/TRUE);
            priv->m_pPaintPool = nullptr;
        }

        // Tell the document's other views to drop our cursor and selections.
        std::erase(g_aLiveViews, pDocView);
        for (LOKDocView* pOther : g_aLiveViews)
        {
            LOKDocViewPrivate& rOther = getPrivate(pOther);
            if (priv->m_pDocument && rOther->m_pDocument == priv->m_pDocument
                && rOther->m_aOtherViews.erase(priv->m_nViewId))
                gtk_widget_queue_draw(GTK_WIDGET(pOther));
        }

        std::unique_lock<std::mutex> aGuard(g_aLOKMutex);
        if (priv->m_pDocument)
        {
            setDocumentView(priv->m_pDocument, priv->m_nViewId);
            priv->m_pDocument->pClass->registerCallback(priv->m_pDocument, nullptr, nullptr);

            // This call may drop several views - e.g., embedded OLE in-place clients
            g_debug("dispose: destroying view %d", priv->m_nViewId);
            priv->m_pDocument->pClass->destroyView(priv->m_pDocument, priv->m_nViewId);
            if (priv->m_pDocument->pClass->getViewsCount(priv->m_pDocument) == 0)
            {
                // Last view(s) gone
                g_debug("dispose: destroying document");
                priv->m_pDocument->pClass->destroy(priv->m_pDocument);
                if (priv->m_pOffice && priv->m_pOffice->pClass->getDocsCount(priv->m_pOffice) == 0)
                {
                    g_debug("dispose: destroying office");
                    priv->m_pOffice->pClass->destroy(priv->m_pOffice);
                    priv->m_pOffice = nullptr;
                }
            }
            priv->m_pDocument = nullptr;
        }
        aGuard.unlock();
        g_debug("dispose: done");

        priv->m_pTileBuffer = std::make_unique<TileBuffer>();
        priv->m_pPreviousTiles.reset();
    }

    G_OBJECT_CLASS (lok_doc_view_parent_class)->dispose (object);
}

static void lok_doc_view_finalize (GObject* object)
{
    LOKDocView* pDocView = LOK_DOC_VIEW (object);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    if (priv->m_pHAdjustment)
    {
        g_signal_handlers_disconnect_by_func(priv->m_pHAdjustment, reinterpret_cast<gpointer>(onAdjustmentValueChanged), pDocView);
        g_object_unref(priv->m_pHAdjustment);
    }
    if (priv->m_pVAdjustment)
    {
        g_signal_handlers_disconnect_by_func(priv->m_pVAdjustment, reinterpret_cast<gpointer>(onAdjustmentValueChanged), pDocView);
        g_object_unref(priv->m_pVAdjustment);
    }
    // m_pHandle is intentionally leaked, see ViewHandle.
    g_clear_object(&priv->m_pExportedContent);
    g_clear_pointer(&priv->m_pLastPasteData, g_bytes_unref);
    g_clear_object(&priv->m_pClipboardCancellable);

    delete priv.m_pImpl;
    priv.m_pImpl = nullptr;

    G_OBJECT_CLASS (lok_doc_view_parent_class)->finalize (object);
}

// kicks the mainloop awake
static gboolean timeout_wakeup(void *)
{
    return FALSE;
}

// integrate our mainloop with LOK's
static int lok_poll_callback(void*, int timeoutUs)
{
    bool bWasEvent(false);
    if (timeoutUs > 0)
    {
        guint timeout = g_timeout_add(timeoutUs / 1000, timeout_wakeup, nullptr);
        bWasEvent = g_main_context_iteration(nullptr, true);
        g_source_remove(timeout);
    }
    else
        bWasEvent = g_main_context_iteration(nullptr, timeoutUs < 0);

    return bWasEvent ? 1 : 0;
}

// thread-safe wakeup of our mainloop
static void lok_wake_callback(void *)
{
    g_main_context_wakeup(nullptr);
}

static gboolean spin_lok_loop(void *pData)
{
    LOKDocView *pDocView = LOK_DOC_VIEW (pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_pOffice->pClass->runLoop(priv->m_pOffice, lok_poll_callback, lok_wake_callback, nullptr);
    return FALSE;
}

static gboolean lok_doc_view_initable_init (GInitable *initable, GCancellable* /*cancellable*/, GError **error)
{
    LOKDocView *pDocView = LOK_DOC_VIEW (initable);
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    if (priv->m_pOffice != nullptr)
        return true;

    if (priv->m_bUnipoll)
        (void)g_setenv("SAL_LOK_OPTIONS", "unipoll", FALSE);

    priv->m_pOffice = lok_init_2(priv->m_aLOPath.c_str(), priv->m_aUserProfileURL.empty() ? nullptr : priv->m_aUserProfileURL.c_str());

    if (priv->m_pOffice == nullptr)
    {
        g_set_error (error,
                     g_quark_from_static_string ("LOK initialization error"), 0,
                     "Failed to get LibreOfficeKit context. Make sure path (%s) is correct",
                     priv->m_aLOPath.c_str());
        return FALSE;
    }
    applyOptionalFeatures(priv);

    if (priv->m_bUnipoll)
        g_idle_add(spin_lok_loop, pDocView);

    return true;
}

static void lok_doc_view_initable_iface_init (GInitableIface *iface)
{
    iface->init = lok_doc_view_initable_init;
}

static void lok_doc_view_class_init (LOKDocViewClass* pClass)
{
    GObjectClass *pGObjectClass = G_OBJECT_CLASS(pClass);
    GtkWidgetClass *pWidgetClass = GTK_WIDGET_CLASS(pClass);

    pGObjectClass->get_property = lok_doc_view_get_property;
    pGObjectClass->set_property = lok_doc_view_set_property;
    pGObjectClass->dispose = lok_doc_view_dispose;
    pGObjectClass->finalize = lok_doc_view_finalize;

    pWidgetClass->snapshot = lok_doc_view_snapshot;
    pWidgetClass->measure = lok_doc_view_measure;
    pWidgetClass->size_allocate = lok_doc_view_size_allocate;
    pWidgetClass->realize = lok_doc_view_realize;
    pWidgetClass->unrealize = lok_doc_view_unrealize;

    gtk_widget_class_set_css_name(pWidgetClass, "lokdocview");

    /**
     * LOKDocView:lopath:
     *
     * The absolute path of the LibreOffice install.
     */
    properties[PROP_LO_PATH] =
        g_param_spec_string("lopath",
                            "LO Path",
                            "LibreOffice Install Path",
                            nullptr,
                            static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:unipoll:
     *
     * Whether we use our own unified polling mainloop in place of glib's
     */
    properties[PROP_LO_UNIPOLL] =
        g_param_spec_boolean("unipoll",
                             "Unified Polling",
                             "Whether we use a custom unified polling loop",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));
    /**
     * LOKDocView:lopointer:
     *
     * A LibreOfficeKit* in case lok_init() is already called
     * previously.
     */
    properties[PROP_LO_POINTER] =
        g_param_spec_pointer("lopointer",
                             "LO Pointer",
                             "A LibreOfficeKit* from lok_init()",
                             static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:userprofileurl:
     *
     * The absolute path of the LibreOffice user profile.
     */
    properties[PROP_USER_PROFILE_URL] =
        g_param_spec_string("userprofileurl",
                            "User profile path",
                            "LibreOffice user profile path",
                            nullptr,
                            static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                     G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:docpath:
     *
     * The path of the document that is currently being viewed.
     */
    properties[PROP_DOC_PATH] =
        g_param_spec_string("docpath",
                            "Document Path",
                            "The URI of the document to open",
                            nullptr,
                            static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                     G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:docpointer:
     *
     * A LibreOfficeKitDocument* in case documentLoad() is already called
     * previously.
     */
    properties[PROP_DOC_POINTER] =
        g_param_spec_pointer("docpointer",
                             "Document Pointer",
                             "A LibreOfficeKitDocument* from documentLoad()",
                             static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:editable:
     *
     * Whether the document loaded inside of #LOKDocView is editable or not.
     */
    properties[PROP_EDITABLE] =
        g_param_spec_boolean("editable",
                             "Editable",
                             "Whether the content is in edit mode or not",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:load-progress:
     *
     * The percent completion of the current loading operation of the
     * document. This can be used for progress bars. Note that this is not a
     * very accurate progress indicator, and its value might reset it couple of
     * times to 0 and start again. You should not rely on its numbers.
     */
    properties[PROP_LOAD_PROGRESS] =
        g_param_spec_double("load-progress",
                            "Estimated Load Progress",
                            "Shows the progress of the document load operation",
                            0.0, 1.0, 0.0,
                            static_cast<GParamFlags>(G_PARAM_READABLE |
                                                     G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:zoom-level:
     *
     * The current zoom level of the document loaded inside #LOKDocView. The
     * default value is 1.0.
     */
    properties[PROP_ZOOM] =
        g_param_spec_float("zoom-level",
                           "Zoom Level",
                           "The current zoom level of the content",
                           0, 5.0, 1.0,
                           static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                    G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:is-loading:
     *
     * Whether the requested document is being loaded or not. %TRUE if it is
     * being loaded, otherwise %FALSE.
     */
    properties[PROP_IS_LOADING] =
        g_param_spec_boolean("is-loading",
                             "Is Loading",
                             "Whether the view is loading a document",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:is-initialized:
     *
     * Whether the requested document has completely loaded or not.
     */
    properties[PROP_IS_INITIALIZED] =
        g_param_spec_boolean("is-initialized",
                             "Has initialized",
                             "Whether the view has completely initialized",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:doc-width:
     *
     * The width of the currently loaded document in #LOKDocView in twips.
     */
    properties[PROP_DOC_WIDTH] =
        g_param_spec_long("doc-width",
                          "Document Width",
                          "Width of the document in twips",
                          0, G_MAXLONG, 0,
                          static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                   G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:doc-height:
     *
     * The height of the currently loaded document in #LOKDocView in twips.
     */
    properties[PROP_DOC_HEIGHT] =
        g_param_spec_long("doc-height",
                          "Document Height",
                          "Height of the document in twips",
                          0, G_MAXLONG, 0,
                          static_cast<GParamFlags>(G_PARAM_READWRITE |
                                                   G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:can-zoom-in:
     *
     * It tells whether the view can further be zoomed in or not.
     */
    properties[PROP_CAN_ZOOM_IN] =
        g_param_spec_boolean("can-zoom-in",
                             "Can Zoom In",
                             "Whether the view can be zoomed in further",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READABLE
                                                      | G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:can-zoom-out:
     *
     * It tells whether the view can further be zoomed out or not.
     */
    properties[PROP_CAN_ZOOM_OUT] =
        g_param_spec_boolean("can-zoom-out",
                             "Can Zoom Out",
                             "Whether the view can be zoomed out further",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READABLE
                                                      | G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:doc-password:
     *
     * Set it to true if client supports providing password for viewing
     * password protected documents
     */
    properties[PROP_DOC_PASSWORD] =
        g_param_spec_boolean("doc-password",
                             "Document password capability",
                             "Whether client supports providing document passwords",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READWRITE
                                                      | G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:doc-password-to-modify:
     *
     * Set it to true if client supports providing password for edit-protected documents
     */
    properties[PROP_DOC_PASSWORD_TO_MODIFY] =
        g_param_spec_boolean("doc-password-to-modify",
                             "Edit document password capability",
                             "Whether the client supports providing passwords to edit documents",
                             FALSE,
                             static_cast<GParamFlags>(G_PARAM_READWRITE
                                                      | G_PARAM_STATIC_STRINGS));

    /**
     * LOKDocView:tiled-annotations-rendering:
     *
     * Set it to false if client does not want LO to render comments in tiles and
     * instead interested in using comments API to access comments
     */
    properties[PROP_TILED_ANNOTATIONS] =
        g_param_spec_boolean("tiled-annotations",
                             "Render comments in tiles",
                             "Whether the client wants in tile comment rendering",
                             true,
                             static_cast<GParamFlags>(G_PARAM_READWRITE
                                                      | G_PARAM_STATIC_STRINGS));

    g_object_class_install_properties(pGObjectClass, PROP_LAST, properties);

    g_object_class_override_property(pGObjectClass, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(pGObjectClass, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(pGObjectClass, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(pGObjectClass, PROP_VSCROLL_POLICY, "vscroll-policy");

    /**
     * LOKDocView::load-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @fLoadProgress: the new progress value
     */
    doc_view_signals[LOAD_CHANGED] =
        g_signal_new("load-changed",
                     G_TYPE_FROM_CLASS (pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__DOUBLE,
                     G_TYPE_NONE, 1,
                     G_TYPE_DOUBLE);

    /**
     * LOKDocView::edit-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @bEdit: the new edit value of the view
     */
    doc_view_signals[EDIT_CHANGED] =
        g_signal_new("edit-changed",
                     G_TYPE_FROM_CLASS (pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__BOOLEAN,
                     G_TYPE_NONE, 1,
                     G_TYPE_BOOLEAN);

    /**
     * LOKDocView::command-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: the command that was changed
     */
    doc_view_signals[COMMAND_CHANGED] =
        g_signal_new("command-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::search-not-found:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: the string for which the search was not found.
     */
    doc_view_signals[SEARCH_NOT_FOUND] =
        g_signal_new("search-not-found",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::part-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: the part number which the view changed to
     */
    doc_view_signals[PART_CHANGED] =
        g_signal_new("part-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__INT,
                     G_TYPE_NONE, 1,
                     G_TYPE_INT);

    /**
     * LOKDocView::size-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: NULL, we just notify that want to notify the UI elements that are interested.
     */
    doc_view_signals[SIZE_CHANGED] =
        g_signal_new("size-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__INT,
                     G_TYPE_NONE, 1,
                     G_TYPE_INT);

    /**
     * LOKDocView::hyperlinked-clicked:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aHyperlink: the URI which the application should handle
     */
    doc_view_signals[HYPERLINK_CLICKED] =
        g_signal_new("hyperlink-clicked",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::cursor-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @nX: The new cursor position (X coordinate) in pixels
     * @nY: The new cursor position (Y coordinate) in pixels
     * @nWidth: The width of new cursor
     * @nHeight: The height of new cursor
     */
    doc_view_signals[CURSOR_CHANGED] =
        g_signal_new("cursor-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 4,
                     G_TYPE_INT, G_TYPE_INT,
                     G_TYPE_INT, G_TYPE_INT);

    /**
     * LOKDocView::search-result-count:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: number of matches.
     */
    doc_view_signals[SEARCH_RESULT_COUNT] =
        g_signal_new("search-result-count",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::command-result:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: JSON containing the info about the command that finished,
     * and its success status.
     */
    doc_view_signals[COMMAND_RESULT] =
        g_signal_new("command-result",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::address-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: formula text content
     */
    doc_view_signals[ADDRESS_CHANGED] =
        g_signal_new("address-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::formula-changed:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @aCommand: formula text content
     */
    doc_view_signals[FORMULA_CHANGED] =
        g_signal_new("formula-changed",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__STRING,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::text-selection:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @bIsTextSelected: whether text selected is non-null
     */
    doc_view_signals[TEXT_SELECTION] =
        g_signal_new("text-selection",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_VOID__BOOLEAN,
                     G_TYPE_NONE, 1,
                     G_TYPE_BOOLEAN);

    /**
     * LOKDocView::content-control:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pPayload: the JSON string containing the information about ruler properties
     */
    doc_view_signals[CONTENT_CONTROL] =
        g_signal_new("content-control",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::password-required:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pUrl: URL of the document for which password is required
     * @bModify: whether password id required to modify the document
     * This is true when password is required to edit the document,
     * while it can still be viewed without password. In such cases, provide a NULL
     * password for read-only access to the document.
     * If false, password is required for opening the document, and document
     * cannot be opened without providing a valid password.
     *
     * Password must be provided by calling lok_doc_view_set_document_password
     * function with pUrl as provided by the callback.
     *
     * Upon entering an invalid password, another `password-required` signal is
     * emitted.
     * Upon entering a valid password, document starts to load.
     * Upon entering a NULL password: if bModify is %TRUE, document starts to
     * open in view-only mode, else loading of document is aborted.
     */
    doc_view_signals[PASSWORD_REQUIRED] =
        g_signal_new("password-required",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 2,
                     G_TYPE_STRING,
                     G_TYPE_BOOLEAN);

    /**
     * LOKDocView::comment:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pComment: the JSON string containing comment notification
     * 'action' can be 'Add', 'Remove' or 'Modify' depending on whether
     *  comment has been added, removed or modified.
     * 'parent' is a non-zero comment id if this comment is a reply comment,
     *  otherwise it's a root comment.
     */
    doc_view_signals[COMMENT] =
        g_signal_new("comment",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::ruler:
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pPayload: the JSON string containing the information about ruler properties
     */
    doc_view_signals[RULER] =
        g_signal_new("ruler",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::window::
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pPayload: the JSON string containing the information about the window
     *
     * This signal emits information about external windows like dialogs, autopopups for now.
     */
    doc_view_signals[WINDOW] =
        g_signal_new("window",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * LOKDocView::invalidate-header::
     * @pDocView: the #LOKDocView on which the signal is emitted
     * @pPayload: can be either "row", "column", or "all".
     *
     * The column/row header is no more valid because of a column/row insertion
     * or a similar event. Clients must query a new column/row header set.
     */
    doc_view_signals[INVALIDATE_HEADER] =
        g_signal_new("invalidate-header",
                     G_TYPE_FROM_CLASS(pGObjectClass),
                     G_SIGNAL_RUN_FIRST,
                     0,
                     nullptr, nullptr,
                     g_cclosure_marshal_generic,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void
LOKPostCommand (LOKDocView* pDocView,
                const gchar* pCommand,
                const gchar* pArguments,
                bool bNotifyWhenFinished)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->postUnoCommand(priv->m_pDocument, pCommand, pArguments, bNotifyWhenFinished);
}

/**
 * Commands that only change how the view displays the document, never the
 * document itself, so lok_doc_view_post_command() lets them through in
 * view-only mode. (The GTK3 widget dropped every command in view-only mode.)
 */
static bool isViewOnlyCommand(const gchar* pCommand)
{
    static const char* const aCommands[] = {
        ".uno:ShowBoundaries",     // Writer formatting aids, LibreOffice >= 25.2
        ".uno:TableBoundaries",    // deprecated 25.2
        ".uno:ViewBounds",         // text boundaries, deprecated 25.2
        ".uno:SectionBoundaries",  // deprecated 25.2
        ".uno:Copy",               // reads the selection only
    };
    for (const char* pViewCommand : aCommands)
        if (g_strcmp0(pCommand, pViewCommand) == 0)
            return true;
    return false;
}

static void addTypedValue(JsonBuilder* pBuilder, const char* pName, const char* pType, auto&& fnValue)
{
    json_builder_set_member_name(pBuilder, pName);
    json_builder_begin_object(pBuilder);
    json_builder_set_member_name(pBuilder, "type");
    json_builder_add_string_value(pBuilder, pType);
    json_builder_set_member_name(pBuilder, "value");
    fnValue();
    json_builder_end_object(pBuilder);
}

static void
doSearch(LOKDocView* pDocView, const char* pText, bool bBackwards, bool highlightAll)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    // Start searching from the top-left of the visible area.
    int nX, nY;
    widgetToTwip(pDocView, 0, 0, nX, nY);
    nX = std::max(nX, 0);
    nY = std::max(nY, 0);

    g_autoptr(JsonBuilder) pBuilder = json_builder_new();
    json_builder_begin_object(pBuilder);
    addTypedValue(pBuilder, "SearchItem.SearchString", "string",
                  [&] { json_builder_add_string_value(pBuilder, pText); });
    addTypedValue(pBuilder, "SearchItem.Backward", "boolean",
                  [&] { json_builder_add_boolean_value(pBuilder, bBackwards); });
    if (highlightAll)
    {
        // SvxSearchCmd::FIND_ALL
        addTypedValue(pBuilder, "SearchItem.Command", "unsigned short",
                      [&] { json_builder_add_string_value(pBuilder, "1"); });
    }
    addTypedValue(pBuilder, "SearchItem.SearchStartPointX", "long",
                  [&] { json_builder_add_int_value(pBuilder, nX); });
    addTypedValue(pBuilder, "SearchItem.SearchStartPointY", "long",
                  [&] { json_builder_add_int_value(pBuilder, nY); });
    json_builder_end_object(pBuilder);

    g_autoptr(JsonNode) pRoot = json_builder_get_root(pBuilder);
    g_autofree gchar* pArguments = json_to_string(pRoot, FALSE);
    LOKPostCommand(pDocView, ".uno:ExecuteSearch", pArguments, false);
}

static bool isLOProgramDir(const char* pDir)
{
    g_autofree gchar* pMerged = g_build_filename(pDir, "libmergedlo.so", nullptr);
    g_autofree gchar* pApp = g_build_filename(pDir, "libsofficeapp.so", nullptr);
    return g_file_test(pMerged, G_FILE_TEST_EXISTS) || g_file_test(pApp, G_FILE_TEST_EXISTS);
}

/// Program dir of a LibreOffice launcher on PATH, resolved through symlinks
/// the same way LibreOffice's own soffice script finds itself.
static std::string findLOProgramDirFromPath()
{
    for (const char* pLauncher : { "soffice", "libreoffice" })
    {
        g_autofree gchar* pFound = g_find_program_in_path(pLauncher);
        if (!pFound)
            continue;
        char* pReal = realpath(pFound, nullptr);
        if (!pReal)
            continue;
        g_autofree gchar* pDir = g_path_get_dirname(pReal);
        free(pReal);
        if (isLOProgramDir(pDir))
            return pDir;
    }
    return std::string();
}

/// Newest /opt/libreoffice*/program (upstream tarball/RPM layout, e.g. /opt/libreoffice26.8).
static std::string findLOProgramDirInOpt()
{
    GDir* pOpt = g_dir_open("/opt", 0, nullptr);
    if (!pOpt)
        return std::string();

    std::string aBest;
    while (const gchar* pName = g_dir_read_name(pOpt))
    {
        if (!g_str_has_prefix(pName, "libreoffice"))
            continue;
        g_autofree gchar* pDir = g_build_filename("/opt", pName, "program", nullptr);
        // Compare as version strings (GNU strverscmp) so libreoffice26.8 beats libreoffice7.6.
        if (isLOProgramDir(pDir) && (aBest.empty() || strverscmp(pDir, aBest.c_str()) > 0))
            aBest = pDir;
    }
    g_dir_close(pOpt);
    return aBest;
}

/**
 * Resolves the LibreOffice program directory when the caller passes none:
 * $LOK_PATH, then the build-time default, then a launcher on PATH, then
 * well-known locations. Falls back to the build-time default so the
 * GInitable error names a concrete path.
 */
static std::string getDefaultLOPath()
{
    if (const char* pEnv = g_getenv("LOK_PATH"); pEnv && *pEnv)
        return pEnv;

    if (isLOProgramDir(LOK_PATH))
        return LOK_PATH;

    if (std::string aDir = findLOProgramDirFromPath(); !aDir.empty())
        return aDir;

    for (const char* pCandidate : { "/usr/lib64/libreoffice/program", "/usr/lib/libreoffice/program" })
        if (isLOProgramDir(pCandidate))
            return pCandidate;

    if (std::string aDir = findLOProgramDirInOpt(); !aDir.empty())
        return aDir;

    return LOK_PATH;
}

LOK_DOC_VIEW_DLLPUBLIC GtkWidget*
lok_doc_view_new (const gchar* pPath, GCancellable *cancellable, GError **error)
{
    return GTK_WIDGET (g_initable_new (LOK_TYPE_DOC_VIEW, cancellable, error,
                                       "lopath", pPath == nullptr ? getDefaultLOPath().c_str() : pPath,
                                       nullptr));
}

LOK_DOC_VIEW_DLLPUBLIC GtkWidget*
lok_doc_view_new_from_user_profile (const gchar* pPath, const gchar* pUserProfile, GCancellable *cancellable, GError **error)
{
    return GTK_WIDGET(g_initable_new(LOK_TYPE_DOC_VIEW, cancellable, error,
                                     "lopath", pPath == nullptr ? getDefaultLOPath().c_str() : pPath,
                                     "userprofileurl", pUserProfile,
                                     nullptr));
}

LOK_DOC_VIEW_DLLPUBLIC GtkWidget* lok_doc_view_new_from_widget(LOKDocView* pOldLOKDocView,
                                                               const gchar* pRenderingArguments)
{
    LOKDocViewPrivate& pOldPriv = getPrivate(pOldLOKDocView);
    GtkWidget* pNewDocView = GTK_WIDGET(g_initable_new(LOK_TYPE_DOC_VIEW, /*cancellable=*/nullptr, /*error=*/nullptr,
                                                       "lopath", pOldPriv->m_aLOPath.c_str(),
                                                       "userprofileurl", pOldPriv->m_aUserProfileURL.c_str(),
                                                       "lopointer", pOldPriv->m_pOffice,
                                                       "docpointer", pOldPriv->m_pDocument,
                                                       nullptr));

    // No documentLoad(), just a createView().
    LibreOfficeKitDocument* pDocument = lok_doc_view_get_document(LOK_DOC_VIEW(pNewDocView));
    LOKDocViewPrivate& pNewPriv = getPrivate(LOK_DOC_VIEW(pNewDocView));
    int nViewId;
    {
        // postDocumentLoad() stores the final id, as initializeForRendering()
        // changes it in Impress.
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        nViewId = pDocument->pClass->createView(pDocument);
    }
    pNewPriv->m_aRenderingArguments = pRenderingArguments ? pRenderingArguments : "";

    postDocumentLoad(LOK_DOC_VIEW(pNewDocView), nViewId);
    return pNewDocView;
}

LOK_DOC_VIEW_DLLPUBLIC gboolean
lok_doc_view_open_document_finish (LOKDocView* pDocView, GAsyncResult* res, GError** error)
{
    GTask* task = G_TASK(res);

    g_return_val_if_fail(g_task_is_valid(res, pDocView), false);
    g_return_val_if_fail(g_task_get_source_tag(task) == lok_doc_view_open_document, false);
    g_return_val_if_fail(error == nullptr || *error == nullptr, false);

    return g_task_propagate_boolean(task, error);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_open_document (LOKDocView* pDocView,
                            const gchar* pPath,
                            const gchar* pRenderingArguments,
                            GCancellable* cancellable,
                            GAsyncReadyCallback callback,
                            gpointer userdata)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GTask* pOuterTask = g_task_new(pDocView, cancellable, callback, userdata);
    g_task_set_source_tag(pOuterTask, reinterpret_cast<gpointer>(lok_doc_view_open_document));

    if (priv->m_bIsLoading || priv->m_pDocument)
    {
        // TODO: support replacing the current document.
        g_task_return_new_error(pOuterTask, G_IO_ERROR, G_IO_ERROR_BUSY, "a document is already open");
        g_object_unref(pOuterTask);
        return;
    }

    g_object_set(G_OBJECT(pDocView), "docpath", pPath, nullptr);
    if (pRenderingArguments)
        priv->m_aRenderingArguments = pRenderingArguments;

    applyOptionalFeatures(priv);
    // Office-level callbacks: password requests and load progress.
    priv->m_pOffice->pClass->registerCallback(priv->m_pOffice, lokCallback, priv->m_pHandle);

    // documentLoad() can't be interrupted, and the load task keeps this view
    // alive until it returns. Keep the application running until then too:
    // exiting with LibreOffice mid-load (never destroy()ed) crashes in its
    // static destructors.
    if (GApplication* pApplication = g_application_get_default())
    {
        priv->m_pHeldApplication = G_APPLICATION(g_object_ref(pApplication));
        g_application_hold(pApplication);
    }

    setIsLoading(pDocView, true);
    GTask* pTask = g_task_new(pDocView, cancellable, openDocumentDone, pOuterTask);
    g_task_run_in_thread(pTask, openDocumentInThread);
    g_object_unref(pTask);
}

LOK_DOC_VIEW_DLLPUBLIC LibreOfficeKitDocument*
lok_doc_view_get_document (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    return priv->m_pDocument;
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_visible_area (LOKDocView* pDocView, GdkRectangle* pVisibleArea)
{
    if (!pVisibleArea)
        return;

    // The widget keeps LOK's visible area in sync with its viewport itself; an
    // area set here applies until the viewport next changes.
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_aVisibleArea = *pVisibleArea;
    scheduleClientViewUpdate(pDocView);
}

namespace {
// This used to be rtl::math::approxEqual() but since that isn't inline anymore
// in rtl/math.hxx and was moved into libuno_sal as rtl_math_approxEqual() to
// cater for representable integer cases and we don't want to link against
// libuno_sal, we'll have to have an own implementation. The special large
// integer cases seems not be needed here.
bool lok_approxEqual(double a, double b)
{
    static const double e48 = 1.0 / (16777216.0 * 16777216.0);
    if (a == b)
        return true;
    if (a == 0.0 || b == 0.0)
        return false;
    const double d = fabs(a - b);
    return (d < fabs(a) * e48 && d < fabs(b) * e48);
}
}

static gboolean applyClientViewState(gpointer pData)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pData);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_nClientViewIdleId = 0;
    if (!priv->m_pDocument)
        return G_SOURCE_REMOVE;

    if (std::exchange(priv->m_bClientZoomDirty, false))
        updateClientZoom(pDocView);

    const GdkRectangle& rArea = priv->m_aVisibleArea;
    if (rArea.width > 0 && rArea.height > 0 && !gdk_rectangle_equal(&rArea, &priv->m_aSentVisibleArea))
    {
        priv->m_aSentVisibleArea = rArea;
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->setClientVisibleArea(priv->m_pDocument, rArea.x, rArea.y, rArea.width, rArea.height);
    }
    return G_SOURCE_REMOVE;
}

/**
 * Sends the client zoom (if marked dirty) and visible area to LOK once things
 * settle: zooming and scrolling change them every frame, and each call may
 * wait for the LOK mutex behind a tile paint.
 */
static void scheduleClientViewUpdate(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_nClientViewIdleId)
        priv->m_nClientViewIdleId = g_idle_add_full(G_PRIORITY_LOW, applyClientViewState, pDocView, nullptr);
}

/**
 * Sets the zoom so that the document point under (fAnchorX, fAnchorY)
 * (widget coordinates) stays there.
 *
 * The scroll position can only be fixed up in size_allocate, once the
 * adjustments know the new document size, so the target is recorded here.
 * Several zoom steps may come before the next allocation (Ctrl+scroll,
 * pinch); they continue from the pending target rather than the stale
 * scroll position.
 */
static void setZoom(LOKDocView* pDocView, float fZoom, double fAnchorX, double fAnchorY)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    if (!priv->m_pDocument)
        return;

    // Clamp the input value in [MIN_ZOOM, MAX_ZOOM]
    fZoom = std::clamp(fZoom, MIN_ZOOM, MAX_ZOOM);

    if (lok_approxEqual(fZoom, priv->m_fZoom))
        return;

    // Document pixel under the anchor at the current zoom.
    double fDocX, fDocY;
    if (priv->m_bZoomAnchorPending)
    {
        fDocX = priv->m_fAnchorDocX + (fAnchorX - priv->m_fAnchorX);
        fDocY = priv->m_fAnchorDocY + (fAnchorY - priv->m_fAnchorY);
    }
    else
    {
        double fOriginX, fOriginY;
        getDocumentOrigin(pDocView, fOriginX, fOriginY);
        fDocX = fAnchorX - fOriginX;
        fDocY = fAnchorY - fOriginY;
    }
    double fRatio = fZoom / priv->m_fZoom;
    priv->m_fAnchorX = fAnchorX;
    priv->m_fAnchorY = fAnchorY;
    priv->m_fAnchorDocX = fDocX * fRatio;
    priv->m_fAnchorDocY = fDocY * fRatio;
    priv->m_bZoomAnchorPending = true;

    startNewTileGeneration(pDocView, true); // before m_fZoom changes, see there
    priv->m_fZoom = fZoom;
    gtk_widget_queue_resize(GTK_WIDGET(pDocView));

    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_ZOOM]);

    // set properties to indicate if view can be further zoomed in/out
    bool bCanZoomIn  = priv->m_fZoom < MAX_ZOOM;
    bool bCanZoomOut = priv->m_fZoom > MIN_ZOOM;
    if (bCanZoomIn != priv->m_bCanZoomIn)
    {
        priv->m_bCanZoomIn = bCanZoomIn;
        g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_CAN_ZOOM_IN]);
    }
    if (bCanZoomOut != priv->m_bCanZoomOut)
    {
        priv->m_bCanZoomOut = bCanZoomOut;
        g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_CAN_ZOOM_OUT]);
    }

    priv->m_bClientZoomDirty = true;
    scheduleClientViewUpdate(pDocView);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_zoom (LOKDocView* pDocView, float fZoom)
{
    // Keep the center of the view fixed.
    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    setZoom(pDocView, fZoom, gtk_widget_get_width(pWidget) / 2.0, gtk_widget_get_height(pWidget) / 2.0);
}

LOK_DOC_VIEW_DLLPUBLIC gfloat
lok_doc_view_get_zoom (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    return priv->m_fZoom;
}

LOK_DOC_VIEW_DLLPUBLIC gint
lok_doc_view_get_parts (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return -1;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->getParts( priv->m_pDocument );
}

LOK_DOC_VIEW_DLLPUBLIC gint
lok_doc_view_get_part (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return -1;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->getPart( priv->m_pDocument );
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_part (LOKDocView* pDocView, int nPart)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    // Ask LOK rather than trust the count from load time: sheets/slides may
    // have been inserted or deleted since.
    priv->m_nParts = lok_doc_view_get_parts(pDocView);
    if (nPart < 0 || nPart >= priv->m_nParts)
    {
        g_warning("Invalid part request : %d", nPart);
        return;
    }

    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->setPart( priv->m_pDocument, nPart );
    }
    priv->m_nPartId = nPart;
    lok_doc_view_reset_view(pDocView);
}

LOK_DOC_VIEW_DLLPUBLIC void lok_doc_view_send_content_control_event(LOKDocView* pDocView,
                                                                    const gchar* pArguments)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
    {
        return;
    }

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->sendContentControlEvent(priv->m_pDocument, pArguments);
}

LOK_DOC_VIEW_DLLPUBLIC gchar*
lok_doc_view_get_part_name (LOKDocView* pDocView, int nPart)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return nullptr;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->getPartName( priv->m_pDocument, nPart );
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_partmode(LOKDocView* pDocView,
                          int nPartMode)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    priv->m_pDocument->pClass->setPartMode( priv->m_pDocument, nPartMode );
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_reset_view(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    // Different content (e.g. another part): old tiles are no use as placeholder.
    startNewTileGeneration(pDocView, false);
    priv->m_fLoadProgress = 0.0;
    priv->m_nMouseButtons = 0;
    priv->m_nClickCount = 0;
    priv->m_nLastPressTime = 0;

    // Nothing is drawn until LOK reports a new cursor position anyway.
    priv->m_aVisibleCursor = {};
    priv->m_bCursorVisible = true;
    priv->m_aTextSelectionRectangles.clear();
    priv->m_aGraphicSelection = {};
    priv->m_aCellCursor = {};
    restartCursorBlink(pDocView); // stops it: no cursor

    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_edit(LOKDocView* pDocView,
                      gboolean bEdit)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    bool bWasEdit = priv->m_bEdit;
    if (bWasEdit == bool(bEdit))
        return;

    if (bWasEdit)
    {
        g_info("lok_doc_view_set_edit: leaving edit mode");
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        setDocumentView(priv->m_pDocument, priv->m_nViewId);
        priv->m_pDocument->pClass->resetSelection(priv->m_pDocument);
    }
    else
        g_info("lok_doc_view_set_edit: entering edit mode");

    priv->m_bEdit = bEdit;
    // The cursor is only shown, and the IM only active, in edit mode.
    restartCursorBlink(pDocView);
    updateIMContext(pDocView);
    // NB: the GTK3 widget emitted the *old* value here, contrary to its docs.
    g_signal_emit(pDocView, doc_view_signals[EDIT_CHANGED], 0, bEdit);
    g_object_notify_by_pspec(G_OBJECT(pDocView), properties[PROP_EDITABLE]);
    gtk_widget_queue_draw(GTK_WIDGET(pDocView));
}

LOK_DOC_VIEW_DLLPUBLIC gboolean
lok_doc_view_get_edit (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    return priv->m_bEdit;
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_post_command (LOKDocView* pDocView,
                           const gchar* pCommand,
                           const gchar* pArguments,
                           gboolean bNotifyWhenFinished)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return;

    setActiveView(pDocView);
    if (!priv->m_bEdit && !isViewOnlyCommand(pCommand))
    {
        g_info ("LOK_POST_COMMAND: ignoring commands in view-only mode");
        return;
    }

    // Paste from / copy to the system clipboard, not just LOK's internal one.
    if (g_strcmp0(pCommand, ".uno:Paste") == 0 || g_strcmp0(pCommand, ".uno:PasteUnformattedText") == 0)
    {
        pasteFromClipboard(pDocView, g_strcmp0(pCommand, ".uno:PasteUnformattedText") == 0);
        return;
    }
    if (g_strcmp0(pCommand, ".uno:Copy") == 0 || g_strcmp0(pCommand, ".uno:Cut") == 0)
    {
        postClipboardCommand(pDocView, pCommand, !bNotifyWhenFinished);
        return;
    }
    LOKPostCommand(pDocView, pCommand, pArguments, bNotifyWhenFinished);
}

LOK_DOC_VIEW_DLLPUBLIC gchar *
lok_doc_view_get_command_values (LOKDocView* pDocView,
                                 const gchar* pCommand)
{
    g_return_val_if_fail (LOK_IS_DOC_VIEW (pDocView), nullptr);
    g_return_val_if_fail (pCommand != nullptr, nullptr);

    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return nullptr;

    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->getCommandValues(priv->m_pDocument, pCommand);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_find_prev (LOKDocView* pDocView,
                        const gchar* pText,
                        gboolean bHighlightAll)
{
    doSearch(pDocView, pText, true, bHighlightAll);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_find_next (LOKDocView* pDocView,
                        const gchar* pText,
                        gboolean bHighlightAll)
{
    doSearch(pDocView, pText, false, bHighlightAll);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_highlight_all (LOKDocView* pDocView,
                            const gchar* pText)
{
    doSearch(pDocView, pText, false, true);
}

LOK_DOC_VIEW_DLLPUBLIC gchar*
lok_doc_view_copy_selection (LOKDocView* pDocView,
                             const gchar* pMimeType,
                             gchar** pUsedMimeType)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (!priv->m_pDocument)
        return nullptr;

    g_info("lok::Document::getTextSelection('%s')", pMimeType);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(priv->m_pDocument, priv->m_nViewId);
    return priv->m_pDocument->pClass->getTextSelection(priv->m_pDocument, pMimeType, pUsedMimeType);
}

LOK_DOC_VIEW_DLLPUBLIC gboolean
lok_doc_view_paste (LOKDocView* pDocView,
                    const gchar* pMimeType,
                    const gchar* pData,
                    gsize nSize)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    if (priv->m_pDocument && !priv->m_bEdit)
        g_info ("ignoring paste in view-only mode");
    return pasteData(pDocView, pMimeType, pData, nSize);
}

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_document_password (LOKDocView* pDocView,
                                    const gchar* pURL,
                                    const gchar* pPassword)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    // No g_aLOKMutex: the loading thread holds it while LOK waits for this call.
    priv->m_pOffice->pClass->setDocumentPassword(priv->m_pOffice, pURL, pPassword);
}

LOK_DOC_VIEW_DLLPUBLIC gchar*
lok_doc_view_get_version_info (LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    return priv->m_pOffice->pClass->getVersionInfo(priv->m_pOffice);
}


LOK_DOC_VIEW_DLLPUBLIC gfloat
lok_doc_view_pixel_to_twip (LOKDocView* pDocView, float fInput)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    return pixelToTwip(fInput, priv->m_fZoom);
}

LOK_DOC_VIEW_DLLPUBLIC gfloat
lok_doc_view_twip_to_pixel (LOKDocView* pDocView, float fInput)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    return twipToPixel(fInput, priv->m_fZoom);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
