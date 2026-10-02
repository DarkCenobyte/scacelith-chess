#include "online_session.h"
#include "../core/log.h"
#include "../i18n/i18n.h"
#include "../net/protocol_gen.h"
#include "../platform/platform.h"
#include "../ui/ui.h"
#include "online_mock.h"
#include "settings.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace game {

namespace {

using Kind = net::Event::Kind;

// ---- Backends ---------------------------------------------------------------------------------
// The same commands on net::OnlineClient (the process-wide client) or on a fake server it owns.
template <class C>
class ServerApiOf final : public ServerApi {
public:
    explicit ServerApiOf(C& c) : c_(&c) {}
    explicit ServerApiOf(std::unique_ptr<C> own) : own_(std::move(own)), c_(own_.get()) {}
    void setServer(const net::ServerEndpoint& ep) override { c_->setServer(ep); }
    void fetchServerInfo() override { c_->fetchServerInfo(); }
    bool hasSavedSession() const override { return c_->hasSavedSession(); }
    std::string savedUsername() const override { return c_->savedUsername(); }
    void registerAccount(const std::string& u, const std::string& e, const std::string& p) override { c_->registerAccount(u, e, p); }
    void login(const std::string& u, const std::string& p) override { c_->login(u, p); }
    void loginMfa(const std::string& code) override { c_->loginMfa(code); }
    void startGoogleSso() override { c_->startGoogleSso(); }
    void completeSso(const std::string& u) override { c_->completeSso(u); }
    void cancelSso() override { c_->cancelSso(); }
    void logout(bool all) override { c_->logout(all); }
    void fetchAccount() override { c_->fetchAccount(); }
    void resendVerification(const std::string& e) override { c_->resendVerification(e); }
    void forgotPassword(const std::string& e) override { c_->forgotPassword(e); }
    void changePassword(const std::string& a, const std::string& b) override { c_->changePassword(a, b); }
    void mfaSetup(const std::string& p) override { c_->mfaSetup(p); }
    void mfaEnable(const std::string& code) override { c_->mfaEnable(code); }
    void mfaDisable(const std::string& p, const std::string& c) override { c_->mfaDisable(p, c); }
    void regenerateRecoveryCodes(const std::string& p, const std::string& c) override { c_->regenerateRecoveryCodes(p, c); }
    void report(uint64_t id, const std::string& u, const std::string& cat, const std::string& comment) override {
        c_->report(id, u, cat, comment);
    }
    void fetchMyGames(uint64_t before, int limit, const net::GamesFilter& f) override { c_->fetchMyGames(before, limit, f); }
    void fetchGame(uint64_t id) override { c_->fetchGame(id); }
    void downloadPgn(uint64_t id) override { c_->downloadPgn(id); }
    void fetchSessions() override { c_->fetchSessions(); }
    void revokeSession(int64_t id) override { c_->revokeSession(id); }
    void setAcceptChallenges(bool accept) override { c_->setAcceptChallenges(accept); }
    void changeEmail(const std::string& e, const std::string& p, const std::string& c) override { c_->changeEmail(e, p, c); }
    void exportAccount(const std::string& p, const std::string& c) override { c_->exportAccount(p, c); }
    void deleteAccount(const std::string& p, const std::string& c) override { c_->deleteAccount(p, c); }
    void downloadGameGif(uint64_t id, const net::GifOptions& o) override { c_->downloadGameGif(id, o); }
    void renderPgnGif(const std::string& pgn, const net::GifOptions& o) override { c_->renderPgnGif(pgn, o); }
    void connect() override { c_->connect(); }
    void disconnect() override { c_->disconnect(); }
    net::ConnState state() const override { return c_->state(); }
    int pingMs() const override { return c_->pingMs(); }
    double serverNowMs() const override { return c_->serverNowMs(); }
    void joinQueue(const std::string& cat, bool rated) override { c_->joinQueue(cat, rated); }
    void leaveQueue() override { c_->leaveQueue(); }
    void challenge(const std::string& u, int b, int i, bool r, int col) override { c_->challenge(u, b, i, r, col); }
    void createPrivateGame(int b, int i, bool r, int col) override { c_->createPrivateGame(b, i, r, col); }
    void joinPrivateGame(const std::string& code) override { c_->joinPrivateGame(code); }
    void acceptChallenge(uint32_t id) override { c_->acceptChallenge(id); }
    void declineChallenge(uint32_t id) override { c_->declineChallenge(id); }
    void cancelChallenge(uint32_t id) override { c_->cancelChallenge(id); }
    void sendMove(uint64_t id, int ply, uint16_t mv, const std::string& fen, uint32_t think, bool offer) override {
        c_->sendMove(id, ply, mv, fen, think, offer);
    }
    void resign(uint64_t id) override { c_->resign(id); }
    void offerDraw(uint64_t id) override { c_->offerDraw(id); }
    void answerDraw(uint64_t id, bool accept) override { c_->answerDraw(id, accept); }
    void claimDraw(uint64_t id) override { c_->claimDraw(id); }
    void abortGame(uint64_t id) override { c_->abortGame(id); }
    void requestResync(uint64_t id) override { c_->requestResync(id); }
    void rematch(uint64_t id, bool accept) override { c_->rematch(id, accept); }
    void sendGesture(uint64_t id, const net::Gesture& g) override { c_->sendGesture(id, g); }
    bool poll(net::Event& out) override { return c_->poll(out); }

private:
    std::unique_ptr<C> own_;
    C* c_;
};

template <class D>
class DirectApiOf final : public DirectApi {
public:
    explicit DirectApiOf(D& d) : d_(&d) {}
    explicit DirectApiOf(std::unique_ptr<D> own) : own_(std::move(own)), d_(own_.get()) {}
    void host(const net::DirectHostOptions& opt) override { d_->host(opt); }
    void join(const std::string& a, uint16_t port, const std::string& code, const std::string& name) override {
        d_->join(a, port, code, name);
    }
    void close() override { d_->close(); }
    net::DirectMatch::State state() const override { return d_->state(); }
    std::string lastError() const override { return d_->lastError(); }
    net::DirectInvite invite() const override { return d_->invite(); }
    net::UpnpStatus upnp() const override { return d_->upnp(); }
    bool isHost() const override { return d_->isHost(); }
    void sendMove(int ply, uint16_t mv, const std::string& fen, uint32_t think, bool offer) override {
        d_->sendMove(ply, mv, fen, think, offer);
    }
    void resign() override { d_->resign(); }
    void offerDraw() override { d_->offerDraw(); }
    void answerDraw(bool accept) override { d_->answerDraw(accept); }
    void claimDraw() override { d_->claimDraw(); }
    void abortGame() override { d_->abortGame(); }
    void requestResync() override { d_->requestResync(); }
    void rematch(bool accept) override { d_->rematch(accept); }
    void sendGesture(const net::Gesture& g) override { d_->sendGesture(g); }
    int pingMs() const override { return d_->pingMs(); }
    double serverNowMs() const override { return d_->serverNowMs(); }
    bool poll(net::Event& out) override { return d_->poll(out); }

private:
    std::unique_ptr<D> own_;
    D* d_;
};

// ---- Game links -------------------------------------------------------------------------------
class ServerLink final : public GameLink {
public:
    ServerLink(ServerApi& api, uint64_t id, std::string name) : api_(api), id_(id), name_(std::move(name)) {}
    LinkKind kind() const override { return LinkKind::Server; }
    uint64_t gameId() const override { return id_; }
    void sendMove(int ply, uint16_t mv, const std::string& fen, uint32_t think, bool offer) override {
        api_.sendMove(id_, ply, mv, fen, think, offer);
    }
    void resign() override { api_.resign(id_); }
    void offerDraw() override { api_.offerDraw(id_); }
    void answerDraw(bool accept) override { api_.answerDraw(id_, accept); }
    void claimDraw() override { api_.claimDraw(id_); }
    void abortGame() override { api_.abortGame(id_); }
    void requestResync() override { api_.requestResync(id_); }
    void rematch(bool accept) override { api_.rematch(id_, accept); }
    void sendGesture(const net::Gesture& g) override { api_.sendGesture(id_, g); }
    bool canReport() const override { return true; }
    void report(const std::string& u, const std::string& cat, const std::string& comment) override {
        api_.report(id_, u, cat, comment);
    }
    int pingMs() const override { return api_.pingMs(); }
    double serverNowMs() const override { return api_.serverNowMs(); }
    bool reconnecting() const override { return api_.state() != net::ConnState::Online; }
    std::string eventName() const override { return name_; }

private:
    ServerApi& api_;
    uint64_t id_;
    std::string name_;
};

class DirectLink final : public GameLink {
public:
    // conn: the direct match's connection as OnlineSession last saw it (Reconnecting while a
    // guest restores its link: the match stays Playing meanwhile).
    DirectLink(DirectApi& d, uint64_t id, const net::ConnState& conn) : d_(d), id_(id), conn_(conn) {}
    LinkKind kind() const override { return LinkKind::Direct; }
    uint64_t gameId() const override { return id_; }
    void sendMove(int ply, uint16_t mv, const std::string& fen, uint32_t think, bool offer) override {
        d_.sendMove(ply, mv, fen, think, offer);
    }
    void resign() override { d_.resign(); }
    void offerDraw() override { d_.offerDraw(); }
    void answerDraw(bool accept) override { d_.answerDraw(accept); }
    void claimDraw() override { d_.claimDraw(); }
    void abortGame() override { d_.abortGame(); }
    void requestResync() override { d_.requestResync(); }
    void rematch(bool accept) override { d_.rematch(accept); }
    void sendGesture(const net::Gesture& g) override { d_.sendGesture(g); }
    bool canReport() const override { return false; }
    void report(const std::string&, const std::string&, const std::string&) override {}
    int pingMs() const override { return d_.pingMs(); }
    double serverNowMs() const override { return d_.serverNowMs(); }
    bool reconnecting() const override {
        return d_.state() != net::DirectMatch::State::Playing || conn_ == net::ConnState::Reconnecting;
    }
    std::string eventName() const override { return i18n::tr("direct.event"); }

private:
    DirectApi& d_;
    uint64_t id_;
    const net::ConnState& conn_;
};

bool isGameEvent(Kind k) {
    return k == Kind::GameSnapshot || k == Kind::MoveMade || k == Kind::MoveRejected || k == Kind::GameEvent ||
           k == Kind::GameEnd || k == Kind::RatingUpdate || k == Kind::OpponentGesture;
}

// Protocol values (dedicated-server/src/protocol/schema.js).
enum ChallengeState { ChPending = 0, ChAccepted = 1, ChDeclined = 2, ChCancelled = 3, ChExpired = 4, ChUnavailable = 5 };
enum QueueState { QLeft = 0, QSearching = 1, QMatched = 2 };
enum NoticeCode { NShutdown = 1, NBanned = 2, NRevoked = 3, NCooldown = 4, NReplaced = 5, NRatingRestored = 7 };
constexpr int kErrAlreadyInGame = 106, kErrInvalidCategory = 107, kErrMatchmakingCooldown = 207;
// The names above are the generated ones (net/protocol_gen.h): a schema change fails here.
static_assert(ChPending == int(net::proto::ChallengeState::Pending) && ChAccepted == int(net::proto::ChallengeState::Accepted) &&
                  ChDeclined == int(net::proto::ChallengeState::Declined) &&
                  ChCancelled == int(net::proto::ChallengeState::Cancelled) &&
                  ChExpired == int(net::proto::ChallengeState::Expired) &&
                  ChUnavailable == int(net::proto::ChallengeState::Unavailable),
              "ChallengeState");
static_assert(QLeft == int(net::proto::QueueState::Left) && QSearching == int(net::proto::QueueState::Searching) &&
                  QMatched == int(net::proto::QueueState::Matched),
              "QueueState");
static_assert(NShutdown == int(net::proto::NoticeCode::ServerShutdown) && NBanned == int(net::proto::NoticeCode::Banned) &&
                  NRevoked == int(net::proto::NoticeCode::SessionRevoked) &&
                  NCooldown == int(net::proto::NoticeCode::MatchmakingCooldown) &&
                  NReplaced == int(net::proto::NoticeCode::ReplacedByNewConnection) &&
                  NRatingRestored == int(net::proto::NoticeCode::RatingRestored),
              "NoticeCode");
static_assert(kErrAlreadyInGame == int(net::proto::ErrorCode::AlreadyInGame) &&
                  kErrInvalidCategory == int(net::proto::ErrorCode::InvalidCategory) &&
                  kErrMatchmakingCooldown == int(net::proto::ErrorCode::MatchmakingCooldown),
              "ErrorCode");

// Seconds of the monotonic clock: when a page last showed the state of the GIF being made.
double gifClock() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

// =============================================================================================
// Session
// =============================================================================================

OnlineSession& onlineSession() {
    static OnlineSession s;
    return s;
}

OnlineSession::OnlineSession() = default;
OnlineSession::~OnlineSession() = default;

void OnlineSession::init(bool mock, bool virtualClock) {
    if (ready_) return;
    ready_ = true;
    mock_ = mock;
    virtual_ = virtualClock;
    net::mock::useVirtualClock(virtualClock);
    if (mock) {
        const std::vector<std::string> args = plat::commandLine();
        net::mock::useManualClock(std::find(args.begin(), args.end(), "--online-manual-clock") != args.end());
        api_.reset(new ServerApiOf<net::mock::FakeServer>(std::unique_ptr<net::mock::FakeServer>(new net::mock::FakeServer())));
        direct_.reset(new DirectApiOf<net::mock::FakeDirect>(std::unique_ptr<net::mock::FakeDirect>(new net::mock::FakeDirect())));
        LOGI("online: in-process mock server (--online-mock)");
    } else {
        api_.reset(new ServerApiOf<net::OnlineClient>(net::onlineClient()));
        direct_.reset(new DirectApiOf<net::DirectMatch>(net::directMatch()));
    }
    applyServer();
}

ServerApi& OnlineSession::api() {
    if (!ready_) init(false, false);
    return *api_;
}

DirectApi& OnlineSession::direct() {
    if (!ready_) init(false, false);
    directUsed_ = true;
    return *direct_;
}

double OnlineSession::nowMs() const { return api_ ? api_->serverNowMs() : net::mock::nowMs(); }

// ---- Server ---------------------------------------------------------------------------------------

bool OnlineSession::officialAvailable() const { return net::officialServer().valid(); }

net::ServerEndpoint OnlineSession::endpoint() const {
    const Settings& s = settings();
    if (!s.onlineCustomServer && officialAvailable()) return net::officialServer();
    net::ServerEndpoint ep;
    ep.host = s.onlineHost;
    ep.apiPort = uint16_t(std::clamp(s.onlineApiPort, 1, 65535));
    ep.wsPort = s.onlineWsPort > 0 ? uint16_t(std::min(s.onlineWsPort, 65535)) : ep.apiPort;  // empty = the API port
    ep.pinnedSha256 = s.onlinePin;
    return ep;
}

void OnlineSession::applyServer() {
    if (!ready_) return;
    net::ServerEndpoint ep = endpoint();
    if (!ep.valid()) return;
    if (queue_.searching) api_->leaveQueue();
    api_->setServer(ep);
    info_ = net::ServerInfo();
    infoKnown_ = false;
    infoError_.clear();
    signedIn_ = false;
    account_ = net::AccountInfo();
    data_.clear();
    conn_ = api_->state();
    queue_ = Queue();
    outgoing_ = Outgoing();
    incoming_.clear();
    answers_.setServer(ep.origin());
    ratingRestored_ = live::HeldNotice();  // points of the previous server's account
    LOGI("online: server %s", ep.origin().c_str());
}

void OnlineSession::refreshInfo() {
    if (!serverConfigured()) return;
    api().fetchServerInfo();
    expect(Kind::ServerInfoResult);
}

void OnlineSession::testServer(const net::ServerEndpoint& ep) {
    if (!ep.valid() || testing_) return;
    ServerApi& a = api();
    net::ServerEndpoint cur = endpoint();
    testSwitched_ = !(ep.origin() == cur.origin() && ep.wsPort == cur.wsPort && ep.pinnedSha256 == cur.pinnedSha256);
    if (testSwitched_) a.setServer(ep);
    a.fetchServerInfo();
    testing_ = true;
    testDone_ = false;
}

bool OnlineSession::takeTest(net::Event& out) {
    if (!testDone_) return false;
    out = testResult_;
    testDone_ = false;
    return true;
}

std::string OnlineSession::serverName() const {
    if (infoKnown_ && !info_.name.empty()) return info_.name;
    if (!serverNameRt_.empty()) return serverNameRt_;
    return endpoint().host;
}

// ---- Account --------------------------------------------------------------------------------------

bool OnlineSession::hasSavedSession() const { return ready_ && api_ && serverConfigured() && api_->hasSavedSession(); }

std::string OnlineSession::savedUsername() const { return hasSavedSession() ? api_->savedUsername() : std::string(); }

void OnlineSession::resume() {
    if (!serverConfigured()) return;
    ServerApi& a = api();
    if (!a.hasSavedSession()) return;
    signedIn_ = true;
    if (account_.username.empty()) account_.username = a.savedUsername();
    a.connect();
    a.fetchAccount();
    expect(Kind::AccountResult);
}

const net::RatingInfo* OnlineSession::rating(const std::string& category) const {
    for (const net::RatingInfo& r : account_.ratings)
        if (r.category == category) return &r;
    return nullptr;
}

void OnlineSession::signOut(bool everywhere) {
    cancelSearch();
    cancelOutgoing();
    api().logout(everywhere);
    expect(Kind::LogoutResult);
    signedIn_ = false;
    account_ = net::AccountInfo();
    data_.clear();
    incoming_.clear();
}

// ---- Requests -------------------------------------------------------------------------------------

void OnlineSession::expect(Kind k) { answers_.expect(k); }

bool OnlineSession::busy(Kind k) const { return answers_.busy(k); }

bool OnlineSession::take(Kind k, net::Event& out) { return answers_.take(k, out); }

// ---- Account API ----------------------------------------------------------------------------------

void OnlineSession::loadHistory(const net::GamesFilter& filter) {
    uint64_t before = data_.history.restart(filter);
    api().fetchMyGames(before, HistoryPager::kPageSize, filter);
    expect(Kind::GamesResult);
}

void OnlineSession::historyNext() {
    uint64_t before = 0;
    if (!data_.history.next(before)) return;
    api().fetchMyGames(before, HistoryPager::kPageSize, data_.history.filter());
    expect(Kind::GamesResult);
}

void OnlineSession::historyPrevious() {
    uint64_t before = 0;
    if (!data_.history.previous(before)) return;
    api().fetchMyGames(before, HistoryPager::kPageSize, data_.history.filter());
    expect(Kind::GamesResult);
}

void OnlineSession::historyReload() {
    uint64_t before = data_.history.reload();
    api().fetchMyGames(before, HistoryPager::kPageSize, data_.history.filter());
    expect(Kind::GamesResult);
}

void OnlineSession::openGame(uint64_t gameId) {
    if (data_.gameWanted != gameId) {
        data_.gameLoaded = false;
        data_.game = net::GameDetails();
    }
    data_.gameWanted = gameId;
    data_.gameError.clear();
    data_.gameRetryAfter = 0;
    api().fetchGame(gameId);
    expect(Kind::GameDetailsResult);
}

void OnlineSession::loadSessions() {
    data_.sessionsError.clear();
    data_.sessionsRetryAfter = 0;
    api().fetchSessions();
    expect(Kind::SessionsResult);
}

void OnlineSession::revokeSession(int64_t sessionId) {
    api().revokeSession(sessionId);
    expect(Kind::SessionRevoked);
}

void OnlineSession::setAcceptChallenges(bool accept) {
    api().setAcceptChallenges(accept);
    expect(Kind::PreferencesResult);
}

// ---- Animated GIFs ----------------------------------------------------------------------------------

bool OnlineSession::saveGameGif(const std::string& owner, uint64_t gameId, const net::GifOptions& options, const std::string& folder,
                                const std::string& fileName) {
    if (!gif_.begin(owner, gameId, folder, fileName)) return false;
    api().downloadGameGif(gameId, options);
    gifShownAt_ = gifClock();
    return true;
}

bool OnlineSession::savePgnGif(const std::string& owner, const std::string& pgn, const net::GifOptions& options, const std::string& folder,
                               const std::string& fileName) {
    if (!gif_.begin(owner, 0, folder, fileName)) return false;
    api().renderPgnGif(pgn, options);
    gifShownAt_ = gifClock();
    return true;
}

void OnlineSession::gifShown(const std::string& owner) {
    if (owner == gif_.owner()) gifShownAt_ = gifClock();
}

void OnlineSession::clearGif() {
    if (!gif_.busy()) gif_.clear();
}

// ---- Realtime -------------------------------------------------------------------------------------

int OnlineSession::pingMs() const {
    if (link_ && gameKind_ == LinkKind::Direct) return link_->pingMs();
    return api_ && conn_ == net::ConnState::Online ? api_->pingMs() : -1;
}

void OnlineSession::findOpponent(const std::string& category, bool rated) {
    api().joinQueue(category, rated);
    queue_.searching = true;
    queue_.category = category;
    queue_.rated = rated;
    queue_.sinceMs = nowMs();
    queue_.window = 0;
    queue_.queued = 0;
}

void OnlineSession::cancelSearch() {
    if (!queue_.searching) return;
    api().leaveQueue();
    queue_.searching = false;
}

void OnlineSession::challenge(const std::string& username, int baseSec, int incSec, bool rated, int colorPref) {
    api().challenge(username, baseSec, incSec, rated, colorPref);
    outgoing_ = Outgoing();
    outgoing_.active = true;
    outgoing_.target = username;
    outgoing_.baseSec = baseSec;
    outgoing_.incSec = incSec;
    outgoing_.rated = rated;
}

void OnlineSession::createPrivateGame(int baseSec, int incSec, bool rated, int colorPref) {
    api().createPrivateGame(baseSec, incSec, rated, colorPref);
    outgoing_ = Outgoing();
    outgoing_.active = true;
    outgoing_.baseSec = baseSec;
    outgoing_.incSec = incSec;
    outgoing_.rated = rated;
}

void OnlineSession::joinPrivateGame(const std::string& code) { api().joinPrivateGame(code); }

void OnlineSession::cancelOutgoing() {
    if (!outgoing_.active) return;
    if (outgoing_.id) api().cancelChallenge(outgoing_.id);
    outgoing_ = Outgoing();
}

void OnlineSession::answerChallenge(uint32_t id, bool accept) {
    if (accept) {
        cancelSearch();
        api().acceptChallenge(id);
    } else {
        api().declineChallenge(id);
    }
    incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(), [id](const Incoming& c) { return c.id == id; }),
                    incoming_.end());
}

// ---- Direct match ---------------------------------------------------------------------------------

void OnlineSession::hostDirect(const net::DirectHostOptions& opt) {
    cancelSearch();
    direct().host(opt);
    directConn_ = net::ConnState::Offline;
}

void OnlineSession::joinDirect(const std::string& address, uint16_t port, const std::string& code) {
    cancelSearch();
    const std::string& n = settings().playerName;
    std::string name = n.empty() || n == "Human" ? std::string(i18n::tr("player.default_name")) : n;
    direct().join(address, port, code, name);
    directConn_ = net::ConnState::Offline;
}

void OnlineSession::closeDirect() {
    if (!directUsed_) return;
    direct_->close();
    directConn_ = net::ConnState::Offline;
    if (gameKind_ == LinkKind::Direct) {
        link_.reset();
        gameReady_ = false;
        gameEvents_.clear();
        gameId_ = 0;
    }
}

// ---- Games ----------------------------------------------------------------------------------------

std::unique_ptr<GameLink> OnlineSession::makeLink(LinkKind kind, uint64_t id) {
    if (kind == LinkKind::Direct) return std::unique_ptr<GameLink>(new DirectLink(*direct_, id, directConn_));
    return std::unique_ptr<GameLink>(new ServerLink(*api_, id, serverName()));
}

GameLink* OnlineSession::takeGame(net::OnlineGame& snapshot) {
    if (!gameReady_) return nullptr;
    gameReady_ = false;
    snapshot = snapshot_;
    // Events up to the snapshot are part of it.
    while (!gameEvents_.empty() && gameEvents_.front().kind == Kind::GameSnapshot && gameEvents_.front().game.id == gameId_) {
        snapshot = gameEvents_.front().game;
        gameEvents_.pop_front();
    }
    link_ = makeLink(gameKind_, gameId_);
    return link_.get();
}

bool OnlineSession::nextGameEvent(net::Event& e) {
    if (gameReady_ || gameEvents_.empty()) return false;  // a new game waits for takeGame()
    e = std::move(gameEvents_.front());
    gameEvents_.pop_front();
    return true;
}

void OnlineSession::leaveGame() {
    if (gameKind_ == LinkKind::Direct) closeDirect();
    link_.reset();
    gameEvents_.clear();
    gameReady_ = false;
    gameId_ = 0;
}

void OnlineSession::quickStart(const std::string& category, const std::string& username) {
    if (!ready_) init(false, false);
    autoQueue_ = category;
    if (!signedIn_) {
        if (api_->hasSavedSession()) {
            resume();
        } else if (mock_ || !net::officialServer().valid()) {
            api_->login(username, "mock-password");
            expect(Kind::LoginResult);
        }
    }
    if (!(mock_ && virtual_)) return;
    // The fakes answer through the virtual clock: run it until the game starts.
    for (int i = 0; i < 2400 && !gameReady_; ++i) update(0.025f);
}

void OnlineSession::runMock(double ms) {
    if (!(mock_ && virtual_)) return;
    for (double t = 0.0; t < ms; t += 25.0) update(0.025f);
    gif_.poll(true);   // a GIF file being written: on the disk before the page is drawn
}

// ---- Event pump -----------------------------------------------------------------------------------

void OnlineSession::update(float dt) {
    if (virtual_ && dt > 0.0f) net::mock::advance(double(dt) * 1000.0);
    if (!ready_) return;
    const GifSaver::Stage gifBefore = gif_.stage();
    net::Event e;
    for (int guard = 0; guard < 256 && api_->poll(e); ++guard) handleServer(e);
    if (directUsed_)
        for (int guard = 0; guard < 256 && direct_->poll(e); ++guard) handleDirect(e);
    // A GIF finished while no page shows it (the player went elsewhere): said by a notification.
    const bool gifEnded = gif_.poll() || (gifBefore == GifSaver::Stage::Rendering && gif_.stage() == GifSaver::Stage::Failed);
    if (gifEnded && gifClock() - gifShownAt_ > 0.5) {
        if (gif_.stage() == GifSaver::Stage::Saved) {
            const std::string& path = gif_.path();
            const size_t cut = path.find_last_of("/\\");
            ui::notify(i18n::trf("gif.saved_as", {i18n::ltr(cut == std::string::npos ? path : path.substr(cut + 1))}), 6.0f);
        } else {
            ui::notify(gifErrorText(gif_.error(), gif_.retryAfterSec()), 6.0f);
        }
    }
    double restored = 0.0;
    if (ratingRestored_.take(inGame_, restored))
        ui::notify(i18n::trn("online.notice.rating_restored", std::lround(restored)), 8.0f);
    // --start-online: queue as soon as the connection is up.
    if (!autoQueue_.empty() && signedIn_ && conn_ == net::ConnState::Online) {
        findOpponent(autoQueue_, true);
        autoQueue_.clear();
    }
}

void OnlineSession::handleServer(net::Event& e) {
    if (isGameEvent(e.kind)) {
        if (e.kind == Kind::RatingUpdate && !e.game.category.empty()) {
            // The account page shows the new rating at once.
            for (net::RatingInfo& r : account_.ratings) {
                if (r.category != e.game.category) continue;
                const net::Event::Rating& mine = e.game.you == 1 ? e.ratingBlack : e.ratingWhite;
                r.rating = mine.after;
                r.games = mine.games;
                r.provisional = mine.provisional;
                r.peak = std::max(r.peak, mine.after);
            }
        }
        routeGame(e, LinkKind::Server);
        return;
    }
    // An answer of the server used before the last applyServer(): not this server's history, game,
    // devices, account or session. Its GIF is still written and its PGN still saved (ServerAnswers
    // keeps it for the game page), each as the game it was asked for. (The info of the server being
    // tested in Options names that server.)
    if (answers_.foreign(e) && !(e.kind == Kind::ServerInfoResult && testing_)) {
        if (e.kind == Kind::GifResult) gif_.finish(std::move(e));
        else if (!answers_.keep(std::move(e))) LOGI("online: an answer of %s dropped (another server since)", e.origin.c_str());
        return;
    }
    // A call that found the saved session refused (expired, revoked): the network layer erased the
    // token, whatever the call (net::Event::sessionLost).
    if (e.sessionLost && signedIn_) {
        signedIn_ = false;
        LOGI("online: session refused, signed out");
    }
    // HTTPS results are kept for the page that asked (moved: store() is the last use of e).
    auto store = [&]() { answers_.keep(std::move(e)); };
    switch (e.kind) {
    case Kind::ServerInfoResult:
        if (testing_) {
            testing_ = false;
            testDone_ = true;
            testResult_ = e;
            if (testSwitched_) {  // back to the server in use
                testSwitched_ = false;
                net::ServerEndpoint cur = endpoint();
                if (cur.valid()) {
                    api_->setServer(cur);
                    if (signedIn_) api_->connect();
                }
                break;
            }
        }
        infoKnown_ = e.ok;
        infoError_ = e.ok ? std::string() : e.error;
        if (e.ok) info_ = e.info;
        store();
        break;
    case Kind::LoginResult:
        if (e.ok) {
            signedIn_ = true;
            account_ = e.account;
            data_.clear();  // the history and devices of whoever was signed in before
            api_->connect();
            LOGI("online: signed in as %s", account_.username.c_str());
        }
        store();
        break;
    case Kind::LogoutResult:
        signedIn_ = false;
        account_ = net::AccountInfo();
        data_.clear();
        store();
        break;
    case Kind::AccountResult:
        if (e.ok) account_ = e.account;
        else if (e.error == "unauthorized") signedIn_ = false;
        store();
        break;
    case Kind::MfaEnableResult:
        if (e.ok) account_.mfaEnabled = true;
        store();
        break;
    case Kind::GamesResult:
    case Kind::GameDetailsResult:
    case Kind::PgnResult:
    case Kind::SessionsResult:
    case Kind::SessionRevoked:
    case Kind::PreferencesResult:
    case Kind::EmailChangeResult:
    case Kind::AccountExportResult:
    case Kind::AccountDeleted: {
        const bool wasSignedIn = signedIn_;
        data_.apply(e, account_, signedIn_);
        if (e.kind == Kind::AccountDeleted && e.ok) {
            // The network layer erased the session and stopped the realtime connection.
            queue_ = Queue();
            outgoing_ = Outgoing();
            incoming_.clear();
            ratingRestored_ = live::HeldNotice();
            LOGI("online: account deleted");
        } else if (e.kind == Kind::EmailChangeResult && e.ok && signedIn_) {
            // The pending change (or the new address) shows on the account page.
            api_->fetchAccount();
            expect(Kind::AccountResult);
        }
        if (wasSignedIn && !signedIn_ && !(e.kind == Kind::AccountDeleted && e.ok)) LOGI("online: session refused, signed out");
        store();
        break;
    }
    case Kind::GifResult: {
        // The GifSaver writes the file (not kept with the other answers: up to 16 MiB, moved to
        // its write thread); a refused token signs out like any account API answer.
        const bool wasSignedIn = signedIn_;
        data_.apply(e, account_, signedIn_);
        gif_.finish(std::move(e));
        if (wasSignedIn && !signedIn_) LOGI("online: session refused, signed out");
        break;
    }
    case Kind::MfaDisableResult:
        if (e.ok) account_.mfaEnabled = false;
        store();
        break;
    case Kind::ConnectionChanged:
        conn_ = e.state;
        if (e.state == net::ConnState::Unauthorized) {
            signedIn_ = false;
            ui::notify(onlineErrorText("unauthorized"), 5.0f);
        } else if (e.state == net::ConnState::Banned) {
            signedIn_ = false;
            ui::notify(onlineErrorText("banned", 0, int64_t(bannedUntilMs_)), 6.0f);
        } else if (e.state == net::ConnState::Incompatible) {
            ui::notify(onlineErrorText("incompatible"), 6.0f);
        }
        if (e.state != net::ConnState::Online) queue_.searching = false;
        break;
    case Kind::Welcome:
        if (!e.account.username.empty()) {
            account_.username = e.account.username;
            account_.userId = e.account.userId;
            if (!e.account.ratings.empty()) account_.ratings = e.account.ratings;
        }
        serverNameRt_ = e.serverName;
        conn_ = net::ConnState::Online;
        break;
    case Kind::QueueStatus:
        if (e.queueState == QSearching) {
            if (!queue_.searching) queue_.sinceMs = nowMs() - double(e.queueWaitMs);
            queue_.searching = true;
            queue_.category = e.queueCategory;
            queue_.rated = e.queueRated;
            queue_.window = e.queueWindow;
            queue_.queued = e.queued;
        } else {
            queue_.searching = false;
        }
        break;
    case Kind::ChallengeReceived: {
        Incoming c;
        c.id = e.challengeId;
        c.from = e.challenger;
        c.baseSec = e.challengeBaseSec;
        c.incSec = e.challengeIncSec;
        c.rated = e.challengeRated;
        c.yourColor = e.challengeColor;
        c.expiresMs = nowMs() + double(e.challengeExpiresMs);
        incoming_.push_back(c);
        LOGI("online: challenge from %s", c.from.name.c_str());
        break;
    }
    case Kind::ChallengeStatus: {
        bool mine = outgoing_.active && (outgoing_.id == 0 || outgoing_.id == e.challengeId) &&
                    !std::any_of(incoming_.begin(), incoming_.end(), [&](const Incoming& c) { return c.id == e.challengeId; });
        if (mine) {
            outgoing_.id = e.challengeId;
            if (!e.challengeCode.empty()) outgoing_.code = e.challengeCode;
            if (e.challengeState == ChDeclined) ui::notify(i18n::trf("online.challenge.declined", {outgoing_.target}), 4.0f);
            if (e.challengeState == ChExpired) ui::notify(i18n::tr("online.challenge.expired"), 4.0f);
            if (e.challengeState == ChUnavailable) ui::notify(i18n::trf("online.challenge.unavailable", {outgoing_.target}), 4.0f);
            if (e.challengeState != ChPending) outgoing_ = Outgoing();
        } else {
            incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(),
                                           [&](const Incoming& c) { return c.id == e.challengeId; }),
                            incoming_.end());
        }
        break;
    }
    case Kind::Notice:
        switch (e.noticeCode) {
        case NShutdown: ui::notify(i18n::trf("online.notice.shutdown", {durationText(e.noticeArg)}), 8.0f); break;
        case NBanned:
            bannedUntilMs_ = e.noticeArg;
            ui::notify(onlineErrorText("banned", 0, int64_t(e.noticeArg)), 8.0f);
            break;
        case NRevoked:
            signedIn_ = false;
            ui::notify(i18n::tr("online.notice.revoked"), 6.0f);
            break;
        case NCooldown: cooldownUntilMs_ = e.noticeArg; break;
        case NReplaced: ui::notify(i18n::tr("online.notice.replaced"), 6.0f); break;
        case NRatingRestored:
            // An opponent of rated games was banned for cheating: the points come back. The toast
            // never interrupts a game: it waits for its end (update()), the points adding up.
            ratingRestored_.add(e.noticeArg);
            if (signedIn_) {
                api_->fetchAccount();
                expect(Kind::AccountResult);
            }
            break;
        default: break;
        }
        break;
    case Kind::ServerError:
        if ((e.gameId != 0 && e.gameId == gameId_) || live::gameError(e.code)) {
            routeGame(e, LinkKind::Server);
            break;
        }
        if (e.code == kErrMatchmakingCooldown && cooldownUntilMs_ < nowMs()) cooldownUntilMs_ = nowMs() + 60000.0;
        // A QueueJoin refused: no search (106 and 107 may also answer a challenge accepted or joined).
        queue_.searching = queue_.searching && e.code != kErrMatchmakingCooldown && e.code != kErrAlreadyInGame &&
                           e.code != kErrInvalidCategory;
        if (e.code >= 200 && e.code < 210) outgoing_ = Outgoing();
        if (e.code == 0 && e.error == "offline") {  // a command sent while not connected: dropped
            queue_.searching = false;
            outgoing_ = Outgoing();
        }
        ui::notify(eventErrorText(e), 4.5f);
        break;
    default: store(); break;
    }
}

void OnlineSession::handleDirect(const net::Event& e) {
    if (isGameEvent(e.kind) || (e.kind == Kind::ServerError && (e.gameId != 0 || live::gameError(e.code)))) {
        routeGame(e, LinkKind::Direct);
        return;
    }
    if (e.kind == Kind::ConnectionChanged) directConn_ = e.state;
    if (e.kind == Kind::ServerError) ui::notify(eventErrorText(e), 4.5f);
}

void OnlineSession::routeGame(const net::Event& e, LinkKind from) {
    uint64_t id = e.game.id ? e.game.id : e.gameId;
    if (e.kind == Kind::GameSnapshot && id != gameId_ && e.game.status == 0) {
        // A new game: matchmaking, a challenge, a rematch, a direct match.
        gameId_ = id;
        gameKind_ = from;
        snapshot_ = e.game;
        gameReady_ = true;
        gameEvents_.clear();
        queue_.searching = false;
        outgoing_ = Outgoing();
        LOGI("online: game %llu ready (%s)", (unsigned long long)id, from == LinkKind::Direct ? "direct" : "server");
        return;
    }
    if (from != gameKind_ || (id != 0 && id != gameId_)) return;
    if (e.kind == Kind::GameSnapshot) snapshot_ = e.game;
    if (e.kind == Kind::OpponentGesture) {
        // Only the latest state matters: it replaces the one the scene has not taken yet.
        auto old = std::find_if(gameEvents_.begin(), gameEvents_.end(),
                                [](const net::Event& q) { return q.kind == Kind::OpponentGesture; });
        if (old != gameEvents_.end()) gameEvents_.erase(old);
    }
    gameEvents_.push_back(e);
}

// =============================================================================================
// Texts
// =============================================================================================

std::string durationText(double ms) {
    long long s = std::max(0LL, (long long)std::ceil(ms / 1000.0));
    char buf[32];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return i18n::ltr(buf);
}

std::string onlineErrorText(const std::string& code, int retryAfterSec, int64_t bannedUntilMs) {
    if (code.empty()) return "";
    if (code == "rate_limited") {
        if (retryAfterSec > 0) return i18n::trf("online.err.rate_limited_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.rate_limited");
    }
    if (code == "server_busy") {
        // Too many password checks at once on the server (its hash queue is full): not the player's fault.
        if (retryAfterSec > 0) return i18n::trf("online.err.server_busy_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.server_busy");
    }
    if (code == "too_many_attempts") {
        if (retryAfterSec > 0) return i18n::trf("online.err.too_many_attempts_for", {durationText(retryAfterSec * 1000.0)});
        return i18n::tr("online.err.too_many_attempts");
    }
    if (code == "banned") {
        if (bannedUntilMs > 0) return i18n::trf("online.err.banned_until", {localTimeText(double(bannedUntilMs))});
        return i18n::tr("online.err.banned");
    }
    static const char* known[] = {"invalid_credentials", "email_unverified", "network", "tls", "certificate", "incompatible",
                                  "unauthorized", "username_taken", "email_taken", "invalid_username", "invalid_email",
                                  "weak_password", "invalid_code", "expired", "registration_closed", "sso_cancelled",
                                  "server_error", "timeout", "offline", "invalid_password", "mfa_code_required",
                                  "password_not_set", "same_email", "not_found", "invalid_response"};
    for (const char* k : known)
        if (code == k) return i18n::tr(std::string("online.err.") + k);
    return i18n::trf("online.err.other", {code});
}

std::string gifErrorText(const std::string& code, int retryAfterSec) {
    if (code == "rate_limited") {
        if (retryAfterSec > 0) return i18n::trf("gif.err.rate_limited_for", {waitText(retryAfterSec)});
        return i18n::tr("gif.err.rate_limited");
    }
    if (code == "server_busy") {
        if (retryAfterSec > 0) return i18n::trf("gif.err.server_busy_for", {waitText(retryAfterSec)});
        return i18n::tr("gif.err.server_busy");
    }
    // Signed out: the token was refused (expired, revoked), or there is none (the network layer
    // says "unauthorized" for both).
    if (code == "unauthorized") return i18n::tr("gif.err.signed_out");
    static const char* known[] = {"game_too_long", "pgn_too_large", "invalid_pgn", "render_failed", "gif_disabled", "write_failed",
                                  "not_found"};
    for (const char* k : known)
        if (code == k) return i18n::tr(std::string("gif.err.") + k);
    return onlineErrorText(code, retryAfterSec);
}

std::string serverErrorText(int code) {
    struct Entry { int code; const char* key; };
    static const Entry entries[] = {
        {3, "online.err.unauthorized"},        {4, "online.err.banned"},
        {5, "online.err.slow_down"},           {6, "online.err.server_full"},
        {8, "online.err.restarting"},          {11, "online.err.email_unverified"},
        {101, "online.err.move_refused"},      {102, "online.err.move_refused"},
        {103, "online.err.move_refused"},      {104, "online.err.move_refused"},
        {105, "online.err.game_over"},         {106, "online.err.already_in_game"},
        {107, "online.err.invalid_category"},  {108, "online.err.draw_offer_limit"},
        {109, "notify.no_draw_claim"},         {110, "online.err.abort_not_allowed"},
        {111, "online.err.no_pending_offer"},  {112, "online.err.flag_fell"},
        {200, "online.err.queue_not_allowed"}, {201, "online.err.challenge_not_found"},
        {202, "online.err.user_unavailable"},  {203, "online.err.challenge_limit"},
        {204, "online.err.challenge_self"},    {205, "online.err.code_invalid"},
        {206, "online.err.rated_official_tc"}, {207, "online.err.cooldown"},
        {208, "online.err.invalid_tc"},        {209, "online.err.rematch_unavailable"},
        {241, "online.err.slow_down"},         {242, "online.err.cheat_detected"},
    };
    for (const Entry& e : entries)
        if (e.code == code) return i18n::tr(e.key);
    return i18n::trf("online.err.code", {std::to_string(code)});
}

std::string eventErrorText(const net::Event& e) {
    if (e.code == 0 && !e.error.empty()) return onlineErrorText(e.error, e.retryAfterSec);
    return serverErrorText(e.code);
}

std::string directErrorText(const std::string& code) {
    static const char* known[] = {"refused",  "timeout",     "wrong_code", "incompatible", "port_in_use",
                                  "network",  "too_many_attempts", "invalid_code", "bad_address", "not_found",
                                  "unreachable", "reset",    "closed",     "host_left"};
    for (const char* k : known)
        if (code == k) return i18n::tr(std::string("direct.err.") + k);
    return i18n::trf("direct.err.other", {code});
}

}  // namespace game
