#ifndef MODEL_HPP
#define MODEL_HPP

#include "ble_queue.h"           /* BLE_MSG_LEN */
#include "music_display_task.h"  /* music_done_msg_t, xMusicDoneQueue */

class ModelListener;

static const int MSG_LOG_SIZE = 10;

class Model
{
public:
    Model();

    void bind(ModelListener* listener)
    {
        modelListener = listener;
    }

    void tick();

    /** Read-only access to the message log.
     *  Index 0 = most recent word, index 9 = oldest. */
    const char* getMessageLog(int index) const
    {
        return messageLog[index];
    }

    /* ── Pending music data (set by tick, consumed by MusicScreenPresenter::activate) */
    bool hasPendingTrack() const { return m_hasPendingTrack; }
    bool hasPendingThumb() const { return m_hasPendingThumb; }

    music_done_msg_t consumePendingTrack()
    {
        m_hasPendingTrack = false;
        return m_cachedTrack;
    }
    music_done_msg_t consumePendingThumb()
    {
        m_hasPendingThumb = false;
        return m_cachedThumb;
    }

protected:
    ModelListener* modelListener;

private:
    char messageLog[MSG_LOG_SIZE][BLE_MSG_LEN];

    bool             m_hasPendingTrack;
    bool             m_hasPendingThumb;
    music_done_msg_t m_cachedTrack;
    music_done_msg_t m_cachedThumb;
};

#endif // MODEL_HPP
