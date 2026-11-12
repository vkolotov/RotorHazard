// Small helpers for the relay's settings page: form decoding and HTML escaping.
#pragma once
#include <map>
#include <string>

namespace relay {

// application/x-www-form-urlencoded -> name/value ('+' is a space, %XX a byte).
std::map<std::string, std::string> parseForm(const std::string& body);
std::string urlDecode(const std::string& s);
// Safe inside HTML text and double-quoted attributes.
std::string htmlEscape(const std::string& s);

}  // namespace relay
