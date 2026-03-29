#ifndef MUSICSCREENVIEW_HPP
#define MUSICSCREENVIEW_HPP

#include <gui_generated/music_screen/MusicScreenViewBase.hpp>
#include <gui/music_screen/MusicScreenPresenter.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
#include <touchgfx/widgets/Box.hpp>
#include <touchgfx/Unicode.hpp>

class MusicScreenView : public MusicScreenViewBase
{
public:
    MusicScreenView();
    virtual ~MusicScreenView() {}

    virtual void setupScreen()    override;
    virtual void tearDownScreen() override;

    /* Called by Presenter */
    void setTrackInfo(const char* title, const char* artist,
                      const char* videoId);
    void setThumbnail(uint8_t* rgb888, uint32_t width, uint32_t height);
    void setError(const char* reason);

    /* Button callbacks — wired by TouchGFX Designer */
    void backClicked();
    void prevClicked();
    void playClicked();
    void nextClicked();

private:
    /** Decode UTF-8 string into a TouchGFX Unicode buffer.
     *  Returns true if any Hebrew codepoint (U+0590-U+05FF) was found. */
    static bool utf8ToUnicode(const char* src,
                               touchgfx::Unicode::UnicodeChar* dst,
                               uint16_t dstMaxChars);

    static const uint16_t TITLE_BUF_LEN  = 128u;
    static const uint16_t ARTIST_BUF_LEN = 64u;
    static const uint16_t TIME_BUF_LEN   = 20u;

    touchgfx::Unicode::UnicodeChar titleBuf [TITLE_BUF_LEN];
    touchgfx::Unicode::UnicodeChar artistBuf[ARTIST_BUF_LEN];
    touchgfx::Unicode::UnicodeChar timeBuf  [TIME_BUF_LEN];

    bool m_onMusicScreen;
};

#endif // MUSICSCREENVIEW_HPP
