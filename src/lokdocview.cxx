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
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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

    long m_nDocumentWidthTwips = 0;
    long m_nDocumentHeightTwips = 0;
    float m_fZoom = 1.0f;
    gint m_nScaleFactor = 1;

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

    GdkRectangle m_aVisibleArea = {};
    bool m_bVisibleAreaSet = false;

    GtkAdjustment* m_pHAdjustment = nullptr;
    GtkAdjustment* m_pVAdjustment = nullptr;
    GtkScrollablePolicy m_eHScrollPolicy = GTK_SCROLL_MINIMUM;
    GtkScrollablePolicy m_eVScrollPolicy = GTK_SCROLL_MINIMUM;

    /// Buttons currently held down (VCL MOUSE_* bits) and click-count state.
    int m_nMouseButtons = 0;
    int m_nClickCount = 0;
    guint32 m_nLastPressTime = 0;
    guint m_nLastPressButton = 0;

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

    double fScale = priv->m_nScaleFactor;
    rX = std::round(rX * fScale) / fScale;
    rY = std::round(rY * fScale) / fScale;
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

    int nTilePixels = nTileSizePixels * priv->m_nScaleFactor;
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
// LOK callbacks
// ---------------------------------------------------------------------------

/// Parses "x, y, width, height[, part[, mode]]" (twips) or "EMPTY[, part[, mode]]".
static bool parseInvalidation(const std::string& rPayload, GdkRectangle& rRect, bool& rbAll)
{
    rbAll = rPayload.compare(0, 5, "EMPTY") == 0;
    if (rbAll)
        return true;

    long aValues[4];
    const char* p = rPayload.c_str();
    for (long& rValue : aValues)
    {
        char* pEnd = nullptr;
        rValue = std::strtol(p, &pEnd, 10);
        if (pEnd == p)
            return false;
        p = pEnd;
        while (*p == ',' || *p == ' ')
            ++p;
    }
    rRect = { static_cast<int>(std::min<long>(aValues[0], INT_MAX)),
              static_cast<int>(std::min<long>(aValues[1], INT_MAX)),
              static_cast<int>(std::min<long>(aValues[2], INT_MAX)),
              static_cast<int>(std::min<long>(aValues[3], INT_MAX)) };
    return true;
}

static void invalidateTiles(LOKDocView* pDocView, const std::string& rPayload)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    GdkRectangle aTwips;
    bool bAll;
    if (!parseInvalidation(rPayload, aTwips, bAll))
    {
        g_warning("invalidateTiles: unexpected payload '%s'", rPayload.c_str());
        return;
    }

    // TODO: honour the part number (LOK_FEATURE_PART_IN_INVALIDATION_CALLBACK)
    // and skip invalidations for other parts.
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
        priv->m_nPartId = std::atoi(pPayload);
        g_signal_emit(pDocView, doc_view_signals[PART_CHANGED], 0, priv->m_nPartId);
        break;
    case LOK_CALLBACK_UNO_COMMAND_RESULT:
        g_signal_emit(pDocView, doc_view_signals[COMMAND_RESULT], 0, pPayload);
        break;
    case LOK_CALLBACK_CELL_ADDRESS:
        g_signal_emit(pDocView, doc_view_signals[ADDRESS_CHANGED], 0, pPayload);
        break;
    case LOK_CALLBACK_CELL_FORMULA:
        g_signal_emit(pDocView, doc_view_signals[FORMULA_CHANGED], 0, pPayload);
        break;
    case LOK_CALLBACK_TEXT_SELECTION:
        // TODO: store the selection rectangles and draw them as an overlay.
        g_signal_emit(pDocView, doc_view_signals[TEXT_SELECTION], 0, gboolean(!rPayload.empty() && rPayload != "EMPTY"));
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
    GdkTexture* m_pTexture = nullptr; // result
};
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
    if (pRequest->m_pTexture && pRequest->m_nGeneration == pRequest->m_pHandle->m_nTileGeneration)
    {
        if (gpointer pView = g_weak_ref_get(&pRequest->m_pHandle->m_aView))
        {
            LOKDocView* pDocView = LOK_DOC_VIEW(pView);
            getPrivate(pDocView)->m_pTileBuffer->setTile(pRequest->m_nColumn, pRequest->m_nRow,
                                                         std::exchange(pRequest->m_pTexture, nullptr),
                                                         pRequest->m_nSerial);
            gtk_widget_queue_draw(GTK_WIDGET(pDocView));
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

    // Skip requests superseded while queued (zoom change, part switch, dispose).
    // Dispose waits for a running paint before destroying the document.
    if (pRequest->m_nGeneration == pRequest->m_pHandle->m_nTileGeneration)
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

static void requestTile(LOKDocView* pDocView, int nColumn, int nRow, Tile& rTile)
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
                                       nTileSizePixels * priv->m_nScaleFactor,
                                       getTileSizeTwips(priv) };
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

    // TODO: request tiles nearest the viewport center first, and prefetch
    // one row/column beyond the visible area.
    bool bComplete = true;
    for (int nRow = nRowStart; nRow < nRowEnd; ++nRow)
    {
        for (int nColumn = nColStart; nColumn < nColEnd; ++nColumn)
        {
            Tile& rTile = priv->m_pTileBuffer->getTile(nColumn, nRow);
            if (!rTile.m_bValid && !rTile.m_bPending)
                requestTile(pDocView, nColumn, nRow, rTile);

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

    gtk_snapshot_pop(pSnapshot);
    gtk_snapshot_restore(pSnapshot);

    // TODO: overlays (cursor, selections, handles, graphic selection, other
    // views) — upstream renderOverlay(); use gtk_snapshot_append_color() or
    // gtk_snapshot_append_cairo().
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
    configureAdjustments(LOK_DOC_VIEW(pWidget));
}

static void onScaleFactorChanged(GObject* pObject, GParamSpec*, gpointer)
{
    LOKDocView* pDocView = LOK_DOC_VIEW(pObject);
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    // TODO: fractional scaling (gdk_surface_get_scale()).
    priv->m_nScaleFactor = gtk_widget_get_scale_factor(GTK_WIDGET(pDocView));
    // Same layout, new resolution: old tiles are a fine placeholder.
    startNewTileGeneration(pDocView, true);
    updateClientZoom(pDocView);
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

// TODO: attach a GtkIMMulticontext (gtk_event_controller_key_set_im_context)
// and post committed text, for dead keys / compose / input methods.
static gboolean onKeyPressed(GtkEventControllerKey*, guint nKeyval, guint, GdkModifierType eState, gpointer pData)
{
    return postKey(LOK_DOC_VIEW(pData), LOK_KEYEVENT_KEYINPUT, nKeyval, eState);
}

static void onKeyReleased(GtkEventControllerKey*, guint nKeyval, guint, GdkModifierType eState, gpointer pData)
{
    postKey(LOK_DOC_VIEW(pData), LOK_KEYEVENT_KEYUP, nKeyval, eState);
}

// ---------------------------------------------------------------------------
// Document loading
// ---------------------------------------------------------------------------

/// Main-thread setup once documentLoad() succeeded (upstream postDocumentLoad()).
static void postDocumentLoad(LOKDocView* pDocView)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);
    LibreOfficeKitDocument* pDocument = priv->m_pDocument;

    {
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        pDocument->pClass->initializeForRendering(pDocument, priv->m_aRenderingArguments.c_str());
        // initializeForRendering() may change the view id in Impress, so query it afterwards.
        priv->m_nViewId = pDocument->pClass->getView(pDocument);
        pDocument->pClass->registerCallback(pDocument, lokCallback, priv->m_pHandle);
        priv->m_eTileMode = pDocument->pClass->getTileMode(pDocument);
        priv->m_nParts = pDocument->pClass->getParts(pDocument);
        priv->m_nPartId = pDocument->pClass->getPart(pDocument);
    }

    startNewTileGeneration(pDocView, false);
    refreshSize(pDocView);
    updateClientZoom(pDocView);

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
    postDocumentLoad(pDocView);
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
    priv->m_pPaintPool = g_thread_pool_new_full(paintTileInThread, nullptr, freePaintRequest,
                                                /*max_threads=*/1, /*exclusive=*/FALSE, nullptr);

    GtkWidget* pWidget = GTK_WIDGET(pDocView);
    gtk_widget_set_focusable(pWidget, TRUE);
    gtk_widget_set_overflow(pWidget, GTK_OVERFLOW_HIDDEN);
    priv->m_nScaleFactor = gtk_widget_get_scale_factor(pWidget);

    setAdjustment(pDocView, priv->m_pHAdjustment, nullptr);
    setAdjustment(pDocView, priv->m_pVAdjustment, nullptr);

    GtkGesture* pDrag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pDrag), 0);
    g_signal_connect(pDrag, "drag-begin", G_CALLBACK(onDragBegin), pDocView);
    g_signal_connect(pDrag, "drag-update", G_CALLBACK(onDragUpdate), pDocView);
    g_signal_connect(pDrag, "drag-end", G_CALLBACK(onDragEnd), pDocView);
    gtk_widget_add_controller(pWidget, GTK_EVENT_CONTROLLER(pDrag));

    GtkEventController* pKey = gtk_event_controller_key_new();
    g_signal_connect(pKey, "key-pressed", G_CALLBACK(onKeyPressed), pDocView);
    g_signal_connect(pKey, "key-released", G_CALLBACK(onKeyReleased), pDocView);
    gtk_widget_add_controller(pWidget, pKey);

    g_signal_connect(pDocView, "notify::scale-factor", G_CALLBACK(onScaleFactorChanged), nullptr);
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
    {
        // Store the view id only later in postDocumentLoad(), as
        // initializeForRendering() changes the id in Impress.
        std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
        pDocument->pClass->createView(pDocument);
    }
    pNewPriv->m_aRenderingArguments = pRenderingArguments ? pRenderingArguments : "";

    postDocumentLoad(LOK_DOC_VIEW(pNewDocView));
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

    LOKDocViewPrivate& priv = getPrivate(pDocView);
    priv->m_aVisibleArea = *pVisibleArea;
    priv->m_bVisibleAreaSet = true;
    // TODO: pass on via setClientVisibleArea() before the next key event (upstream postKeyEventInThread).
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

LOK_DOC_VIEW_DLLPUBLIC void
lok_doc_view_set_zoom (LOKDocView* pDocView, float fZoom)
{
    LOKDocViewPrivate& priv = getPrivate(pDocView);

    if (!priv->m_pDocument)
        return;

    // Clamp the input value in [MIN_ZOOM, MAX_ZOOM]
    fZoom = std::clamp(fZoom, MIN_ZOOM, MAX_ZOOM);

    if (lok_approxEqual(fZoom, priv->m_fZoom))
        return;

    // TODO: keep the point under the pointer / viewport center fixed while zooming.
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

    updateClientZoom(pDocView);
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
    // TODO: reset cursor/selection/handle overlay state once ported.

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

    if (priv->m_bEdit || isViewOnlyCommand(pCommand))
        LOKPostCommand(pDocView, pCommand, pArguments, bNotifyWhenFinished);
    else
        g_info ("LOK_POST_COMMAND: ignoring commands in view-only mode");
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
    LibreOfficeKitDocument* pDocument = priv->m_pDocument;

    if (!pDocument || !pData)
        return false;

    if (!priv->m_bEdit)
    {
        g_info ("ignoring paste in view-only mode");
        return false;
    }

    g_info("lok::Document::paste('%s', %" G_GSIZE_FORMAT " bytes)", pMimeType, nSize);
    std::scoped_lock<std::mutex> aGuard(g_aLOKMutex);
    setDocumentView(pDocument, priv->m_nViewId);
    return pDocument->pClass->paste(pDocument, pMimeType, pData, nSize);
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
