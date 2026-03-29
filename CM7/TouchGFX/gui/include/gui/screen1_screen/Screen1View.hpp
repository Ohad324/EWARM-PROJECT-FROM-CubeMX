#ifndef SCREEN1VIEW_HPP
#define SCREEN1VIEW_HPP

#include <gui_generated/screen1_screen/Screen1ViewBase.hpp>
#include <gui/screen1_screen/Screen1Presenter.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
#include <touchgfx/Unicode.hpp>
#include "ble_queue.h"  /* BLE_MSG_LEN */

class Screen1View : public Screen1ViewBase
{
public:
    Screen1View();
    virtual ~Screen1View() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    /** Called by the Presenter when a new BLE/translated word arrives. */
    void setBleMessage(const char* msg);

protected:
    touchgfx::TextAreaWithOneWildcard centerTextArea;
    touchgfx::Unicode::UnicodeChar    centerTextBuf[BLE_MSG_LEN];

private:
};

#endif // SCREEN1VIEW_HPP
