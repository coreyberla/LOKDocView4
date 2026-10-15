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
   A cached tile.

   A tile keeps showing its last texture while it is stale (invalidated) or
   being re-rendered, so edits don't flicker.
*/
struct Tile
{
    /// Last rendered content, or nullptr if never rendered. Owned.
    GdkTexture* m_pTexture = nullptr;
    /// Whether m_pTexture reflects the current document content.
    bool m_bValid = false;
    /// Whether a paint request for this tile is queued or running.
    bool m_bPending = false;
    /// Bumped on every invalidation; a paint result is only valid if it was
    /// requested at the current serial.
    unsigned m_nSerial = 0;
};

/**
   Cache of rendered tiles, keyed by (column, row), for one zoom level and
   scale factor.

   GTK4 replacement for the cairo-surface based upstream TileBuffer: tiles are
   immutable GdkTextures that can be appended to a GtkSnapshot directly.
*/
class TileBuffer
{
public:
    TileBuffer() = default;
    ~TileBuffer();
    TileBuffer(const TileBuffer&) = delete;
    TileBuffer& operator=(const TileBuffer&) = delete;

    /// Returns the tile or nullptr if it was never requested.
    const Tile* findTile(int nColumn, int nRow) const;
    /// Returns the tile, creating an empty one if needed.
    Tile& getTile(int nColumn, int nRow);
    /**
       Stores a paint result. Takes ownership of pTexture. The tile becomes
       valid unless it was invalidated after the paint was requested.
    */
    void setTile(int nColumn, int nRow, GdkTexture* pTexture, unsigned nRequestSerial);
    /// Marks all tiles intersecting the given rectangle (in logical pixels) stale.
    void invalidate(const GdkRectangle& rRect);
    /// Marks all tiles stale.
    void invalidateAll();
    /// A paint request for the tile was dropped: allow requesting it again.
    void clearPending(int nColumn, int nRow);
    /**
       Bounds memory: if there are more than nMaxTiles tiles, drops the ones
       farthest from (fCenterColumn, fCenterRow) (in tile units). Tiles with a
       paint in flight are kept.
    */
    void evictFarthest(double fCenterColumn, double fCenterRow, size_t nMaxTiles);
    /// Whether any tile has content to show.
    bool hasTextures() const;

private:
    void invalidateTile(Tile& rTile);

    std::map<std::pair<int, int>, Tile> m_aTiles;
};

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
