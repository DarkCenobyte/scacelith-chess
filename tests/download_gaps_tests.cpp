// net::download (net/download.h), the paths tests/download_tests.cpp leaves out, against the
// scripted loopback server of tests/http_fake.h (plain HTTP to 127.0.0.1 only, never a real URL):
// a path with bytes above ASCII, a redirect to a URL with user information, a partial answer that
// does not fit twice, one for a file of another size, a complete .part that does not hash right,
// a body over the cap or the expected size without a Content-Length, a body that ends early, a
// destination that cannot be replaced, a download cancelled before it starts and one cancelled
// while it waits to continue.
#include "test.h"
#include "headless.h"
#include "http_fake.h"
#include "net/crypto.h"
#include "net/download.h"
#include "net/net_sys.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
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
    std::string s;
    net::sys::readFile(path, s, 64u << 20);
    return s;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::FILE* f = net::sys::openFile(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

net::DownloadRequest request(const std::string& url, const std::string& path, const std::string& data) {
    net::DownloadRequest r;
    r.url = url;
    r.path = path;
    r.userAgent = "Scacelith/test";
    r.expectedSize = data.size();
    r.sha256 = sha(data);
    r.timeoutMs = 5000;
    r.retryDelayMs = 50;
    return r;
}

}  // namespace

TEST(download_gaps_path_bytes_percent_encoded) {
    net::Url u;
    CHECK(net::parseUrl("http://127.0.0.1:8080/voix/mod\xC3\xA8le.bin?v=1", u));
    CHECK_EQ(u.path, std::string("/voix/mod%C3%A8le.bin?v=1"));
    const std::string data = bytes(3000, 11);
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(data, r); });
    REQUIRE(srv.ok());
    headless::TempDir dir("dl-gaps-path");
    net::DownloadResult r = net::download(request(srv.url("/mod\xC3\xA8le.bin"), dir.file("m.bin"), data));
    CHECK(r.ok);
    CHECK(srv.requests().size() == 1 && srv.requests()[0].path == "/mod%C3%A8le.bin");
}

TEST(download_gaps_redirect_with_user_information) {
    const std::string data = bytes(1000, 12);
    fakehttp::Server srv([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.status = 302;
        rep.headers.emplace_back("Location", "http://user:secret@127.0.0.1/file");
        return rep;
    });
    REQUIRE(srv.ok());
    headless::TempDir dir("dl-gaps-userinfo");
    net::DownloadResult r = net::download(request(srv.url("/f"), dir.file("f.bin"), data));
    CHECK_EQ(r.error, std::string("redirect"));
    CHECK(r.detail.find("bad Location") != std::string::npos);
    CHECK_EQ(srv.requests().size(), size_t(1));   // never followed
    CHECK(!net::sys::fileExists(dir.file("f.bin")));
}

TEST(download_gaps_partial_answers_that_do_not_fit) {
    const std::string data = bytes(5000, 13);
    headless::TempDir dir("dl-gaps-partial");
    const std::string path = dir.file("p.bin");
    // 206 for another part than the one asked, and again once the file starts over: given up.
    {
        fakehttp::Server srv([&](const fakehttp::Request&) {
            fakehttp::Reply rep;
            rep.status = 206;
            rep.headers.emplace_back("Content-Range", "bytes 0-4999/5000");
            rep.body = data;
            return rep;
        });
        REQUIRE(srv.ok());
        CHECK(writeFile(path + ".part", data.substr(0, 1000)));
        net::DownloadResult r = net::download(request(srv.url("/f"), path, data));
        CHECK_EQ(r.error, std::string("http"));
        CHECK(r.detail.find("unusable partial answer") != std::string::npos);
        auto reqs = srv.requests();
        CHECK_EQ(reqs.size(), size_t(2));
        if (reqs.size() == 2) {
            CHECK_EQ(reqs[0].get("range"), std::string("bytes=1000-"));
            CHECK(!reqs[1].has("range"));   // the .part was dropped
        }
        CHECK(!net::sys::fileExists(path));
    }
    // 206 for the right part of a file of another size: "size", and the .part goes.
    {
        fakehttp::Server srv([&](const fakehttp::Request&) {
            fakehttp::Reply rep;
            rep.status = 206;
            rep.headers.emplace_back("Content-Range", "bytes 1000-7999/8000");
            rep.body = std::string(7000, 'x');
            return rep;
        });
        REQUIRE(srv.ok());
        CHECK(writeFile(path + ".part", data.substr(0, 1000)));
        net::DownloadResult r = net::download(request(srv.url("/f"), path, data));
        CHECK_EQ(r.error, std::string("size"));
        CHECK(r.detail.find("8000") != std::string::npos);
        CHECK(!net::sys::fileExists(path + ".part"));
        CHECK(!net::sys::fileExists(path));
    }
}

TEST(download_gaps_complete_part_that_does_not_hash_starts_over) {
    // A .part of the expected size but other bytes: checked, dropped, and the file comes again
    // from the start (no Range).
    const std::string data = bytes(4000, 14);
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(data, r); });
    REQUIRE(srv.ok());
    headless::TempDir dir("dl-gaps-rehash");
    const std::string path = dir.file("h.bin");
    CHECK(writeFile(path + ".part", bytes(4000, 99)));
    net::DownloadResult r = net::download(request(srv.url("/f"), path, data));
    CHECK(r.ok);
    CHECK(!r.resumed);
    CHECK(slurp(path) == data);
    CHECK(srv.requests().size() == 1 && !srv.requests()[0].has("range"));
}

TEST(download_gaps_body_sizes_without_length) {
    const std::string data = bytes(6000, 15);
    headless::TempDir dir("dl-gaps-nolength");
    const std::string path = dir.file("n.bin");
    std::atomic<size_t> sent{data.size()};
    fakehttp::Server srv([&](const fakehttp::Request&) {
        fakehttp::Reply rep;
        rep.noLength = true;   // the body ends with the connection
        rep.body = bytes(sent.load(), 15);
        return rep;
    });
    REQUIRE(srv.ok());
    // Over the cap (no expected size): stopped there, nothing kept.
    net::DownloadRequest rq = request(srv.url("/f"), path, data);
    rq.expectedSize = 0;
    rq.sha256.clear();
    rq.maxBytes = 2500;
    net::DownloadResult r = net::download(rq);
    CHECK_EQ(r.error, std::string("too_large"));
    CHECK(!net::sys::fileExists(path + ".part"));
    // More than the expected size, within a larger cap: "size" once it is all there.
    sent = data.size() + 10;
    rq = request(srv.url("/f"), path, data);
    rq.maxBytes = 2 * data.size();
    r = net::download(rq);
    CHECK_EQ(r.error, std::string("size"));
    CHECK_EQ(r.bytes, uint64_t(data.size() + 10));
    CHECK(!net::sys::fileExists(path));
    // Less: the body ended early ("truncated" once no retry is left), the .part kept for later.
    sent = 2000;
    rq = request(srv.url("/f"), path, data);
    rq.retries = 0;
    r = net::download(rq);
    CHECK_EQ(r.error, std::string("truncated"));
    CHECK(r.detail.find("2000 of 6000") != std::string::npos);
    uint64_t partSize = 0;
    CHECK(net::sys::fileSize(path + ".part", partSize) && partSize == 2000);
}

TEST(download_gaps_destination_cannot_be_replaced) {
    // The destination is a folder (with a file in it): the .part is complete and right, but cannot
    // take its place.
    const std::string data = bytes(2000, 16);
    fakehttp::Server srv([&](const fakehttp::Request& r) { return fakehttp::fileReply(data, r); });
    REQUIRE(srv.ok());
    headless::TempDir dir("dl-gaps-rename");
    const std::string path = dir.file("taken");
    CHECK(net::sys::makeDirectories(path + "/"));
    CHECK(writeFile(path + "/inside.txt", "kept"));
    net::DownloadResult r = net::download(request(srv.url("/f"), path, data));
    CHECK_EQ(r.error, std::string("io"));
    CHECK(r.detail.find("cannot rename") != std::string::npos);
    CHECK_EQ(slurp(path + "/inside.txt"), std::string("kept"));
    // The .part stays; once the place is free, the next download only checks and renames it.
    uint64_t partSize = 0;
    CHECK(net::sys::fileSize(path + ".part", partSize) && partSize == data.size());
    CHECK(net::sys::removeFile(path + "/inside.txt"));
    std::error_code ec;
    CHECK(std::filesystem::remove(path, ec));
    const size_t before = srv.requests().size();
    r = net::download(request(srv.url("/f"), path, data));
    CHECK(r.ok);
    CHECK(slurp(path) == data);
    CHECK_EQ(srv.requests().size(), before);
}

TEST(download_gaps_cancelled_before_and_while_waiting) {
    const std::string data = bytes(100000, 17);
    std::atomic<int> answered{0};
    fakehttp::Server srv([&](const fakehttp::Request& r) {
        fakehttp::Reply rep = fakehttp::fileReply(data, r);
        if (answered++ == 0) rep.cutAfter = 30000;
        return rep;
    });
    REQUIRE(srv.ok());
    headless::TempDir dir("dl-gaps-cancel");
    const std::string path = dir.file("c.bin");
    // Cancelled before it starts: no request.
    net::CancelToken early;
    early.cancel();
    net::DownloadResult r = net::download(request(srv.url("/f"), path, data), &early);
    CHECK_EQ(r.error, std::string("cancelled"));
    CHECK_EQ(srv.requests().size(), size_t(0));
    // Cut after 30000 bytes, then cancelled while it waits to continue (10 s): it stops there,
    // the .part kept.
    net::CancelToken cancel;
    net::DownloadRequest rq = request(srv.url("/f"), path, data);
    rq.retryDelayMs = 10000;
    std::atomic<bool> cut{false};
    rq.onProgress = [&](uint64_t done, uint64_t) {
        if (done >= 30000) cut = true;
    };
    std::thread canceller([&] {
        while (!cut.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the transfer has broken off by then
        cancel.cancel();
    });
    r = net::download(rq, &cancel);
    canceller.join();
    CHECK_EQ(r.error, std::string("cancelled"));
    CHECK_EQ(srv.requests().size(), size_t(1));
    uint64_t partSize = 0;
    CHECK(net::sys::fileSize(path + ".part", partSize) && partSize == 30000);
}
