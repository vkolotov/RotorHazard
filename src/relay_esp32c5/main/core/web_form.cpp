#include "web_form.h"

namespace relay {

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += (char)(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseForm(const std::string& body)
{
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i <= body.size()) {
        size_t j = body.find('&', i);
        if (j == std::string::npos)
            j = body.size();
        const std::string pair = body.substr(i, j - i);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos)
                out[urlDecode(pair)] = "";
            else
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        i = j + 1;
    }
    return out;
}

std::string htmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

}  // namespace relay
