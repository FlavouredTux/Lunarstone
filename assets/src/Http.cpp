// A tiny blocking HTTPS client over libcurl: the transport RobloxWeb uses
// outside tests.

#include <lunar/assets/Web.hpp>

#include <stop_token>
#include <utility>

#if LUNAR_WITH_CURL
#include <curl/curl.h>

#include <format>
#include <memory>
#include <mutex>
#include <string_view>
#endif

namespace lunar::assets {

namespace {

thread_local std::stop_token httpStop; // HttpStopScope's

} // namespace

HttpStopScope::HttpStopScope(std::stop_token stop) : previous_(std::exchange(httpStop, std::move(stop))) {}

HttpStopScope::~HttpStopScope()
{
    httpStop = std::move(previous_);
}

#if LUNAR_WITH_CURL

namespace {

// Nothing Roblox serves as a mesh, texture or place comes near this.
constexpr std::size_t maxBody = 512u << 20;

std::size_t writeBody(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* body = static_cast<std::string*>(user);
    const std::size_t bytes = size * count;
    if (body->size() + bytes > maxBody)
        return 0; // aborts the transfer
    body->append(data, bytes);
    return bytes;
}

// Keeps the last response's headers (addHeaderLine).
std::size_t readHeader(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
    const std::size_t bytes = size * count;
    addHeaderLine(*headers, std::string_view(data, bytes));
    return bytes;
}

struct EasyDeleter {
    void operator()(CURL* curl) const { curl_easy_cleanup(curl); }
};

struct MultiDeleter {
    void operator()(CURLM* multi) const { curl_multi_cleanup(multi); }
};

struct ListDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

std::expected<HttpResponse, std::string> curlRequest(const HttpRequest& request)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    std::unique_ptr<CURL, EasyDeleter> curl(curl_easy_init());
    if (!curl)
        return std::unexpected("can't start a download");

    curl_slist* list = nullptr;
    for (const std::string& header : request.headers) {
        curl_slist* appended = curl_slist_append(list, header.c_str());
        if (!appended) {
            curl_slist_free_all(list);
            return std::unexpected("out of memory");
        }
        list = appended;
    }
    const std::unique_ptr<curl_slist, ListDeleter> headers(list);

    HttpResponse response;
    CURL* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    const char* protocols = request.plainHttp ? "http,https" : "https";
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    // No cookie engine, no .netrc: the only credentials sent are the headers asked for.
    curl_easy_setopt(handle, CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    // Let curl undo gzip, zstd or brotli, whichever the CDN picked.
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Lunarstone/0.1");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 300L);
    // A download that stalls (under a byte a second for 30 s) gives up, so a
    // loader being torn down for a teleport isn't held up for minutes.
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &writeBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &readHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);
    if (request.method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (request.method == "HEAD") {
        curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
    } else if (request.method == "PUT" || request.method == "PATCH" || request.method == "DELETE"
               || request.method == "OPTIONS") {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
    } else if (request.method != "GET") {
        return std::unexpected(std::format("unsupported HTTP method {}", request.method));
    }

    // Run through a multi handle in short polls rather than curl_easy_perform,
    // so the thread's HttpStopScope ends it in any phase (curl's progress
    // callback isn't called while a TLS handshake hangs).
    const std::unique_ptr<CURLM, MultiDeleter> multi(curl_multi_init());
    if (!multi || curl_multi_add_handle(multi.get(), handle) != CURLM_OK)
        return std::unexpected("can't start a download");
    CURLcode result = CURLE_OK;
    for (int running = 1; running > 0;) {
        if (httpStop.stop_requested()) {
            curl_multi_remove_handle(multi.get(), handle);
            return std::unexpected("download stopped");
        }
        if (curl_multi_perform(multi.get(), &running) != CURLM_OK
            || (running > 0 && curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr) != CURLM_OK)) {
            curl_multi_remove_handle(multi.get(), handle);
            return std::unexpected("download failed: curl's transfer loop broke");
        }
    }
    int left = 0;
    while (CURLMsg* message = curl_multi_info_read(multi.get(), &left))
        if (message->msg == CURLMSG_DONE && message->easy_handle == handle)
            result = message->data.result;
    curl_multi_remove_handle(multi.get(), handle);
    if (result != CURLE_OK)
        return std::unexpected(std::format("download failed: {}", curl_easy_strerror(result)));
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
    const char* location = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location) == CURLE_OK && location)
        response.location = location;
    return response;
}

} // namespace

bool curlAvailable()
{
    return true;
}

HttpTransport curlTransport()
{
    return &curlRequest;
}

#else

bool curlAvailable()
{
    return false;
}

HttpTransport curlTransport()
{
    return [](const HttpRequest&) -> std::expected<HttpResponse, std::string> {
        return std::unexpected("this build can't download (built without LUNAR_WITH_CURL)");
    };
}

#endif

} // namespace lunar::assets
