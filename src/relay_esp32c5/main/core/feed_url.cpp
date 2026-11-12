#include "feed_url.h"

#include <cctype>
#include <vector>

namespace relay {

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        a++;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

std::string socketUrl(const std::string& raw, const std::string& uid)
{
    const std::string feed = trim(raw);
    std::string scheme;
    size_t rest;
    if (feed.rfind("https://", 0) == 0) {
        scheme = "wss";
        rest = 8;
    } else if (feed.rfind("http://", 0) == 0) {
        scheme = "ws";
        rest = 7;
    } else {
        return "";
    }
    const size_t pathAt = feed.find('/', rest);
    if (pathAt == std::string::npos)
        return "";
    const std::string host = feed.substr(rest, pathAt - rest);
    const size_t queryAt = feed.find('?', pathAt);
    const size_t fragmentAt = feed.find('#', pathAt);
    const std::string path = feed.substr(pathAt, queryAt == std::string::npos ? std::string::npos : queryAt - pathAt);
    std::string query;
    if (queryAt != std::string::npos)
        query = feed.substr(queryAt + 1, fragmentAt == std::string::npos ? std::string::npos : fragmentAt - queryAt - 1);
    std::vector<std::string> segments;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        if (j > i)
            segments.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    if (host.empty() || segments.size() < 3 || segments[segments.size() - 2] != "feed")
        return "";
    const std::string& club = segments[segments.size() - 3];
    const std::string& source = segments.back();
    std::string key;
    i = 0;
    while (i <= query.size()) {
        size_t j = query.find('&', i);
        if (j == std::string::npos)
            j = query.size();
        const std::string pair = query.substr(i, j - i);
        if (pair.rfind("key=", 0) == 0)
            key = pair.substr(4);
        i = j + 1;
    }
    if (key.empty())
        return "";
    return scheme + "://" + host + "/ws/timers/" + club + "/" + source + "/?key=" + key + "&uid=" + uid;
}

}  // namespace relay
