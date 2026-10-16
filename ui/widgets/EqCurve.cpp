#include "ui/widgets/EqCurve.h"

#include "ui/Theme.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QFont>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace sonar::ui {

using sonar::ui::currentTheme;

using dsp::kMaxFreq;
using dsp::kMaxGainDb;
using dsp::kMinFreq;
using dsp::kMinGainDb;

namespace {
constexpr float kLogMin = 1.30103f;   // log10(20)
constexpr float kLogMax = 4.30103f;   // log10(20000)
constexpr int kNodeRadius = 7;

// Read from the active theme rather than fixed, so a theme reaches the curve too.
// A plot is not a stylable widget — every pixel of it is painted here — so the
// colours have to be fetched rather than inherited from the style sheet.
[[nodiscard]] QColor mixed(const QColor& a, const QColor& b, double t) {
    const double u = 1.0 - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t, a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t);
}

[[nodiscard]] QColor withAlpha(QColor c, int alpha) {
    c.setAlpha(alpha);
    return c;
}

[[nodiscard]] QColor plotBg() {
    // A shade below the card it sits on, so the plot reads as recessed.
    const ThemeTokens& t = currentTheme();
    return mixed(t.surface, t.bg, 0.55);
}
[[nodiscard]] QColor plotBorder() { return currentTheme().border; }
[[nodiscard]] QColor grid() {
    const ThemeTokens& t = currentTheme();
    return mixed(plotBg(), t.border, 0.55);
}
[[nodiscard]] QColor zeroLine() {
    const ThemeTokens& t = currentTheme();
    return mixed(t.border, t.textDim, 0.28);
}
[[nodiscard]] QColor accent() { return currentTheme().accent; }
[[nodiscard]] QColor accentHi() { return currentTheme().accentHi; }
[[nodiscard]] QColor dimText() { return currentTheme().textDim; }

QString freqLabel(float f) {
    if (f >= 1000.0f) {
        const float k = f / 1000.0f;
        return std::abs(k - std::round(k)) < 0.05f
                   ? QStringLiteral("%1k").arg(static_cast<int>(std::round(k)))
                   : QStringLiteral("%1k").arg(k, 0, 'f', 1);
    }
    return QStringLiteral("%1").arg(static_cast<int>(std::round(f)));
}

QString readoutText(float freq, float gain) {
    const QString g = QStringLiteral("%1%2 dB")
                          .arg(gain >= 0 ? QStringLiteral("+") : QStringLiteral(""))
                          .arg(gain, 0, 'f', 1);
    if (freq >= 1000.0f) {
        return QStringLiteral("%1 kHz  %2").arg(freq / 1000.0f, 0, 'f', 1).arg(g);
    }
    return QStringLiteral("%1 Hz  %2").arg(static_cast<int>(std::round(freq))).arg(g);
}
}  // namespace

EqCurve::EqCurve(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    dsp::resetBands(settings_, dsp::BandCount::Bands10);
}

QSize EqCurve::sizeHint() const { return QSize(680, 300); }
QSize EqCurve::minimumSizeHint() const { return QSize(420, 200); }

void EqCurve::setSettings(const dsp::EqSettings& settings) {
    settings_ = settings;
    update();
}

QRectF EqCurve::plotRect() const {
    return QRectF(rect()).adjusted(46, 14, -16, -28);
}

float EqCurve::freqToX(float freq) const {
    const QRectF r = plotRect();
    const float t = (std::log10(freq) - kLogMin) / (kLogMax - kLogMin);
    // The rectangle is in qreal; the curve is drawn in float. The cast is
    // here so the narrowing is a decision rather than something the compiler
    // does quietly.
    return static_cast<float>(r.left() + t * r.width());
}

float EqCurve::gainToY(float gainDb) const {
    const QRectF r = plotRect();
    const float t = (gainDb - kMinGainDb) / (kMaxGainDb - kMinGainDb);
    return static_cast<float>(r.bottom() - t * r.height());
}

float EqCurve::yToGain(float y) const {
    const QRectF r = plotRect();
    const float t = static_cast<float>((r.bottom() - y) / r.height());
    return kMinGainDb + t * (kMaxGainDb - kMinGainDb);
}

int EqCurve::nearestBand(const QPointF& pos) const {
    int best = -1;
    float bestDx = 1e9f;
    for (std::size_t i = 0; i < settings_.bands.size(); ++i) {
        const float dx = std::abs(freqToX(settings_.bands[i].frequency) -
                                  static_cast<float>(pos.x()));
        if (dx < bestDx) {
            bestDx = dx;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void EqCurve::applyDrag(const QPointF& pos) {
    if (dragBand_ < 0 || dragBand_ >= static_cast<int>(settings_.bands.size())) {
        return;
    }
    const float gain = std::clamp(yToGain(static_cast<float>(pos.y())), kMinGainDb, kMaxGainDb);
    settings_.bands[dragBand_].gainDb = gain;
    emit bandChanged(dragBand_, gain);
    update();
}

void EqCurve::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    dragBand_ = nearestBand(event->position());
    applyDrag(event->position());
}

void EqCurve::mouseMoveEvent(QMouseEvent* event) {
    if (dragBand_ >= 0) {
        applyDrag(event->position());
        return;
    }
    const int band = nearestBand(event->position());
    if (band != hoverBand_) {
        hoverBand_ = band;
        update();
    }
}

void EqCurve::mouseReleaseEvent(QMouseEvent* /*event*/) { dragBand_ = -1; }

void EqCurve::leaveEvent(QEvent* /*event*/) {
    hoverBand_ = -1;
    update();
}

void EqCurve::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Disabled means the curve is not editable (Qt already withholds the mouse
    // events); fading it is what says so. Every setOpacity below multiplies by
    // this factor instead of replacing it, or the later layers — the response
    // curve and the band nodes, the two things that look most clickable — would
    // paint back at full strength.
    const qreal dim = isEnabled() ? 1.0 : 0.3;
    p.setOpacity(dim);

    const QRectF r = plotRect();

    // Plot background.
    QPainterPath bg;
    bg.addRoundedRect(r.adjusted(-8, -8, 8, 8), 12, 12);
    p.fillPath(bg, plotBg());
    p.setPen(QPen(plotBorder(), 1));
    p.drawPath(bg);

    // Horizontal dB grid + labels.
    p.setFont(QFont(font().family(), 8));
    for (int db = static_cast<int>(kMinGainDb); db <= static_cast<int>(kMaxGainDb); db += 6) {
        const float y = gainToY(static_cast<float>(db));
        p.setPen(QPen(db == 0 ? zeroLine() : grid(), db == 0 ? 1.4 : 1.0));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(dimText());
        p.drawText(QRectF(0, y - 8, r.left() - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1%2").arg(db > 0 ? QStringLiteral("+") : QStringLiteral(""))
                       .arg(db));
    }

    // Vertical frequency grid + labels.
    const std::array<float, 6> ticks = {30, 100, 300, 1000, 3000, 10000};
    for (const float f : ticks) {
        const float x = freqToX(f);
        p.setPen(QPen(grid(), 1.0));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(dimText());
        p.drawText(QRectF(x - 24, r.bottom() + 6, 48, 16), Qt::AlignHCenter | Qt::AlignTop,
                   freqLabel(f));
    }

    const bool on = settings_.enabled;
    p.setOpacity(dim * (on ? 1.0 : 0.45));

    // Response curve.
    QPainterPath curve;
    const int steps = static_cast<int>(r.width());
    for (int i = 0; i <= steps; ++i) {
        const float x = static_cast<float>(r.left()) + static_cast<float>(i);
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float freq = std::pow(10.0f, kLogMin + t * (kLogMax - kLogMin));
        const float g = std::clamp(dsp::responseDbAt(settings_, freq), kMinGainDb, kMaxGainDb);
        const float y = gainToY(g);
        if (i == 0) {
            curve.moveTo(x, y);
        } else {
            curve.lineTo(x, y);
        }
    }

    // Fill between the curve and the 0 dB line.
    QPainterPath fill = curve;
    fill.lineTo(r.right(), gainToY(0));
    fill.lineTo(r.left(), gainToY(0));
    fill.closeSubpath();
    QLinearGradient grad(0, r.top(), 0, r.bottom());
    grad.setColorAt(0.0, withAlpha(accent(), 120));
    grad.setColorAt(0.5, withAlpha(accent(), 40));
    grad.setColorAt(1.0, withAlpha(accent(), 120));
    p.save();
    p.setClipRect(r);
    p.fillPath(fill, grad);
    p.restore();

    p.setPen(QPen(accent(), 2.4));
    p.drawPath(curve);

    // Band nodes.
    const int active = dragBand_ >= 0 ? dragBand_ : hoverBand_;
    for (std::size_t i = 0; i < settings_.bands.size(); ++i) {
        const dsp::EqBand& band = settings_.bands[i];
        const QPointF c(freqToX(band.frequency), gainToY(band.gainDb));
        const bool hot = static_cast<int>(i) == active;
        if (hot) {
            p.setBrush(withAlpha(accent(), 60));
            p.setPen(Qt::NoPen);
            p.drawEllipse(c, kNodeRadius + 6, kNodeRadius + 6);
        }
        p.setBrush(hot ? accentHi() : accent());
        p.setPen(QPen(currentTheme().text.lighter(140), hot ? 2.2 : 1.6));
        p.drawEllipse(c, kNodeRadius, kNodeRadius);
    }

    p.setOpacity(dim);

    // Readout pill for the active band.
    if (active >= 0 && active < static_cast<int>(settings_.bands.size())) {
        const dsp::EqBand& band = settings_.bands[active];
        const QString text = readoutText(band.frequency, band.gainDb);
        QFont f = font();
        f.setPointSize(9);
        f.setBold(true);
        p.setFont(f);
        const QRectF tb = p.fontMetrics().boundingRect(text).adjusted(-10, -5, 10, 5);
        QPointF c(freqToX(band.frequency), gainToY(band.gainDb));
        QRectF pill(0, 0, tb.width(), tb.height());
        pill.moveCenter(QPointF(c.x(), c.y() - kNodeRadius - 16));
        pill.moveLeft(std::clamp(pill.left(), r.left(), r.right() - pill.width()));
        QPainterPath pp;
        pp.addRoundedRect(pill, 8, 8);
        p.fillPath(pp, currentTheme().hover);
        p.setPen(currentTheme().text.lighter(140));
        p.drawText(pill, Qt::AlignCenter, text);
    }
}

}  // namespace sonar::ui
