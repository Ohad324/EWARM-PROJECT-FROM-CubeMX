#include <gui/music_screen/MusicScreenView.hpp>
#include <touchgfx/Color.hpp>
#include <touchgfx/Application.hpp>   /* Application::getInstance for Bug Y fix */
#include <texts/TextKeysAndLanguages.hpp>
#include <string.h>
#include "log_mutex.h"
#include "music_display_task.h"   /* Music_SendCtrl -- Phase 2 stub */
#ifndef RELEASE_BUILD
#include "stm32h7xx.h"   /* LTDC, DMA1, DMA2, SCB for Bug Y register probe */
#endif

using namespace touchgfx;

static const colortype WHITE      = Color::getColorFromRGB(255, 255, 255);
static const colortype LIGHT_GRAY = Color::getColorFromRGB(200, 200, 200);

MusicScreenView::MusicScreenView()
    : m_onMusicScreen(false)
{
    memset(titleBuf,  0, sizeof(titleBuf));
    memset(artistBuf, 0, sizeof(artistBuf));
    memset(timeBuf,   0, sizeof(timeBuf));
}

void MusicScreenView::setupScreen()
{
    MusicScreenViewBase::setupScreen();
    LOG("[MusicView] setupScreen()\n");
    /* Hide thumbnail until setThumbnail() sets valid pixel data.
     * Without this, DMA2D tries to blit from a null pointer and crashes. */
    imgThumbnail.setAlpha(0);
    /* Phase 1: image only — lblTitle and lblArtist are NOT added to screen */
    m_onMusicScreen = true;

    /* PFB diagnostic 2026-05-06: force full-screen invalidate so all 4 strips
     * (y=0,120,240,360) get rendered + transmitted at boot. Without this the
     * framework only marks dirty regions where widgets changed, leaving the
     * bottom 360 rows of the panel as uninitialized GRAM (rainbow noise). */
    {
        touchgfx::Rect fullScreen(0, 0, 800, 480);
        invalidateRect(fullScreen);
    }
}

void MusicScreenView::tearDownScreen()
{
    m_onMusicScreen = false;
    MusicScreenViewBase::tearDownScreen();
}

/* ── Presenter callbacks ────────────────────────────────────────────────── */

void MusicScreenView::setTrackInfo(const char* title,
                                    const char* artist,
                                    const char* /*videoId*/)
{
    /* Title */
    bool titleHebrew = utf8ToUnicode(title, titleBuf, TITLE_BUF_LEN);
    (void)titleHebrew;
    LOG("[MusicView] Title set (%s): %s\n",
        titleHebrew ? "Hebrew RTL" : "Latin LTR", title);

    /* Artist */
    bool artistHebrew = utf8ToUnicode(artist, artistBuf, ARTIST_BUF_LEN);
    (void)artistHebrew;
    LOG("[MusicView] Artist set (%s): %s\n",
        artistHebrew ? "Hebrew RTL" : "Latin LTR", artist);

#ifdef RELEASE_BUILD
    /* ORIGINAL behavior — invalidates lblTitle / lblArtist even though they
     * are NOT in the screen tree (Phase-1 design — see setupScreen()).
     * This poke-on-orphan-widget produces a DMA2D Configuration Error every
     * frame: pDst=NULL → OMAR=0, NLR PL=0, FGPFCCR=A4 (font glyph). The
     * `signalDMAInterrupt` path in STM32DMA.cpp recovers cleanly so it's
     * not visible as a hang, but it floods the RTT log and wastes ISR time.
     * Kept here so a Release build matches the binary the user has been
     * running while we validate the Debug fix.                            */
    lblTitle.invalidate();
    lblTitle.setWildcard(titleBuf);
    lblTitle.setColor(WHITE);
    lblTitle.invalidate();

    lblArtist.invalidate();
    lblArtist.setWildcard(artistBuf);
    lblArtist.setColor(LIGHT_GRAY);
    lblArtist.invalidate();
#else
    /* DEBUG — skip the invalidate / setWildcard pokes on widgets that are
     * not in the screen tree. titleBuf / artistBuf are still populated
     * above so any downstream reader keeps working. progressFill below IS
     * in the tree, so its invalidate stays in both builds.                */
#endif

#ifdef RELEASE_BUILD
    /* Reset progress bar (static in Phase 1).
     * progressFill is also an orphan widget (declared in ViewBase but never
     * add()'d to the screen tree). setWidth(0) + invalidate() triggers a
     * DMA2D fill with pDst=NULL — this is the actual OMAR=0 / NLR PL=0 NL=16
     * fingerprint we've been chasing. Kept in Release for binary parity with
     * the running build until Debug confirms the fix. */
    progressFill.setWidth(0);
    progressFill.invalidate();
#else
    /* DEBUG — skip the progressFill poke. progressFill is an orphan widget
     * (never add()'d). Until Phase 2 adds it via Designer, invalidating it
     * pokes DMA2D with pDst=NULL → CE fault every frame.                   */
#endif

    /* Switch to MusicScreen if currently on another screen */
    if (!m_onMusicScreen)
    {
        LOG("[MUSIC] Switching to MusicScreen\n");
        application().gotoMusicScreenNoTransition();
    }
}

void MusicScreenView::setThumbnail(uint8_t* rgb888,
                                    uint32_t  width,
                                    uint32_t  height)
{
    /* rgb888 points to the 800x480 scaled buffer in SDRAM (jpeg_decoder.c).
     * D-Cache was already cleaned by JPEG_Decode() so DMA2D reads fresh data. */
    LOG("[MusicView] setThumbnail ptr=%p %lux%lu\n",
        (void*)rgb888, (unsigned long)width, (unsigned long)height);
#ifndef RELEASE_BUILD
    /* NOTE: R↔B swap experiment didn't change the visual output. Either the
     * colors were already correct (album art with stage-lit blue/green
     * tones) or the bug is not a channel swap. Removed for performance. */

    /* Bug Y / FMC-conflict probe per Gemini hint — register snapshot BEFORE
     * the invalidate. If LTDC.ISR or DMA error flags differ between BEFORE
     * and AFTER the invalidate, we've localized when the fault fires. */
    LOG("[HW@setThumb-pre] LTDC.ISR=0x%08lX D1L=0x%08lX D1H=0x%08lX D2L=0x%08lX "
        "D2H=0x%08lX CFSR=0x%08lX BFAR=0x%08lX\n",
        (unsigned long)LTDC->ISR,
        (unsigned long)DMA1->LISR, (unsigned long)DMA1->HISR,
        (unsigned long)DMA2->LISR, (unsigned long)DMA2->HISR,
        (unsigned long)SCB->CFSR, (unsigned long)SCB->BFAR);
#endif
#ifndef RELEASE_BUILD
    /* Bug Y attempt 2 — cache coherency. Both source buffer (rgb888 at
     * 0xD057E400) and framebuffer (0xD0000000) live in cacheable SDRAM.
     * The JPEG decoder wrote new pixels to rgb888 then cleaned the cache
     * for THAT range (per setThumbnail() comment), but:
     *   1. The framebuffer cache lines from press 1's render are stale
     *      relative to whatever DMA2D writes next.
     *   2. Subsequent presses use the SAME source buffer pointer; if any
     *      framework-internal state caches the "content snapshot" and
     *      checks against cached SDRAM, it will see stale data.
     * Sledgehammer fix: clean+invalidate ALL D-Cache before the invalidate.
     * Costs a few hundred microseconds; less risky than Application::
     * invalidate() which starved the UART TX task. */
    SCB_CleanInvalidateDCache();
#endif
    imgThumbnail.setPixelData(rgb888);
    imgThumbnail.setAlpha(255);
    imgThumbnail.invalidate();
#ifndef RELEASE_BUILD
    /* Also clean+invalidate AFTER, so any framework state changes done by
     * invalidate() are flushed to SDRAM (since DMA2D will read SDRAM, not
     * cache, on next render walk). */
    SCB_CleanInvalidateDCache();
#endif
    /* NOTE: tried Application::getInstance()->invalidate() here — it broke
     * the audio path (REC_425+ saw "StreamWav: UART timeout at 0/96512" on
     * NORA). Reverted. Bug Y still open; needs different fix. */
#ifndef RELEASE_BUILD
    /* Bug Y diagnostic — emit a unique marker per setThumbnail call. After
     * each marker, watch the [DMA2D blits] counter. If marker N+1 appears
     * but DMA2D blit count didn't grow between marker N and N+1, then the
     * invalidate() didn't propagate to a render walk → H2. If blit count
     * DID grow but [LCD-EF] updated didn't tick → H1.                     */
    static uint32_t setThumb_count = 0;
    ++setThumb_count;
    LOG("[Bug-Y] setThumbnail #%lu invalidate() returned -- watching for dma2d/EF/EOR\n",
        (unsigned long)setThumb_count);
    LOG("[HW@setThumb-post] LTDC.ISR=0x%08lX D1L=0x%08lX D1H=0x%08lX D2L=0x%08lX "
        "D2H=0x%08lX CFSR=0x%08lX BFAR=0x%08lX\n",
        (unsigned long)LTDC->ISR,
        (unsigned long)DMA1->LISR, (unsigned long)DMA1->HISR,
        (unsigned long)DMA2->LISR, (unsigned long)DMA2->HISR,
        (unsigned long)SCB->CFSR, (unsigned long)SCB->BFAR);
#endif
    LOG("[MusicView] Thumbnail displayed: %lu x %lu -> 800x480 fullscreen\n",
        (unsigned long)width, (unsigned long)height);
}

void MusicScreenView::setError(const char* reason)
{
    /* Show error text in the title area */
    bool hebrew = utf8ToUnicode(reason, titleBuf, TITLE_BUF_LEN);
    (void)hebrew;

    /* Clear artist (always — both builds) */
    artistBuf[0] = 0u;

#ifdef RELEASE_BUILD
    /* See setTrackInfo() — original-behavior orphan-widget pokes that fire
     * DMA2D OMAR=0 every frame. Kept in Release for binary parity. */
    lblTitle.invalidate();
    lblTitle.setWildcard(titleBuf);
    lblTitle.setColor(WHITE);
    lblTitle.invalidate();

    lblArtist.setWildcard(artistBuf);
    lblArtist.invalidate();
#endif

    if (!m_onMusicScreen)
        application().gotoMusicScreenNoTransition();

    LOG("[MUSIC] Error displayed: %s\n", reason);
}

/* ── Button callbacks ───────────────────────────────────────────────────── */

void MusicScreenView::backClicked()
{
    LOG("[MUSIC] Touch: BACK -> returning to Screen1\n");
    m_onMusicScreen = false;
    application().gotoScreen1ScreenNoTransition();
}

void MusicScreenView::prevClicked()
{
    LOG("[MUSIC] Phase2 stub: CTRL:prev (button pressed, not sent)\n");
    /* Phase 2: Music_SendCtrl("prev"); */
}

void MusicScreenView::playClicked()
{
    LOG("[MUSIC] Phase2 stub: CTRL:play (button pressed, not sent)\n");
    /* Phase 2: Music_SendCtrl("play"); */
}

void MusicScreenView::nextClicked()
{
    LOG("[MUSIC] Phase2 stub: CTRL:next (button pressed, not sent)\n");
    /* Phase 2: Music_SendCtrl("next"); */
}

/* ── UTF-8 -> Unicode helper ─────────────────────────────────────────────── */

bool MusicScreenView::utf8ToUnicode(const char* src,
                                     Unicode::UnicodeChar* dst,
                                     uint16_t dstMaxChars)
{
    bool     hebrew = false;
    uint16_t in     = 0u;
    uint16_t out    = 0u;

    while (src[in] != '\0' && out < dstMaxChars - 1u)
    {
        uint8_t b0 = static_cast<uint8_t>(src[in]);

        if (b0 < 0x80u)
        {
            dst[out++] = static_cast<Unicode::UnicodeChar>(b0);
            in += 1u;
        }
        else if ((b0 & 0xE0u) == 0xC0u && src[in + 1u] != '\0')
        {
            uint8_t b1 = static_cast<uint8_t>(src[in + 1u]);
            Unicode::UnicodeChar cp = static_cast<Unicode::UnicodeChar>(
                ((b0 & 0x1Fu) << 6u) | (b1 & 0x3Fu));
            dst[out++] = cp;
            if (cp >= 0x0590u && cp <= 0x05FFu) hebrew = true;
            in += 2u;
        }
        else if ((b0 & 0xF0u) == 0xE0u &&
                  src[in + 1u] != '\0' && src[in + 2u] != '\0')
        {
            uint8_t b1 = static_cast<uint8_t>(src[in + 1u]);
            uint8_t b2 = static_cast<uint8_t>(src[in + 2u]);
            dst[out++] = static_cast<Unicode::UnicodeChar>(
                ((b0 & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu));
            in += 3u;
        }
        else
        {
            in += 1u; /* skip malformed lead byte */
        }
    }
    dst[out] = 0u;
    return hebrew;
}
