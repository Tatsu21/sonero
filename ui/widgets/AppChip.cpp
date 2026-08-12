#include "ui/widgets/AppChip.h"

#include "ui/widgets/ChannelStrip.h"

#include <QApplication>
#include <QByteArray>
#include <QDrag>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>

namespace sonar::ui {

namespace {
QString channelText(sonar::audio::ChannelId id) {
    const auto name = sonar::audio::channelName(id);
    return QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
}
}  // namespace

std::uint32_t AppChip::s_draggedAppId = 0;

std::uint32_t AppChip::draggedAppId() noexcept { return s_draggedAppId; }

AppChip::AppChip(std::uint32_t appId, const QString& name,
                 std::optional<audio::ChannelId> channel, QWidget* parent)
    : QFrame(parent), appId_(appId) {
    setObjectName(QStringLiteral("AppChip"));
    // A chip must never be squeezed thinner than its own text.
    // No height ceiling: the chip must fit its own text, or the name is clipped.
    setMinimumHeight(26);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drag onto a channel to route it"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 11, 4);
    layout->setSpacing(7);

    auto* dot = new QLabel(QStringLiteral("☰"), this);  // grip glyph
    dot->setObjectName(QStringLiteral("ChipDot"));

    auto* nameLabel = new QLabel(name, this);
    nameLabel->setObjectName(QStringLiteral("ChipName"));

    layout->addWidget(dot);
    layout->addWidget(nameLabel);

    if (channel) {
        // Tint the chip with its channel's colour, so a glance across the mixer
        // shows where everything went. The chip already sits inside that channel's
        // column, which is why the old "-> Channel" badge is gone.
        const QString accent = ChannelStrip::accentFor(*channel);
        setStyleSheet(QStringLiteral("#AppChip { background:%1; border-radius:8px; }"
                                     " #ChipName { color:#12131b; font-weight:700; }")
                          .arg(accent));
        dot->setStyleSheet(QStringLiteral("color:#12131b; font-size:12px;"));
        setToolTip(QStringLiteral("%1 — on %2. Drag it onto another channel to move it.")
                       .arg(name, channelText(*channel)));
    }
}

void AppChip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressPos_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
    QFrame::mousePressEvent(event);
}

void AppChip::mouseReleaseEvent(QMouseEvent* event) {
    setCursor(Qt::OpenHandCursor);
    QFrame::mouseReleaseEvent(event);
}

void AppChip::mouseMoveEvent(QMouseEvent* event) {
    if ((event->buttons() & Qt::LeftButton) == 0) {
        return;
    }
    if ((event->pos() - pressPos_).manhattanLength() < QApplication::startDragDistance()) {
        return;
    }

    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kAppMimeType), QByteArray::number(appId_));

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(grab());
    drag->setHotSpot(pressPos_);

    // exec() runs a nested event loop; guard against this chip being deleted by a
    // list rebuild while the drag is in flight.
    s_draggedAppId = appId_;
    const QPointer<AppChip> guard(this);
    drag->exec(Qt::MoveAction);
    s_draggedAppId = 0;
    if (guard) {
        setCursor(Qt::OpenHandCursor);
    }
}

}  // namespace sonar::ui
