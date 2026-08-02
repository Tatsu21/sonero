#pragma once

#include <QWidget>

namespace sonar::ui {

// A vertical stereo peak meter with fast attack, slow release and a peak-hold
// marker. Levels are linear in the range 0.0 .. 1.0. The widget only paints;
// it is fed by whoever owns it (the mixer page timer).
class VuMeter : public QWidget {
    Q_OBJECT

public:
    explicit VuMeter(QWidget* parent = nullptr);

    // Push a new stereo reading; triggers smoothing and a repaint.
    void setLevel(float left, float right);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void drawBar(QPainter& painter, const QRectF& area, float value, float peak) const;

    float left_ = 0.0f;
    float right_ = 0.0f;
    float peakLeft_ = 0.0f;
    float peakRight_ = 0.0f;
};

}  // namespace sonar::ui
