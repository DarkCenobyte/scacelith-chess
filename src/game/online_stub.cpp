// net::OnlineClient and net::DirectMatch for builds without the real network layer: both are
// backed by the in-process fakes of online_mock.h, so every online page and a whole online game
// work offline (any password is accepted, a random-mover opponent is found after 2 s).
//
// The real implementation (src/net/, compiled with SCACELITH_NET_REAL defined) replaces this
// file; the fakes then stay reachable with --online-mock.
#ifndef SCACELITH_NET_REAL

#include "online_mock.h"
#include <cstdlib>
#include <cctype>

namespace net {

std::string ServerEndpoint::origin() const {
    std::string h = host;
    for (char& c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
    return h + ":" + std::to_string(apiPort);
}

bool ServerEndpoint::valid() const { return !host.empty() && apiPort != 0; }

ServerEndpoint officialServer() {
    ServerEndpoint ep;
#ifdef SCACELITH_OFFICIAL_SERVER
    std::string spec = SCACELITH_OFFICIAL_SERVER;
    size_t a = spec.find(':');
    ep.host = spec.substr(0, a);
    if (a != std::string::npos) {
        size_t b = spec.find(':', a + 1);
        ep.apiPort = uint16_t(std::atoi(spec.substr(a + 1, b - a - 1).c_str()));
        ep.wsPort = b != std::string::npos ? uint16_t(std::atoi(spec.substr(b + 1).c_str())) : ep.apiPort;
    }
#endif
    return ep;
}

uint32_t positionDigest(const std::string& fen) { return mock::digest(fen); }

// ---- OnlineClient -------------------------------------------------------------------------------

struct OnlineClient::Impl {
    mock::FakeServer fake;
};

OnlineClient::OnlineClient() : impl_(new Impl()) {}
OnlineClient::~OnlineClient() = default;

void OnlineClient::setServer(const ServerEndpoint& ep) { impl_->fake.setServer(ep); }
const ServerEndpoint& OnlineClient::server() const { return impl_->fake.server(); }
void OnlineClient::fetchServerInfo() { impl_->fake.fetchServerInfo(); }
bool OnlineClient::hasSavedSession() const { return impl_->fake.hasSavedSession(); }
std::string OnlineClient::savedUsername() const { return impl_->fake.savedUsername(); }
void OnlineClient::registerAccount(const std::string& u, const std::string& e, const std::string& p) { impl_->fake.registerAccount(u, e, p); }
void OnlineClient::login(const std::string& u, const std::string& p) { impl_->fake.login(u, p); }
void OnlineClient::loginMfa(const std::string& code) { impl_->fake.loginMfa(code); }
void OnlineClient::startGoogleSso() { impl_->fake.startGoogleSso(); }
void OnlineClient::completeSso(const std::string& username) { impl_->fake.completeSso(username); }
void OnlineClient::cancelSso() { impl_->fake.cancelSso(); }
void OnlineClient::logout(bool all) { impl_->fake.logout(all); }
void OnlineClient::fetchAccount() { impl_->fake.fetchAccount(); }
void OnlineClient::resendVerification(const std::string& email) { impl_->fake.resendVerification(email); }
void OnlineClient::forgotPassword(const std::string& email) { impl_->fake.forgotPassword(email); }
void OnlineClient::changePassword(const std::string& c, const std::string& n) { impl_->fake.changePassword(c, n); }
void OnlineClient::mfaSetup(const std::string& password) { impl_->fake.mfaSetup(password); }
void OnlineClient::mfaEnable(const std::string& code) { impl_->fake.mfaEnable(code); }
void OnlineClient::mfaDisable(const std::string& p, const std::string& c) { impl_->fake.mfaDisable(p, c); }
void OnlineClient::regenerateRecoveryCodes(const std::string& p, const std::string& c) { impl_->fake.regenerateRecoveryCodes(p, c); }
void OnlineClient::report(uint64_t id, const std::string& u, const std::string& cat, const std::string& comment) {
    impl_->fake.report(id, u, cat, comment);
}
void OnlineClient::connect() { impl_->fake.connect(); }
void OnlineClient::disconnect() { impl_->fake.disconnect(); }
ConnState OnlineClient::state() const { return impl_->fake.state(); }
int OnlineClient::pingMs() const { return impl_->fake.pingMs(); }
double OnlineClient::serverNowMs() const { return impl_->fake.serverNowMs(); }
void OnlineClient::joinQueue(const std::string& category, bool rated) { impl_->fake.joinQueue(category, rated); }
void OnlineClient::leaveQueue() { impl_->fake.leaveQueue(); }
void OnlineClient::challenge(const std::string& u, int b, int i, bool r, int c) { impl_->fake.challenge(u, b, i, r, c); }
void OnlineClient::createPrivateGame(int b, int i, bool r, int c) { impl_->fake.createPrivateGame(b, i, r, c); }
void OnlineClient::joinPrivateGame(const std::string& code) { impl_->fake.joinPrivateGame(code); }
void OnlineClient::acceptChallenge(uint32_t id) { impl_->fake.acceptChallenge(id); }
void OnlineClient::declineChallenge(uint32_t id) { impl_->fake.declineChallenge(id); }
void OnlineClient::cancelChallenge(uint32_t id) { impl_->fake.cancelChallenge(id); }
void OnlineClient::sendMove(uint64_t id, int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) {
    impl_->fake.sendMove(id, ply, move, fen, thinkMs, drawOffer);
}
void OnlineClient::resign(uint64_t id) { impl_->fake.resign(id); }
void OnlineClient::offerDraw(uint64_t id) { impl_->fake.offerDraw(id); }
void OnlineClient::answerDraw(uint64_t id, bool accept) { impl_->fake.answerDraw(id, accept); }
void OnlineClient::claimDraw(uint64_t id) { impl_->fake.claimDraw(id); }
void OnlineClient::abortGame(uint64_t id) { impl_->fake.abortGame(id); }
void OnlineClient::requestResync(uint64_t id) { impl_->fake.requestResync(id); }
void OnlineClient::rematch(uint64_t id, bool accept) { impl_->fake.rematch(id, accept); }
const OnlineGame* OnlineClient::currentGame() const { return impl_->fake.currentGame(); }
bool OnlineClient::poll(Event& out) { return impl_->fake.poll(out); }

OnlineClient& onlineClient() {
    static OnlineClient client;
    return client;
}

// ---- DirectMatch --------------------------------------------------------------------------------

struct DirectMatch::Impl {
    mock::FakeDirect fake;
};

DirectMatch::DirectMatch() : impl_(new Impl()) {}
DirectMatch::~DirectMatch() = default;

void DirectMatch::host(const DirectHostOptions& opt) { impl_->fake.host(opt); }
void DirectMatch::join(const std::string& a, uint16_t port, const std::string& code, const std::string& name) {
    impl_->fake.join(a, port, code, name);
}
void DirectMatch::close() { impl_->fake.close(); }
DirectMatch::State DirectMatch::state() const { return impl_->fake.state(); }
std::string DirectMatch::lastError() const { return impl_->fake.lastError(); }
DirectInvite DirectMatch::invite() const { return impl_->fake.invite(); }
UpnpStatus DirectMatch::upnp() const { return impl_->fake.upnp(); }
bool DirectMatch::isHost() const { return impl_->fake.isHost(); }
void DirectMatch::sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) {
    impl_->fake.sendMove(ply, move, fen, thinkMs, drawOffer);
}
void DirectMatch::resign() { impl_->fake.resign(); }
void DirectMatch::offerDraw() { impl_->fake.offerDraw(); }
void DirectMatch::answerDraw(bool accept) { impl_->fake.answerDraw(accept); }
void DirectMatch::claimDraw() { impl_->fake.claimDraw(); }
void DirectMatch::abortGame() { impl_->fake.abortGame(); }
void DirectMatch::requestResync() { impl_->fake.requestResync(); }
void DirectMatch::rematch(bool accept) { impl_->fake.rematch(accept); }
const OnlineGame* DirectMatch::currentGame() const { return impl_->fake.currentGame(); }
int DirectMatch::pingMs() const { return impl_->fake.pingMs(); }
double DirectMatch::serverNowMs() const { return impl_->fake.serverNowMs(); }
bool DirectMatch::poll(Event& out) { return impl_->fake.poll(out); }

DirectMatch& directMatch() {
    static DirectMatch match;
    return match;
}

}  // namespace net

#endif  // SCACELITH_NET_REAL
