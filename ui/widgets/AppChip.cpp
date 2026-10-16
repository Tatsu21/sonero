#include "ui/widgets/AppChip.h"

#include <QColor>

#include <QEvent>

#include "ui/Theme.h"

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

    dot_ = dot;
    channel_ = channel;
    applyTint();
}

void AppChip::applyTint() {
    if (!channel_ || restyling_) {
        return;
    }
    restyling_ = true;
    // Tint the chip with its channel's colour, so a glance across the mixer
    // shows where everything went. The chip already sits inside that channel's
    // column, which is why the old "-> Channel" badge is gone.
    const QString accent = ChannelStrip::accentFor(*channel_);
    // The label sits on the channel's own colour, so it cannot come from the
    // theme's text token — it has to contrast with whatever that tint is. Qt has
    // no contrast function, so: dark text on a light tint and the reverse,
    // decided by the tint's own lightness.
    const QColor tint(accent);
    const QString ink = tint.lightness() > 140 ? currentTheme().bg.name(QColor::HexRgb)
                                               : currentTheme().text.name(QColor::HexRgb);
    setStyleSheet(QStringLiteral("#AppChip { background:%1; border-radius:8px; }"
                                 " #ChipName { color:%2; font-weight:700; }")
                      .arg(accent, ink));
    if (dot_ != nullptr) {
        dot_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;").arg(ink));
    }
    restyling_ = false;
}

void AppChip::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        applyTint();
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
