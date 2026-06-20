#pragma once

#include "fh6/audio_source.hpp"
#include "fh6/config.hpp"
#include "fh6/playback_dsp.hpp"
#include "fh6/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fh6::sources {

// Track metadata fetched from the NeteaseCloudMusicApi.
struct NetEaseTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string pic_url;       // cover art URL from the API
    std::uint64_t duration_ms = 0;
};

// Resolves NetEase Cloud Music playlists/albums over HTTP via a
// user-deployed NeteaseCloudMusicApi server, then streams each item
// through ffmpeg -f s16le -ar 48000 -ac 2. The PCM pipe is drained by
// pump() in non-blocking mode (PeekNamedPipe-gated).
class NetEaseSource final : public IAudioSource {
public:
    NetEaseSource(NetEaseConfig cfg, std::filesystem::path ffmpeg_path);
    ~NetEaseSource() override;

    std::string_view name() const noexcept override         { return "net_ease"; }
    std::string_view display_name() const noexcept override { return "NetEase Cloud Music"; }

    bool initialize() override;
    void shutdown() noexcept override;

    void play() override;
    void pause() override;
    void stop() override;
    void next() override;
    void previous() override;
    void pump(RingBuffer& ring) override;

    // Settings drawer hot-update; re-fetches when api_url/playlist fields
    // change, re-shuffles in place when only shuffle flips.
    void set_config(NetEaseConfig cfg);
    void set_ffmpeg_path(std::filesystem::path p);

    // POST /api/source/net_ease/cast: swap to a specific playlist/album.
    // Accepts a full URL (music.163.com/#/playlist?id=XXXXX) or a bare
    // numeric ID. Returns false if the fetch fails.
    bool cast(std::string playlist_or_album_url);

    void set_shuffle(bool shuffle);
    void set_playback_options(const PlaybackConfig& opts) override;

    TrackInfo current_track() const override;
    PlaybackState playback_state() const noexcept override {
        return state_.load(std::memory_order_acquire);
    }
    AuthState auth_state() const noexcept override;
    SourceCapabilities capabilities() const noexcept override { return {false, true, true}; }

    bool shuffle() const noexcept { return cfg_.shuffle; }

private:
    struct Pipe;

    // mu_ held.
    bool refresh_queue_locked();   // releases mu_ across the HTTP fetch, re-acquires to swap
    std::unique_ptr<Pipe> spawn_pipe_locked(std::size_t for_idx);
    void start_pipe_locked();
    void stop_pipe_locked();
    void discard_prefetch_locked() noexcept;
    bool promote_prefetch_locked(std::size_t expected_idx);
    void maybe_spawn_prefetch_locked();
    std::size_t next_queue_idx_locked() const noexcept;
    void advance_locked(std::ptrdiff_t step);

    NetEaseConfig cfg_;
    std::filesystem::path ffmpeg_path_;

    mutable std::mutex mu_;
    std::vector<NetEaseTrack> queue_;
    std::size_t current_idx_ = 0;
    std::unique_ptr<Pipe> pipe_;
    std::unique_ptr<Pipe> prefetch_;
    std::atomic<PlaybackState> state_{PlaybackState::stopped};

    EqualizerStage eq_;
    std::atomic<bool> volume_norm_{false};
    std::atomic<bool> prebuffer_next_{true};
};

} // namespace fh6::sources