/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * GTK4 test viewer for LOKDocView: zoom, edit mode, Writer boundaries,
 * bold/italic/underline, sheet/slide selector, find bar, Calc cell
 * address/formula. Plain C on purpose, so the public headers keep compiling
 * as C.
 *
 *   lokdocview4-demo [--lo-path=DIR] FILE
 */

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitGtk.h>
#include <LibreOfficeKit/LibreOfficeKitEnums.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char* g_pLOPath = NULL;

/// Formatting toggles: command, icon, tooltip.
static const struct
{
    const char* pCommand;
    const char* pIcon;
    const char* pTooltip;
} aFormatCommands[] = {
    { ".uno:Bold", "format-text-bold-symbolic", "Bold" },
    { ".uno:Italic", "format-text-italic-symbolic", "Italic" },
    { ".uno:Underline", "format-text-underline-symbolic", "Underline" },
};
#define FORMAT_COMMAND_COUNT (sizeof(aFormatCommands) / sizeof(aFormatCommands[0]))

typedef struct
{
    LOKDocView* pDocView;
    GtkWidget* pProgress;
    GtkWidget* pStatus;
    GtkWidget* pBoundaries;
    GtkWidget* aFormatButtons[FORMAT_COMMAND_COUNT];
    /// Sheet/slide selector (Calc, Impress, Draw).
    GtkWidget* pParts;
    GtkStringList* pPartNames;
    GtkWidget* pSearchBar;
    GtkWidget* pSearchEntry;
    GtkWidget* pHighlightAll;
    /// Calc: cell address and formula of the cell cursor.
    GtkWidget* pCalcBar;
    GtkWidget* pAddress;
    GtkWidget* pFormula;
    /// Set while widgets are updated from document state, so their handlers
    /// don't send that state back to the document.
    gboolean bUpdating;
} DemoWindow;

/**
 * file:// URL of a LibreOffice user profile private to this demo, so it
 * neither shares settings with nor fights over the profile lock of a desktop
 * LibreOffice. Created on first use by LibreOffice itself.
 */
static char* getUserProfileURL(void)
{
    g_autofree char* pDir = g_build_filename(g_get_user_config_dir(), "lokdocview4", NULL);
    g_mkdir_with_parents(pDir, 0700);
    return g_filename_to_uri(pDir, NULL, NULL);
}

/// The DemoWindow owned by a toplevel (freed together with it).
static DemoWindow* getDemoWindow(gpointer pAppWindow)
{
    return g_object_get_data(G_OBJECT(pAppWindow), "demo-window");
}

static void setStatus(DemoWindow* pWindow, const char* pText)
{
    gtk_label_set_text(GTK_LABEL(pWindow->pStatus), pText);
}

// --- View options --------------------------------------------------------

/// Writer formatting aids (table/text/section boundaries); LibreOffice >= 25.2.
static void applyBoundaries(DemoWindow* pWindow)
{
    gboolean bShow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pWindow->pBoundaries));
    g_autofree char* pArguments = g_strdup_printf(
        "{\"ShowBoundaries\":{\"type\":\"boolean\",\"value\":\"%s\"}}", bShow ? "true" : "false");
    lok_doc_view_post_command(pWindow->pDocView, ".uno:ShowBoundaries", pArguments, FALSE);
}

static void onBoundariesToggled(GtkToggleButton* pButton, DemoWindow* pWindow)
{
    (void)pButton;
    applyBoundaries(pWindow);
}

static void onZoom(GtkButton* pButton, DemoWindow* pWindow)
{
    float fFactor = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pButton), "zoom-in")) ? 1.25f : 0.8f;
    lok_doc_view_set_zoom(pWindow->pDocView, lok_doc_view_get_zoom(pWindow->pDocView) * fFactor);
}

static void onEditToggled(GtkToggleButton* pButton, DemoWindow* pWindow)
{
    gboolean bEdit = gtk_toggle_button_get_active(pButton);
    lok_doc_view_set_edit(pWindow->pDocView, bEdit);
    // Formatting commands are ignored in view-only mode anyway.
    for (size_t i = 0; i < FORMAT_COMMAND_COUNT; ++i)
        gtk_widget_set_sensitive(pWindow->aFormatButtons[i], bEdit);
}

// --- Formatting ----------------------------------------------------------

static void onFormatToggled(GtkToggleButton* pButton, DemoWindow* pWindow)
{
    if (pWindow->bUpdating)
        return;
    // Toggles the attribute; the resulting state comes back via command-changed.
    const char* pCommand = g_object_get_data(G_OBJECT(pButton), "command");
    lok_doc_view_post_command(pWindow->pDocView, pCommand, NULL, FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(pWindow->pDocView));
}

// --- Parts (sheets/slides) -----------------------------------------------

/// Re-reads the part names (sheets may have been added/renamed) and the current part.
static void refreshParts(DemoWindow* pWindow)
{
    int nParts = lok_doc_view_get_parts(pWindow->pDocView);
    if (nParts < 1)
        return;

    GPtrArray* pNames = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < nParts; ++i)
    {
        char* pName = lok_doc_view_get_part_name(pWindow->pDocView, i);
        g_ptr_array_add(pNames, pName ? pName : g_strdup_printf("%d", i + 1));
    }
    g_ptr_array_add(pNames, NULL);

    pWindow->bUpdating = TRUE;
    guint nOld = g_list_model_get_n_items(G_LIST_MODEL(pWindow->pPartNames));
    gtk_string_list_splice(pWindow->pPartNames, 0, nOld, (const char* const*)pNames->pdata);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(pWindow->pParts), (guint)lok_doc_view_get_part(pWindow->pDocView));
    pWindow->bUpdating = FALSE;
    g_ptr_array_unref(pNames);
}

static void onPartSelected(GtkDropDown* pDropDown, GParamSpec* pSpec, DemoWindow* pWindow)
{
    (void)pSpec;
    if (pWindow->bUpdating)
        return;
    guint nPart = gtk_drop_down_get_selected(pDropDown);
    if (nPart != GTK_INVALID_LIST_POSITION)
        lok_doc_view_set_part(pWindow->pDocView, (int)nPart);
}

// --- Find bar ------------------------------------------------------------

static void find(DemoWindow* pWindow, gboolean bBackwards)
{
    const char* pText = gtk_editable_get_text(GTK_EDITABLE(pWindow->pSearchEntry));
    if (!*pText)
        return;
    gboolean bHighlightAll = gtk_check_button_get_active(GTK_CHECK_BUTTON(pWindow->pHighlightAll));
    setStatus(pWindow, "");
    if (bBackwards)
        lok_doc_view_find_prev(pWindow->pDocView, pText, bHighlightAll);
    else
        lok_doc_view_find_next(pWindow->pDocView, pText, bHighlightAll);
}

static void onFindNext(GtkWidget* pWidget, DemoWindow* pWindow)
{
    (void)pWidget;
    find(pWindow, FALSE);
}

static void onFindPrevious(GtkWidget* pWidget, DemoWindow* pWindow)
{
    (void)pWidget;
    find(pWindow, TRUE);
}

static void onHighlightAllToggled(GtkCheckButton* pButton, DemoWindow* pWindow)
{
    const char* pText = gtk_editable_get_text(GTK_EDITABLE(pWindow->pSearchEntry));
    if (gtk_check_button_get_active(pButton) && *pText)
        lok_doc_view_highlight_all(pWindow->pDocView, pText);
}

static void onStopSearch(GtkSearchEntry* pEntry, DemoWindow* pWindow)
{
    (void)pEntry;
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(pWindow->pSearchBar), FALSE);
    gtk_widget_grab_focus(GTK_WIDGET(pWindow->pDocView));
}

/// Ctrl+F: toggle the find bar.
static gboolean onFindShortcut(GtkWidget* pAppWindow, GVariant* pArgs, gpointer pData)
{
    (void)pArgs;
    (void)pData;
    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    GtkSearchBar* pSearchBar = GTK_SEARCH_BAR(pWindow->pSearchBar);
    gboolean bShow = !gtk_search_bar_get_search_mode(pSearchBar);
    gtk_search_bar_set_search_mode(pSearchBar, bShow);
    gtk_widget_grab_focus(bShow ? pWindow->pSearchEntry : GTK_WIDGET(pWindow->pDocView));
    return TRUE;
}

// --- LOKDocView signals --------------------------------------------------
// Handlers get the toplevel, connected with g_signal_connect_object() so
// they're disconnected when it goes away: the view can outlive its window
// (a load in progress keeps it alive).

static void onLoadChanged(LOKDocView* pDocView, gdouble fProgress, GtkWindow* pAppWindow)
{
    (void)pDocView;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(getDemoWindow(pAppWindow)->pProgress), fProgress);
}

/// Payload ".uno:Bold=true" etc.; only the formatting toggles care.
static void onCommandChanged(LOKDocView* pDocView, const gchar* pPayload, GtkWindow* pAppWindow)
{
    (void)pDocView;
    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    for (size_t i = 0; i < FORMAT_COMMAND_COUNT; ++i)
    {
        size_t nLength = strlen(aFormatCommands[i].pCommand);
        if (strncmp(pPayload, aFormatCommands[i].pCommand, nLength) != 0 || pPayload[nLength] != '=')
            continue;
        pWindow->bUpdating = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pWindow->aFormatButtons[i]),
                                     strcmp(pPayload + nLength + 1, "true") == 0);
        pWindow->bUpdating = FALSE;
    }
}

static void onPartChanged(LOKDocView* pDocView, int nPart, GtkWindow* pAppWindow)
{
    (void)pDocView;
    (void)nPart;
    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    if (gtk_widget_get_visible(pWindow->pParts))
        refreshParts(pWindow);
}

static void onSearchNotFound(LOKDocView* pDocView, const gchar* pText, GtkWindow* pAppWindow)
{
    (void)pDocView;
    g_autofree char* pMessage = g_strdup_printf("“%s” not found", pText);
    setStatus(getDemoWindow(pAppWindow), pMessage);
}

static void onSearchResultCount(LOKDocView* pDocView, const gchar* pCount, GtkWindow* pAppWindow)
{
    (void)pDocView;
    g_autofree char* pMessage = g_strdup_printf("%s matches", pCount);
    setStatus(getDemoWindow(pAppWindow), pMessage);
}

static void onAddressChanged(LOKDocView* pDocView, const gchar* pAddress, GtkWindow* pAppWindow)
{
    (void)pDocView;
    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    gtk_widget_set_visible(pWindow->pCalcBar, TRUE);
    gtk_label_set_text(GTK_LABEL(pWindow->pAddress), pAddress);
}

static void onFormulaChanged(LOKDocView* pDocView, const gchar* pFormula, GtkWindow* pAppWindow)
{
    (void)pDocView;
    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    gtk_widget_set_visible(pWindow->pCalcBar, TRUE);
    gtk_label_set_text(GTK_LABEL(pWindow->pFormula), pFormula);
}

static void onHyperlinkClicked(LOKDocView* pDocView, const gchar* pURI, gpointer pData)
{
    (void)pData;
    GtkUriLauncher* pLauncher = gtk_uri_launcher_new(pURI);
    gtk_uri_launcher_launch(pLauncher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(pDocView))), NULL, NULL, NULL);
    g_object_unref(pLauncher);
}

/// pData: a heap GWeakRef to the toplevel, which may be gone by now.
static void onDocumentOpened(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    GWeakRef* pWindowRef = pData;
    g_autoptr(GtkWindow) pAppWindow = g_weak_ref_get(pWindowRef);
    g_weak_ref_clear(pWindowRef);
    g_free(pWindowRef);

    GError* pError = NULL;
    gboolean bLoaded = lok_doc_view_open_document_finish(LOK_DOC_VIEW(pSource), pResult, &pError);
    if (!pAppWindow)
    {
        // Window closed while loading; nothing left to update.
        g_clear_error(&pError);
        return;
    }

    DemoWindow* pWindow = getDemoWindow(pAppWindow);
    if (!bLoaded)
    {
        setStatus(pWindow, pError->message);
        g_warning("Error opening document: %s", pError->message);
        g_clear_error(&pError);
        return;
    }
    gtk_widget_set_visible(pWindow->pProgress, FALSE);
    setStatus(pWindow, "Loaded");
    // The profile remembers the last state; make the view match the toggle.
    applyBoundaries(pWindow);

    // Sheets/slides: Writer's "parts" (pages) aren't something to switch to.
    // getDocumentType() doesn't depend on the view, so calling LOK directly
    // here is fine.
    LibreOfficeKitDocument* pDocument = lok_doc_view_get_document(pWindow->pDocView);
    if (pDocument->pClass->getDocumentType(pDocument) != LOK_DOCTYPE_TEXT)
    {
        gtk_widget_set_visible(pWindow->pParts, TRUE);
        refreshParts(pWindow);
    }
}

// --- Window --------------------------------------------------------------

static GtkWidget* createFindBar(DemoWindow* pWindow)
{
    pWindow->pSearchEntry = gtk_search_entry_new();
    gtk_widget_set_size_request(pWindow->pSearchEntry, 300, -1);
    g_signal_connect(pWindow->pSearchEntry, "activate", G_CALLBACK(onFindNext), pWindow);
    g_signal_connect(pWindow->pSearchEntry, "next-match", G_CALLBACK(onFindNext), pWindow);
    g_signal_connect(pWindow->pSearchEntry, "previous-match", G_CALLBACK(onFindPrevious), pWindow);
    g_signal_connect(pWindow->pSearchEntry, "stop-search", G_CALLBACK(onStopSearch), pWindow);

    GtkWidget* pPrevious = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(pPrevious, "Find previous (Shift+Ctrl+G)");
    g_signal_connect(pPrevious, "clicked", G_CALLBACK(onFindPrevious), pWindow);
    GtkWidget* pNext = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(pNext, "Find next (Enter, Ctrl+G)");
    g_signal_connect(pNext, "clicked", G_CALLBACK(onFindNext), pWindow);

    pWindow->pHighlightAll = gtk_check_button_new_with_label("Highlight all");
    g_signal_connect(pWindow->pHighlightAll, "toggled", G_CALLBACK(onHighlightAllToggled), pWindow);

    GtkWidget* pBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(pBox), pWindow->pSearchEntry);
    gtk_box_append(GTK_BOX(pBox), pPrevious);
    gtk_box_append(GTK_BOX(pBox), pNext);
    gtk_box_append(GTK_BOX(pBox), pWindow->pHighlightAll);

    pWindow->pSearchBar = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(pWindow->pSearchBar), pBox);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(pWindow->pSearchBar), GTK_EDITABLE(pWindow->pSearchEntry));
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(pWindow->pSearchBar), TRUE);
    return pWindow->pSearchBar;
}

/// Calc: "A1 | =SUM(...)", hidden until the document reports a cell address.
static GtkWidget* createCalcBar(DemoWindow* pWindow)
{
    pWindow->pAddress = gtk_label_new("");
    gtk_widget_set_size_request(pWindow->pAddress, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(pWindow->pAddress), 0);
    pWindow->pFormula = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pWindow->pFormula), 0);
    gtk_label_set_ellipsize(GTK_LABEL(pWindow->pFormula), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(pWindow->pFormula), TRUE);
    gtk_widget_set_hexpand(pWindow->pFormula, TRUE);

    pWindow->pCalcBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(pWindow->pCalcBar, 6);
    gtk_widget_set_margin_end(pWindow->pCalcBar, 6);
    gtk_widget_set_margin_top(pWindow->pCalcBar, 3);
    gtk_widget_set_margin_bottom(pWindow->pCalcBar, 3);
    gtk_box_append(GTK_BOX(pWindow->pCalcBar), pWindow->pAddress);
    gtk_box_append(GTK_BOX(pWindow->pCalcBar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(pWindow->pCalcBar), pWindow->pFormula);
    gtk_widget_set_visible(pWindow->pCalcBar, FALSE);
    return pWindow->pCalcBar;
}

static GtkWidget* createHeaderBar(DemoWindow* pWindow)
{
    GtkWidget* pHeader = gtk_header_bar_new();

    GtkWidget* pZoomOut = gtk_button_new_from_icon_name("zoom-out-symbolic");
    GtkWidget* pZoomIn = gtk_button_new_from_icon_name("zoom-in-symbolic");
    g_object_set_data(G_OBJECT(pZoomIn), "zoom-in", GINT_TO_POINTER(1));
    g_signal_connect(pZoomOut, "clicked", G_CALLBACK(onZoom), pWindow);
    g_signal_connect(pZoomIn, "clicked", G_CALLBACK(onZoom), pWindow);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pZoomOut);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pZoomIn);

    GtkWidget* pFormatBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(pFormatBox, "linked");
    for (size_t i = 0; i < FORMAT_COMMAND_COUNT; ++i)
    {
        GtkWidget* pButton = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(pButton), aFormatCommands[i].pIcon);
        gtk_widget_set_tooltip_text(pButton, aFormatCommands[i].pTooltip);
        gtk_widget_set_focus_on_click(pButton, FALSE);
        gtk_widget_set_sensitive(pButton, FALSE); // edit mode only
        g_object_set_data(G_OBJECT(pButton), "command", (gpointer)aFormatCommands[i].pCommand);
        g_signal_connect(pButton, "toggled", G_CALLBACK(onFormatToggled), pWindow);
        gtk_box_append(GTK_BOX(pFormatBox), pButton);
        pWindow->aFormatButtons[i] = pButton;
    }
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pFormatBox);

    pWindow->pPartNames = gtk_string_list_new(NULL);
    pWindow->pParts = gtk_drop_down_new(G_LIST_MODEL(pWindow->pPartNames), NULL);
    gtk_widget_set_tooltip_text(pWindow->pParts, "Sheet / slide");
    gtk_widget_set_visible(pWindow->pParts, FALSE); // shown for spreadsheets/presentations
    g_signal_connect(pWindow->pParts, "notify::selected", G_CALLBACK(onPartSelected), pWindow);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pWindow->pParts);

    GtkWidget* pEdit = gtk_toggle_button_new_with_label("Edit");
    g_signal_connect(pEdit, "toggled", G_CALLBACK(onEditToggled), pWindow);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(pHeader), pEdit);

    // Off by default, like a typical desktop setup; applied once the document is loaded.
    pWindow->pBoundaries = gtk_toggle_button_new_with_label("Boundaries");
    gtk_widget_set_tooltip_text(pWindow->pBoundaries, "Show table, text and section boundaries (Writer)");
    g_signal_connect(pWindow->pBoundaries, "toggled", G_CALLBACK(onBoundariesToggled), pWindow);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(pHeader), pWindow->pBoundaries);

    GtkWidget* pFind = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(pFind), "edit-find-symbolic");
    gtk_widget_set_tooltip_text(pFind, "Find (Ctrl+F)");
    g_object_bind_property(pFind, "active", pWindow->pSearchBar, "search-mode-enabled",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(pHeader), pFind);

    return pHeader;
}

static void openWindow(GtkApplication* pApp, GFile* pFile)
{
    GError* pError = NULL;
    g_autofree char* pProfileURL = getUserProfileURL();
    GtkWidget* pDocView = lok_doc_view_new_from_user_profile(g_pLOPath, pProfileURL, NULL, &pError);
    if (!pDocView)
    {
        g_printerr("%s\n", pError->message);
        g_clear_error(&pError);
        return;
    }

    DemoWindow* pWindow = g_new0(DemoWindow, 1);
    pWindow->pDocView = LOK_DOC_VIEW(pDocView);

    GtkWidget* pAppWindow = gtk_application_window_new(pApp);
    g_object_set_data_full(G_OBJECT(pAppWindow), "demo-window", pWindow, g_free);
    g_autofree char* pBasename = g_file_get_basename(pFile);
    gtk_window_set_title(GTK_WINDOW(pAppWindow), pBasename);
    gtk_window_set_default_size(GTK_WINDOW(pAppWindow), 1000, 800);

    GtkWidget* pFindBar = createFindBar(pWindow); // before the header bar, which binds to it
    gtk_window_set_titlebar(GTK_WINDOW(pAppWindow), createHeaderBar(pWindow));

    // Capture phase: otherwise the document view gets Ctrl+F first and posts it to LOK.
    GtkEventController* pShortcuts = gtk_shortcut_controller_new();
    gtk_event_controller_set_propagation_phase(pShortcuts, GTK_PHASE_CAPTURE);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(pShortcuts),
                                         gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>f"),
                                                          gtk_callback_action_new(onFindShortcut, NULL, NULL)));
    gtk_widget_add_controller(pAppWindow, pShortcuts);

    GtkWidget* pBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pWindow->pProgress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(pBox), pWindow->pProgress);
    gtk_box_append(GTK_BOX(pBox), pFindBar);
    gtk_box_append(GTK_BOX(pBox), createCalcBar(pWindow));

    GtkWidget* pScrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(pScrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pScrolled), pDocView);
    gtk_box_append(GTK_BOX(pBox), pScrolled);

    pWindow->pStatus = gtk_label_new("Loading…");
    gtk_label_set_xalign(GTK_LABEL(pWindow->pStatus), 0);
    gtk_label_set_ellipsize(GTK_LABEL(pWindow->pStatus), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(pWindow->pStatus, 6);
    gtk_widget_set_margin_end(pWindow->pStatus, 6);
    gtk_box_append(GTK_BOX(pBox), pWindow->pStatus);

    gtk_window_set_child(GTK_WINDOW(pAppWindow), pBox);

    g_signal_connect_object(pDocView, "load-changed", G_CALLBACK(onLoadChanged), pAppWindow, 0);
    g_signal_connect_object(pDocView, "command-changed", G_CALLBACK(onCommandChanged), pAppWindow, 0);
    g_signal_connect_object(pDocView, "part-changed", G_CALLBACK(onPartChanged), pAppWindow, 0);
    g_signal_connect_object(pDocView, "search-not-found", G_CALLBACK(onSearchNotFound), pAppWindow, 0);
    g_signal_connect_object(pDocView, "search-result-count", G_CALLBACK(onSearchResultCount), pAppWindow, 0);
    g_signal_connect_object(pDocView, "address-changed", G_CALLBACK(onAddressChanged), pAppWindow, 0);
    g_signal_connect_object(pDocView, "formula-changed", G_CALLBACK(onFormulaChanged), pAppWindow, 0);
    g_signal_connect(pDocView, "hyperlink-clicked", G_CALLBACK(onHyperlinkClicked), NULL);

    GWeakRef* pWindowRef = g_new(GWeakRef, 1);
    g_weak_ref_init(pWindowRef, pAppWindow);
    g_autofree char* pPath = g_file_get_path(pFile);
    lok_doc_view_open_document(LOK_DOC_VIEW(pDocView), pPath, "{}", NULL, onDocumentOpened, pWindowRef);

    gtk_window_present(GTK_WINDOW(pAppWindow));
    gtk_widget_grab_focus(pDocView);
}

static void onOpen(GApplication* pApp, GFile** pFiles, gint nFiles, const gchar* pHint, gpointer pData)
{
    (void)pHint;
    (void)pData;
    for (gint i = 0; i < nFiles; ++i)
        openWindow(GTK_APPLICATION(pApp), pFiles[i]);
}

static void onActivate(GApplication* pApp, gpointer pData)
{
    (void)pApp;
    (void)pData;
    g_printerr("Usage: lokdocview4-demo [--lo-path=DIR] FILE\n");
}

int main(int argc, char** argv)
{
    // LOK supports a single document per process at a time in this demo;
    // NON_UNIQUE keeps each invocation in its own process.
    GtkApplication* pApp = gtk_application_new("org.libreoffice.LOKDocView4Demo",
                                               G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
    const GOptionEntry aEntries[] = {
        { "lo-path", 0, 0, G_OPTION_ARG_FILENAME, &g_pLOPath, "LibreOffice program directory (default: auto-detect)", "DIR" },
        { NULL, 0, 0, 0, NULL, NULL, NULL },
    };
    g_application_add_main_option_entries(G_APPLICATION(pApp), aEntries);
    g_signal_connect(pApp, "open", G_CALLBACK(onOpen), NULL);
    g_signal_connect(pApp, "activate", G_CALLBACK(onActivate), NULL);

    int nRet = g_application_run(G_APPLICATION(pApp), argc, argv);
    g_debug("main loop finished, exiting");
    g_object_unref(pApp);
    g_free(g_pLOPath);

    // LibreOffice has been destroy()ed by now (LOKDocView dispose), but some of
    // its static destructors still reach into LOK (e.g. Writer asking for the
    // LOK clipboard, which LibreOffice 26.8 dereferences after deinit) and
    // crash. Like Collabora Online's kit process, skip them.
    // TODO: return normally again once LibreOffice null-checks in
    // LOKClipboardFactory::getClipboardForCurView() (see PORTING.md).
    fflush(NULL);
    _exit(nRet);
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
