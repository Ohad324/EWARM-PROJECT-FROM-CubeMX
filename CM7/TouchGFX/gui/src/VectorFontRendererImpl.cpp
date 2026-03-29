/**
 * Stub implementation of getVectorFontBuffers().
 * Required by TouchGFX 4.26.1 linker when vector fonts
 * are not used. We use bitmap fonts only.
 */
#include <touchgfx/VectorFontRendererImpl.hpp>

namespace touchgfx
{
    void VectorFontRendererImpl::getVectorFontBuffers(
        float*&        pathBuffer,
        int&           pathBufferSize,
        unsigned char*& frameBuffer,
        int&           frameBufferSize)
    {
        // Not used — bitmap fonts only
        pathBuffer      = nullptr;
        pathBufferSize  = 0;
        frameBuffer     = nullptr;
        frameBufferSize = 0;
    }
}
