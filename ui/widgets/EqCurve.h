#pragma once

#include <QRectF>
#include <QWidget>

#include "dsp/Equalizer.h"

namespace sonar::ui {

// Interactive equalizer curve: draws a log-frequency response and lets each band
// be dragged vertically to set its gain. Emits bandChanged() while dragging.
class EqCurve : public QWidget {
    Q_OBJECT

public:
    explicit EqCurve(QWidget* parent = nullptr);

    void setSettings(const dsp::EqSettings& settings);
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void bandChanged(int index, float gainDb);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] float freqToX(float freq) const;
    [[nodiscard]] float gainToY(float gainDb) const;
    [[nodiscard]] float yToGain(float y) const;
    [[nodiscard]] int nearestBand(const QPointF& pos) const;
    void applyDrag(const QPointF& pos);

    dsp::EqSettings settings_;
    int dragBand_ = -1;
    int hoverBand_ = -1;
};

}  // namespace sonar::ui
