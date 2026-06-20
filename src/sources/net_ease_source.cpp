#include "fh6/sources/net_ease_source.hpp"
#include "fh6/log.hpp"
#include "fh6/subprocess.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

using subprocess::create_kill_on_close_job;
using subprocess::describe_launch_failure;
using subprocess::open_nul;
using subprocess::open_stderr_log;
using subprocess::quote;
using subprocess::spawn_in_job;
using subprocess::widen;

constexpr std::uint64_t kPcmBytesPerSec = 48000ull * 2ull * 2ull;
constexpr int kHttpTimeoutMs = 5000;

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

bool config_complete(const NetEaseConfig& c) noexcept {
    return !c.api_url.empty();
}

bool same_query_target(const NetEaseConfig& a, const NetEaseConfig& b) noexcept {
    return a.api_url == b.api_url && a.default_playlist == b.default_playlist;
}

std::optional<std::string> http_get(const NetEaseConfig& cfg, const std::string& path) {
    URL_COMPONENTS comp{};
    comp.dwStructSize     = sizeof(comp);
    comp.dwHostNameLength = (DWORD)-1;
    const std::wstring url = widen(cfg.api_url);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &comp) || !comp.lpszHostName) {
        log::error("[net_ease] invalid api_url '{}' -- expected http:// or https://",
                   cfg.api_url);
        return std::nullopt;
    }
    const std::wstring host(comp.lpszHostName, comp.dwHostNameLength);

    WinHttpHandle session{WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle conn{WinHttpConnect(session.get(), host.c_str(), comp.nPort, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), L"GET", widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          comp.nScheme == INTERNET_SCHEME_HTTPS
                                              ? WINHTTP_FLAG_SECURE : 0)};
    if (!req) return std::nullopt;

    if (!cfg.cookie.empty()) {
        const std::wstring cookie = widen("Cookie: " + cfg.cookie);
        WinHttpAddRequestHeaders(req.get(), cookie.c_str(), (ULONG)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[net_ease] HTTP send/receive failed (err {})", GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                        WINHTTP_NO_HEADER_INDEX);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail) || avail == 0) break;
        const std::size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.get(), body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if (got == 0) break;
    }

    if (status != 200) {
        log::error("[net_ease] HTTP {} from api: {}", status, body);
        return std::nullopt;
    }
    return body;
}

std::optional<std::pair<std::string, std::string>> parse_input(const std::string& input) {
    for (const char* kind : {"playlist", "album"}) {
        std::string needle = std::string("/") + kind + "?id=";
        auto p = input.find(needle);
        if (p != std::string::npos) {
            std::size_t start = p + needle.size();
            std::size_t end = start;
            while (end < input.size() && std::isdigit(static_cast<unsigned char>(input[end])))
                ++end;
            if (end > start)
                return std::make_pair(std::string(kind), input.substr(start, end - start));
        }
    }
    for (const char* kind : {"playlist", "album"}) {
        std::string needle = std::string("/") + kind + "/";
        auto p = input.find(needle);
        if (p != std::string::npos) {
            std::size_t start = p + needle.size();
            std::size_t end = start;
            while (end < input.size() && std::isdigit(static_cast<unsigned char>(input[end])))
                ++end;
            if (end > start)
                return std::make_pair(std::string(kind), input.substr(start, end - start));
        }
    }
    if (!input.empty() && std::all_of(input.begin(), input.end(),
            [](unsigned char c) { return std::isdigit(c); })) {
        return std::make_pair(std::string("playlist"), input);
    }
    return std::nullopt;
}

std::optional<std::vector<NetEaseTrack>> resolve_playlist(const NetEaseConfig& cfg,
                                                           const std::string& id) {
    auto body = http_get(cfg, "/playlist/detail?id=" + id);
    if (!body) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(*body);
        if (j.value("code", -1) != 200) {
            log::error("[net_ease] playlist API returned code {}", j.value("code", -1));
            return std::nullopt;
        }
        std::vector<NetEaseTrack> tracks;
        const auto& list = j["playlist"]["tracks"];
        for (const auto& t : list) {
            NetEaseTrack trk;
            trk.id = std::to_string(t["id"].get<std::uint64_t>());
            trk.title = t.value("name", "");
            if (!t["ar"].empty() && !t["ar"][0]["name"].get_ref<const std::string&>().empty())
                trk.artist = t["ar"][0]["name"];
            if (t.contains("al")) {
                trk.album = t["al"].value("name", "");
                trk.pic_url = t["al"].value("picUrl", "");
            }
            trk.duration_ms = t.value("dt", 0);
            tracks.push_back(std::move(trk));
        }
        log::info("[net_ease] resolved {} tracks from playlist {}", tracks.size(), id);
        return tracks;
    } catch (const std::exception& e) {
        log::error("[net_ease] failed to parse playlist response: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::vector<NetEaseTrack>> resolve_album(const NetEaseConfig& cfg,
                                                        const std::string& id) {
    auto body = http_get(cfg, "/album?id=" + id);
    if (!body) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(*body);
        if (j.value("code", -1) != 200) {
            log::error("[net_ease] album API returned code {}", j.value("code", -1));
            return std::nullopt;
        }
        std::vector<NetEaseTrack> tracks;
        const auto& songs = j["album"]["songs"];
        for (const auto& s : songs) {
            NetEaseTrack trk;
            trk.id = std::to_string(s["id"].get<std::uint64_t>());
            trk.title = s.value("name", "");
            if (!s["ar"].empty() && !s["ar"][0]["name"].get_ref<const std::string&>().empty())
                trk.artist = s["ar"][0]["name"];
            if (s.contains("al")) {
                trk.album = s["al"].value("name", "");
                trk.pic_url = s["al"].value("picUrl", "");
            }
            trk.duration_ms = s.value("dt", 0);
            tracks.push_back(std::move(trk));
        }
        log::info("[net_ease] resolved {} tracks from album {}", tracks.size(), id);
        return tracks;
    } catch (const std::exception& e) {
        log::error("[net_ease] failed to parse album response: {}", e.what());
        return std::nullopt;
    }
}

std::optional<std::string> resolve_song_url(const NetEaseConfig& cfg, const std::string& id) {
    auto body = http_get(cfg, "/song/url?id=" + id);
    if (!body) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(*body);
        if (j.value("code", -1) != 200 || j["data"].empty()) return std::nullopt;
        auto url = j["data"][0].value("url", "");
        if (url.empty()) return std::nullopt;
        return url;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pipe — ffmpeg subprocess handle + read-side state
// ---------------------------------------------------------------------------
struct NetEaseSource::Pipe {
    HANDLE job       = nullptr;
    HANDLE proc      = nullptr;
    HANDLE read_pipe = nullptr;
    std::uint64_t bytes_written = 0;
    std::atomic<std::uint64_t> position_ms{0};
    bool ended = false;
    std::size_t for_queue_idx = 0;

    ~Pipe() {
        // Close the read side first so ffmpeg's next write returns
        // ERROR_BROKEN_PIPE; dropping the job handle then reaps it via
        // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
        if (read_pipe) CloseHandle(read_pipe);
        if (job)       CloseHandle(job);
        if (proc)      CloseHandle(proc);
    }
};

// ---------------------------------------------------------------------------
// Constructor / destructor / initialize / shutdown
// ---------------------------------------------------------------------------

NetEaseSource::NetEaseSource(NetEaseConfig cfg, std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)}, ffmpeg_path_{std::move(ffmpeg_path)} {}

NetEaseSource::~NetEaseSource() = default;

bool NetEaseSource::initialize() {
    return cfg_.enabled && config_complete(cfg_);
}

void NetEaseSource::shutdown() noexcept {
    std::scoped_lock lk{mu_};
    stop_pipe_locked();
    discard_prefetch_locked();
    queue_.clear();
}

// ---------------------------------------------------------------------------
// Config hot-update, shuffle, queue refresh, cast
// ---------------------------------------------------------------------------

void NetEaseSource::set_config(NetEaseConfig cfg) {
    std::scoped_lock lk{mu_};
    const bool re_query = !same_query_target(cfg_, cfg);
    cfg_ = std::move(cfg);
    if (re_query && !cfg_.default_playlist.empty()) {
        refresh_queue_locked();
    } else if (cfg_.shuffle) {
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(queue_.begin(), queue_.end(), rng);
    }
}

void NetEaseSource::set_shuffle(bool shuffle) {
    std::scoped_lock lk{mu_};
    cfg_.shuffle = shuffle;
    if (shuffle && !queue_.empty()) {
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(queue_.begin(), queue_.end(), rng);
    }
}

bool NetEaseSource::refresh_queue_locked() {
    auto parsed = parse_input(cfg_.default_playlist);
    if (!parsed) {
        log::warn("[net_ease] could not parse input: {}", cfg_.default_playlist);
        return false;
    }
    const auto& [type, id] = *parsed;
    std::optional<std::vector<NetEaseTrack>> tracks;
    if (type == "album")
        tracks = resolve_album(cfg_, id);
    else
        tracks = resolve_playlist(cfg_, id);
    if (!tracks || tracks->empty()) return false;
    queue_ = std::move(*tracks);
    current_idx_ = 0;
    if (cfg_.shuffle) {
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(queue_.begin(), queue_.end(), rng);
    }
    return true;
}

bool NetEaseSource::cast(std::string playlist_or_album_url) {
    std::scoped_lock lk{mu_};
    cfg_.default_playlist = std::move(playlist_or_album_url);
    if (!refresh_queue_locked()) return false;
    current_idx_ = 0;
    stop_pipe_locked();
    discard_prefetch_locked();
    start_pipe_locked();
    state_.store(PlaybackState::playing, std::memory_order_release);
    return true;
}

TrackInfo NetEaseSource::current_track() const {
    std::scoped_lock lk{mu_};
    if (queue_.empty() || current_idx_ >= queue_.size()) return {};
    const auto& t = queue_[current_idx_];
    TrackInfo info{
        .title = t.title,
        .artist = t.artist,
        .album = t.album,
        .artwork_url = t.pic_url,
        .duration_ms = t.duration_ms,
        .position_ms = 0,
    };
    if (pipe_) info.position_ms = pipe_->position_ms.load(std::memory_order_acquire);
    return info;
}

AuthState NetEaseSource::auth_state() const noexcept {
    return config_complete(cfg_) ? AuthState::none_required : AuthState::needs_auth;
}

void NetEaseSource::set_ffmpeg_path(std::filesystem::path p) {
    std::scoped_lock lk{mu_};
    ffmpeg_path_ = std::move(p);
}

void NetEaseSource::set_playback_options(const PlaybackConfig& opts) {
    {
        std::scoped_lock lk{mu_};
        eq_.set_options(opts.equalizer_enabled, opts.equalizer_bands, 48000.0f);
    }
    // loudnorm is in the ffmpeg argv; new state takes effect on the next track.
    volume_norm_.store(opts.volume_normalization, std::memory_order_release);
    const bool prev = prebuffer_next_.exchange(opts.prebuffer_next_track,
                                                std::memory_order_acq_rel);
    if (prev && !opts.prebuffer_next_track) {
        std::scoped_lock lk{mu_};
        discard_prefetch_locked();
    }
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

void NetEaseSource::play() {
    std::scoped_lock lk{mu_};
    if (queue_.empty()) return;            // cast()/set_config() will populate
    if (!pipe_) start_pipe_locked();
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

void NetEaseSource::pause() {
    state_.store(PlaybackState::paused, std::memory_order_release);
}

void NetEaseSource::stop() {
    std::scoped_lock lk{mu_};
    discard_prefetch_locked();
    stop_pipe_locked();
}

void NetEaseSource::next()     { std::scoped_lock lk{mu_}; advance_locked(+1); }
void NetEaseSource::previous() { std::scoped_lock lk{mu_}; advance_locked(-1); }

// ---------------------------------------------------------------------------
// PCM pump — non-blocking drain via PeekNamedPipe + ReadFile
// ---------------------------------------------------------------------------

void NetEaseSource::pump(RingBuffer& ring) {
    if (state_.load(std::memory_order_acquire) != PlaybackState::playing) return;

    std::scoped_lock lk{mu_};
    Pipe* p = pipe_.get();
    if (!p) return;

    auto update_position = [&] {
        const std::size_t r = ring.readable();
        const std::uint64_t played = p->bytes_written > r ? p->bytes_written - r : 0;
        p->position_ms.store(played * 1000ull / kPcmBytesPerSec, std::memory_order_release);
    };
    auto on_eof = [&] {
        if (p->read_pipe) {
            CloseHandle(p->read_pipe);
            p->read_pipe = nullptr;
        }
        p->ended = true;
    };

    if (p->ended) {
        update_position();
        if (ring.readable() == 0) advance_locked(+1);
        return;
    }
    if (!p->read_pipe) return;

    DWORD avail = 0;
    if (!PeekNamedPipe(p->read_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
        on_eof();
        return;
    }
    while (avail > 0) {
        const std::size_t writable = ring.writable();
        if (writable < 4) break;
        std::size_t want = std::min<std::size_t>(writable, avail);
        if (want > 4096) want = 4096;
        want &= ~std::size_t{3};   // whole stereo s16 frames — EQ never sees half a sample
        if (!want) break;

        std::byte buf[4096];
        DWORD got = 0;
        if (!ReadFile(p->read_pipe, buf, (DWORD)want, &got, nullptr) || got == 0) {
            on_eof();
            break;
        }
        const DWORD aligned = (got / 4u) * 4u;
        if (aligned) eq_.process(reinterpret_cast<int16_t*>(buf), aligned / 4u);
        ring.write(buf, aligned);
        p->bytes_written += aligned;
        avail = avail > got ? avail - got : 0;
    }
    update_position();
    maybe_spawn_prefetch_locked();
}

// ---------------------------------------------------------------------------
// Pipe lifecycle
// ---------------------------------------------------------------------------

std::unique_ptr<NetEaseSource::Pipe>
NetEaseSource::spawn_pipe_locked(std::size_t for_idx) {
    if (queue_.empty() || for_idx >= queue_.size()) return nullptr;

    // Resolve the song URL before spawning ffmpeg.
    auto url = resolve_song_url(cfg_, queue_[for_idx].id);
    if (!url) {
        log::warn("[net_ease] failed to resolve song URL for track {}", queue_[for_idx].id);
        return nullptr;
    }

    auto pipe = std::make_unique<Pipe>();
    pipe->for_queue_idx = for_idx;
    pipe->job = create_kill_on_close_job();
    if (!pipe->job) {
        log::warn("[net_ease] CreateJobObject failed ({})", GetLastError());
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 1 << 20)) return nullptr;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE err_log = open_stderr_log();

    const std::wstring ff = ffmpeg_path_.empty() ? std::wstring{L"ffmpeg"}
                                                  : ffmpeg_path_.wstring();
    std::wstring cmd = quote(ff) +
        L" -hide_banner -loglevel error -i " + quote(widen(*url));
    if (volume_norm_.load(std::memory_order_acquire))
        cmd += L" -af loudnorm=I=-14:TP=-2:LRA=11";
    cmd += L" -f s16le -acodec pcm_s16le -ar 48000 -ac 2 pipe:1";

    pipe->proc = spawn_in_job(pipe->job, cmd, nul_in, out_w, err_log);
    const DWORD ec = pipe->proc ? 0u : GetLastError();
    CloseHandle(out_w);
    if (nul_in)  CloseHandle(nul_in);
    if (err_log) CloseHandle(err_log);
    if (!pipe->proc) {
        CloseHandle(out_r);
        log::warn("[net_ease] failed to launch ffmpeg -- {}",
                  describe_launch_failure(ff, ec, !ffmpeg_path_.empty()));
        return nullptr;  // ~Pipe reaps the job
    }

    pipe->read_pipe = out_r;
    return pipe;
}

void NetEaseSource::start_pipe_locked() {
    stop_pipe_locked();
    pipe_ = spawn_pipe_locked(current_idx_);
}

void NetEaseSource::stop_pipe_locked() {
    // Symmetric with Jellyfin: prefetch is preserved across stop_pipe_locked()
    // so a pending promotion isn't dropped on re-spawn. stop()/shutdown() drop
    // it explicitly.
    pipe_.reset();
    state_.store(PlaybackState::stopped, std::memory_order_release);
}

void NetEaseSource::discard_prefetch_locked() noexcept { prefetch_.reset(); }

// ---------------------------------------------------------------------------
// Queue navigation
// ---------------------------------------------------------------------------

std::size_t NetEaseSource::next_queue_idx_locked() const noexcept {
    if (queue_.empty()) return 0;
    return (current_idx_ + 1) % queue_.size();
}

void NetEaseSource::advance_locked(std::ptrdiff_t step) {
    if (queue_.empty()) return;
    const auto n = (std::ptrdiff_t)queue_.size();
    auto i = (std::ptrdiff_t)current_idx_ + step;
    current_idx_ = (std::size_t)(((i % n) + n) % n);
    if (step == 1 && promote_prefetch_locked(current_idx_)) {
        // Promoted: pipe_ is the pre-warmed pipeline, no fresh spawn needed.
    } else {
        discard_prefetch_locked();   // backwards step invalidates the prefetch
        start_pipe_locked();
    }
    if (pipe_) state_.store(PlaybackState::playing, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Prefetch
// ---------------------------------------------------------------------------

bool NetEaseSource::promote_prefetch_locked(std::size_t expected_idx) {
    if (!prefetch_ || prefetch_->for_queue_idx != expected_idx) {
        discard_prefetch_locked();
        return false;
    }
    pipe_ = std::move(prefetch_);
    return true;
}

void NetEaseSource::maybe_spawn_prefetch_locked() {
    if (!prebuffer_next_.load(std::memory_order_acquire)) return;
    if (prefetch_ || !pipe_ || queue_.size() < 2) return;
    // Match Jellyfin's threshold: ~0.5 s of PCM proves the current pipe is
    // viable before we commit a second ffmpeg.
    constexpr std::uint64_t kViableBytes = 96 * 1024;
    if (pipe_->bytes_written < kViableBytes) return;
    prefetch_ = spawn_pipe_locked(next_queue_idx_locked());
}

} // namespace fh6::sources