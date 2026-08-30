#pragma once

#include <string>

namespace sonar {

// What to show when the app names itself.
//
// SONAR_VERSION is the release number from CMakeLists.txt; SONERO_BUILD_ID says
// which code it was built from — a tag ("v0.1.2"), or a branch and commit
// ("main@d807a8a"), with "-dirty" when the tree had uncommitted changes. The id
// is empty for a build from something that is not a git checkout, and then the
// plain release number is all that gets shown.
[[nodiscard]] inline std::string versionString() {
    const std::string version = SONAR_VERSION;
    const std::string build = SONERO_BUILD_ID;
    if (build.empty()) {
        return version;
    }
    return version + " (" + build + ")";
}

}  // namespace sonar
