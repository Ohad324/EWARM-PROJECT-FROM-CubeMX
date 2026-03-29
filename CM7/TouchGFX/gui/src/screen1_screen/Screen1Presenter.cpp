#include <gui/screen1_screen/Screen1View.hpp>
#include <gui/screen1_screen/Screen1Presenter.hpp>
#include <gui/common/FrontendApplication.hpp>
#include <touchgfx/Application.hpp>
#include "log_mutex.h"    /* LOG() — mutex-guarded printf */

Screen1Presenter::Screen1Presenter(Screen1View& v)
    : view(v)
{

}

void Screen1Presenter::activate()
{
}

void Screen1Presenter::deactivate()
{
}

void Screen1Presenter::onBleMessage(const char* msg)
{
    LOG("[Presenter] forwarding to View: \"%s\"\n", msg);
    view.setBleMessage(msg);
}

void Screen1Presenter::onMusicPending()
{
    LOG("[Presenter] onMusicPending -- switching to MusicScreen\n");
    static_cast<FrontendApplication*>(
        touchgfx::Application::getInstance())->gotoMusicScreenNoTransition();
}
