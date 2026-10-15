/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "tilebuffer.hxx"

// 1 inch = 1440 twips = 96 px
float pixelToTwip(float fInput, float zoom)
{
    return (fInput / zoom) * 15.0f;
}

float twipToPixel(float fInput, float zoom)
{
    return (fInput * zoom) / 15.0f;
}

TileBuffer::~TileBuffer()
{
    for (auto& rEntry : m_aTiles)
        g_clear_object(&rEntry.second.m_pTexture);
}

const Tile* TileBuffer::findTile(int nColumn, int nRow) const
{
    auto it = m_aTiles.find({ nColumn, nRow });
    return it == m_aTiles.end() ? nullptr : &it->second;
}

Tile& TileBuffer::getTile(int nColumn, int nRow)
{
    return m_aTiles[{ nColumn, nRow }];
}

void TileBuffer::setTile(int nColumn, int nRow, GdkTexture* pTexture, unsigned nRequestSerial)
{
    Tile& rTile = getTile(nColumn, nRow);
    g_clear_object(&rTile.m_pTexture);
    rTile.m_pTexture = pTexture;
    rTile.m_bPending = false;
    rTile.m_bValid = nRequestSerial == rTile.m_nSerial;
}

void TileBuffer::invalidateTile(Tile& rTile)
{
    rTile.m_bValid = false;
    ++rTile.m_nSerial;
}

void TileBuffer::invalidate(const GdkRectangle& rRect)
{
    if (rRect.width <= 0 || rRect.height <= 0)
        return;

    int nColStart = rRect.x / nTileSizePixels;
    int nRowStart = rRect.y / nTileSizePixels;
    int nColEnd = (rRect.x + rRect.width - 1) / nTileSizePixels;
    int nRowEnd = (rRect.y + rRect.height - 1) / nTileSizePixels;

    for (auto& [rPosition, rTile] : m_aTiles)
    {
        auto [nCol, nRow] = rPosition;
        if (nCol >= nColStart && nCol <= nColEnd && nRow >= nRowStart && nRow <= nRowEnd)
            invalidateTile(rTile);
    }
}

void TileBuffer::invalidateAll()
{
    for (auto& rEntry : m_aTiles)
        invalidateTile(rEntry.second);
}

bool TileBuffer::hasTextures() const
{
    for (const auto& rEntry : m_aTiles)
        if (rEntry.second.m_pTexture)
            return true;
    return false;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
