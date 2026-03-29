#include <gui/music_screen/MusicScreenView.hpp>
#include <touchgfx/Color.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <string.h>
#include "log_mutex.h"
#include "music_display_task.h"   /* Music_SendCtrl -- Phase 2 stub */

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
    (void)titleHebrew;   /* Phase 1: alignment fixed; RTL switchable in Phase 2 via Designer */
    lblTitle.invalidate();
    lblTitle.setWildcard(titleBuf);
    lblTitle.setColor(WHITE);
    lblTitle.invalidate();
    LOG("[MusicView] Title set (%s): %s\n",
        titleHebrew ? "Hebrew RTL" : "Latin LTR", title);

    /* Artist */
    bool artistHebrew = utf8ToUnicode(artist, artistBuf, ARTIST_BUF_LEN);
    (void)artistHebrew;
    lblArtist.invalidate();
    lblArtist.setWildcard(artistBuf);
    lblArtist.setColor(LIGHT_GRAY);
    lblArtist.invalidate();
    LOG("[MusicView] Artist set (%s): %s\n",
        artistHebrew ? "Hebrew RTL" : "Latin LTR", artist);

    /* Reset progress bar (static in Phase 1) */
    progressFill.setWidth(0);
    progressFill.invalidate();

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
    imgThumbnail.setPixelData(rgb888);
    imgThumbnail.setAlpha(255);
    imgThumbnail.invalidate();
    LOG("[MusicView] Thumbnail displayed: %lu x %lu -> 800x480 fullscreen\n",
        (unsigned long)width, (unsigned long)height);
}

void MusicScreenView::setError(const char* reason)
{
    /* Show error text in the title area */
    bool hebrew = utf8ToUnicode(reason, titleBuf, TITLE_BUF_LEN);
    (void)hebrew;
    lblTitle.invalidate();
    lblTitle.setWildcard(titleBuf);
    lblTitle.setColor(WHITE);
    lblTitle.invalidate();

    /* Clear artist */
    artistBuf[0] = 0u;
    lblArtist.setWildcard(artistBuf);
    lblArtist.invalidate();

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
