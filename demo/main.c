/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Minimal GTK4 viewer for LOKDocView. Plain C on purpose, so the public
 * header keeps compiling as C.
 *
 *   lokdocview4-demo [--lo-path=DIR] FILE
 */

#include <LibreOfficeKit/LibreOfficeKitGtk.h>

#include <stdio.h>
#include <unistd.h>

static char* g_pLOPath = NULL;

typedef struct
{
    LOKDocView* pDocView;
    GtkWidget* pProgress;
    GtkWidget* pStatus;
    GtkWidget* pBoundaries;
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

/// The DemoWindow owned by a toplevel (freed together with it).
static DemoWindow* getDemoWindow(gpointer pAppWindow)
{
    return g_object_get_data(G_OBJECT(pAppWindow), "demo-window");
}

static void onZoom(GtkButton* pButton, DemoWindow* pWindow)
{
    float fFactor = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pButton), "zoom-in")) ? 1.25f : 0.8f;
    lok_doc_view_set_zoom(pWindow->pDocView, lok_doc_view_get_zoom(pWindow->pDocView) * fFactor);
}

static void onEditToggled(GtkToggleButton* pButton, DemoWindow* pWindow)
{
    lok_doc_view_set_edit(pWindow->pDocView, gtk_toggle_button_get_active(pButton));
}

// LOKDocView signal handlers get the toplevel, connected with
// g_signal_connect_object() so they're disconnected when it goes away: the
// view can outlive its window (a load in progress keeps it alive).

static void onLoadChanged(LOKDocView* pDocView, gdouble fProgress, GtkWindow* pAppWindow)
{
    (void)pDocView;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(getDemoWindow(pAppWindow)->pProgress), fProgress);
}

static void onCommandChanged(LOKDocView* pDocView, const gchar* pPayload, GtkWindow* pAppWindow)
{
    (void)pDocView;
    gtk_label_set_text(GTK_LABEL(getDemoWindow(pAppWindow)->pStatus), pPayload);
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
        gtk_label_set_text(GTK_LABEL(pWindow->pStatus), pError->message);
        g_warning("Error opening document: %s", pError->message);
        g_clear_error(&pError);
        return;
    }
    gtk_widget_set_visible(pWindow->pProgress, FALSE);
    gtk_label_set_text(GTK_LABEL(pWindow->pStatus), "Loaded");
    // The profile remembers the last state; make the view match the toggle.
    applyBoundaries(pWindow);
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

    GtkWidget* pHeader = gtk_header_bar_new();
    GtkWidget* pZoomOut = gtk_button_new_from_icon_name("zoom-out-symbolic");
    GtkWidget* pZoomIn = gtk_button_new_from_icon_name("zoom-in-symbolic");
    g_object_set_data(G_OBJECT(pZoomIn), "zoom-in", GINT_TO_POINTER(1));
    g_signal_connect(pZoomOut, "clicked", G_CALLBACK(onZoom), pWindow);
    g_signal_connect(pZoomIn, "clicked", G_CALLBACK(onZoom), pWindow);
    GtkWidget* pEdit = gtk_toggle_button_new_with_label("Edit");
    g_signal_connect(pEdit, "toggled", G_CALLBACK(onEditToggled), pWindow);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pZoomOut);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(pHeader), pZoomIn);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(pHeader), pEdit);
    // Off by default, like a typical desktop setup; applied once the document is loaded.
    pWindow->pBoundaries = gtk_toggle_button_new_with_label("Boundaries");
    gtk_widget_set_tooltip_text(pWindow->pBoundaries, "Show table, text and section boundaries (Writer)");
    g_signal_connect(pWindow->pBoundaries, "toggled", G_CALLBACK(onBoundariesToggled), pWindow);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(pHeader), pWindow->pBoundaries);
    gtk_window_set_titlebar(GTK_WINDOW(pAppWindow), pHeader);

    GtkWidget* pBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    pWindow->pProgress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(pBox), pWindow->pProgress);

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
