#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QSize>
#include <QStyle>

namespace sonar::ui {

// A layout that arranges its items left-to-right and wraps to the next line when
// it runs out of horizontal space (adapted from the classic Qt FlowLayout
// example). Used for the draggable application chips.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent, int margin = 0, int hSpacing = 8, int vSpacing = 8);
    explicit FlowLayout(int margin = 0, int hSpacing = 8, int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int horizontalSpacing() const;
    [[nodiscard]] int verticalSpacing() const;
    [[nodiscard]] Qt::Orientations expandingDirections() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] int count() const override;
    [[nodiscard]] QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem*> items_;
    int hSpace_;
    int vSpace_;
};

}  // namespace sonar::ui
