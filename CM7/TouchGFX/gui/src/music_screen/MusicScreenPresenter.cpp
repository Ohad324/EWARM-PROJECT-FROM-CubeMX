#include <gui/music_screen/MusicScreenView.hpp>
#include <gui/music_screen/MusicScreenPresenter.hpp>
#include "log_mutex.h"

MusicScreenPresenter::MusicScreenPresenter(MusicScreenView& v)
    : view(v)
{
}

void MusicScreenPresenter::activate()
{
    LOG("[Presenter] MusicScreen activated -- dispatching cached data\n");

    if (model->hasPendingTrack())
    {
        music_done_msg_t t = model->consumePendingTrack();
        if (t.type == MUSIC_DONE_ERROR)
        {
            LOG("[Presenter] activate: pending ERROR \"%s\"\n", t.error.reason);
            view.setError(t.error.reason);
        }
        else
        {
            LOG("[Presenter] activate: pending TRACK \"%s\"\n", t.track.title);
            view.setTrackInfo(t.track.title, t.track.artist, t.track.videoId);
        }
    }

    if (model->hasPendingThumb())
    {
        music_done_msg_t t = model->consumePendingThumb();
        LOG("[Presenter] activate: pending THUMB %lu x %lu\n",
            t.thumb.width, t.thumb.height);
        view.setThumbnail(t.thumb.rgb888, t.thumb.width, t.thumb.height);
    }
}

void MusicScreenPresenter::onMusicTrack(const char* title,
                                         const char* artist,
                                         const char* videoId)
{
    LOG("[Presenter] onMusicTrack: \"%s\"\n", title);
    view.setTrackInfo(title, artist, videoId);
}

void MusicScreenPresenter::onMusicThumbnail(uint8_t* rgb888,
                                             uint32_t width,
                                             uint32_t height)
{
    LOG("[Presenter] onMusicThumbnail: %lu x %lu\n", width, height);
    view.setThumbnail(rgb888, width, height);
}

void MusicScreenPresenter::onMusicError(const char* reason)
{
    LOG("[Presenter] onMusicError: \"%s\"\n", reason);
    view.setError(reason);
}
