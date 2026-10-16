#pragma once

#include <cstdint>
#include <optional>

#include <QFrame>
#include <QPoint>

#include "audio/Channel.h"

class QEvent;
class QLabel;

namespace sonar::ui {

// MIME type carrying a PipeWire application node id during a drag.
inline constexpr char kAppMimeType[] = "application/x-sonar-appid";

// A draggable pill representing a running application. Dragging it onto a
// ChannelStrip routes the app to that channel.
class AppChip : public QFrame {
    Q_OBJECT

public:
    AppChip(std::uint32_t appId, const QString& name,
            std::optional<sonar::audio::ChannelId> channel, QWidget* parent = nullptr);

    // The app id of the chip being dragged right now (0 when no drag is active).
    // Drop targets read this instead of the MIME data, which avoids the flaky
    // pipe transfer for same-process drags on Wayland.
    [[nodiscard]] static std::uint32_t draggedAppId() noexcept;

protected:
    // The chip is painted in its channel's accent, which comes from the theme,
    // so it has to be re-derived when one is switched at runtime.
    void changeEvent(QEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void applyTint();  // (re)style from the channel's accent

    std::uint32_t appId_;
    QPoint pressPos_;
    std::optional<sonar::audio::ChannelId> channel_;
    QLabel* dot_ = nullptr;
    // setStyleSheet() inside changeEvent() makes Qt send another palette change,
    // which re-enters changeEvent. Without this the first theme applied recursed
    // until the stack ran out — 2158 frames deep, crashing inside Qt's own CSS
    // parser, which made it look like a style sheet problem rather than a loop.
    bool restyling_ = false;

    static std::uint32_t s_draggedAppId;
};

}  // namespace sonar::ui
