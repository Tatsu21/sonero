#include "ui/Theme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace sonar::ui {

namespace {

// Refined dark palette.
//   bg        #0d0e14   window
//   sidebar   #111219
//   surface   #191b26   cards
//   input     #1f2230
//   hover     #262a3a
//   border    #2a2e3f
//   text      #e7e9f2
//   text-dim  #8b90a8
//   accent    #6366f1   (indigo)  hi #8b8ff9
//   danger    #ff5c7a   (mute)
//   warn      #ffb454   (solo)
//   ok        #4ade80   (connected)
const char* const kStyleSheet = R"QSS(
* {
    font-family: "Inter", "Segoe UI", "Roboto", "Cantarell", "Noto Sans", sans-serif;
    outline: 0;
}

QWidget { background: #0d0e14; color: #e7e9f2; font-size: 13px; }

/* Text/controls must be transparent so cards show through (no boxed text). */
QLabel, QCheckBox, QRadioButton, QGroupBox { background: transparent; }
QToolTip {
    background: #191b26; color: #e7e9f2; border: 1px solid #2a2e3f;
    border-radius: 8px; padding: 6px 9px;
}

/* ---- Top bar ---- */
/* No padding: the wordmark is centred by the layout, and any asymmetric padding
   would push it off the mark's and the nav pills' shared centre line. */
#AppLogo { font-size: 18px; font-weight: 800; padding: 0; }

/* ---- Page content ---- */
#PageTitle { font-size: 25px; font-weight: 800; color: #ffffff; }
#PageSubtitle { color: #8b90a8; font-size: 13px; }
#SectionTitle { font-size: 15px; font-weight: 700; color: #eef0f8; }
#Hint { color: #6b7091; }
#Sep { background: #1e2130; max-height: 1px; min-height: 1px; border: none; }

/* ---- Cards ---- */
#Card, #ChannelStrip {
    background: #191b26; border: 1px solid #262a38; border-radius: 16px;
}
#Card:hover { border-color: #323750; }
#ChannelStrip[dropActive="true"] {
    background: #1b2036; border: 1px solid #6366f1;
}
#StatusValueOk { color: #4ade80; font-weight: 800; }
#StatusValueBad { color: #ff5c7a; font-weight: 800; }
#CardKey { color: #8b90a8; }
#CardVal { color: #e7e9f2; font-weight: 600; }

/* ---- Channel strip ---- */
#ChannelName { font-weight: 700; color: #ffffff; font-size: 13px; }
#VolumeValue { color: #aeb4d6; font-size: 13px; font-weight: 700; }
#BalanceCaption { color: #565b74; font-size: 10px; font-weight: 700; letter-spacing: 1px; }
#AppsLabel { color: #8b8ff9; font-size: 10px; font-weight: 600; }

/* ---- Application chips (draggable) ---- */
#AppChip {
    background: #1f2230; border: 1px solid #2e3346; border-radius: 17px;
}
#AppChip:hover { background: #262a3a; border-color: #3d435c; }
#ChipName { color: #e7e9f2; font-weight: 600; }
#ChipBadge { color: #8b8ff9; font-weight: 700; }
#ChipDot { color: #565b74; }

/* ---- Microphone page ---- */
#SoonBadge {
    background: #23263a; color: #8b8ff9; font-size: 9px; font-weight: 800;
    border-radius: 6px; padding: 3px 8px;
}
#MicLevel {
    background: #14151d; border: 1px solid #262a38; border-radius: 6px;
    min-height: 14px; max-height: 14px;
}
#MicLevel::chunk {
    border-radius: 5px; margin: 0px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #4ade80, stop:0.7 #4ade80, stop:0.85 #ffb454, stop:1 #ff5c7a);
}
#BatteryBar {
    background: #14151d; border: 1px solid #262a38; border-radius: 6px;
    min-height: 14px; max-height: 14px;
}
#BatteryBar::chunk {
    border-radius: 5px; margin: 0px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4ade80, stop:1 #7ee89a);
}

/* ---- Segmented control ---- */
#Segmented { background: #14151d; border: 1px solid #262a38; border-radius: 11px; }
#Segmented QPushButton {
    background: transparent; border: none; border-radius: 8px;
    padding: 7px 14px; color: #8b90a8; font-weight: 600;
}
#Segmented QPushButton:hover { color: #e7e9f2; }
#Segmented QPushButton:checked { background: #6366f1; color: #ffffff; }

/* ---- Buttons ---- */
QPushButton {
    background: #1f2230; color: #e7e9f2; border: 1px solid #2e3346;
    border-radius: 10px; padding: 8px 14px; font-weight: 600;
}
QPushButton:hover { background: #262a3a; border-color: #3d435c; }
QPushButton:pressed { background: #1a1d29; }
QPushButton#Accent { background: #6366f1; color: #ffffff; border: none; }
QPushButton#Accent:hover { background: #7c7ff5; }

QPushButton[toggleRole="mute"], QPushButton[toggleRole="solo"] {
    padding: 8px 4px; font-size: 12px;
}
QPushButton[toggleRole="mute"]:checked {
    background: #ff5c7a; color: #14060b; border-color: #ff5c7a; font-weight: 800;
}
QPushButton[toggleRole="solo"]:checked {
    background: #ffb454; color: #1a1204; border-color: #ffb454; font-weight: 800;
}

/* ---- Sliders ---- */
QSlider::groove:horizontal { height: 6px; background: #2a2e3f; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #6366f1; border-radius: 3px; }
QSlider::handle:horizontal {
    background: #ffffff; width: 15px; height: 15px; margin: -6px 0;
    border-radius: 8px; border: 2px solid #6366f1;
}
QSlider::handle:horizontal:hover { border-color: #8b8ff9; }

QSlider::groove:vertical { width: 7px; background: #2a2e3f; border-radius: 4px; }
QSlider::add-page:vertical { background: #6366f1; border-radius: 4px; }
QSlider::sub-page:vertical { background: #2a2e3f; border-radius: 4px; }
QSlider::handle:vertical {
    background: #ffffff; height: 17px; width: 17px; margin: 0 -6px;
    border-radius: 9px; border: 2px solid #6366f1;
}
QSlider::handle:vertical:hover { border-color: #8b8ff9; }

/* ---- Combo box ---- */
QComboBox {
    background: #1f2230; color: #e7e9f2; border: 1px solid #2e3346;
    border-radius: 10px; padding: 7px 12px; min-height: 18px;
}
QComboBox:hover { border-color: #6366f1; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow {
    image: none; width: 0; height: 0;
    border-left: 4px solid transparent; border-right: 4px solid transparent;
    border-top: 5px solid #8b90a8; margin-right: 10px;
}
QComboBox QAbstractItemView {
    background: #191b26; border: 1px solid #2a2e3f; border-radius: 10px;
    selection-background-color: #262a3a; color: #e7e9f2; padding: 6px;
}
QComboBox QAbstractItemView::item { padding: 7px 10px; border-radius: 7px; min-height: 20px; }

/* ---- Check box ---- */
QCheckBox { color: #e7e9f2; spacing: 8px; }
QCheckBox::indicator {
    width: 18px; height: 18px; border-radius: 6px;
    border: 1px solid #2e3346; background: #1f2230;
}
QCheckBox::indicator:hover { border-color: #6366f1; }
QCheckBox::indicator:checked { background: #6366f1; border-color: #6366f1; }

/* ---- Disabled: present in the UI, not yet wired to anything ---- */
/* Qt's disabled palette never reaches these widgets — the rules above set their
   colours outright — so every inert control needs its own grey here. One grey
   for all of them, so "you cannot touch this" reads the same on a button, a
   slider and a checkbox. */
QPushButton:disabled { background: #171922; color: #4a4e63; border-color: #23262f; }
QSlider::groove:horizontal:disabled,
QSlider::groove:vertical:disabled,
QSlider::sub-page:horizontal:disabled,
QSlider::sub-page:vertical:disabled,
QSlider::add-page:vertical:disabled { background: #23262f; }
QSlider::handle:horizontal:disabled,
QSlider::handle:vertical:disabled { background: #3a3e4e; border-color: #23262f; }
QComboBox:disabled { background: #171922; color: #4a4e63; border-color: #23262f; }
QComboBox::down-arrow:disabled { border-top: 5px solid #4a4e63; }
QCheckBox:disabled { color: #4a4e63; }
QCheckBox::indicator:disabled { background: #171922; border-color: #23262f; }
#MicLevel::chunk:disabled { background: #2f333f; }
/* An id selector outranks QLabel:disabled, so every named label needs its own
   disabled colour or it stays bright next to the control it belongs to. */
QLabel:disabled { color: #4a4e63; }
#CardKey:disabled, #CardVal:disabled, #VolumeValue:disabled,
#BalanceCaption:disabled, #ChannelName:disabled { color: #4a4e63; }

/* ---- Scroll bars ---- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #2a2e3f; border-radius: 5px; min-height: 32px; }
QScrollBar::handle:vertical:hover { background: #3a3f57; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #2a2e3f; border-radius: 5px; min-width: 32px; }
QScrollBar::handle:horizontal:hover { background: #3a3f57; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)QSS";

}  // namespace

void applyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, QColor(0x0d, 0x0e, 0x14));
    p.setColor(QPalette::WindowText, QColor(0xe7, 0xe9, 0xf2));
    p.setColor(QPalette::Base, QColor(0x19, 0x1b, 0x26));
    p.setColor(QPalette::AlternateBase, QColor(0x1f, 0x22, 0x30));
    p.setColor(QPalette::Text, QColor(0xe7, 0xe9, 0xf2));
    p.setColor(QPalette::Button, QColor(0x1f, 0x22, 0x30));
    p.setColor(QPalette::ButtonText, QColor(0xe7, 0xe9, 0xf2));
    p.setColor(QPalette::BrightText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Highlight, QColor(0x63, 0x66, 0xf1));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::ToolTipBase, QColor(0x19, 0x1b, 0x26));
    p.setColor(QPalette::ToolTipText, QColor(0xe7, 0xe9, 0xf2));
    p.setColor(QPalette::PlaceholderText, QColor(0x8b, 0x90, 0xa8));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x56, 0x5b, 0x74));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x56, 0x5b, 0x74));
    app.setPalette(p);

    app.setStyleSheet(QString::fromUtf8(kStyleSheet));
}

}  // namespace sonar::ui
