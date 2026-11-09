#pragma once

#include <QWidget>

namespace sonar::ui {

// Vertical stereo peak meter (fast attack, slow release, peak hold), linear 0..1.
// Paint only; fed by the mixer page's timer.
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
