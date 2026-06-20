#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace fh6 {

struct PlaybackConfig {
    std::string race_start_playback = "ignore"; // "next" | "restart" | "ignore"
    bool quick_station_skip         = false;
    bool volume_normalization       = false;
    bool equalizer_enabled          = false;
    std::array<float, 5> equalizer_bands{}; // 60 / 250 / 1000 / 4000 / 12000 Hz, [-6, +6] dB
    bool force_stereo_audio         = true;
    // Pre-spawn the next track's pipeline so transitions (skip / end-of-track)
    // are instant.
    bool prebuffer_next_track       = true;
    // When false (default), the station name written to the game HUD is always
    // "Horizon CloudBeats" so the in-game notification banner does not pop up
    // on every track change. When true, the song title is written instead,
    // which triggers the banner on each new track (original behaviour).
    bool show_track_notification    = false;
};

struct GeneralConfig {
    uint16_t port                       = 8420;
    uint32_t ring_buffer_mb             = 4;
    std::string default_source          = "local_files";
    std::string fallback_source         = "local_files";
    std::filesystem::path ffmpeg_path;  // empty = look up on PATH; shared by all sources
};

struct LocalFilesConfig {
    bool enabled = true;
    std::filesystem::path music_dir;
    bool recursive = true;
    bool shuffle   = false;
    std::vector<std::string> supported_formats{"mp3",  "flac", "wav", "ogg", "m4a",
                                                "opus", "aac",  "wma", "aiff", "aif"};
};

struct YouTubeMusicConfig {
    bool enabled = false;
    std::filesystem::path cookies_path;
    std::filesystem::path yt_dlp_path; // empty = look up on PATH
    std::string default_playlist;
    bool shuffle = false;
};

struct JellyfinConfig {
    bool enabled = false;
    std::string server_url;
    std::string api_key;
    std::string user_id;
    std::string default_playlist;
    bool use_favorites = false;
    bool shuffle = false;
};

// Spotify support resolves a public playlist/album/track to its track list
// (titles + artists), then streams the matching audio from YouTube via the
// shared yt-dlp pipeline. No Spotify login or Premium required; the audio is
// sourced from YouTube, not from Spotify's DRM-protected streams.
struct SpotifyConfig {
    bool enabled = false;
    std::string default_playlist;   // optional open.spotify.com URL loaded at startup
    bool shuffle = false;
};

// NetEase Cloud Music (NetEase) support. Connects to a user-deployed
// NeteaseCloudMusicApi server (https://github.com/Binaryify/NeteaseCloudMusicApi)
// to fetch playlists/albums and stream audio. Optionally supply a cookie for
// high-quality (320 kbps) streaming and private playlist access.
struct NetEaseConfig {
    bool enabled = false;
    std::string api_url;           // e.g. "http://127.0.0.1:3000"
    std::string cookie;            // optional login cookie for high-quality audio
    std::string default_playlist;  // optional playlist/album URL or ID loaded at startup
    bool shuffle = false;
};

struct AudioConfig {
    float output_gain = 1.0f;
};

struct Config {
    GeneralConfig general;
    LocalFilesConfig local_files;
    YouTubeMusicConfig youtube_music;
    AudioConfig audio;
    JellyfinConfig jellyfin;
    SpotifyConfig spotify;
    NetEaseConfig net_ease;
    PlaybackConfig playback;
};

// Missing file is fine, defaults are returned.
Config load_config(const std::filesystem::path& path);

// Atomic write (temp + rename). Throws std::system_error on failure.
void save_config(const std::filesystem::path& path, const Config& cfg);

} // namespace fh6
