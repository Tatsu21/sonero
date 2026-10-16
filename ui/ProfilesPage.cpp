#include "ui/ProfilesPage.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QStyle>
#include <QVBoxLayout>

#include "config/SettingsStore.h"
#include "core/Log.h"

namespace sonar::ui {

namespace {

constexpr const char* kSection = "appearance";
constexpr const char* kThemeKey = "theme";  // a path, or empty for the built-in one

QLabel* caption(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("CardKey"));
    label->setMinimumWidth(96);
    return label;
}

}  // namespace

void ProfilesPage::applySavedTheme(config::SettingsStore* settings) {
    if (settings == nullptr || qApp == nullptr) {
        return;
    }
    const QString path = settings->section(QString::fromLatin1(kSection))
                             .value(QString::fromLatin1(kThemeKey))
                             .toString();
    if (path.isEmpty()) {
        return;  // the built-in theme, which main() has already applied
    }
    ThemeTokens theme;
    QString error;
    if (readTheme(path, theme, &error)) {
        applyTheme(*qApp, theme);
        log::info("Theme: '{}' from {}", theme.name.toStdString(), path.toStdString());
        return;
    }
    // A theme file deleted or broken since it was chosen leaves the built-in one
    // in place rather than failing the launch. Logged rather than shown: this
    // runs before there is a window to put a message in, and the Profiles page
    // reports which theme is actually active.
    log::warn("Theme: '{}' {} — using the built-in theme", path.toStdString(),
              error.toStdString());
}

ProfilesPage::ProfilesPage(config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(16);

    auto* title = new QLabel(QStringLiteral("Profiles"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    auto* subtitle = new QLabel(
        QStringLiteral("How Sonero looks. Audio settings live on the other pages."), this);
    subtitle->setObjectName(QStringLiteral("PageSubtitle"));
    root->addWidget(title);
    root->addWidget(subtitle);

    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("Card"));
    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(22, 18, 22, 20);
    body->setSpacing(10);

    auto* sectionTitle = new QLabel(QStringLiteral("Theme"), this);
    sectionTitle->setObjectName(QStringLiteral("SectionTitle"));
    body->addWidget(sectionTitle);

    auto* hint = new QLabel(
        QStringLiteral(
            "A theme is a small JSON file: thirteen colours, a font, and an optional "
            "block of Qt style sheet for anything colours cannot express. The shades "
            "in between are derived from the thirteen, so you do not have to list "
            "them. The built-in theme is compiled into Sonero and is always the base "
            "a theme is applied over — it cannot be broken or removed, so there is "
            "always a way back."),
        this);
    hint->setObjectName(QStringLiteral("Hint"));
    hint->setWordWrap(true);
    body->addWidget(hint);
    body->addSpacing(4);

    themeCombo_ = new QComboBox(this);
    themeCombo_->setMinimumWidth(240);
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(caption(QStringLiteral("Active"), this));
    row->addWidget(themeCombo_, 1);
    body->addLayout(row);

    detail_ = new QLabel(this);
    detail_->setObjectName(QStringLiteral("Hint"));
    detail_->setWordWrap(true);
    body->addWidget(detail_);
    body->addSpacing(6);

    auto* importBtn = new QPushButton(QStringLiteral("Import…"), this);
    importBtn->setToolTip(QStringLiteral(
        "Add a theme. Takes a .json theme, or a bare .qss style sheet — a style "
        "sheet becomes a theme with the built-in colours and your sheet on top."));
    auto* exportBtn = new QPushButton(QStringLiteral("Export theme…"), this);
    exportBtn->setToolTip(QStringLiteral(
        "Write the active theme out as JSON — the starting point for editing one."));
    auto* exportQssBtn = new QPushButton(QStringLiteral("Export stylesheet…"), this);
    exportQssBtn->setToolTip(QStringLiteral(
        "Write out the whole style sheet as Sonero is currently using it, with every "
        "colour resolved. This is what your theme's `qss` block overrides, so it is "
        "the reference for writing one."));
    auto* folderBtn = new QPushButton(QStringLiteral("Open folder"), this);
    folderBtn->setToolTip(QStringLiteral("The directory themes are read from."));
    for (QPushButton* b : {importBtn, exportBtn, exportQssBtn, folderBtn}) {
        b->setCursor(Qt::PointingHandCursor);
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(importBtn);
    buttons->addWidget(exportBtn);
    buttons->addWidget(exportQssBtn);
    buttons->addStretch(1);
    buttons->addWidget(folderBtn);
    body->addLayout(buttons);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("Hint"));
    status_->setWordWrap(true);
    body->addWidget(status_);

    root->addWidget(frame);
    root->addStretch(1);

    connect(themeCombo_, &QComboBox::activated, this, [this](int) { applySelected(); });
    connect(importBtn, &QPushButton::clicked, this, &ProfilesPage::onImport);
    connect(exportBtn, &QPushButton::clicked, this, &ProfilesPage::onExportTheme);
    connect(exportQssBtn, &QPushButton::clicked, this, &ProfilesPage::onExportStyleSheet);
    connect(folderBtn, &QPushButton::clicked, this, &ProfilesPage::onOpenFolder);

    reloadThemes(currentTheme().filePath);
}

void ProfilesPage::reloadThemes(const QString& selectPath) {
    themes_ = availableThemes();
    const QSignalBlocker block(themeCombo_);
    themeCombo_->clear();
    int select = 0;
    for (int i = 0; i < themes_.size(); ++i) {
        const ThemeTokens& t = themes_.at(i);
        themeCombo_->addItem(t.isBuiltIn()
                                 ? QStringLiteral("%1 (built-in)").arg(t.name)
                                 : t.name,
                             t.filePath);
        if (!selectPath.isEmpty() && t.filePath == selectPath) {
            select = i;
        }
    }
    themeCombo_->setCurrentIndex(select);

    const ThemeTokens& active = themes_.at(select);
    if (active.isBuiltIn()) {
        detail_->setText(QStringLiteral("Compiled into Sonero. %1 themes found in %2.")
                             .arg(themes_.size() - 1)
                             .arg(themesDirectory()));
    } else {
        detail_->setText(
            QStringLiteral("%1%2")
                .arg(active.filePath)
                .arg(active.extraQss.isEmpty()
                         ? QString()
                         : QStringLiteral("  ·  carries a custom style sheet")));
    }
}

ThemeTokens ProfilesPage::selectedTheme() const {
    const int i = themeCombo_->currentIndex();
    if (i < 0 || i >= themes_.size()) {
        return builtInTheme();
    }
    return themes_.at(i);
}

void ProfilesPage::applySelected() {
    const ThemeTokens theme = selectedTheme();
    if (qApp != nullptr) {
        applyTheme(*qApp, theme);
        // The style sheet change repolishes every styled widget, but the
        // custom-painted ones (the curve, the meters, the strips' accents) read
        // currentTheme() when they paint and have no way to know it moved.
        refreshTheme(*qApp);
    }
    if (settings_ != nullptr) {
        QJsonObject section = settings_->section(QString::fromLatin1(kSection));
        section[QString::fromLatin1(kThemeKey)] = theme.filePath;  // empty = built-in
        settings_->putSection(QString::fromLatin1(kSection), section);
    }
    reloadThemes(theme.filePath);
    setStatus(QStringLiteral("Applied “%1”.").arg(theme.name));
}

void ProfilesPage::onImport() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import a theme"), QDir::homePath(),
        QStringLiteral("Themes and style sheets (*.json *.qss);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    QString landed;
    if (QFileInfo(path).suffix().compare(QLatin1String("qss"), Qt::CaseInsensitive) == 0) {
        // A bare style sheet: wrap it in a theme that keeps the built-in colours
        // and layers the sheet on top. That is the whole point of the sheet being
        // additive — a user can restyle one widget without redescribing the rest.
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            setStatus(QStringLiteral("Could not read %1: %2").arg(path, file.errorString()),
                      true);
            return;
        }
        ThemeTokens t = builtInTheme();
        t.name = QFileInfo(path).completeBaseName();
        t.extraQss = QString::fromUtf8(file.readAll());
        QDir().mkpath(themesDirectory());
        landed = themesDirectory() + QLatin1Char('/') + t.name + QStringLiteral(".json");
        for (int n = 2; QFile::exists(landed); ++n) {
            landed = themesDirectory() + QLatin1Char('/') +
                     QStringLiteral("%1 (%2).json").arg(t.name).arg(n);
        }
        if (!writeTheme(t, landed, &error)) {
            setStatus(QStringLiteral("Could not save the theme: %1").arg(error), true);
            return;
        }
    } else {
        landed = importTheme(path, &error);
        if (landed.isEmpty()) {
            setStatus(QStringLiteral("%1 %2").arg(QFileInfo(path).fileName(), error), true);
            return;
        }
    }

    reloadThemes(landed);
    setStatus(QStringLiteral("Imported into %1. Select it above to apply it.")
                  .arg(themesDirectory()));
}

void ProfilesPage::onExportTheme() {
    const ThemeTokens theme = selectedTheme();
    const QString suggested = QDir::homePath() + QLatin1Char('/') +
                              theme.name.simplified().replace(QLatin1Char(' '),
                                                              QLatin1Char('-')) +
                              QStringLiteral(".json");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export theme"), suggested, QStringLiteral("Theme (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!writeTheme(theme, path, &error)) {
        setStatus(QStringLiteral("Could not write %1: %2").arg(path, error), true);
        return;
    }
    setStatus(QStringLiteral("Wrote %1. Edit the colours and import it back.").arg(path));
}

void ProfilesPage::onExportStyleSheet() {
    const QString suggested = QDir::homePath() + QStringLiteral("/sonero.qss");
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("Export stylesheet"),
                                                      suggested,
                                                      QStringLiteral("Style sheet (*.qss)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("Could not write %1: %2").arg(path, file.errorString()),
                  true);
        return;
    }
    // The resolved sheet, not the template: a user editing this wants to see the
    // colours they are actually looking at, not "@ACCENT@".
    file.write(resolvedStyleSheet(selectedTheme()).toUtf8());
    setStatus(QStringLiteral("Wrote %1. Import it back to use it as a theme.").arg(path));
}

void ProfilesPage::onOpenFolder() {
    const QString dir = themesDirectory();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void ProfilesPage::setStatus(const QString& text, bool bad) {
    status_->setText(text);
    status_->setObjectName(bad ? QStringLiteral("StatusValueBad") : QStringLiteral("Hint"));
    // The object name is what the style sheet selects on, and Qt only re-reads it
    // on a repolish.
    status_->style()->unpolish(status_);
    status_->style()->polish(status_);
}

}  // namespace sonar::ui
