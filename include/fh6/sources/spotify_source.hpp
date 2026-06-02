#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/sources/youtube_music_source.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace fh6::sources {

// Plays Spotify playlists / albums / tracks WITHOUT a Spotify login or Premium.
// It resolves the public track list (titles + artists) by scraping Spotify's
// open embed page (no credentials), then streams the matching audio from
// YouTube through an internal YouTubeMusicSource (yt-dlp `ytsearch1:` + ffmpeg).
// The audio therefore comes from YouTube, never from Spotify's DRM streams.
class SpotifySource final : public IAudioSource {
public:
    SpotifySource(SpotifyConfig cfg, YouTubeMusicConfig yt_backend_cfg,
                  std::filesystem::path ffmpeg_path);
    ~SpotifySource() override;

    std::string_view name() const noexcept override { return "spotify"; }
    std::string_view display_name() const noexcept override { return "Spotify"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override     { yt_->play(); }
    void pause() override    { yt_->pause(); }
    void stop() override     { yt_->stop(); }
    void next() override     { yt_->next(); }
    void previous() override { yt_->previous(); }
    bool skip_next() override       { return yt_->skip_next(); }
    bool restart_current() override { return yt_->restart_current(); }
    void pump(RingBuffer& ring) override { yt_->pump(ring); }
    void set_playback_options(const PlaybackConfig& opts) override {
        yt_->set_playback_options(opts);
    }

    // Resolve a Spotify URL and start playing. Returns false if the playlist
    // could not be resolved (private/empty/unreachable). Runs the HTTP fetch
    // synchronously (a few seconds at most), like the Jellyfin cast.
    bool cast(std::string spotify_url);

    void set_shuffle(bool shuffle);
    void set_ffmpeg_path(std::filesystem::path p) { yt_->set_ffmpeg_path(std::move(p)); }

    TrackInfo current_track() const override { return yt_->current_track(); }
    PlaybackState playback_state() const noexcept override { return yt_->playback_state(); }
    AuthState auth_state() const noexcept override { return auth_; }
    std::string auth_instructions() const override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    bool shuffle() const noexcept { return cfg_.shuffle; }

private:
    SpotifyConfig cfg_;
    std::unique_ptr<YouTubeMusicSource> yt_;   // internal audio engine (YouTube)
    mutable std::mutex mu_;
    std::string last_url_;
    AuthState auth_ = AuthState::none_required;
};

} // namespace fh6::sources
