#include "ui/TrayMixer.h"

#include <cmath>

#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QScreen>
#include <QSlider>
#include <QVBoxLayout>

#include "audio/IMixer.h"

namespace sonar::ui {

using audio::ChannelId;

namespace {
QString toQString(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

int toPercent(float volume) { return static_cast<int>(std::lround(volume * 100.0F)); }

// The channel whose fader scales all the others, as MixerPage defines it.
constexpr ChannelId kMasterChannel = ChannelId::System;
}  // namespace

TrayMixer::TrayMixer(audio::IMixer& mixer, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
      mixer_(mixer) {
    // A title bar the compositor draws, so there is always a close button — and
    // no WindowStaysOnTopHint, so a window left open can never sit over the whole
    // desktop. See the header for what the frameless version cost.
    setWindowTitle(QStringLiteral("Sonero — Volume"));

    // Qt quits when the last window with this attribute closes. Opened from the
    // tray while the main window is hidden, this is the only visible window —
    // so without this, closing the volume window shut the whole app down.
    setAttribute(Qt::WA_QuitOnClose, false);

    // The window itself is the surface. The first version put a bordered Card
    // inside it, which drew a second frame a pixel inside the compositor's own —
    // two nested boxes around six sliders.
    setObjectName(QStringLiteral("TrayMixer"));
    setStyleSheet(QStringLiteral(
        "#TrayMixer { background:#161822; }"
        "#TrayMixer QLabel { background:transparent; }"
        "#TrayMixerName { color:#c2c7de; font-size:13px; font-weight:600; }"
        "#TrayMixerValue { color:#8b90a8; font-size:12px; font-weight:700; }"
        "#TrayMixerTag { color:#8b8ff9; font-size:9px; font-weight:800;"
        " letter-spacing:1px; }"
        "#TrayMixerRule { background:#2c3042; max-height:1px; min-height:1px; }"));

    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(22, 20, 22, 20);
    body->setSpacing(14);
    // Six fixed rows: let the window be exactly its contents rather than
    // something the user can stretch into a strip of floating sliders.
    body->setSizeConstraint(QLayout::SetFixedSize);

    for (const ChannelId id : mixer_.channels()) {
        if (id == ChannelId::Microphone) {
            continue;  // an input, not part of the output mix (as on the Mixer page)
        }
        const int percent = toPercent(mixer_.state(id).volume);

        auto* row = new QHBoxLayout;
        row->setSpacing(14);

        // The name sits over the fader, not beside it: a left column wide enough
        // for "Microphone" would push the sliders into a narrow strip.
        auto* name = new QLabel(toQString(audio::channelName(id)), this);
        name->setObjectName(QStringLiteral("TrayMixerName"));

        auto* value = new QLabel(QStringLiteral("%1%").arg(percent), this);
        value->setObjectName(QStringLiteral("TrayMixerValue"));
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto* head = new QHBoxLayout;
        head->setSpacing(8);
        head->addWidget(name);
        if (id == kMasterChannel) {
            // Moving this one scales every other channel; without a mark the
            // window gives no hint why the whole mix followed.
            auto* tag = new QLabel(QStringLiteral("MASTER"), this);
            tag->setObjectName(QStringLiteral("TrayMixerTag"));
            head->addWidget(tag);
        }
        head->addStretch(1);
        head->addWidget(value);

        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(0, 100);
        slider->setValue(percent);
        slider->setMinimumWidth(232);

        auto* column = new QVBoxLayout;
        column->setSpacing(6);
        column->addLayout(head);
        column->addWidget(slider);
        row->addLayout(column);
        body->addLayout(row);

        if (id == kMasterChannel) {
            auto* rule = new QFrame(this);
            rule->setObjectName(QStringLiteral("TrayMixerRule"));
            rule->setFrameShape(QFrame::NoFrame);
            body->addWidget(rule);
        }

        connect(slider, &QSlider::valueChanged, this, [this, id, value](int v) {
            value->setText(QStringLiteral("%1%").arg(v));
            emit volumeChanged(id, static_cast<float>(v) / 100.0F);
        });

        rows_[static_cast<int>(id)] = Row{slider, value};
    }
}

void TrayMixer::showVolume(ChannelId id, float volume) {
    const auto it = rows_.find(static_cast<int>(id));
    if (it == rows_.end()) {
        return;
    }
    const int percent = toPercent(volume);
    const QSignalBlocker block(it->second.slider);  // model -> UI, not user intent
    it->second.slider->setValue(percent);
    it->second.value->setText(QStringLiteral("%1%").arg(percent));
}

void TrayMixer::popUp() {
    // The window may have been left holding a stale value while it was hidden.
    for (const ChannelId id : mixer_.channels()) {
        showVolume(id, mixer_.state(id).volume);
    }

    if (isVisible()) {
        raise();  // already open: bring it forward, do not yank it to the pointer
        activateWindow();
        return;
    }

    adjustSize();
    const QPoint cursor = QCursor::pos();
    const QScreen* screen = QGuiApplication::screenAt(cursor);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        // Below and left of the pointer — where a tray icon's menu would drop —
        // kept inside the screen. Wayland ignores the position and places the
        // window itself; nothing else here depends on it landing exactly.
        const QRect area = screen->availableGeometry();
        const QSize own = sizeHint();
        QPoint at(cursor.x() - own.width() / 2, cursor.y() + 12);
        at.setX(std::min(std::max(at.x(), area.left() + 8), area.right() - own.width() - 8));
        at.setY(std::min(std::max(at.y(), area.top() + 8), area.bottom() - own.height() - 8));
        move(at);
    }

    show();
    raise();
    activateWindow();
}

void TrayMixer::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace sonar::ui
