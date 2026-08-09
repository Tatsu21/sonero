#include "core/Format.h"

#include <cctype>

namespace sonar::fmt::detail {

namespace {

// Parse the inside of a replacement field: "", "3", ":.4f", "2:.1f".
// Returns false when it does not look like a field we understand, so the caller
// can copy the text through untouched.
bool parseField(std::string_view body, std::size_t& index, int& precision,
                bool& hasIndex) {
    hasIndex = false;
    precision = -1;

    std::size_t i = 0;
    while (i < body.size() && (std::isdigit(static_cast<unsigned char>(body[i])) != 0)) {
        index = (hasIndex ? index * 10 : 0) + static_cast<std::size_t>(body[i] - '0');
        hasIndex = true;
        ++i;
    }
    if (i == body.size()) {
        return true;  // "{}" or "{2}"
    }
    if (body[i] != ':') {
        return false;
    }
    ++i;
    if (i >= body.size() || body[i] != '.') {
        return false;  // only precision specs are supported
    }
    ++i;
    int value = 0;
    bool anyDigit = false;
    while (i < body.size() && (std::isdigit(static_cast<unsigned char>(body[i])) != 0)) {
        value = value * 10 + (body[i] - '0');
        anyDigit = true;
        ++i;
    }
    if (!anyDigit) {
        return false;
    }
    // Accept "{:.4f}" and "{:.4}" alike; anything else is not ours.
    if (i < body.size() && (body[i] == 'f' || body[i] == 'F')) {
        ++i;
    }
    if (i != body.size()) {
        return false;
    }
    precision = value;
    return true;
}

}  // namespace

std::string apply(std::string_view spec, const std::vector<Arg>& args) {
    std::string out;
    out.reserve(spec.size() + args.size() * 8);

    std::size_t next = 0;  // next argument for a bare "{}"
    for (std::size_t i = 0; i < spec.size(); ++i) {
        const char c = spec[i];

        if (c == '{' && i + 1 < spec.size() && spec[i + 1] == '{') {
            out.push_back('{');
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < spec.size() && spec[i + 1] == '}') {
            out.push_back('}');
            ++i;
            continue;
        }
        if (c != '{') {
            out.push_back(c);
            continue;
        }

        const std::size_t close = spec.find('}', i + 1);
        if (close == std::string_view::npos) {
            out.append(spec.substr(i));  // unterminated: copy the rest verbatim
            break;
        }

        std::size_t index = 0;
        int precision = -1;
        bool hasIndex = false;
        const std::string_view body = spec.substr(i + 1, close - i - 1);
        if (!parseField(body, index, precision, hasIndex)) {
            out.append(spec.substr(i, close - i + 1));  // not ours: pass through
            i = close;
            continue;
        }

        const std::size_t which = hasIndex ? index : next++;
        if (which < args.size()) {
            out.append(args[which](precision));
        } else {
            // More fields than arguments: show the field rather than pretending
            // it was empty, so the mistake is visible in the output.
            out.append(spec.substr(i, close - i + 1));
        }
        i = close;
    }
    return out;
}

}  // namespace sonar::fmt::detail
