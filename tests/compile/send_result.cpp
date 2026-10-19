// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled, not run (tests/CMakeLists.txt, build.send_result_*): HttpClient::send and
// auth::Session::send return false without calling on_done, so ignoring that result is a compile
// warning. With WNK_PROBE_IGNORE the calls drop it and must not compile under -Werror=unused-result;
// without it the same calls check it and must compile.
#include "wnk/auth/session.hpp"
#include "wnk/net/http.hpp"

namespace wnk {

bool probe(net::HttpClient &http, auth::Session &session, async::Lifetime &life) {
#ifdef WNK_PROBE_IGNORE
    http.send(net::HttpRequest{}, {}, life.weak(), [](net::HttpResponse) {});
    session.send(http, net::HttpRequest{}, {}, life.weak(), [](net::HttpResponse) {});
    return true;
#else
    const bool a = http.send(net::HttpRequest{}, {}, life.weak(), [](net::HttpResponse) {});
    const bool b = session.send(http, net::HttpRequest{}, {}, life.weak(), [](net::HttpResponse) {});
    return a && b;
#endif
}

}  // namespace wnk
