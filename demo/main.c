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

static char* g_pLOPath = NULL;

typedef struct
{
    LOKDocView* pDocView;
    GtkWidget* pProgress;
    GtkWidget* pStatus;
} DemoWindow;

static void freeDemoWindow(gpointer pData, GObject* pWindow)
{
    (void)pWindow;
    g_free(pData);
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

static void onLoadChanged(LOKDocView* pDocView, gdouble fProgress, DemoWindow* pWindow)
{
    (void)pDocView;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pWindow->pProgress), fProgress);
}

static void onCommandChanged(LOKDocView* pDocView, const gchar* pPayload, DemoWindow* pWindow)
{
    (void)pDocView;
    gtk_label_set_text(GTK_LABEL(pWindow->pStatus), pPayload);
}

static void onHyperlinkClicked(LOKDocView* pDocView, const gchar* pURI, gpointer pData)
{
    (void)pData;
    GtkUriLauncher* pLauncher = gtk_uri_launcher_new(pURI);
    gtk_uri_launcher_launch(pLauncher, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(pDocView))), NULL, NULL, NULL);
    g_object_unref(pLauncher);
}

static void onDocumentOpened(GObject* pSource, GAsyncResult* pResult, gpointer pData)
{
    DemoWindow* pWindow = pData;
    GError* pError = NULL;
    if (!lok_doc_view_open_document_finish(LOK_DOC_VIEW(pSource), pResult, &pError))
    {
        gtk_label_set_text(GTK_LABEL(pWindow->pStatus), pError->message);
        g_warning("Error opening document: %s", pError->message);
        g_clear_error(&pError);
        return;
    }
    gtk_widget_set_visible(pWindow->pProgress, FALSE);
    gtk_label_set_text(GTK_LABEL(pWindow->pStatus), "Loaded");
}

static void openWindow(GtkApplication* pApp, GFile* pFile)
{
    GError* pError = NULL;
    GtkWidget* pDocView = lok_doc_view_new(g_pLOPath, NULL, &pError);
    if (!pDocView)
    {
        g_printerr("%s\n", pError->message);
        g_clear_error(&pError);
        return;
    }

    DemoWindow* pWindow = g_new0(DemoWindow, 1);
    pWindow->pDocView = LOK_DOC_VIEW(pDocView);

    GtkWidget* pAppWindow = gtk_application_window_new(pApp);
    g_object_weak_ref(G_OBJECT(pAppWindow), freeDemoWindow, pWindow);
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

    g_signal_connect(pDocView, "load-changed", G_CALLBACK(onLoadChanged), pWindow);
    g_signal_connect(pDocView, "command-changed", G_CALLBACK(onCommandChanged), pWindow);
    g_signal_connect(pDocView, "hyperlink-clicked", G_CALLBACK(onHyperlinkClicked), NULL);

    g_autofree char* pPath = g_file_get_path(pFile);
    lok_doc_view_open_document(LOK_DOC_VIEW(pDocView), pPath, "{}", NULL, onDocumentOpened, pWindow);

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
    g_object_unref(pApp);
    g_free(g_pLOPath);
    return nRet;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
