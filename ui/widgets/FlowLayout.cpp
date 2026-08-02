#include "ui/widgets/FlowLayout.h"

#include <QWidget>

namespace sonar::ui {

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::FlowLayout(int margin, int hSpacing, int vSpacing)
    : hSpace_(hSpacing), vSpace_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::horizontalSpacing() const {
    return hSpace_ >= 0 ? hSpace_ : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const {
    return vSpace_ >= 0 ? vSpace_ : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const { return static_cast<int>(items_.size()); }

QLayoutItem* FlowLayout::itemAt(int index) const { return items_.value(index); }

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index >= 0 && index < items_.size()) {
        return items_.takeAt(index);
    }
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins m = contentsMargins();
    size += QSize(m.left() + m.right(), m.top() + m.bottom());
    return size;
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(left, top, -right, -bottom);

    int x = effective.x();
    int y = effective.y();
    int lineHeight = 0;

    for (QLayoutItem* item : items_) {
        const int spaceX = horizontalSpacing();
        const int spaceY = verticalSpacing();
        const QSize hint = item->sizeHint();
        int nextX = x + hint.width() + spaceX;
        if (nextX - spaceX > effective.right() && lineHeight > 0) {
            x = effective.x();
            y = y + lineHeight + spaceY;
            nextX = x + hint.width() + spaceX;
            lineHeight = 0;
        }
        if (!testOnly) {
            item->setGeometry(QRect(QPoint(x, y), hint));
        }
        x = nextX;
        lineHeight = qMax(lineHeight, hint.height());
    }
    return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    QObject* parentObj = parent();
    if (parentObj == nullptr) {
        return -1;
    }
    if (parentObj->isWidgetType()) {
        auto* widget = static_cast<QWidget*>(parentObj);
        return widget->style()->pixelMetric(pm, nullptr, widget);
    }
    return static_cast<QLayout*>(parentObj)->spacing();
}

}  // namespace sonar::ui
