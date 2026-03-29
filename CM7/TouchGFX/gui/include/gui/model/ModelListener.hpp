#ifndef MODELLISTENER_HPP
#define MODELLISTENER_HPP

#include <gui/model/Model.hpp>

class ModelListener
{
public:
    ModelListener() : model(0) {}

    virtual ~ModelListener() {}

    void bind(Model* m)
    {
        model = m;
    }

    /** Called by Model::tick() when a new BLE string arrives from the UART queue. */
    virtual void onBleMessage(const char* msg) {}

    /** Called when TRACK: metadata arrives -- switch to MusicScreen. */
    virtual void onMusicTrack(const char* title,
                               const char* artist,
                               const char* videoId) {}

    /** Called when THUMB: JPEG is decoded and scaled to 800x480 RGB888. */
    virtual void onMusicThumbnail(uint8_t* rgb888,
                                   uint32_t width, uint32_t height) {}

    /** Called when ERROR: arrives -- display reason on MusicScreen. */
    virtual void onMusicError(const char* reason) {}

    /** Called when any music message arrives and MusicScreen is NOT active.
     *  Screen1Presenter overrides this to trigger gotoMusicScreenNoTransition(). */
    virtual void onMusicPending() {}

    /** Returns true only when MusicScreenPresenter is the active presenter.
     *  Used by Model::tick() to choose direct dispatch vs. cache+switch. */
    virtual bool isMusicScreenActive() const { return false; }

protected:
    Model* model;
};

#endif // MODELLISTENER_HPP
