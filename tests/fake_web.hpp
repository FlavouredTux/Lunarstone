#pragma once

// A fake HTTP layer for the web tests: answers from tables, remembers every
// request and checks no login went anywhere it shouldn't. Logins in tests
// are fake cookies; no test ever sees a real one.

#include "framework.hpp"

#include <lunar/assets/Web.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fakeweb {

using lunar::assets::Credentials;
using lunar::assets::HttpRequest;
using lunar::assets::HttpResponse;
using lunar::assets::HttpTransport;
using lunar::assets::isCredentialHost;

// Obviously not a real cookie; every test that checks for leaks looks for it.
constexpr std::string_view fakeCookie
    = "_|WARNING:-FAKE-COOKIE-FOR-TESTS.--not-a-real-login.|_FAKE-COOKIE-FOR-TESTS-0123456789abcdef";

inline bool hasHeader(const HttpRequest& request, std::string_view prefix)
{
    for (const std::string& header : request.headers)
        if (header.starts_with(prefix))
            return true;
    return false;
}

// Answers requests from a table keyed by URL and keeps every request it
// saw, so tests can check what went where.
struct FakeServer {
    struct Answer {
        long status = 200;
        std::string body;
        std::string location;
        std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
    };
    std::map<std::string, Answer, std::less<>> answers;         // anonymous requests
    std::map<std::string, Answer, std::less<>> signedInAnswers; // requests with a Cookie
    std::vector<HttpRequest> seen;
    std::mutex mutex;

    HttpTransport transport()
    {
        return [this](const HttpRequest& request) -> std::expected<HttpResponse, std::string> {
            const std::scoped_lock lock(mutex);
            seen.push_back(request);
            const auto& table = hasHeader(request, "Cookie: ") ? signedInAnswers : answers;
            auto it = table.find(request.url);
            if (it == table.end())
                return HttpResponse{.status = 404, .body = R"({"errors":[{"code":0,"message":"Not found"}]})"};
            return HttpResponse{.status = it->second.status,
                                .body = it->second.body,
                                .location = it->second.location,
                                .headers = it->second.headers};
        };
    }

    // Every request with a login went to an API host without following
    // redirects, and nothing else carried the cookie anywhere.
    void checkNoLeaks()
    {
        for (const HttpRequest& request : seen) {
            bool login = false;
            for (const std::string& header : request.headers) {
                if (header.starts_with("Cookie: ") || header.starts_with("x-api-key: ")) {
                    login = true;
                    continue;
                }
                CHECK(!header.contains(fakeCookie));
            }
            CHECK(!request.url.contains(fakeCookie));
            CHECK(!request.body.contains(fakeCookie));
            if (login) {
                CHECK(isCredentialHost(request.url));
                CHECK(!request.followRedirects);
            }
            if (request.url.contains("rbxcdn.com"))
                CHECK(request.headers.empty());
        }
    }
};

inline std::shared_ptr<const Credentials> fakeLogin()
{
    auto credentials = Credentials::fromCookie(fakeCookie, "a test");
    CHECK(credentials.has_value());
    return std::make_shared<const Credentials>(std::move(*credentials));
}

} // namespace fakeweb
