#include "ui/UiKit.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

namespace sonar::ui {

QString channelKey(audio::ChannelId id) { return toQString(audio::channelName(id)); }

QLabel* makeLabel(const QString& text, const char* objectName, bool wordWrap) {
    auto* label = new QLabel(text);
    label->setObjectName(QLatin1String(objectName));
    label->setWordWrap(wordWrap);
    return label;
}

QLabel* makeValueLabel(const QString& text) {
    QLabel* label = makeLabel(text, "VolumeValue");
    label->setMinimumWidth(52);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QPushButton* makeButton(const QString& text) {
    auto* button = new QPushButton(text);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QProgressBar* makeLevelBar(const char* objectName) {
    auto* bar = new QProgressBar;
    bar->setObjectName(QLatin1String(objectName));
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    return bar;
}

QVBoxLayout* makeScrollPage(QWidget* owner, int spacing) {
    auto* outer = new QVBoxLayout(owner);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(owner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* page = new QWidget;
    scroll->setWidget(page);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(spacing);
    return root;
}

void addPageHeader(QBoxLayout* root, const QString& title, const QString& subtitle) {
    auto* head = new QVBoxLayout;
    head->setSpacing(4);
    head->addWidget(makeLabel(title, "PageTitle"));
    head->addWidget(makeLabel(subtitle, "PageSubtitle"));
    root->addLayout(head);
}

QVBoxLayout* makeCard(QBoxLayout* root, const QString& title, const QString& subtitle,
                      int spacing, const QString& badge) {
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 18, 22, 20);
    body->setSpacing(spacing);

    auto* headRow = new QHBoxLayout;
    headRow->addWidget(makeLabel(title, "SectionTitle"));
    headRow->addStretch(1);
    if (!badge.isEmpty()) {
        headRow->addWidget(makeLabel(badge, "SoonBadge"));
    }
    body->addLayout(headRow);

    if (!subtitle.isEmpty()) {
        body->addWidget(makeLabel(subtitle, "Hint", true));
    }
    body->addSpacing(4);
    root->addWidget(frame);
    return body;
}

void fillDeviceCombo(QComboBox* combo, const QString& defaultText,
                     const std::vector<std::pair<QString, QString>>& items,
                     const QString& selected, const QString& missingText) {
    const QSignalBlocker block(combo);
    combo->clear();
    combo->addItem(defaultText, QString());
    for (const auto& [text, name] : items) {
        combo->addItem(text, name);
    }
    if (!missingText.isEmpty() && combo->findData(selected) < 0) {
        combo->addItem(missingText, selected);
    }
    combo->setCurrentIndex(std::max(0, combo->findData(selected)));
}

void clearLayout(QLayout* layout) {
    if (layout == nullptr) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        if (QLayout* child = item->layout()) {
            clearLayout(child);
        }
        delete item;
    }
}

}  // namespace sonar::ui
