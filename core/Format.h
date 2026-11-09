#pragma once

#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A small brace formatter standing in for std::format, which GCC 11 (Ubuntu 22.04,
// Mint 21) lacks. Supports {}, {N}, {:.Nf} and {{ }}; anything else is copied
// through verbatim, because a crash while logging is worse than a malformed line.
namespace sonar::fmt {

namespace detail {

template <class T>
std::string render(const T& value, int precision) {
    std::ostringstream out;
    if (precision >= 0) {
        out << std::fixed << std::setprecision(precision);
    }
    out << value;
    return out.str();
}

// bool would otherwise print as 1/0, which reads badly in a log line.
inline std::string render(bool value, int) { return value ? "true" : "false"; }

using Arg = std::function<std::string(int)>;

std::string apply(std::string_view spec, const std::vector<Arg>& args);

}  // namespace detail

template <class... Args>
std::string format(std::string_view spec, Args&&... args) {
    const std::vector<detail::Arg> boxed{detail::Arg(
        [value = std::forward<Args>(args)](int precision) {
            return detail::render(value, precision);
        })...};
    return detail::apply(spec, boxed);
}

// Still goes through apply(): a format string with no arguments can legitimately
// contain "{{" escapes, and returning it verbatim would leave them doubled.
inline std::string format(std::string_view spec) { return detail::apply(spec, {}); }

}  // namespace sonar::fmt
