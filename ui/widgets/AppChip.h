#pragma once

#include <cstdint>
#include <optional>

#include <QFrame>
#include <QPoint>

#include "audio/Channel.h"

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
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    std::uint32_t appId_;
    QPoint pressPos_;

    static std::uint32_t s_draggedAppId;
};

}  // namespace sonar::ui
