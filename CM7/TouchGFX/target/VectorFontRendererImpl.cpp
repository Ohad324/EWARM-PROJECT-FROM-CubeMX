/**
 * @file VectorFontRendererImpl.cpp
 * @brief Defines the static buffer provider required by touchgfx::VectorFontRendererImpl.
 *
 * The TouchGFX core library declares getVectorFontBuffers() on
 * VectorFontRendererImpl but leaves it undefined, expecting the application
 * to supply the memory. This file provides that definition.
 *
 * Buffers are sized for an 800px-wide display. They are only consumed at
 * runtime if a typography with IsVector="yes" is actually rendered.
 */

#include <touchgfx/VectorFontRendererImpl.hpp>

namespace touchgfx
{

void VectorFontRendererImpl::getVectorFontBuffers(float*& pointArray,    int& pointArraySize,
                                                  uint8_t*& commandArray, int& commandArraySize)
{
    static float   points[2000];
    static uint8_t commands[2000];

    pointArray       = points;
    pointArraySize   = 2000;
    commandArray     = commands;
    commandArraySize = 2000;
}

} // namespace touchgfx
