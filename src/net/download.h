// Downloads of public files into a file over HTTPS (the coach's voice model, src/tts/model_store.h).
// Built on httpStream (transport.h). Unlike the online client, which never follows a redirect, a
// download follows them itself because file hosts answer with one (Hugging Face sends a file to
// its CDN, GitHub a release asset to its storage host), under these rules:
//   - at most DownloadRequest::maxRedirects (5) redirects (301, 302, 303, 307, 308), the Location
//     absolute, scheme-relative or relative;
//   - HTTPS only. Plain HTTP only to a loopback host (tests, development servers), and never
//     after an HTTPS answer (no downgrade);
//   - no credentials, ever: no Authorization or Cookie header is sent, a URL with user
//     information ("user:password@host") is refused, and nothing of one answer is passed on to
//     the next host;
//   - the body streams into "<path>.part" (never whole in memory), hashed as it arrives; the file
//     is renamed to <path> only once its size and SHA-256 match the expected ones;
//   - a .part left by an earlier attempt is continued with a Range request when the server
//     honours it (206 and the matching Content-Range), else the file starts over; a transfer that
//     breaks off is continued the same way, a few times;
//   - a size cap (the expected size when it is known), a timeout on connecting and on every wait
//     for data (no limit on the whole transfer while data keeps coming).
// Blocking: call it from a worker thread. A CancelToken aborts it from another thread.
#pragma once
#include "transport.h"
#include <cstdint>
#include <functional>
#include <string>

namespace net {

struct Url {
    bool tls = true;              // https (else http)
    std::string host;             // DNS name or IP literal (IPv6 without brackets), lower-case
    uint16_t port = 443;
    std::string path = "/";       // path and query, as sent in the request line
};
// "https://host[:port][/path][?query]" (or http://). False for other schemes, user information,
// a bad port or characters that cannot be sent (spaces, controls). A fragment is dropped.
bool parseUrl(const std::string& text, Url& out);
// The target of a redirect: 'location' resolved against the URL that answered (absolute URL,
// "//host/path", "/path" or a path relative to the current one; "." and ".." segments removed).
bool resolveLocation(const Url& base, const std::string& location, Url& out);
std::string urlText(const Url& u);

struct DownloadRequest {
    std::string url;
    std::string path;                 // destination file (UTF-8); the data goes to path + ".part"
    std::string userAgent = "Scacelith";
    uint64_t expectedSize = 0;        // 0 = unknown; otherwise the file must have exactly this size
    std::string sha256;               // expected digest, hex lower-case ("" = not checked)
    uint64_t maxBytes = 0;            // size cap; 0 = expectedSize, or 2 GiB when that is unknown
    bool resume = true;               // continue an existing .part (else it is discarded)
    int maxRedirects = 5;
    int timeoutMs = 30000;            // connecting, and each wait for data
    int retries = 3;                  // reconnections after a transfer broke off (each continues it)
    int retryDelayMs = 1000;
    // Progress from the downloading thread: bytes of the file so far, its total size (0 while
    // unknown). Called for every piece received (about every 16-64 KiB), so keep it cheap.
    std::function<void(uint64_t done, uint64_t total)> onProgress;
};

struct DownloadResult {
    bool ok = false;
    // "" on success, else: "url" (bad URL), "insecure" (plain HTTP to another host than loopback,
    // or after HTTPS), "redirect" (too many, or one without a Location), "http" (an error status,
    // see status), "network", "timeout", "tls", "certificate", "truncated" (the connection ended
    // early, retries used up), "too_large", "size" (not the expected size), "hash" (not the
    // expected SHA-256), "io" (the file cannot be written), "cancelled", "unavailable".
    std::string error;
    int status = 0;                   // last HTTP status received
    std::string detail;               // for the log
    std::string host;                 // the host that sent the data (after the redirects)
    uint64_t bytes = 0;               // size of the file (or of the .part on failure)
    std::string sha256;               // digest of the file (hex), when complete
    bool resumed = false;             // some of the data came from an earlier .part or attempt
    int redirects = 0;                // redirects followed by the last request
};

DownloadResult download(const DownloadRequest& req, CancelToken* cancel = nullptr);

// SHA-256 of a file, read in pieces (hex lower-case; "" when it cannot be read or 'cancel' is set).
// 'onProgress' receives the bytes read so far.
std::string fileSha256(const std::string& path, const std::atomic<bool>* cancel = nullptr,
                       const std::function<void(uint64_t)>& onProgress = nullptr);

}  // namespace net
