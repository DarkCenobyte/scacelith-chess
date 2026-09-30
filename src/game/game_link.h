// The line from the 3D scene to the authority of an online game: the Scacelith server
// (net::OnlineClient) or the host of a direct match (net::DirectMatch). Both speak the same
// protocol and emit the same net::Event values, so the scene plays both kinds of games with one
// code path; GameLink hides the few differences (a game id, the report button, the scoresheet
// event). OnlineSession (online_session.h) creates the link of each game it announces.
#pragma once
#include "../net/gesture.h"
#include <cstdint>
#include <string>

namespace game {

enum class LinkKind { Server, Direct };

class GameLink {
public:
    virtual ~GameLink() = default;
    virtual LinkKind kind() const = 0;
    virtual uint64_t gameId() const = 0;

    // Commands of the game (net::OnlineClient semantics): results come back as events.
    virtual void sendMove(int ply, uint16_t move, const std::string& fen, uint32_t thinkMs, bool drawOffer) = 0;
    virtual void resign() = 0;
    virtual void offerDraw() = 0;
    virtual void answerDraw(bool accept) = 0;
    virtual void claimDraw() = 0;
    virtual void abortGame() = 0;
    virtual void requestResync() = 0;
    virtual void rematch(bool accept) = 0;
    // Live gestures of the local player for the opponent's robot (net/gesture.h): cosmetic, only
    // the latest one is kept and the network layer paces them, so it may be called every frame.
    // The opponent's come back as OpponentGesture events of this game.
    virtual void sendGesture(const net::Gesture& g) = 0;
    // Report the opponent (server games only): category "cheating", "abuse" or "other".
    virtual bool canReport() const = 0;
    virtual void report(const std::string& username, const std::string& category, const std::string& comment) = 0;

    virtual int pingMs() const = 0;          // round trip, -1 when unknown
    virtual double serverNowMs() const = 0;  // the authority's clock (clock display)
    virtual bool reconnecting() const = 0;   // the connection is lost and being restored
    virtual std::string eventName() const = 0;  // scoresheet "Event": the server's name, or "Friendly match"
};

}  // namespace game
