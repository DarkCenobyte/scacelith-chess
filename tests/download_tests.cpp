// net::download (net/download.h) against a scripted loopback server (tests/http_fake.h): URLs and
// redirects as file hosts send them, Range continuation after a cut, the checks (size, SHA-256,
// cap), cancellation and timeouts.
#include "test.h"
#include "http_fake.h"
#include "net/crypto.h"
#include "net/download.h"
#include "net/net_sys.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

std::string bytes(size_t n, uint32_t seed) {
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = char(x >> 24);
    }
    return s;
}

std::string sha(const std::string& s) { return net::crypto::hex(net::crypto::sha256(s)); }

std::string slurp(const std::string& path) {
    std::string out;
    if (!net::sys::readFile(path, out, size_t(1) << 28)) return "<missing>";
    return out;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::FILE* f = net::sys::openFile(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

// A destination file in the test folder, removed with its .part before and after the test.
struct Dest {
    std::string path;
    explicit Dest(const std::string& tag) : path(net::sys::exeDirectory() + "dltest-" + tag + ".bin") { clean(); }
    ~Dest() { clean(); }
    void clean() {
        net::sys::removeFile(path);
        net::sys::removeFile(path + ".part");
    }
    std::string part() const { return path + ".part"; }
};

net::DownloadRequest request(const std::string& url, const Dest& d, const std::string& data) {
    net::DownloadRequest r;
    r.url = url;
    r.path = d.path;
    r.userAgent = "Scacelith/test";
    r.expectedSize = data.size();
    r.sha256 = sha(data);
    r.timeoutMs = 5000;
    r.retryDelayMs = 50;
    return r;
}

}  // namespace

TEST(download_url_parsing) {
    net::Url u;
    CHECK(net::parseUrl("https://huggingface.co/csukuangfj2/x/resolve/main/voice.bin", u));
    CHECK(u.tls);
    CHECK_EQ(u.host, std::string("huggingface.co"));
    CHECK_EQ(int(u.port), 443);
    CHECK_EQ(u.path, std::string("/csukuangfj2/x/resolve/main/voice.bin"));
    CHECK(net::parseUrl("HTTP://LocalHost:8080?x=1#frag", u));
    CHECK(!u.tls);
    CHECK_EQ(u.host, std::string("localhost"));
    CHECK_EQ(int(u.port), 8080);
    CHECK_EQ(u.path, std::string("/?x=1"));
    CHECK(net::parseUrl("http://[::1]:9/a/./b/../c", u));
    CHECK_EQ(u.host, std::string("::1"));
    CHECK_EQ(u.path, std::string("/a/c"));
    CHECK_EQ(net::urlText(u), std::string("http://[::1]:9/a/c"));
    // Never: credentials in the URL, other schemes, bad ports, spaces.
    CHECK(!net::parseUrl("https://user:secret@huggingface.co/x", u));
    CHECK(!net::parseUrl("ftp://example.com/x", u));
    CHECK(!net::parseUrl("https://example.com:0/x", u));
    CHECK(!net::parseUrl("https://example.com:70000/x", u));
    CHECK(!net::parseUrl("https://example.com:/x", u));
    CHECK(!net::parseUrl("https://exa mple.com/x", u));
    CHECK(!net::parseUrl("https://example.com/a b", u));
    CHECK(!net::parseUrl("https:///x", u));

    // Redirect targets, as Hugging Face and GitHub send them.
    net::Url base, next;
    CHECK(net::parseUrl("https://huggingface.co/old/repo/resolve/main/tts.json", base));
    CHECK(net::resolveLocation(base, "/new/repo/resolve/main/tts.json", next));   // renamed repo: 307, relative
    CHECK_EQ(net::urlText(next), std::string("https://huggingface.co/new/repo/resolve/main/tts.json"));
    CHECK(net::resolveLocation(base, "https://cas-bridge.xethub.hf.co/xet-bridge-us/abc?X-Amz-Signature=1&y=%22a%22", next));
    CHECK_EQ(next.host, std::string("cas-bridge.xethub.hf.co"));
    CHECK_EQ(next.path, std::string("/xet-bridge-us/abc?X-Amz-Signature=1&y=%22a%22"));
    CHECK(net::resolveLocation(base, "//cdn-lfs.hf.co/repos/1/2", next));
    CHECK(next.tls && next.host == "cdn-lfs.hf.co" && next.path == "/repos/1/2");
    CHECK(net::resolveLocation(base, "../../x.bin", next));
    CHECK_EQ(next.path, std::string("/old/repo/x.bin"));
    CHECK(net::resolveLocation(base, "?download=true", next));
    CHECK_EQ(next.path, std::string("/old/repo/resolve/main/tts.json?download=true"));
    CHECK(!net::resolveLocation(base, "https://a:b@evil.example/x", next));
    CHECK(!net::resolveLocation(base, "   ", next));
}

TEST(download_plain_file_and_progress) {
    const std::string data = bytes(200000, 1);
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(data, r); });
    CHECK(srv.ok());
    Dest d("plain");
    net::DownloadRequest rq = request(srv.url("/file.bin"), d, data);
    uint64_t last = 0, calls = 0, total = 0;
    bool monotonic = true;
    rq.onProgress = [&](uint64_t done, uint64_t t) {
        monotonic = monotonic && done >= last;
        last = done;
        total = t;
        ++calls;
    };
    net::DownloadResult r = net::download(rq);
    CHECK(r.ok);
    CHECK_EQ(r.error, std::string(""));
    CHECK_EQ(r.status, 200);
    CHECK_EQ(r.bytes, uint64_t(data.size()));
    CHECK_EQ(r.sha256, sha(data));
    CHECK(!r.resumed);
    CHECK(slurp(d.path) == data);
    CHECK(!net::sys::fileExists(d.part()));
    CHECK(monotonic && calls > 1);
    CHECK_EQ(last, uint64_t(data.size()));
    CHECK_EQ(total, uint64_t(data.size()));
    // What the server saw: a GET with our agent, no Range, no credentials of any kind.
    std::vector<fakehttp::Request> reqs = srv.requests();
    CHECK_EQ(reqs.size(), size_t(1));
    if (!reqs.empty()) {
        CHECK_EQ(reqs[0].method, std::string("GET"));
        CHECK_EQ(reqs[0].get("user-agent"), std::string("Scacelith/test"));
        CHECK(!reqs[0].has("range") && !reqs[0].has("authorization") && !reqs[0].has("cookie"));
    }
    // A file of unknown size and digest, streamed in chunks.
    Dest d2("chunked");
    fakehttp::Server chunked([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.body = data;
        rep.chunked = true;
        return rep;
    });
    net::DownloadRequest rq2;
    rq2.url = chunked.url("/c");
    rq2.path = d2.path;
    r = net::download(rq2);
    CHECK(r.ok);
    CHECK(slurp(d2.path) == data);
    // A body that runs to the end of the connection.
    Dest d3("eof");
    fakehttp::Server eof([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.body = data;
        rep.noLength = true;
        return rep;
    });
    rq2.url = eof.url("/e");
    rq2.path = d3.path;
    r = net::download(rq2);
    CHECK(r.ok);
    CHECK(slurp(d3.path) == data);
}

// The Hugging Face resolve endpoint: a renamed repository answers 307 with a relative Location,
// then the file answers 302 with an absolute Location on the CDN (signed query), which honours Range.
TEST(download_follows_hub_redirects) {
    const std::string data = bytes(150000, 2);
    uint16_t port = 0;
    fakehttp::Server srv([&](const fakehttp::Request& r) {
        fakehttp::Reply rep;
        if (r.path == "/old-name/repo/resolve/main/voice.bin") {
            rep.status = 307;
            rep.headers.emplace_back("Location", "/new-name/repo/resolve/main/voice.bin");
        } else if (r.path == "/new-name/repo/resolve/main/voice.bin") {
            rep.status = 302;   // to "another host" (localhost instead of 127.0.0.1)
            rep.headers.emplace_back("Location",
                                     "http://localhost:" + std::to_string(port) + "/cdn/xet/abc?X-Amz-Signature=deadbeef&Expires=1");
            rep.headers.emplace_back("X-Linked-Size", std::to_string(data.size()));
            rep.headers.emplace_back("X-Linked-Etag", "\"" + sha(data) + "\"");
            rep.body = "Found. Redirecting";
        } else if (r.path == "/cdn/xet/abc?X-Amz-Signature=deadbeef&Expires=1") {
            rep = fakehttp::fileReply(data, r);
        } else {
            rep.status = 404;
        }
        return rep;
    });
    port = srv.port();
    Dest d("hub");
    net::DownloadResult r = net::download(request(srv.url("/old-name/repo/resolve/main/voice.bin"), d, data));
    CHECK(r.ok);
    CHECK_EQ(r.redirects, 2);
    CHECK_EQ(r.host, std::string("localhost"));
    CHECK(slurp(d.path) == data);
    for (const fakehttp::Request& q : srv.requests()) CHECK(!q.has("authorization") && !q.has("cookie"));
}

TEST(download_checks_size_and_hash) {
    const std::string data = bytes(50000, 3);
    std::string served = data;
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(served, r); });
    // One byte changed: the digest differs, nothing is left behind.
    served[1234] ^= 1;
    Dest d("hash");
    net::DownloadResult r = net::download(request(srv.url("/f"), d, data));
    CHECK(!r.ok);
    CHECK_EQ(r.error, std::string("hash"));
    CHECK(!net::sys::fileExists(d.path) && !net::sys::fileExists(d.part()));
    // Another size: refused from the Content-Length, before the body.
    served = data + "x";
    r = net::download(request(srv.url("/f"), d, data));
    CHECK_EQ(r.error, std::string("size"));
    CHECK(!net::sys::fileExists(d.path));
    // A size cap without an expected size.
    net::DownloadRequest rq;
    rq.url = srv.url("/f");
    rq.path = d.path;
    rq.maxBytes = 1000;
    r = net::download(rq);
    CHECK_EQ(r.error, std::string("too_large"));
    CHECK(!net::sys::fileExists(d.path) && !net::sys::fileExists(d.part()));
    // ... also when the server announces no length.
    fakehttp::Server open([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.body = data;
        rep.noLength = true;
        return rep;
    });
    rq.url = open.url("/f");
    r = net::download(rq);
    CHECK_EQ(r.error, std::string("too_large"));
    CHECK(!net::sys::fileExists(d.path));
}

// The connection breaks off in the middle of the body: the download goes on with a Range request
// from the byte where it stopped.
TEST(download_cut_connection_continues_with_range) {
    const std::string data = bytes(300000, 4);
    int answered = 0;
    fakehttp::Server srv([&](const fakehttp::Request& r) {
        fakehttp::Reply rep = fakehttp::fileReply(data, r);
        if (answered++ == 0) rep.cutAfter = 120000;
        return rep;
    });
    Dest d("cut");
    net::DownloadResult r = net::download(request(srv.url("/f"), d, data));
    CHECK(r.ok);
    CHECK(r.resumed);
    CHECK(slurp(d.path) == data);
    std::vector<fakehttp::Request> reqs = srv.requests();
    CHECK_EQ(reqs.size(), size_t(2));
    if (reqs.size() == 2) {
        CHECK(!reqs[0].has("range"));
        CHECK_EQ(reqs[1].get("range"), std::string("bytes=120000-"));
    }
    // The same through a chunked body.
    answered = 0;
    srv.setHandler([&](const fakehttp::Request& r) {
        fakehttp::Reply rep = fakehttp::fileReply(data, r);
        rep.chunked = true;
        if (answered++ == 0) rep.cutAfter = 70000;
        return rep;
    });
    Dest d2("cut2");
    r = net::download(request(srv.url("/f"), d2, data));
    CHECK(r.ok && r.resumed);
    CHECK(slurp(d2.path) == data);
}

TEST(download_without_range_support_starts_over) {
    const std::string data = bytes(200000, 5);
    int answered = 0;
    fakehttp::Server srv([&](const fakehttp::Request& r) {
        fakehttp::Reply rep = fakehttp::fileReply(data, r, false);   // Range ignored: always 200
        if (answered++ == 0) rep.cutAfter = 90000;
        return rep;
    });
    Dest d("norange");
    net::DownloadResult r = net::download(request(srv.url("/f"), d, data));
    CHECK(r.ok);
    CHECK(!r.resumed);
    CHECK(slurp(d.path) == data);
    CHECK_EQ(srv.requests().size(), size_t(2));
}

// A .part from an earlier run (the game was closed or the download cancelled): continued.
TEST(download_continues_an_earlier_part) {
    const std::string data = bytes(180000, 6);
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(data, r); });
    Dest d("earlier");
    CHECK(writeFile(d.part(), data.substr(0, 100000)));
    net::DownloadResult r = net::download(request(srv.url("/f"), d, data));
    CHECK(r.ok && r.resumed);
    CHECK(slurp(d.path) == data);
    std::vector<fakehttp::Request> reqs = srv.requests();
    CHECK(reqs.size() == 1 && reqs[0].get("range") == "bytes=100000-");
    // A complete .part needs no request at all.
    Dest d2("complete");
    CHECK(writeFile(d2.part(), data));
    r = net::download(request(srv.url("/f"), d2, data));
    CHECK(r.ok);
    CHECK_EQ(srv.requests().size(), size_t(1));
    CHECK(slurp(d2.path) == data);
    // A .part of another file: the continued file does not hash right, so it starts over.
    Dest d3("stale");
    CHECK(writeFile(d3.part(), bytes(100000, 99)));
    r = net::download(request(srv.url("/f"), d3, data));
    CHECK(r.ok);
    CHECK(slurp(d3.path) == data);
    reqs = srv.requests();
    CHECK(reqs.size() == 3 && reqs[1].get("range") == "bytes=100000-" && !reqs[2].has("range"));
    // A .part longer than the file: 416, then from the start.
    Dest d4("long");
    net::DownloadRequest rq = request(srv.url("/f"), d4, data);
    rq.expectedSize = 0;   // unknown size: the .part is kept and sent as a Range
    CHECK(writeFile(d4.part(), data + "trailing"));
    r = net::download(rq);
    CHECK(r.ok);
    CHECK(slurp(d4.path) == data);
}

TEST(download_redirect_rules_and_errors) {
    const std::string data = bytes(1000, 7);
    fakehttp::Server srv([&](const fakehttp::Request& r) {
        fakehttp::Reply rep;
        if (r.path.compare(0, 6, "/loop/") == 0) {   // /loop/N -> /loop/N+1
            rep.status = 302;
            rep.headers.emplace_back("Location", "/loop/" + std::to_string(std::atoi(r.path.c_str() + 6) + 1));
        } else if (r.path == "/plain") {
            rep.status = 302;
            rep.headers.emplace_back("Location", "http://example.com/file");   // plain HTTP elsewhere
        } else if (r.path == "/nolocation") {
            rep.status = 302;
        } else if (r.path == "/server-error") {
            rep.status = 503;
        } else {
            rep.status = 404;
        }
        return rep;
    });
    Dest d("rules");
    net::DownloadResult r = net::download(request(srv.url("/loop/0"), d, data));
    CHECK_EQ(r.error, std::string("redirect"));
    CHECK_EQ(srv.requests().size(), size_t(6));   // the first answer and five redirects
    r = net::download(request(srv.url("/plain"), d, data));
    CHECK_EQ(r.error, std::string("insecure"));
    r = net::download(request(srv.url("/nolocation"), d, data));
    CHECK_EQ(r.error, std::string("redirect"));
    r = net::download(request(srv.url("/missing"), d, data));
    CHECK_EQ(r.error, std::string("http"));
    CHECK_EQ(r.status, 404);
    r = net::download(request(srv.url("/server-error"), d, data));
    CHECK_EQ(r.error, std::string("http"));
    CHECK_EQ(r.status, 503);
    // Plain HTTP to another host than loopback is refused before any connection.
    r = net::download(request("http://example.com/file", d, data));
    CHECK_EQ(r.error, std::string("insecure"));
    r = net::download(request("ftp://example.com/file", d, data));
    CHECK_EQ(r.error, std::string("url"));
    CHECK(!net::sys::fileExists(d.path));
}

TEST(download_cancel_and_timeout) {
    const std::string data = bytes(400000, 8);
    fakehttp::Server slow([&](const fakehttp::Request& r) {
        fakehttp::Reply rep = fakehttp::fileReply(data, r);
        rep.pieceDelayMs = 40;   // 16 KiB every 40 ms: about 1 s for the file
        return rep;
    });
    Dest d("cancel");
    net::CancelToken cancel;
    net::DownloadRequest rq = request(slow.url("/f"), d, data);
    std::atomic<uint64_t> seen{0};
    rq.onProgress = [&](uint64_t done, uint64_t) { seen = done; };
    net::DownloadResult r;
    std::thread t([&] { r = net::download(rq, &cancel); });
    while (seen.load() < 50000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto t0 = std::chrono::steady_clock::now();
    cancel.cancel();
    t.join();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(r.error, std::string("cancelled"));
    CHECK(ms < 500.0);
    CHECK(!net::sys::fileExists(d.path));
    uint64_t partSize = 0;
    CHECK(net::sys::fileSize(d.part(), partSize) && partSize >= 50000 && partSize < data.size());   // kept for later
    // ... and continued by the next download.
    rq.onProgress = nullptr;
    r = net::download(rq);
    CHECK(r.ok && r.resumed);
    CHECK(slurp(d.path) == data);

    // A server that never answers: the idle timeout.
    fakehttp::Server silent([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.silenceMs = 1500;
        return rep;
    });
    Dest d2("timeout");
    rq = request(silent.url("/f"), d2, data);
    rq.timeoutMs = 300;
    t0 = std::chrono::steady_clock::now();
    r = net::download(rq);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(r.error, std::string("timeout"));
    CHECK(ms >= 250.0 && ms < 1400.0);
}

TEST(download_file_sha256) {
    Dest d("filesha");
    const std::string data = bytes(3 * 1048576 + 17, 9);
    CHECK(writeFile(d.path, data));
    uint64_t last = 0;
    CHECK_EQ(net::fileSha256(d.path, nullptr, [&](uint64_t n) { last = n; }), sha(data));
    CHECK_EQ(last, uint64_t(data.size()));
    CHECK_EQ(net::fileSha256(d.path + ".absent"), std::string(""));
    // The incremental hash gives the one-shot digest whatever the pieces.
    net::crypto::Sha256Stream h;
    for (size_t at = 0; at < data.size();) {
        size_t k = std::min<size_t>(data.size() - at, 1 + (at * 7) % 70000);
        h.update(data.data() + at, k);
        at += k;
    }
    CHECK_EQ(h.bytes(), uint64_t(data.size()));
    CHECK_EQ(net::crypto::hex(h.finish()), sha(data));
    CHECK_EQ(net::crypto::hex(h.finish()), sha(""));
    CHECK_EQ(sha(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
