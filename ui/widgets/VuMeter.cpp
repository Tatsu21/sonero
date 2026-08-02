#include "ui/widgets/VuMeter.h"

#include <algorithm>

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace sonar::ui {

namespace {
constexpr float kReleaseFactor = 0.80f;    // per-update decay of the bar
constexpr float kPeakReleaseFactor = 0.98f;  // slower decay of the peak marker
}  // namespace

VuMeter::VuMeter(QWidget* parent) : QWidget(parent) {}

QSize VuMeter::sizeHint() const { return QSize(30, 150); }
QSize VuMeter::minimumSizeHint() const { return QSize(24, 60); }

void VuMeter::setLevel(float left, float right) {
    left = std::clamp(left, 0.0f, 1.0f);
    right = std::clamp(right, 0.0f, 1.0f);

    // Fast attack (jump up), slow release (decay down).
    left_ = std::max(left, left_ * kReleaseFactor);
    right_ = std::max(right, right_ * kReleaseFactor);

    peakLeft_ = std::max(left_, peakLeft_ * kPeakReleaseFactor);
    peakRight_ = std::max(right_, peakRight_ * kPeakReleaseFactor);

    update();
}

void VuMeter::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF content = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0);
    constexpr qreal kGap = 4.0;
    const qreal barWidth = (content.width() - kGap) / 2.0;

    const QRectF leftRect(content.left(), content.top(), barWidth, content.height());
    const QRectF rightRect(content.left() + barWidth + kGap, content.top(), barWidth,
                           content.height());

    drawBar(painter, leftRect, left_, peakLeft_);
    drawBar(painter, rightRect, right_, peakRight_);
}

void VuMeter::drawBar(QPainter& painter, const QRectF& area, float value,
                      float peak) const {
    constexpr qreal kRadius = 3.0;
    QPainterPath path;
    path.addRoundedRect(area, kRadius, kRadius);
    painter.fillPath(path, QColor(0x23, 0x26, 0x3a));  // track

    const qreal filled = std::clamp(value, 0.0f, 1.0f) * area.height();
    if (filled > 0.5) {
        QLinearGradient gradient(area.bottomLeft(), area.topLeft());
        gradient.setColorAt(0.00, QColor(0x9e, 0xce, 0x6a));  // green
        gradient.setColorAt(0.62, QColor(0x9e, 0xce, 0x6a));
        gradient.setColorAt(0.80, QColor(0xe0, 0xaf, 0x68));  // amber
        gradient.setColorAt(0.93, QColor(0xf7, 0x76, 0x8e));  // red
        gradient.setColorAt(1.00, QColor(0xf7, 0x76, 0x8e));

        painter.save();
        painter.setClipPath(path);
        painter.fillRect(QRectF(area.left(), area.bottom() - filled, area.width(), filled),
                         gradient);
        painter.restore();
    }

    if (peak > 0.02f) {
        const qreal y = area.bottom() - std::clamp(peak, 0.0f, 1.0f) * area.height();
        painter.save();
        painter.setClipPath(path);
        painter.fillRect(QRectF(area.left(), y - 1.0, area.width(), 2.0),
                         QColor(0xe6, 0xec, 0xff, 220));
        painter.restore();
    }
}

}  // namespace sonar::ui
