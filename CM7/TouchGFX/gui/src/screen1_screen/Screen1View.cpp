#include <gui/screen1_screen/Screen1View.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/Color.hpp>
#include "log_mutex.h"    /* LOG() — mutex-guarded printf */

static const touchgfx::colortype WHITE = touchgfx::Color::getColorFromRGB(255, 255, 255);

Screen1View::Screen1View()
{
}

void Screen1View::setupScreen()
{
    LOG("[View] setupScreen() — center word display initialised\n");
    Screen1ViewBase::setupScreen();

    centerTextBuf[0] = 0;

    /* Position the text area exactly over the Sightsys logo area */
    centerTextArea.setPosition(75, 85, 644, 319);
    centerTextArea.setTypedText(touchgfx::TypedText(T_UARTMESSAGE));
    centerTextArea.setWildcard(centerTextBuf);
    centerTextArea.setColor(WHITE);
    add(centerTextArea);
}

void Screen1View::tearDownScreen()
{
    Screen1ViewBase::tearDownScreen();
}

void Screen1View::setBleMessage(const char* msg)
{
    LOG("[View] setBleMessage() — raw input: \"%s\"\n", msg);

    /* ── UTF-8 → UnicodeChar decode ────────────────────────────────────────
     * Handles 1-byte (ASCII), 2-byte (Hebrew U+05B0–U+05EA), 3-byte (other).
     * Nikud diacritics (U+05B0–U+05C7) are kept — Noto Sans Hebrew stores
     * them with advance-width = 0, so in RTL mode TouchGFX overlays each
     * nikud mark on the consonant rendered immediately before it. */
    uint16_t in  = 0u;
    uint16_t out = 0u;
    while (msg[in] != '\0' && out < BLE_MSG_LEN - 1u)
    {
        uint8_t b0 = static_cast<uint8_t>(msg[in]);
        if (b0 < 0x80u)
        {
            centerTextBuf[out++] = static_cast<touchgfx::Unicode::UnicodeChar>(b0);
            in += 1u;
        }
        else if ((b0 & 0xE0u) == 0xC0u && msg[in + 1u] != '\0')
        {
            uint8_t b1 = static_cast<uint8_t>(msg[in + 1u]);
            centerTextBuf[out++] = static_cast<touchgfx::Unicode::UnicodeChar>(
                ((b0 & 0x1Fu) << 6u) | (b1 & 0x3Fu));
            in += 2u;
        }
        else if ((b0 & 0xF0u) == 0xE0u && msg[in + 1u] != '\0' && msg[in + 2u] != '\0')
        {
            uint8_t b1 = static_cast<uint8_t>(msg[in + 1u]);
            uint8_t b2 = static_cast<uint8_t>(msg[in + 2u]);
            centerTextBuf[out++] = static_cast<touchgfx::Unicode::UnicodeChar>(
                ((b0 & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu));
            in += 3u;
        }
        else
        {
            in += 1u; /* skip malformed lead byte */
        }
    }
    centerTextBuf[out] = 0u;

    /* ── Strip trailing CR / LF ──────────────────────────────────────────── */
    while (out > 0u && (centerTextBuf[out - 1u] == 0x000Au || centerTextBuf[out - 1u] == 0x000Du))
    {
        centerTextBuf[--out] = 0u;
    }

    /* ── Select LTR or RTL typography based on content ──────────────────── */
    bool isHebrew = false;
    for (uint16_t j = 0u; j < out && !isHebrew; j++)
    {
        if (centerTextBuf[j] >= 0x05B0u && centerTextBuf[j] <= 0x05EAu)
        {
            isHebrew = true;
        }
    }

    /* Erase the old widget content before changing typography */
    centerTextArea.invalidate();

    centerTextArea.setTypedText(touchgfx::TypedText(isHebrew ? T_UARTMESSAGEHE : T_UARTMESSAGE));
    centerTextArea.setWildcard(centerTextBuf);
    centerTextArea.setColor(WHITE);
    centerTextArea.invalidate();

    LOG("[View] centerTextArea updated (%s)\n",
        isHebrew ? "RTL/Hebrew" : "LTR/Latin");
}
