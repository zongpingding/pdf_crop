// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_view.h"
#include "pdf_backend.h"
#include "raster_backend.h"

#include <poppler-qt6.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTimer>
#include <QTranslator>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <system_error>

class MainWindow final : public QMainWindow {
public:
    Q_DECLARE_TR_FUNCTIONS(MainWindow)
public:
    MainWindow() {
        resize(1100, 820);

        fileMenu_ = menuBar()->addMenu(QString());
        historyMenu_ = menuBar()->addMenu(QString());
        historyMenu_->setObjectName(QStringLiteral("historyMenu"));
        themeMenu_ = menuBar()->addMenu(QString());
        languageMenu_ = menuBar()->addMenu(QString());

        openAction_ = new QAction(this);
        openAction_->setObjectName(QStringLiteral("openAction"));
        openAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
        addAction(openAction_);
        fileMenu_->addAction(openAction_);
        connect(openAction_, &QAction::triggered, this, [this] { openFiles(); });

        closeAction_ = new QAction(this);
        closeAction_->setObjectName(QStringLiteral("closeTabAction"));
        closeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
        addAction(closeAction_);
        fileMenu_->addAction(closeAction_);
        connect(closeAction_, &QAction::triggered, this, [this] { closeCurrentTab(); });

        recentFiles_ = QSettings().value(QStringLiteral("history/files")).toStringList();
        recentFiles_.removeDuplicates();
        while (recentFiles_.size() > 10)
            recentFiles_.removeLast();

        auto *themeGroup = new QActionGroup(this);
        lightTheme_ = themeMenu_->addAction(QString());
        darkTheme_ = themeMenu_->addAction(QString());
        lightTheme_->setObjectName(QStringLiteral("lightTheme"));
        darkTheme_->setObjectName(QStringLiteral("darkTheme"));
        lightTheme_->setCheckable(true);
        darkTheme_->setCheckable(true);
        themeGroup->addAction(lightTheme_);
        themeGroup->addAction(darkTheme_);
        connect(lightTheme_, &QAction::triggered, this, [this] { applyTheme(false); });
        connect(darkTheme_, &QAction::triggered, this, [this] { applyTheme(true); });

        auto *languageGroup = new QActionGroup(this);
        englishAction_ = languageMenu_->addAction(QString());
        chineseAction_ = languageMenu_->addAction(QString());
        englishAction_->setObjectName(QStringLiteral("englishLanguage"));
        chineseAction_->setObjectName(QStringLiteral("chineseLanguage"));
        englishAction_->setCheckable(true);
        chineseAction_->setCheckable(true);
        languageGroup->addAction(englishAction_);
        languageGroup->addAction(chineseAction_);
        connect(englishAction_, &QAction::triggered, this,
                [this] { setLanguage(QStringLiteral("en")); });
        connect(chineseAction_, &QAction::triggered, this,
                [this] { setLanguage(QStringLiteral("zh_CN")); });

        auto *toolbar = addToolBar(QString());
        toolbar->setMovable(false);
        toolbar->addAction(openAction_);
        toolbar->addAction(closeAction_);
        toolbar->addSeparator();
        previous_ = toolbar->addAction(QString());
        next_ = toolbar->addAction(QString());
        pageLabel_ = new QLabel(this);
        toolbar->addWidget(pageLabel_);
        pageNumber_ = new QSpinBox(this);
        pageNumber_->setObjectName(QStringLiteral("pageNumber"));
        pageNumber_->setRange(1, 1);
        pageNumber_->setKeyboardTracking(false);
        pageNumber_->setFixedWidth(85);
        toolbar->addWidget(pageNumber_);
        pageCount_ = new QLabel(this);
        toolbar->addWidget(pageCount_);
        connect(previous_, &QAction::triggered, this, [this] {
            if (auto *view = currentView()) view->setPageIndex(view->pageIndex() - 1);
        });
        connect(next_, &QAction::triggered, this, [this] {
            if (auto *view = currentView()) view->setPageIndex(view->pageIndex() + 1);
        });
        connect(pageNumber_, &QSpinBox::valueChanged, this, [this](int page) {
            if (auto *view = currentView()) view->setPageIndex(page - 1);
        });
        toolbar->addSeparator();

        zoomLabel_ = new QLabel(this);
        toolbar->addWidget(zoomLabel_);
        zoom_ = new QSpinBox(this);
        zoom_->setObjectName(QStringLiteral("zoomPercent"));
        zoom_->setRange(25, 1600);
        zoom_->setSingleStep(10);
        zoom_->setSuffix(QStringLiteral("%"));
        toolbar->addWidget(zoom_);
        connect(zoom_, &QSpinBox::valueChanged, this, [this](int zoom) {
            if (auto *view = currentView()) view->setZoomPercent(zoom);
        });

        rotationLabel_ = new QLabel(this);
        toolbar->addWidget(rotationLabel_);
        rotation_ = new QComboBox(this);
        rotation_->setObjectName(QStringLiteral("previewRotation"));
        for (int turns = 0; turns < 4; ++turns)
            rotation_->addItem(QString::number(turns * 90) + QChar(0x00B0), turns);
        toolbar->addWidget(rotation_);
        connect(rotation_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (auto *view = currentView()) view->setQuarterTurns(index);
        });

        bookmarksAction_ = toolbar->addAction(QString());
        bookmarksAction_->setObjectName(QStringLiteral("bookmarkToggle"));
        bookmarksAction_->setCheckable(true);
        connect(bookmarksAction_, &QAction::toggled, this, [this](bool visible) {
            if (auto *view = currentView()) view->setBookmarksVisible(visible);
        });

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto *settings = new QHBoxLayout();
        scope_ = new QComboBox(this);
        scope_->addItems({QString(), QString()});
        settings->addWidget(scope_);
        modeLabel_ = new QLabel(this);
        settings->addWidget(modeLabel_);
        mode_ = new QComboBox(this);
        mode_->addItems({QString(), QString()});
        settings->addWidget(mode_);
        dpiLabel_ = new QLabel(this);
        dpi_ = new QSpinBox(this);
        dpi_->setRange(150, 600);
        dpi_->setSingleStep(50);
        dpi_->setValue(300);
        settings->addWidget(dpiLabel_);
        settings->addWidget(dpi_);
        settings->addStretch();
        save_ = new QPushButton(this);
        settings->addWidget(save_);
        layout->addLayout(settings);
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
            const bool raster = mode_->currentIndex() == 1;
            dpiLabel_->setVisible(raster);
            dpi_->setVisible(raster);
        });
        dpiLabel_->hide();
        dpi_->hide();
        connect(save_, &QPushButton::clicked, this, [this] { saveFile(); });

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("documentTabs"));
        tabs_->setDocumentMode(true);
        tabs_->setTabsClosable(true);
        tabs_->setMovable(true);
        layout->addWidget(tabs_);
        setCentralWidget(central);
        connect(tabs_, &QTabWidget::currentChanged, this, [this] { syncControls(); });
        connect(tabs_, &QTabWidget::tabCloseRequested, this,
                [this](int index) { closeTab(index); });

        applyTheme(QSettings().value(QStringLiteral("appearance/theme"),
                                     QStringLiteral("light")).toString() == QStringLiteral("dark"));
        setLanguage(QSettings().value(QStringLiteral("appearance/language"),
                                      QStringLiteral("zh_CN")).toString());
        syncControls();
    }

    ~MainWindow() override { qApp->removeTranslator(&translator_); }

    bool loadPath(const QString &path) {
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath().isEmpty()
                                      ? info.absoluteFilePath() : info.canonicalFilePath();
        for (int i = 0; i < tabs_->count(); ++i) {
            auto *existing = static_cast<DocumentView *>(tabs_->widget(i));
            if (existing->path() == canonical) {
                tabs_->setCurrentIndex(i);
                recordRecentFile(canonical);
                return true;
            }
        }

        auto document = Poppler::Document::load(path);
        if (!document || document->isLocked() || document->numPages() < 1) {
            QMessageBox::warning(this, tr("Cannot open PDF"),
                                 tr("The file cannot be read or is password-protected:\n%1")
                                     .arg(path));
            return false;
        }
        auto *view = new DocumentView(std::move(document), canonical, tabs_);
        view->setDarkTheme(darkThemeEnabled_);
        view->onStateChanged = [this, view] {
            if (currentView() == view)
                syncControls();
        };
        const int index = tabs_->addTab(view, info.fileName());
        tabs_->setTabToolTip(index, canonical);
        tabs_->setCurrentIndex(index);
        recordRecentFile(canonical);
        syncControls();
        view->canvas()->setFocus(Qt::OtherFocusReason);
        return true;
    }

private:
    QTranslator translator_;
    QString language_;
    bool darkThemeEnabled_ = false;
    QStringList recentFiles_;
    QMenu *fileMenu_ = nullptr;
    QMenu *historyMenu_ = nullptr;
    QMenu *themeMenu_ = nullptr;
    QMenu *languageMenu_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *closeAction_ = nullptr;
    QAction *previous_ = nullptr;
    QAction *next_ = nullptr;
    QAction *bookmarksAction_ = nullptr;
    QAction *lightTheme_ = nullptr;
    QAction *darkTheme_ = nullptr;
    QAction *englishAction_ = nullptr;
    QAction *chineseAction_ = nullptr;
    QLabel *pageLabel_ = nullptr;
    QSpinBox *pageNumber_ = nullptr;
    QLabel *pageCount_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QSpinBox *zoom_ = nullptr;
    QLabel *rotationLabel_ = nullptr;
    QComboBox *rotation_ = nullptr;
    QComboBox *scope_ = nullptr;
    QLabel *modeLabel_ = nullptr;
    QComboBox *mode_ = nullptr;
    QLabel *dpiLabel_ = nullptr;
    QSpinBox *dpi_ = nullptr;
    QPushButton *save_ = nullptr;
    QTabWidget *tabs_ = nullptr;

    DocumentView *currentView() const {
        return static_cast<DocumentView *>(tabs_->currentWidget());
    }

    void openFiles() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Open PDFs"), QString(), tr("PDF files (*.pdf)"));
        for (const QString &path : paths)
            loadPath(path);
    }

    void closeCurrentTab() {
        closeTab(tabs_->currentIndex());
    }

    void closeTab(int index) {
        if (index < 0 || index >= tabs_->count())
            return;
        QWidget *widget = tabs_->widget(index);
        tabs_->removeTab(index);
        delete widget;
        syncControls();
    }

    void syncControls() {
        auto *view = currentView();
        const bool loaded = view != nullptr;
        previous_->setEnabled(loaded && view->pageIndex() > 0);
        next_->setEnabled(loaded && view->pageIndex() + 1 < view->pageCount());
        closeAction_->setEnabled(loaded);
        pageNumber_->setEnabled(loaded);
        zoom_->setEnabled(loaded);
        rotation_->setEnabled(loaded);
        bookmarksAction_->setEnabled(loaded && view->hasBookmarks());
        scope_->setEnabled(loaded);
        mode_->setEnabled(loaded);
        save_->setEnabled(loaded && view->hasSelection());
        {
            const QSignalBlocker pageBlocker(pageNumber_);
            pageNumber_->setRange(1, loaded ? view->pageCount() : 1);
            pageNumber_->setValue(loaded ? view->pageIndex() + 1 : 1);
        }
        {
            const QSignalBlocker zoomBlocker(zoom_);
            zoom_->setValue(loaded ? view->zoomPercent() : 100);
        }
        {
            const QSignalBlocker rotationBlocker(rotation_);
            rotation_->setCurrentIndex(loaded ? view->quarterTurns() : 0);
        }
        {
            const QSignalBlocker bookmarksBlocker(bookmarksAction_);
            bookmarksAction_->setChecked(loaded && view->bookmarksVisible());
        }
        pageCount_->setText(loaded ? QStringLiteral(" / %1 ").arg(view->pageCount())
                                   : QStringLiteral(" / - "));
        setWindowTitle(loaded ? tr("%1 — PDF Select Crop").arg(QFileInfo(view->path()).fileName())
                              : tr("PDF Select Crop"));
        if (!loaded)
            statusBar()->showMessage(tr("Open a PDF to begin."));
        else if (!view->hasBookmarks())
            statusBar()->showMessage(tr("This PDF has no bookmarks."));
        else
            statusBar()->showMessage(tr("Drag to select; Ctrl+wheel zooms; Space changes pages."));
    }

    void scheduleHistoryMenuRefresh() {
        QTimer::singleShot(0, historyMenu_, [this] { rebuildHistoryMenu(); });
    }

    void rebuildHistoryMenu() {
        historyMenu_->clear();
        for (int i = 0; i < recentFiles_.size(); ++i) {
            const QString path = recentFiles_.at(i);
            const QString label = QStringLiteral("%1. %2")
                                      .arg(i + 1)
                                      .arg(QDir::toNativeSeparators(path).replace('&', "&&"));
            auto *action = historyMenu_->addAction(label);
            action->setToolTip(path);
            connect(action, &QAction::triggered, this, [this, path] {
                if (!QFileInfo::exists(path)) {
                    recentFiles_.removeAll(path);
                    QSettings().setValue(QStringLiteral("history/files"), recentFiles_);
                    scheduleHistoryMenuRefresh();
                    QMessageBox::warning(this, tr("File not found"), path);
                    return;
                }
                loadPath(path);
            });
        }
        if (recentFiles_.isEmpty()) {
            auto *empty = historyMenu_->addAction(tr("No recent files"));
            empty->setEnabled(false);
        }
        historyMenu_->addSeparator();
        auto *clear = historyMenu_->addAction(tr("Clear history"));
        clear->setEnabled(!recentFiles_.isEmpty());
        connect(clear, &QAction::triggered, this, [this] {
            recentFiles_.clear();
            QSettings().remove(QStringLiteral("history/files"));
            scheduleHistoryMenuRefresh();
        });
    }

    void recordRecentFile(const QString &path) {
        recentFiles_.removeAll(path);
        recentFiles_.prepend(path);
        while (recentFiles_.size() > 10)
            recentFiles_.removeLast();
        QSettings().setValue(QStringLiteral("history/files"), recentFiles_);
        scheduleHistoryMenuRefresh();
    }

    void applyTheme(bool dark) {
        darkThemeEnabled_ = dark;
        const QColor window = dark ? QColor(38, 43, 50) : QColor(242, 244, 247);
        const QColor text = dark ? QColor(232, 237, 242) : QColor(32, 37, 43);
        const QColor base = dark ? QColor(28, 32, 38) : QColor(255, 255, 255);
        const QColor button = dark ? QColor(48, 54, 62) : QColor(238, 241, 244);
        QPalette palette;
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, button);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::ToolTipBase, base);
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::Highlight,
                         dark ? QColor(20, 159, 146) : QColor(8, 127, 118));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Link,
                         dark ? QColor(93, 209, 224) : QColor(0, 91, 153));
        palette.setColor(QPalette::PlaceholderText,
                         dark ? QColor(150, 157, 166) : QColor(113, 121, 130));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(140, 147, 155));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(140, 147, 155));
        qApp->setPalette(palette);
        lightTheme_->setChecked(!dark);
        darkTheme_->setChecked(dark);
        for (int i = 0; i < tabs_->count(); ++i)
            static_cast<DocumentView *>(tabs_->widget(i))->setDarkTheme(dark);
        QSettings().setValue(QStringLiteral("appearance/theme"),
                             dark ? QStringLiteral("dark") : QStringLiteral("light"));
    }

    void setLanguage(const QString &language) {
        qApp->removeTranslator(&translator_);
        language_ = language == QStringLiteral("en") ? QStringLiteral("en")
                                                        : QStringLiteral("zh_CN");
        if (language_ == QStringLiteral("zh_CN")) {
            if (!translator_.load(QStringLiteral(":/i18n/pdf-select-crop_zh_CN.qm")))
                language_ = QStringLiteral("en");
            else
                qApp->installTranslator(&translator_);
        }
        englishAction_->setChecked(language_ == QStringLiteral("en"));
        chineseAction_->setChecked(language_ == QStringLiteral("zh_CN"));
        QSettings().setValue(QStringLiteral("appearance/language"), language_);
        retranslateUi();
    }

    void retranslateUi() {
        fileMenu_->setTitle(tr("File"));
        historyMenu_->setTitle(tr("History"));
        themeMenu_->setTitle(tr("Theme"));
        languageMenu_->setTitle(tr("Language"));
        openAction_->setText(tr("Open PDFs…"));
        closeAction_->setText(tr("Close tab"));
        previous_->setText(tr("Previous"));
        next_->setText(tr("Next"));
        pageLabel_->setText(tr("Page "));
        zoomLabel_->setText(tr("Zoom "));
        rotationLabel_->setText(tr("Rotate "));
        bookmarksAction_->setText(tr("Bookmarks"));
        lightTheme_->setText(tr("Light"));
        darkTheme_->setText(tr("Dark"));
        englishAction_->setText(tr("English"));
        chineseAction_->setText(tr("Simplified Chinese"));
        scope_->setItemText(0, tr("Current page only"));
        scope_->setItemText(1, tr("Same relative area on all pages"));
        modeLabel_->setText(tr("Export mode"));
        mode_->setItemText(0, tr("Keep text and vectors (best effort)"));
        mode_->setItemText(1, tr("Remove strictly (rasterize)"));
        dpiLabel_->setText(tr("DPI"));
        save_->setText(tr("Export selection…"));
        scheduleHistoryMenuRefresh();
        syncControls();
    }

    void saveFile() {
        auto *view = currentView();
        if (!view || !view->hasSelection())
            return;
        const QString suggested = QFileInfo(view->path()).completeBaseName()
                                  + QStringLiteral("-cropped.pdf");
        const QString output = QFileDialog::getSaveFileName(
            this, tr("Export crop"),
            QFileInfo(view->path()).dir().filePath(suggested), tr("PDF files (*.pdf)"));
        if (output.isEmpty())
            return;
        if (QFileInfo(output).absoluteFilePath() == view->path() ||
            (QFileInfo(output).exists() &&
             QFileInfo(output).canonicalFilePath() == view->path())) {
            QMessageBox::warning(this, tr("Cannot overwrite original"),
                                 tr("Choose a different output file."));
            return;
        }

        QApplication::setOverrideCursor(Qt::WaitCursor);
        QString error;
        const bool ok = mode_->currentIndex() == 0 ? exportVector(*view, output, error)
                                                     : exportRaster(*view, output, error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::critical(this, tr("Export failed"), error);
            return;
        }
        statusBar()->showMessage(tr("Exported: %1").arg(output), 10000);
        QMessageBox::information(this, tr("Export complete"),
                                 tr("Saved to:\n%1").arg(output));
    }

    bool exportVector(const DocumentView &view, const QString &output, QString &error) {
        const QRectF r = view.selectionForExport();
        const QByteArray inputBytes = QFile::encodeName(view.path());
        const QString staging = QFileInfo(output).dir().filePath(
            QStringLiteral(".pdf-select-crop-%1.pdf")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        const QByteArray outputBytes = QFile::encodeName(staging);
        char message[1024] = {};
        const bool success = export_vector_pdf(inputBytes.constData(), outputBytes.constData(),
                                               view.pageIndex(), scope_->currentIndex() == 1,
                                               float(r.left()), float(r.top()),
                                               float(r.right()), float(r.bottom()),
                                               message, sizeof(message));
        if (!success) {
            QFile::remove(staging);
            error = QString::fromUtf8(message);
            return false;
        }
        std::error_code renameError;
        std::filesystem::rename(std::filesystem::path(outputBytes.constData()),
                                std::filesystem::path(QFile::encodeName(output).constData()),
                                renameError);
        if (renameError) {
            QFile::remove(staging);
            error = QString::fromLocal8Bit(renameError.message().c_str());
            return false;
        }
        return true;
    }

    bool exportRaster(const DocumentView &view, const QString &output, QString &error) {
        const int begin = scope_->currentIndex() == 1 ? 0 : view.pageIndex();
        const int end = scope_->currentIndex() == 1 ? view.pageCount() : view.pageIndex() + 1;
        return export_raster_pdf(view.document(), output, begin, end,
                                 view.selectionForExport(), dpi_->value(), error);
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pdf-select-crop"));
    app.setOrganizationName(QStringLiteral("pdf-select-crop"));
    app.setStyle(QStringLiteral("Fusion"));
    MainWindow window;
    window.show();
    for (int i = 1; i < argc; ++i)
        window.loadPath(QString::fromLocal8Bit(argv[i]));
    return app.exec();
}
