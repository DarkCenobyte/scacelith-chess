// Linux builds without OpenSSL: the online client compiles and reports "unavailable".
#if !defined(_WIN32) && !defined(SCACELITH_HAS_OPENSSL)
#include "transport.h"

namespace net {

bool transportAvailable() { return false; }

void httpRequest(const HttpRequest&, HttpResponse& resp, CancelToken*) {
    resp = HttpResponse();
    resp.error = "unavailable";
    resp.detail = "this build has no TLS library (OpenSSL was not found by CMake)";
}

std::unique_ptr<WebSocket> wsConnect(const WsParams&, std::string& error, int& httpStatus, CancelToken*) {
    error = "unavailable";
    httpStatus = 0;
    return nullptr;
}

}  // namespace net

#endif
