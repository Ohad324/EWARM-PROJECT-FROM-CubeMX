#ifndef MUSICSCREENPRESENTER_HPP
#define MUSICSCREENPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class MusicScreenView;

class MusicScreenPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    explicit MusicScreenPresenter(MusicScreenView& v);
    virtual ~MusicScreenPresenter() {}

    virtual void activate()   override;
    virtual void deactivate() override {}
    virtual bool isMusicScreenActive() const override { return true; }

    /* ModelListener overrides */
    virtual void onMusicTrack(const char* title,
                               const char* artist,
                               const char* videoId) override;

    virtual void onMusicThumbnail(uint8_t* rgb888,
                                   uint32_t width,
                                   uint32_t height) override;

    virtual void onMusicError(const char* reason) override;

private:
    MusicScreenPresenter();
    MusicScreenView& view;
};

#endif // MUSICSCREENPRESENTER_HPP
