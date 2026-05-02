#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include "ble_queue.h"           /* xBleQueue, BLE_MSG_LEN */
#include "music_display_task.h"  /* xMusicDoneQueue, music_done_msg_t */
#include "log_mutex.h"           /* LOG() -- mutex-guarded printf */
#include <string.h>

Model::Model() : modelListener(0),
                 m_hasPendingTrack(false),
                 m_hasPendingThumb(false)
{
    memset(messageLog, 0, sizeof(messageLog));
    memset(&m_cachedTrack, 0, sizeof(m_cachedTrack));
    memset(&m_cachedThumb, 0, sizeof(m_cachedThumb));
}

void Model::tick()
{
    char msg[BLE_MSG_LEN];
    if (xQueueReceive(xBleQueue, msg, 0) == pdTRUE)
    {
        LOG("[Model] tick() dequeued: \"%s\"\n", msg);
        if (modelListener)
        {
            /* Strip "RESULT:" prefix — forward only the translated word */
            const char* payload = msg;
            if (strncmp(msg, "RESULT:", 7) == 0)
            {
                payload = msg + 7;
            }
            /* Shift entries 0..8 → 1..9, write new word into slot 0.
             * Typed array pointers give the analyser full row-count
             * information to verify the (MSG_LOG_SIZE-1)*BLE_MSG_LEN extent. */
            char (*src)[BLE_MSG_LEN] = &messageLog[0];
            char (*dst)[BLE_MSG_LEN] = &messageLog[1];
            memmove(dst, src, (MSG_LOG_SIZE - 1u) * sizeof(*src));
            strncpy(messageLog[0], payload, BLE_MSG_LEN - 1);
            messageLog[0][BLE_MSG_LEN - 1] = '\0';

            LOG("[Model] messageLog after push:\n");
            for (int i = 0; i < MSG_LOG_SIZE; i++)
            {
                LOG("  [%d] \"%s\"\n", i, messageLog[i]);
            }
            modelListener->onBleMessage(messageLog[0]);
        }
        else
        {
            LOG("[Model] WARNING: no modelListener attached!\n");
        }
    }

    /* ── Poll music done queue ───────────────────────────────────────── */
    if (modelListener == 0)
        return;

    music_done_msg_t done;
    if (xQueueReceive(xMusicDoneQueue, &done, 0) == pdTRUE)
    {
        if (modelListener->isMusicScreenActive())
        {
            /* MusicScreen is already showing — dispatch directly. */
            switch (done.type)
            {
            case MUSIC_DONE_TRACK:
                LOG("[Model] MUSIC_DONE_TRACK (direct): \"%s\"\n", done.track.title);
                modelListener->onMusicTrack(done.track.title,
                                             done.track.artist,
                                             done.track.videoId);
                break;
            case MUSIC_DONE_THUMB:
                LOG("[Model] MUSIC_DONE_THUMB (direct): %lu x %lu\n",
                    done.thumb.width, done.thumb.height);
                modelListener->onMusicThumbnail(done.thumb.rgb888,
                                                 done.thumb.width,
                                                 done.thumb.height);
                break;
            case MUSIC_DONE_ERROR:
                LOG("[Model] MUSIC_DONE_ERROR (direct): \"%s\"\n", done.error.reason);
                modelListener->onMusicError(done.error.reason);
                break;
            default:
                break;
            }
        }
        else
        {
            /* Not on MusicScreen — cache data and request screen switch. */
            switch (done.type)
            {
            case MUSIC_DONE_TRACK:
                LOG("[Model] MUSIC_DONE_TRACK (cached): \"%s\"\n", done.track.title);
                m_cachedTrack     = done;
                m_hasPendingTrack = true;
                break;
            case MUSIC_DONE_THUMB:
                LOG("[Model] MUSIC_DONE_THUMB (cached): %lu x %lu\n",
                    done.thumb.width, done.thumb.height);
                m_cachedThumb     = done;
                m_hasPendingThumb = true;
                break;
            case MUSIC_DONE_ERROR:
                LOG("[Model] MUSIC_DONE_ERROR (cached): \"%s\"\n", done.error.reason);
                m_cachedTrack     = done;   /* reuse track slot for error display */
                m_hasPendingTrack = true;
                break;
            default:
                break;
            }
            LOG("[Model] requesting screen switch -> MusicScreen\n");
            modelListener->onMusicPending();
        }
    }
}
