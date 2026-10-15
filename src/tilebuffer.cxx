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
    resetAllTiles();
}

GdkTexture* TileBuffer::getTile(int nColumn, int nRow) const
{
    auto it = m_aTiles.find({ nColumn, nRow });
    return it == m_aTiles.end() ? nullptr : it->second;
}

void TileBuffer::setTile(int nColumn, int nRow, GdkTexture* pTexture)
{
    auto& rpTile = m_aTiles[{ nColumn, nRow }];
    if (rpTile)
        g_object_unref(rpTile);
    rpTile = pTexture;
}

void TileBuffer::invalidate(const GdkRectangle& rRect)
{
    if (rRect.width <= 0 || rRect.height <= 0)
        return;

    int nColStart = rRect.x / nTileSizePixels;
    int nRowStart = rRect.y / nTileSizePixels;
    int nColEnd = (rRect.x + rRect.width - 1) / nTileSizePixels;
    int nRowEnd = (rRect.y + rRect.height - 1) / nTileSizePixels;

    for (auto it = m_aTiles.begin(); it != m_aTiles.end();)
    {
        auto [nCol, nRow] = it->first;
        if (nCol >= nColStart && nCol <= nColEnd && nRow >= nRowStart && nRow <= nRowEnd)
        {
            g_object_unref(it->second);
            it = m_aTiles.erase(it);
        }
        else
            ++it;
    }
}

void TileBuffer::resetAllTiles()
{
    for (auto& rEntry : m_aTiles)
        g_object_unref(rEntry.second);
    m_aTiles.clear();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
