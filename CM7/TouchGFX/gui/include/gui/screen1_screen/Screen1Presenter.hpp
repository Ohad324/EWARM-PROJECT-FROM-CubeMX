#ifndef SCREEN1PRESENTER_HPP
#define SCREEN1PRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class Screen1View;

class Screen1Presenter : public touchgfx::Presenter, public ModelListener
{
public:
    Screen1Presenter(Screen1View& v);

    virtual void activate();
    virtual void deactivate();
    virtual ~Screen1Presenter() {}

    /** Receives a BLE string from the Model and forwards it to the View. */
    virtual void onBleMessage(const char* msg);

    /** Music data arrived while on Screen1 — switch to MusicScreen. */
    virtual void onMusicPending() override;

private:
    Screen1Presenter();
    Screen1View& view;
};

#endif // SCREEN1PRESENTER_HPP
