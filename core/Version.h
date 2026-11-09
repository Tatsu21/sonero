#pragma once

#include <string>

namespace sonar {

// What the app calls itself: SONAR_VERSION plus SONERO_BUILD_ID (a tag, or
// "branch@commit", "-dirty" when modified; empty outside a git checkout).
[[nodiscard]] inline std::string versionString() {
    const std::string version = SONAR_VERSION;
    const std::string build = SONERO_BUILD_ID;
    return build.empty() ? version : version + " (" + build + ")";
}

}  // namespace sonar
