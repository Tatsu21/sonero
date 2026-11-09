#include "ui/ProfilesPage.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#include "ui/UiKit.h"

namespace sonar::ui {

namespace {

constexpr const char* kSection = "appearance";
constexpr const char* kThemeKey = "theme";  // a path, or empty for the built-in one

QPushButton* button(const QString& text, const QString& tooltip) {
    QPushButton* b = makeButton(text);
    b->setToolTip(tooltip);
    return b;
}

}  // namespace

void ProfilesPage::applySavedTheme(config::SettingsStore* settings) {
    if (settings == nullptr || qApp == nullptr) return;
    const QString path =
        settings->value(QString::fromLatin1(kSection), QString::fromLatin1(kThemeKey)).toString();
    if (path.isEmpty()) return;  // the built-in theme, which main() has already applied
    ThemeTokens theme;
    QString error;
    if (readTheme(path, theme, &error)) {
        applyTheme(*qApp, theme);
        log::info("Theme: '{}' from {}", theme.name.toStdString(), path.toStdString());
        return;
    }
    // A theme file broken or deleted since it was chosen leaves the built-in theme;
    // logged, since there is no window yet.
    log::warn("Theme: '{}' {} — using the built-in theme", path.toStdString(),
              error.toStdString());
}

ProfilesPage::ProfilesPage(config::SettingsStore* settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 34, 40, 34);
    root->setSpacing(16);

    addPageHeader(root, QStringLiteral("Profiles"),
                  QStringLiteral("How Sonero looks. Audio settings live on the other pages."));

    QVBoxLayout* body = makeCard(
        root, QStringLiteral("Theme"),
        QStringLiteral("A theme is a small JSON file: thirteen colours, a font, and an optional "
                       "block of Qt style sheet for anything colours cannot express. The shades "
                       "in between are derived from the thirteen, so you do not have to list "
                       "them. The built-in theme is compiled into Sonero and is always the base "
                       "a theme is applied over — it cannot be broken or removed, so there is "
                       "always a way back."),
        10);

    themeCombo_ = new QComboBox(this);
    themeCombo_->setMinimumWidth(240);
    QLabel* active = makeLabel(QStringLiteral("Active"), "CardKey");
    active->setMinimumWidth(96);
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(active);
    row->addWidget(themeCombo_, 1);
    body->addLayout(row);

    detail_ = makeLabel(QString(), "Hint", true);
    body->addWidget(detail_);
    body->addSpacing(6);

    QPushButton* importBtn = button(
        QStringLiteral("Import…"),
        QStringLiteral("Add a theme. Takes a .json theme, or a bare .qss style sheet — a style "
                       "sheet becomes a theme with the built-in colours and your sheet on top."));
    QPushButton* exportBtn = button(
        QStringLiteral("Export theme…"),
        QStringLiteral("Write the active theme out as JSON — the starting point for editing "
                       "one."));
    QPushButton* exportQssBtn = button(
        QStringLiteral("Export stylesheet…"),
        QStringLiteral("Write out the whole style sheet as Sonero is currently using it, with "
                       "every colour resolved. This is what your theme's `qss` block overrides, "
                       "so it is the reference for writing one."));
    QPushButton* folderBtn = button(QStringLiteral("Open folder"),
                                    QStringLiteral("The directory themes are read from."));

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(importBtn);
    buttons->addWidget(exportBtn);
    buttons->addWidget(exportQssBtn);
    buttons->addStretch(1);
    buttons->addWidget(folderBtn);
    body->addLayout(buttons);

    status_ = makeLabel(QString(), "Hint", true);
    body->addWidget(status_);
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
    return i < 0 || i >= themes_.size() ? builtInTheme() : themes_.at(i);
}

void ProfilesPage::applySelected() {
    const ThemeTokens theme = selectedTheme();
    if (qApp != nullptr) {
        applyTheme(*qApp, theme);
        // Custom-painted widgets read currentTheme() when painting; tell them it moved.
        refreshTheme(*qApp);
    }
    if (settings_ != nullptr) {  // an empty path means the built-in theme
        settings_->putValue(QString::fromLatin1(kSection), QString::fromLatin1(kThemeKey),
                            theme.filePath);
    }
    reloadThemes(theme.filePath);
    setStatus(QStringLiteral("Applied “%1”.").arg(theme.name));
}

void ProfilesPage::onImport() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import a theme"), QDir::homePath(),
        QStringLiteral("Themes and style sheets (*.json *.qss);;All files (*)"));
    if (path.isEmpty()) return;

    QString error;
    QString landed;
    if (QFileInfo(path).suffix().compare(QLatin1String("qss"), Qt::CaseInsensitive) == 0) {
        // A bare style sheet becomes a theme: the built-in colours with the sheet on top.
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            setStatus(QStringLiteral("Could not read %1: %2").arg(path, file.errorString()),
                      true);
            return;
        }
        ThemeTokens t = builtInTheme();
        t.name = QFileInfo(path).completeBaseName();
        t.extraQss = QString::fromUtf8(file.readAll());
        landed = newThemePath(t.name);
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
    if (path.isEmpty()) return;
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
    if (path.isEmpty()) return;
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
