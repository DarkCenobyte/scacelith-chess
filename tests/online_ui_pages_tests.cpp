// The online pages of the title menu around the account (src/ui/ui_screens_online.cpp, the QR
// code of ui_qr.cpp), driven through the headless game against the in-process fake server
// (online_ui_app.h): signing in and its refusals (a wrong password, too many attempts, a suspended
// account, an address not verified yet, the two-factor code), creating an account, the forgotten
// password, Google sign-in (a new name, an existing account it is added to, with its two-factor
// code), the account page (the challenges preference, the password, two-factor on and off with its
// QR code and recovery codes, signing out here and everywhere), and the server settings reached
// from the page shown when no server is set (Options > Online server: the custom server's fields,
// Test connection, Apply). The fake's special inputs (online_mock.h) give each refusal. What the
// pages did is read from the session (signed in, the account, the answers waited for), the
// settings and the clipboard; where they went, from the buttons they show.
#include "online_ui_app.h"

#include <string>
#include <vector>

using namespace onlinetest;

namespace {

game::OnlineSession& ses() { return game::onlineSession(); }

// From the Play page (signed in): the account page, its account fetched.
bool toAccount(headless::App& app) {
    if (!app.clickLabel("Account") || !onPage(app, "Change password")) return false;
    return answered(app, Kind::AccountResult, 120) && app.hasLabel("Sign out");
}

// Fills the sign-in form and presses Sign in; true once the answer came.
bool tryToSignIn(headless::App& app, const std::string& user, const std::string& password) {
    if (!fill(app, "User name or e-mail", user) || !fill(app, "Password", password)) return false;
    if (!app.clickLabel("Sign in")) return false;
    return answered(app, Kind::LoginResult, 120);
}

}  // namespace

// The sign-in page: Sign in waits for both fields; a wrong password, too many attempts and a
// suspended account are refused there. An address not verified yet offers the link again: from
// the sign-in page to the page that asks the address, which sends it. Then an account with
// two-factor on: its code page (a code too short, a code that is not one, then the right one).
TEST(online_ui_sign_in_refusals_and_two_factor_code) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toSignInPage(*app));
    CHECK(app->hasLabel("Continue with Google"));   // the fake server offers it
    CHECK(app->hasLabel("Create an account"));
    CHECK(app->hasLabel("Forgot password?"));
    CHECK(!enabled(*app, "Sign in"));
    REQUIRE(fill(*app, "User name or e-mail", "Refused"));
    CHECK(!enabled(*app, "Sign in"));               // no password yet

    REQUIRE(tryToSignIn(*app, "Refused", "wrong"));
    CHECK(!s.signedIn());
    CHECK(app->hasLabel("Sign in"));
    CHECK(!app->hasLabel("Resend the e-mail"));
    for (const char* who : {"ratelimited", "banned"}) {
        REQUIRE(tryToSignIn(*app, who, "correct horse battery"));
        CHECK(!s.signedIn());
        CHECK(app->hasLabel("Sign in"));
    }

    // Not verified: the link again, from the page that asks the address (none was typed here).
    REQUIRE(tryToSignIn(*app, "unverified_one", "correct horse battery"));
    CHECK(!s.signedIn());
    REQUIRE(app->hasLabel("Resend the e-mail"));
    REQUIRE(app->clickLabel("Resend the e-mail"));
    REQUIRE(onPage(*app, "E-mail"));
    REQUIRE(fill(*app, "E-mail", "no address"));
    CHECK(!enabled(*app, "Resend the e-mail"));
    REQUIRE(fill(*app, "E-mail", "unverified@example.com"));
    REQUIRE(enabled(*app, "Resend the e-mail"));
    REQUIRE(app->clickLabel("Resend the e-mail"));
    CHECK(s.busy(Kind::VerificationResent));
    REQUIRE(answered(*app, Kind::VerificationResent, 120));
    REQUIRE(app->clickLabel("Sign in"));             // to the sign-in page
    REQUIRE(onPage(*app, "User name or e-mail"));

    // Two-factor: the code page, Back to the form, then the codes.
    REQUIRE(tryToSignIn(*app, "mfa_keeper", "correct horse battery"));
    REQUIRE(onPage(*app, "Verify"));
    CHECK(!s.signedIn());
    CHECK(!enabled(*app, "Verify"));
    REQUIRE(fill(*app, "Code", "12345", 16));
    CHECK(!enabled(*app, "Verify"));                 // six characters at least
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "User name or e-mail"));
    REQUIRE(tryToSignIn(*app, "mfa_keeper", "correct horse battery"));
    REQUIRE(onPage(*app, "Verify"));
    REQUIRE(fill(*app, "Code", "abcdef", 16));
    REQUIRE(app->clickLabel("Verify"));
    REQUIRE(answered(*app, Kind::LoginResult, 120));
    CHECK(!s.signedIn());
    CHECK(app->hasLabel("Verify"));                  // a wrong code: the code page again
    REQUIRE(fill(*app, "Code", "123456", 16));
    REQUIRE(app->clickLabel("Verify"));
    REQUIRE(app->runUntil([&] { return s.signedIn() && app->hasLabel("Find opponent"); }, 300));
    CHECK_EQ(s.account().username, std::string("mfa_keeper"));
    CHECK(toTitle(*app));
}

// A new account: the form (Create account waits for a name, an address, and the same password
// twice), a name already taken, then the account made: the page asking to open the mailed link,
// which sends it again. Then the forgotten password: its form needs an address.
TEST(online_ui_registration_and_forgotten_password) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toSignInPage(*app));
    REQUIRE(app->clickLabel("Create an account"));
    REQUIRE(onPage(*app, "Create account"));
    CHECK(!enabled(*app, "Create account"));
    REQUIRE(fill(*app, "User name", "taken_name"));
    REQUIRE(fill(*app, "E-mail", "newcomer@example.com"));
    REQUIRE(fill(*app, "Password", "long enough password"));
    REQUIRE(fill(*app, "Confirm password", "another long password"));
    CHECK(!enabled(*app, "Create account"));         // the passwords differ
    REQUIRE(fill(*app, "Confirm password", "long enough password"));
    REQUIRE(enabled(*app, "Create account"));
    REQUIRE(app->clickLabel("Create account"));
    CHECK(s.busy(Kind::RegisterResult));
    REQUIRE(answered(*app, Kind::RegisterResult, 120));
    CHECK(app->hasLabel("Create account"));          // the name is taken: the form again
    REQUIRE(fill(*app, "User name", "Newcomer"));
    // The passwords were kept for this second try.
    REQUIRE(enabled(*app, "Create account"));
    REQUIRE(app->clickLabel("Create account"));
    REQUIRE(answered(*app, Kind::RegisterResult, 120));
    // The fake server verifies addresses: the page about the mailed link (the address is known:
    // no field asks it).
    REQUIRE(onPage(*app, "Resend the e-mail"));
    CHECK(!app->hasLabel("E-mail"));
    CHECK(!s.signedIn());
    REQUIRE(app->clickLabel("Resend the e-mail"));
    REQUIRE(answered(*app, Kind::VerificationResent, 120));
    REQUIRE(app->clickLabel("Sign in"));
    REQUIRE(onPage(*app, "User name or e-mail"));

    REQUIRE(app->clickLabel("Forgot password?"));
    REQUIRE(onPage(*app, "Send the link"));
    REQUIRE(fill(*app, "E-mail", "nobody"));
    CHECK(!enabled(*app, "Send the link"));
    REQUIRE(fill(*app, "E-mail", "newcomer@example.com"));
    REQUIRE(enabled(*app, "Send the link"));
    REQUIRE(app->clickLabel("Send the link"));
    CHECK(s.busy(Kind::PasswordResetRequested));
    REQUIRE(answered(*app, Kind::PasswordResetRequested, 120));
    CHECK(app->hasLabel("Send the link"));           // its note shows on the page
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "User name or e-mail"));
    CHECK(!s.signedIn());
    CHECK(toTitle(*app));
}

// Google sign-in, first time: the page waiting for the browser (cancelled), then Google's answer
// asks a name for the new account (one too short refused), and the account is made, linked to
// Google. Signed out, Google signs that account in again at once.
TEST(online_ui_google_sign_in_new_account) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toSignInPage(*app));
    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(onPage(*app, "Cancel"));
    CHECK(!app->hasLabel("Sign in"));
    REQUIRE(app->clickLabel("Cancel"));
    REQUIRE(onPage(*app, "Continue with Google"));

    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(onPage(*app, "User name", 300));         // Google sent the browser back: a new account
    CHECK(app->hasLabel("Continue"));
    REQUIRE(fill(*app, "User name", "ab"));
    REQUIRE(app->clickLabel("Continue"));
    REQUIRE(answered(*app, Kind::LoginResult, 120));
    CHECK(!s.signedIn());
    CHECK(app->hasLabel("Continue"));                 // too short: the name page again
    REQUIRE(fill(*app, "User name", "Googler"));
    REQUIRE(app->clickLabel("Continue"));
    REQUIRE(app->runUntil([&] { return s.signedIn() && app->hasLabel("Find opponent"); }, 300));
    REQUIRE(toAccount(*app));
    CHECK_EQ(s.account().username, std::string("Googler"));
    CHECK(s.account().googleLinked);

    REQUIRE(app->clickLabel("Sign out"));
    REQUIRE(onPage(*app, "Continue with Google"));
    CHECK(!s.signedIn());
    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(app->runUntil([&] { return s.signedIn() && app->hasLabel("Find opponent"); }, 300));
    CHECK_EQ(s.account().username, std::string("Googler"));
    CHECK(toTitle(*app));
}

// Google sign-in for the address of an existing account (with two-factor on): the page asking the
// account's password before Google is added ("not my account" leaves it; a wrong password stays),
// then the two-factor code of that account (its page left first: Google is not added), and
// finally the account signed in, Google linked to it.
TEST(online_ui_google_sign_in_added_to_an_account) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(toSignInPage(*app));
    REQUIRE(tryToSignIn(*app, "mfa_linker", "correct horse battery"));
    REQUIRE(onPage(*app, "Verify"));
    REQUIRE(fill(*app, "Code", "123456", 16));
    REQUIRE(app->clickLabel("Verify"));
    REQUIRE(app->runUntil([&] { return s.signedIn() && app->hasLabel("Find opponent"); }, 300));
    REQUIRE(toAccount(*app));
    CHECK(!s.account().googleLinked);
    CHECK(s.account().mfaEnabled);
    REQUIRE(app->clickLabel("Sign out"));
    REQUIRE(onPage(*app, "Continue with Google"));

    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(onPage(*app, "Add Google sign-in", 300));
    CHECK(!enabled(*app, "Add Google sign-in"));
    REQUIRE(app->clickLabel("This is not my account"));
    REQUIRE(onPage(*app, "Continue with Google"));
    CHECK(!s.signedIn());

    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(onPage(*app, "Add Google sign-in", 300));
    REQUIRE(fill(*app, "Password", "wrong"));
    REQUIRE(app->clickLabel("Add Google sign-in"));
    REQUIRE(answered(*app, Kind::LoginResult, 120));
    CHECK(app->hasLabel("Add Google sign-in"));      // a wrong password: asked again
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Add Google sign-in"));
    REQUIRE(onPage(*app, "Verify", 120));            // two-factor on: its code first
    REQUIRE(app->clickLabel("Back"));                // Google is not added
    REQUIRE(onPage(*app, "Continue with Google"));
    CHECK(!s.signedIn());

    REQUIRE(app->clickLabel("Continue with Google"));
    REQUIRE(onPage(*app, "Add Google sign-in", 300));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Add Google sign-in"));
    REQUIRE(onPage(*app, "Verify", 120));
    REQUIRE(fill(*app, "Code", "654321", 16));
    REQUIRE(app->clickLabel("Verify"));
    REQUIRE(app->runUntil([&] { return s.signedIn() && app->hasLabel("Find opponent"); }, 300));
    REQUIRE(toAccount(*app));
    CHECK_EQ(s.account().username, std::string("mfa_linker"));
    CHECK(s.account().googleLinked);
    CHECK(toTitle(*app));
}

// The account page and its security pages: the challenges preference (off, then on again), the
// password (a wrong current one, a weak new one, then changed), two-factor turned on (password,
// the QR code and its key, the app's code, the recovery codes copied as sensitive text), new
// recovery codes, two-factor turned off; Sign out everywhere (its confirmation cancelled, then
// confirmed), and Sign out.
TEST(online_ui_account_security_pages) {
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    REQUIRE(signIn(*app, "Securus"));
    REQUIRE(toAccount(*app));
    CHECK_EQ(s.account().username, std::string("Securus"));
    CHECK(app->hasLabel("Turn on two-factor"));
    CHECK(!app->hasLabel("Turn off two-factor"));
    CHECK(app->hasLabel("Game history"));
    CHECK(app->hasLabel("Signed-in devices"));
    CHECK(app->hasLabel("Download my data"));
    CHECK(app->hasLabel("Delete account"));

    REQUIRE(s.account().acceptChallenges);
    REQUIRE(app->clickLabel("Accept challenges"));
    REQUIRE(answered(*app, Kind::PreferencesResult, 120));
    CHECK(!s.account().acceptChallenges);
    REQUIRE(app->clickLabel("Accept challenges"));
    REQUIRE(answered(*app, Kind::PreferencesResult, 120));
    CHECK(s.account().acceptChallenges);

    // The password.
    REQUIRE(app->clickLabel("Change password"));
    REQUIRE(onPage(*app, "Current password"));
    CHECK(!enabled(*app, "Change password"));
    REQUIRE(fill(*app, "Current password", "wrong"));
    REQUIRE(fill(*app, "New password", "a new long password"));
    REQUIRE(fill(*app, "Confirm password", "a new long passwor"));
    CHECK(!enabled(*app, "Change password"));        // the two differ
    REQUIRE(fill(*app, "Confirm password", "a new long password"));
    REQUIRE(enabled(*app, "Change password"));
    REQUIRE(app->clickLabel("Change password"));
    REQUIRE(answered(*app, Kind::PasswordChanged, 120));
    CHECK(app->hasLabel("Current password"));        // refused: the form again
    REQUIRE(fill(*app, "Current password", "correct horse battery"));
    REQUIRE(fill(*app, "New password", "short"));
    REQUIRE(fill(*app, "Confirm password", "short"));
    REQUIRE(app->clickLabel("Change password"));
    REQUIRE(answered(*app, Kind::PasswordChanged, 120));
    CHECK(app->hasLabel("Current password"));        // too weak
    REQUIRE(fill(*app, "New password", "a new long password"));
    REQUIRE(fill(*app, "Confirm password", "a new long password"));
    REQUIRE(app->clickLabel("Change password"));
    REQUIRE(answered(*app, Kind::PasswordChanged, 120));
    REQUIRE(onPage(*app, "Turn on two-factor"));     // changed: the account page
    CHECK(s.signedIn());

    // Two-factor on: the password, then the QR code and the app's code, then the recovery codes.
    REQUIRE(app->clickLabel("Turn on two-factor"));
    REQUIRE(onPage(*app, "Continue"));
    CHECK(!enabled(*app, "Continue"));
    REQUIRE(fill(*app, "Password", "wrong"));
    REQUIRE(app->clickLabel("Continue"));
    REQUIRE(answered(*app, Kind::MfaSetupResult, 120));
    CHECK(app->hasLabel("Continue"));                 // refused: still the password step
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(app->clickLabel("Continue"));
    REQUIRE(answered(*app, Kind::MfaSetupResult, 120));
    REQUIRE(onPage(*app, "Code from the app"));
    CHECK(!enabled(*app, "Turn on"));
    REQUIRE(fill(*app, "Code from the app", "12a45", 8));
    CHECK(!enabled(*app, "Turn on"));                 // four digits
    REQUIRE(fill(*app, "Code from the app", "123 456", 8));
    REQUIRE(enabled(*app, "Turn on"));
    REQUIRE(app->clickLabel("Turn on"));
    REQUIRE(answered(*app, Kind::MfaEnableResult, 120));
    REQUIRE(onPage(*app, "I saved them"));
    fakeplat::record().clipboard.clear();
    REQUIRE(app->clickLabel("Copy"));
#if !defined(_WIN32)   // the test platform's clipboard (Windows' own is not faked: the copy fails)
    CHECK(!fakeplat::record().clipboard.empty());
    CHECK(fakeplat::record().clipboardSensitive);     // kept out of clipboard histories
    CHECK(app->hasLabel("Copied"));
#endif
    REQUIRE(app->clickLabel("I saved them"));
    REQUIRE(onPage(*app, "Turn off two-factor"));
    REQUIRE(answered(*app, Kind::AccountResult, 120));
    CHECK(s.account().mfaEnabled);
    CHECK(app->hasLabel("New recovery codes"));

    // New recovery codes: the password and a code of the app.
    REQUIRE(app->clickLabel("New recovery codes"));
    REQUIRE(onPage(*app, "New codes"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(fill(*app, "Code from the app", "12345", 16));
    CHECK(!enabled(*app, "New codes"));
    REQUIRE(fill(*app, "Code from the app", "123456", 16));
    REQUIRE(enabled(*app, "New codes"));
    REQUIRE(app->clickLabel("New codes"));
    REQUIRE(answered(*app, Kind::RecoveryCodesResult, 120));
    REQUIRE(onPage(*app, "I saved them"));
    REQUIRE(app->clickLabel("I saved them"));
    REQUIRE(onPage(*app, "Turn off two-factor"));

    // Two-factor off.
    REQUIRE(app->clickLabel("Turn off two-factor"));
    REQUIRE(onPage(*app, "Turn off"));
    CHECK(!enabled(*app, "Turn off"));
    REQUIRE(fill(*app, "Password", "correct horse battery"));
    REQUIRE(fill(*app, "Code", "ABCD-EFGH-JK", 16));   // a recovery code
    REQUIRE(enabled(*app, "Turn off"));
    REQUIRE(app->clickLabel("Turn off"));
    REQUIRE(answered(*app, Kind::MfaDisableResult, 120));
    REQUIRE(onPage(*app, "Turn on two-factor"));
    REQUIRE(answered(*app, Kind::AccountResult, 120));
    CHECK(!s.account().mfaEnabled);

    // Sign out everywhere: asked first (cancelled), then confirmed.
    REQUIRE(app->clickLabel("Sign out everywhere"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    app->frames(20);
    REQUIRE(app->clickLabel("Cancel"));
    app->frames(20);
    CHECK(!app->hasLabel("Cancel"));
    CHECK(s.signedIn());
    REQUIRE(app->clickLabel("Sign out everywhere"));
    REQUIRE(app->waitForLabel("Cancel", 30));
    app->frames(20);
    REQUIRE(clickLast(*app, "Sign out everywhere"));
    REQUIRE(onPage(*app, "User name or e-mail"));
    CHECK(!s.signedIn());
    REQUIRE(answered(*app, Kind::LogoutResult, 120));

    // Signed in again, then out from the account page.
    REQUIRE(signInHere(*app, "Securus"));
    REQUIRE(toAccount(*app));
    REQUIRE(app->clickLabel("Sign out"));
    REQUIRE(onPage(*app, "User name or e-mail"));
    CHECK(!s.signedIn());
    CHECK(toTitle(*app));
}

// No server set (a custom server without its host): the page saying so, with its way to the
// server settings (Options on its Online server tab) and to the direct match. There: the official
// server chosen, then the custom one again; its host tested (unreachable, then an old server,
// then one that answers), its ports and fingerprint typed, then applied: the session uses it, and
// closing Options comes back to the online page, now its sign-in page.
TEST(online_ui_no_server_and_server_settings) {
    ReleaseAtEnd release;
    headless::App* app = onlineApp();
    REQUIRE(app);
    game::OnlineSession& s = ses();
    game::Settings& gs = game::settings();
    gs.onlineCustomServer = true;
    gs.onlineHost.clear();
    REQUIRE(!s.serverConfigured());
    REQUIRE(app->clickLabel("Play Online"));
    REQUIRE(onPage(*app, "Server settings"));
    CHECK(app->hasLabel("Direct match"));
    CHECK(!app->hasLabel("Sign in"));
    REQUIRE(app->clickLabel("Server settings"));
    REQUIRE(onPage(*app, "Test connection"));
    REQUIRE(app->hasLabel("Host"));
    CHECK(!enabled(*app, "Test connection"));        // no host

    // The two server choices (above the fields): the official one hides the custom server's fields.
    std::vector<ui::im::TracedItem> choices = bare(*app, 56.0f);
    REQUIRE(choices.size() >= 2);
    app->clickItem(choices[0]);
    app->frames(5);
    CHECK(!app->hasLabel("Host"));
    CHECK(enabled(*app, "Test connection"));
    app->clickItem(choices[1]);
    app->frames(5);
    REQUIRE(app->hasLabel("Host"));

    // Test connection: an unreachable host, a server of another version, then one that answers.
    for (const char* host : {"offline.example", "old.example", "club.example"}) {
        REQUIRE(fill(*app, "Host", host));
        REQUIRE(enabled(*app, "Test connection"));
        REQUIRE(app->clickLabel("Test connection"));
        CHECK(s.testing());
        CHECK(!enabled(*app, "Test connection"));    // while it runs
        REQUIRE(app->runUntil([&] { return !s.testing(); }, 300));
        app->frames(2);
        CHECK(enabled(*app, "Test connection"));
    }
    REQUIRE(fill(*app, "HTTPS / API port", "8443", 6));
    REQUIRE(fill(*app, "WSS port", "8444", 6));
    REQUIRE(fill(*app, "Certificate fingerprint (SHA-256)", "not hexadecimal", 100));
    REQUIRE(fill(*app, "Certificate fingerprint (SHA-256)", "", 100));
    CHECK(gs.onlineHost.empty());                    // nothing applied yet
    REQUIRE(enabled(*app, "Apply"));
    REQUIRE(app->clickLabel("Apply"));
    CHECK_EQ(gs.onlineHost, std::string("club.example"));
    CHECK_EQ(gs.onlineApiPort, 8443);
    CHECK_EQ(gs.onlineWsPort, 8444);
    CHECK(gs.onlineCustomServer);
    CHECK(s.serverConfigured());
    CHECK_EQ(s.endpoint().host, std::string("club.example"));
    REQUIRE(app->clickLabel("Back"));
    REQUIRE(onPage(*app, "User name or e-mail"));    // the online page, signed out on that server
    REQUIRE(app->runUntil([&] { return s.infoKnown(); }, 300));
    CHECK_EQ(s.serverName(), std::string("Scacelith (mock server)"));
    CHECK(toTitle(*app));
}
