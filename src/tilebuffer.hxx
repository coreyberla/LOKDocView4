/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <gdk/gdk.h>

#include <map>
#include <utility>

/// Size of a (square) tile in logical (unscaled) pixels.
const int nTileSizePixels = 256;

/**
   Converts the pixel value to zoom independent twip value.

   @param fInput value to convert
   @param zoom the current zoom level

   @return the pixels value corresponding to given twip value
*/
float pixelToTwip(float fInput, float zoom);

/**
   Converts the zoom independent twip value pixel value.

   @param fInput value to convert
   @param zoom the current zoom level

   @return the twip value corresponding to given pixel value
*/
float twipToPixel(float fInput, float zoom);

/**
   Cache of rendered tiles, keyed by (column, row).

   GTK4 replacement for the cairo-surface based upstream TileBuffer: tiles are
   immutable GdkTextures that can be appended to a GtkSnapshot directly.
   A tile that is missing from the cache needs to be (re)rendered.
*/
class TileBuffer
{
public:
    TileBuffer() = default;
    ~TileBuffer();
    TileBuffer(const TileBuffer&) = delete;
    TileBuffer& operator=(const TileBuffer&) = delete;

    /// Returns the cached tile (owned by the buffer) or nullptr if it must be rendered.
    GdkTexture* getTile(int nColumn, int nRow) const;
    /// Takes ownership of pTexture, replacing any previous tile at that position.
    void setTile(int nColumn, int nRow, GdkTexture* pTexture);
    /// Drops all tiles intersecting the given rectangle (in logical pixels).
    void invalidate(const GdkRectangle& rRect);
    /// Drops all tiles.
    void resetAllTiles();

private:
    std::map<std::pair<int, int>, GdkTexture*> m_aTiles;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
