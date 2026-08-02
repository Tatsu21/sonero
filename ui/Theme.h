#pragma once

class QApplication;

namespace sonar::ui {

// Applies the application-wide modern dark theme (Fusion base + palette + a
// global Qt style sheet). Call once, right after constructing QApplication.
void applyTheme(QApplication& app);

}  // namespace sonar::ui
