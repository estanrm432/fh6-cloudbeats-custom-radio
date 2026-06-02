#include "fh6/sources/spotify_source.hpp"
#include "fh6/log.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fh6::sources {

namespace {

constexpr int kHttpTimeoutMs = 8000;

struct WinHttpDeleter {
    void operator()(void* h) const noexcept { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Parse "open.spotify.com/<type>/<id>" (with optional locale prefix and query)
// into ("<type>", "<id>"). Accepts playlist | album | track.
std::optional<std::pair<std::string, std::string>> parse_spotify_url(const std::string& url) {
    static const char* kinds[] = {"playlist", "album", "track"};
    for (const char* k : kinds) {
        std::string needle = std::string("/") + k + "/";
        auto p = url.find(needle);
        if (p == std::string::npos) continue;
        std::size_t start = p + needle.size();
        std::size_t end   = start;
        while (end < url.size() && (std::isalnum((unsigned char)url[end]) || url[end] == '_'))
            ++end;
        if (end > start) return std::make_pair(std::string(k), url.substr(start, end - start));
    }
    // Also accept the spotify:playlist:ID URI form.
    for (const char* k : kinds) {
        std::string needle = std::string("spotify:") + k + ":";
        auto p = url.find(needle);
        if (p == std::string::npos) continue;
        std::size_t start = p + needle.size();
        std::size_t end   = start;
        while (end < url.size() && (std::isalnum((unsigned char)url[end]) || url[end] == '_'))
            ++end;
        if (end > start) return std::make_pair(std::string(k), url.substr(start, end - start));
    }
    return std::nullopt;
}

// GET https://open.spotify.com/embed/<type>/<id> and return the HTML body.
std::optional<std::string> fetch_embed(const std::string& type, const std::string& id) {
    const std::wstring path = widen("/embed/" + type + "/" + id);

    WinHttpHandle session{WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                      L"AppleWebKit/537.36 (KHTML, like Gecko) "
                                      L"Chrome/120.0 Safari/537.36",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);
    // Let WinHTTP advertise + transparently decode gzip/deflate so Spotify's
    // compressed HTML arrives as plain text (otherwise __NEXT_DATA__ is hidden
    // inside a compressed blob and parsing silently fails).
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_DECOMPRESSION,
                     &decompress, sizeof(decompress));

    WinHttpHandle conn{WinHttpConnect(session.get(), L"open.spotify.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!conn) return std::nullopt;

    WinHttpHandle req{WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)};
    if (!req) return std::nullopt;

    // A desktop Accept-Language helps Spotify return the standard embed payload.
    const wchar_t* hdrs = L"Accept-Language: en-US,en;q=0.9\r\n";
    WinHttpAddRequestHeaders(req.get(), hdrs, (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.get(), nullptr)) {
        log::error("[spotify] HTTP send/receive failed (err {})", GetLastError());
        return std::nullopt;
    }

    DWORD status = 0, status_sz = sizeof(status);
    WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        log::error("[spotify] embed returned HTTP {}", status);
        return std::nullopt;
    }

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
    log::info("[spotify] embed fetch ok: {} bytes", body.size());
    return body;
}

// Extract the JSON embedded in <script id="__NEXT_DATA__" ...>...</script>.
std::optional<nlohmann::json> extract_next_data(const std::string& html) {
    const std::string marker = "__NEXT_DATA__";
    auto m = html.find(marker);
    if (m == std::string::npos) {
        log::warn("[spotify] __NEXT_DATA__ not found in {}-byte response; head: {}",
                  html.size(), html.substr(0, std::min<std::size_t>(html.size(), 160)));
        return std::nullopt;
    }
    auto gt = html.find('>', m);
    if (gt == std::string::npos) return std::nullopt;
    auto end = html.find("</script>", gt);
    if (end == std::string::npos) return std::nullopt;
    std::string json_text = html.substr(gt + 1, end - gt - 1);
    try {
        return nlohmann::json::parse(json_text);
    } catch (const std::exception& e) {
        log::error("[spotify] failed to parse embedded JSON: {}", e.what());
        return std::nullopt;
    }
}

// True if an object looks like a track entry: has a title and some artist
// info (playlists use "subtitle"; single tracks use an "artists" array).
bool looks_like_track(const nlohmann::json& o) {
    return o.is_object() && o.contains("title") &&
           (o.contains("subtitle") || o.contains("artists"));
}

// Derive the artist string from either "subtitle" or the "artists" array.
std::string extract_artist(const nlohmann::json& o) {
    if (auto it = o.find("subtitle"); it != o.end() && it->is_string() &&
        !it->get<std::string>().empty())
        return it->get<std::string>();
    if (auto it = o.find("artists"); it != o.end() && it->is_array()) {
        std::string out;
        for (const auto& a : *it) {
            std::string n = a.value("name", "");
            if (n.empty()) continue;
            if (!out.empty()) out += ", ";
            out += n;
        }
        return out;
    }
    return {};
}

// Recursively locate the first array whose objects look like track entries —
// that is the embed's trackList regardless of its exact path.
const nlohmann::json* find_track_list(const nlohmann::json& node) {
    if (node.is_array()) {
        if (!node.empty() && looks_like_track(node.front())) return &node;
        for (const auto& el : node)
            if (auto* r = find_track_list(el)) return r;
    } else if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it)
            if (auto* r = find_track_list(it.value())) return r;
    }
    return nullptr;
}

// Pull a best-effort cover-art URL out of an entity node (sources[].url).
std::string biggest_cover(const nlohmann::json& obj) {
    if (!obj.is_object()) return {};
    auto find_sources = [](const nlohmann::json& n) -> const nlohmann::json* {
        if (n.is_object()) {
            if (auto it = n.find("sources"); it != n.end() && it->is_array() && !it->empty())
                return &*it;
        }
        return nullptr;
    };
    for (const char* key : {"coverArt", "visualIdentity", "image"}) {
        if (auto it = obj.find(key); it != obj.end()) {
            if (auto* srcs = find_sources(*it)) {
                std::string best; int best_w = -1;
                for (const auto& s : *srcs) {
                    int w = s.value("width", 0);
                    std::string u = s.value("url", "");
                    if (!u.empty() && w > best_w) { best_w = w; best = u; }
                }
                if (!best.empty()) return best;
            }
        }
    }
    return {};
}

struct SpotTrack {
    std::string title;
    std::string artist;
    std::string artwork;
    std::uint64_t duration_ms = 0;
};

std::optional<std::vector<SpotTrack>> resolve_spotify(const std::string& url) {
    auto parsed = parse_spotify_url(url);
    if (!parsed) {
        log::warn("[spotify] not a recognised Spotify URL: {}", url);
        return std::nullopt;
    }
    const auto& [type, id] = *parsed;
    auto html = fetch_embed(type, id);
    if (!html) return std::nullopt;

    auto data = extract_next_data(*html);
    if (!data) return std::nullopt;

    const nlohmann::json* list = find_track_list(*data);
    std::vector<SpotTrack> out;

    if (list) {
        for (const auto& t : *list) {
            SpotTrack s;
            s.title  = t.value("title", "");
            s.artist = extract_artist(t);
            s.artwork = biggest_cover(t);
            if (auto it = t.find("duration"); it != t.end() && it->is_number())
                s.duration_ms = it->get<std::uint64_t>();
            if (!s.title.empty()) out.push_back(std::move(s));
        }
    }

    // Single track / album embeds expose only an entity (no trackList).
    if (out.empty()) {
        std::function<const nlohmann::json*(const nlohmann::json&)> find_entity =
            [&](const nlohmann::json& n) -> const nlohmann::json* {
            if (n.is_object()) {
                if (looks_like_track(n)) return &n;
                for (auto it = n.begin(); it != n.end(); ++it)
                    if (auto* r = find_entity(it.value())) return r;
            } else if (n.is_array()) {
                for (const auto& el : n)
                    if (auto* r = find_entity(el)) return r;
            }
            return nullptr;
        };
        if (auto* e = find_entity(*data)) {
            SpotTrack s;
            s.title   = e->value("title", "");
            s.artist  = extract_artist(*e);
            s.artwork = biggest_cover(*e);
            if (auto it = e->find("duration"); it != e->end() && it->is_number())
                s.duration_ms = it->get<std::uint64_t>();
            if (!s.title.empty()) out.push_back(std::move(s));
        }
    }

    if (out.empty()) {
        log::warn("[spotify] resolved 0 tracks from {} (private or unsupported?)", url);
        return std::nullopt;
    }
    log::info("[spotify] resolved {} track(s) from {}", out.size(), url);
    return out;
}

// Escape a query for a yt-dlp `ytsearch1:"..."` argument.
std::string make_search_token(const SpotTrack& t) {
    std::string q = t.artist.empty() ? t.title : (t.artist + " - " + t.title);
    // Strip characters that would confuse the search / shell quoting.
    std::string clean;
    for (char c : q) {
        if (c == '"' || c == '\r' || c == '\n') continue;
        clean += c;
    }
    return "ytsearch1:" + clean;
}

} // namespace

SpotifySource::SpotifySource(SpotifyConfig cfg, YouTubeMusicConfig yt_backend_cfg,
                             std::filesystem::path ffmpeg_path)
    : cfg_{std::move(cfg)} {
    yt_backend_cfg.enabled  = true;          // internal engine is always usable
    yt_backend_cfg.shuffle  = cfg_.shuffle;  // mirror our shuffle into the engine
    yt_ = std::make_unique<YouTubeMusicSource>(std::move(yt_backend_cfg),
                                               std::move(ffmpeg_path));
}

SpotifySource::~SpotifySource() = default;

bool SpotifySource::initialize() {
    if (!cfg_.enabled) return false;
    yt_->initialize();
    if (!cfg_.default_playlist.empty()) {
        // Resolve the default playlist lazily on first cast/play to avoid a
        // blocking network call during bridge startup. We just remember it.
        std::scoped_lock lk{mu_};
        last_url_ = cfg_.default_playlist;
    }
    return true;
}

void SpotifySource::shutdown() noexcept { yt_->shutdown(); }

bool SpotifySource::cast(std::string spotify_url) {
    auto tracks = resolve_spotify(spotify_url);
    if (!tracks) return false;

    std::vector<std::string> tokens;
    std::vector<TrackInfo>   metas;
    tokens.reserve(tracks->size());
    metas.reserve(tracks->size());
    for (auto& t : *tracks) {
        tokens.push_back(make_search_token(t));
        TrackInfo info;
        info.title       = t.title;
        info.artist      = t.artist;
        info.artwork_url = t.artwork;
        info.duration_ms = t.duration_ms;
        metas.push_back(std::move(info));
    }

    {
        std::scoped_lock lk{mu_};
        last_url_ = std::move(spotify_url);
    }
    yt_->load_external_queue(std::move(tokens), std::move(metas));
    yt_->stop();
    yt_->play();
    return true;
}

void SpotifySource::set_shuffle(bool shuffle) {
    {
        std::scoped_lock lk{mu_};
        cfg_.shuffle = shuffle;
    }
    yt_->set_shuffle(shuffle);
}

std::string SpotifySource::auth_instructions() const {
    return "Paste a public Spotify playlist, album, or track link. No Spotify "
           "login or Premium is needed — audio is streamed from YouTube. "
           "Private playlists must be made public to be readable.";
}

} // namespace fh6::sources
