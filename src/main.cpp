// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_view.h"
#include "pdf_backend.h"
#include "raster_backend.h"
#include "crop_plan.h"

#include <poppler-qt6.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>
#include <QSizePolicy>
#include <QTimer>
#include <QTranslator>
#include <QUuid>
#include <QVBoxLayout>

#include <filesystem>
#include <algorithm>
#include <memory>
#include <system_error>

static bool writeVectorPdf(const QString &input, const QString &output,
                           const QVector<CropJob> &jobs, int rotation, QString &error) {
    const QByteArray inputBytes = QFile::encodeName(input);
    const QString staging = QFileInfo(output).dir().filePath(
        QStringLiteral(".pdf-select-crop-%1.pdf")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QByteArray outputBytes = QFile::encodeName(staging);
    QVector<pdf_crop_job> vectorJobs;
    for (const CropJob &job : jobs) {
        const QRectF r = job.selection;
        vectorJobs.append({job.pageIndex, float(r.left()), float(r.top()),
                           float(r.right()), float(r.bottom())});
    }
    char message[1024] = {};
    const bool success = export_vector_pdf(inputBytes.constData(), outputBytes.constData(),
                                           vectorJobs.constData(), size_t(vectorJobs.size()),
                                           rotation, message, sizeof(message));
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

static bool writeFullPagesPdf(const QString &input, const QString &output,
                              const QVector<int> &pages, QString &error) {
    const QString staging = QFileInfo(output).dir().filePath(
        QStringLiteral(".pdf-select-crop-%1.pdf")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QByteArray inputBytes = QFile::encodeName(input);
    const QByteArray stagingBytes = QFile::encodeName(staging);
    char message[1024] = {};
    if (!export_full_pages_pdf(inputBytes.constData(), stagingBytes.constData(),
                               pages.constData(), size_t(pages.size()), message, sizeof(message))) {
        QFile::remove(staging);
        error = QString::fromUtf8(message);
        return false;
    }
    std::error_code renameError;
    std::filesystem::rename(std::filesystem::path(stagingBytes.constData()),
                            std::filesystem::path(QFile::encodeName(output).constData()),
                            renameError);
    if (renameError) {
        QFile::remove(staging);
        error = QString::fromLocal8Bit(renameError.message().c_str());
        return false;
    }
    return true;
}

class MainWindow final : public QMainWindow {
public:
    Q_DECLARE_TR_FUNCTIONS(MainWindow)
public:
    MainWindow() {
        resize(1100, 820);

        applicationMenu_ = new QMenu(this);
        fileMenu_ = applicationMenu_->addMenu(QString());
        historyMenu_ = applicationMenu_->addMenu(QString());
        historyMenu_->setObjectName(QStringLiteral("historyMenu"));
        themeMenu_ = applicationMenu_->addMenu(QString());
        languageMenu_ = applicationMenu_->addMenu(QString());
        applicationButton_ = new QToolButton(this);
        applicationButton_->setPopupMode(QToolButton::InstantPopup);
        applicationButton_->setMenu(applicationMenu_);
        menuBar()->setCornerWidget(applicationButton_, Qt::TopRightCorner);
        selectionMenu_ = menuBar()->addMenu(QString());
        editMenu_ = menuBar()->addMenu(QString());
        trimMenu_ = menuBar()->addMenu(QString());
        exportMenu_ = menuBar()->addMenu(QString());

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
        auto *toolbarSpacer = new QWidget(toolbar);
        toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolbar->addWidget(toolbarSpacer);
        toolbar->addAction(openAction_);
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
        selectionListAction_ = toolbar->addAction(QString());
        selectionListAction_->setObjectName(QStringLiteral("selectionListToggle"));
        selectionListAction_->setCheckable(true);
        connect(selectionListAction_, &QAction::toggled, this, [this](bool visible) {
            if (auto *view = currentView()) view->setSelectionListVisible(visible);
        });

        auto *central = new QWidget(this);
        auto *layout = new QVBoxLayout(central);
        auto addPanel = [](QMenu *menu, QWidget *panel) -> QWidgetAction * {
            panel->setMinimumWidth(260);
            auto *action = new QWidgetAction(menu);
            action->setDefaultWidget(panel);
            menu->addAction(action);
            return action;
        };
        auto *selectionPanel = new QWidget(selectionMenu_);
        auto *selectionLayout = new QVBoxLayout(selectionPanel);
        selectionModeLabel_ = new QLabel(selectionPanel);
        selectionLayout->addWidget(selectionModeLabel_);
        selectionMode_ = new QComboBox(selectionPanel);
        selectionMode_->setObjectName(QStringLiteral("selectionMode"));
        selectionMode_->addItems({QString(), QString(), QString()});
        selectionLayout->addWidget(selectionMode_);
        exceptionsLabel_ = new QLabel(selectionPanel);
        selectionLayout->addWidget(exceptionsLabel_);
        exceptions_ = new QLineEdit(selectionPanel);
        exceptions_->setObjectName(QStringLiteral("exceptionPages"));
        selectionLayout->addWidget(exceptions_);
        applyExceptionsButton_ = new QPushButton(selectionPanel);
        selectionLayout->addWidget(applyExceptionsButton_);
        addPanel(selectionMenu_, selectionPanel);

        auto *editPanel = new QWidget(editMenu_);
        auto *editLayout = new QVBoxLayout(editPanel);
        fullPage_ = new QPushButton(editPanel);
        grid_ = new QPushButton(editPanel);
        editLayout->addWidget(fullPage_);
        editLayout->addWidget(grid_);
        aspectLabel_ = new QLabel(editPanel);
        editLayout->addWidget(aspectLabel_);
        aspect_ = new QComboBox(editPanel);
        aspect_->addItems({QString(), QStringLiteral("A4"), QStringLiteral("4:3"),
                           QStringLiteral("16:9")});
        editLayout->addWidget(aspect_);
        device_ = new QPushButton(editPanel);
        removeSelection_ = new QPushButton(editPanel);
        editLayout->addWidget(device_);
        editLayout->addWidget(removeSelection_);
        addPanel(editMenu_, editPanel);

        auto *trimPanel = new QWidget(trimMenu_);
        auto *trimLayout = new QVBoxLayout(trimPanel);
        trim_ = new QPushButton(trimPanel);
        trimGroup_ = new QPushButton(trimPanel);
        trimLayout->addWidget(trim_);
        trimLayout->addWidget(trimGroup_);
        paddingLabel_ = new QLabel(trimPanel);
        trimLayout->addWidget(paddingLabel_);
        padding_ = new QDoubleSpinBox(trimPanel);
        padding_->setRange(0, 50);
        padding_->setValue(2);
        padding_->setSuffix(QStringLiteral(" mm"));
        trimLayout->addWidget(padding_);
        addPanel(trimMenu_, trimPanel);

        scopeMenu_ = exportMenu_->addMenu(QString());
        scope_ = new QComboBox(this);
        scope_->addItems({QString(), QString(), QString()});
        scope_->hide();
        auto *scopeGroup = new QActionGroup(this);
        for (int index = 0; index < 3; ++index) {
            auto *action = scopeMenu_->addAction(QString());
            action->setCheckable(true);
            scopeGroup->addAction(action);
            scopeActions_.append(action);
            connect(action, &QAction::triggered, this,
                    [this, index] { scope_->setCurrentIndex(index); });
        }

        auto *rangePanel = new QWidget(exportMenu_);
        auto *rangeLayout = new QVBoxLayout(rangePanel);
        pages_ = new QLineEdit(rangePanel);
        pages_->setObjectName(QStringLiteral("exportPages"));
        rangeLayout->addWidget(pages_);
        pagesAction_ = addPanel(exportMenu_, rangePanel);

        modeMenu_ = exportMenu_->addMenu(QString());
        mode_ = new QComboBox(this);
        mode_->addItems({QString(), QString()});
        mode_->hide();
        auto *modeGroup = new QActionGroup(this);
        for (int index = 0; index < 2; ++index) {
            auto *action = modeMenu_->addAction(QString());
            action->setCheckable(true);
            modeGroup->addAction(action);
            modeActions_.append(action);
            connect(action, &QAction::triggered, this,
                    [this, index] { mode_->setCurrentIndex(index); });
        }

        auto *dpiPanel = new QWidget(exportMenu_);
        auto *dpiLayout = new QVBoxLayout(dpiPanel);
        dpiLabel_ = new QLabel(dpiPanel);
        dpi_ = new QSpinBox(dpiPanel);
        dpi_->setRange(150, 600);
        dpi_->setSingleStep(50);
        dpi_->setValue(300);
        dpiLayout->addWidget(dpiLabel_);
        dpiLayout->addWidget(dpi_);
        dpiAction_ = addPanel(exportMenu_, dpiPanel);

        outputRotationMenu_ = exportMenu_->addMenu(QString());
        outputRotation_ = new QComboBox(this);
        for (int turns = 0; turns < 4; ++turns)
            outputRotation_->addItem(QString::number(turns * 90) + QChar(0x00B0), turns * 90);
        outputRotation_->hide();
        auto *rotationGroup = new QActionGroup(this);
        for (int index = 0; index < 4; ++index) {
            auto *action = outputRotationMenu_->addAction(outputRotation_->itemText(index));
            action->setCheckable(true);
            rotationGroup->addAction(action);
            outputRotationActions_.append(action);
            connect(action, &QAction::triggered, this,
                    [this, index] { outputRotation_->setCurrentIndex(index); });
        }

        auto *outputPanel = new QWidget(exportMenu_);
        auto *outputLayout = new QVBoxLayout(outputPanel);
        save_ = new QPushButton(outputPanel);
        outputLayout->addWidget(save_);
        auto *separator = new QFrame(outputPanel);
        separator->setFrameShape(QFrame::HLine);
        outputLayout->addWidget(separator);
        fullPages_ = new QPushButton(outputPanel);
        outputLayout->addWidget(fullPages_);
        addPanel(exportMenu_, outputPanel);

        connect(scope_, &QComboBox::currentIndexChanged, this, [this](int index) {
            syncControls();
        });
        connect(selectionMode_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (auto *view = currentView()) {
                view->setSelectionMode(static_cast<DocumentView::SelectionMode>(index));
                if (index == 2) scope_->setCurrentIndex(1);
            }
            syncControls();
        });
        connect(applyExceptionsButton_, &QPushButton::clicked, this, [this] {
            applyExceptions();
            selectionMenu_->hide();
        });
        connect(fullPage_, &QPushButton::clicked, this, [this] {
            editMenu_->hide();
            if (auto *view = currentView()) view->canvas()->addFullPage();
        });
        connect(grid_, &QPushButton::clicked, this, [this] { editMenu_->hide(); addGrid(); });
        connect(aspect_, &QComboBox::currentIndexChanged, this, [this] { applyAspect(); });
        connect(device_, &QPushButton::clicked, this, [this] {
            editMenu_->hide(); splitCurrentForDevice();
        });
        connect(removeSelection_, &QPushButton::clicked, this, [this] {
            editMenu_->hide();
            if (auto *view = currentView()) view->deleteSelectedOrActive();
        });
        connect(trim_, &QPushButton::clicked, this, [this] {
            trimMenu_->hide(); trimCurrent(false);
        });
        connect(trimGroup_, &QPushButton::clicked, this, [this] {
            trimMenu_->hide(); trimCurrent(true);
        });
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] { syncControls(); });
        connect(outputRotation_, &QComboBox::currentIndexChanged, this,
                [this] { syncControls(); });
        connect(save_, &QPushButton::clicked, this, [this] {
            exportMenu_->hide(); saveFile();
        });
        connect(fullPages_, &QPushButton::clicked, this, [this] {
            exportMenu_->hide(); saveCompletePages();
        });

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("documentTabs"));
        // Document mode paints a bright native line above the tabs in dark themes.
        tabs_->setDocumentMode(false);
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

    void configureFromOptions(const QCommandLineParser &parser) {
        auto *view = currentView();
        if (!view) return;
        if (parser.isSet(QStringLiteral("initialpage"))) {
            bool valid = false;
            const int page = parser.value(QStringLiteral("initialpage")).toInt(&valid);
            if (valid) view->setPageIndex(page - 1);
        }
        if (parser.isSet(QStringLiteral("grid"))) {
            const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$"))
                                   .match(parser.value(QStringLiteral("grid")));
            if (match.hasMatch()) {
                const int columns = match.captured(1).toInt();
                const int rows = match.captured(2).toInt();
                if (columns > 0 && rows > 0 && columns <= 100 / rows)
                    view->canvas()->addGrid(columns, rows);
            }
        }
        if (parser.isSet(QStringLiteral("selections"))) {
            const QString mode = parser.value(QStringLiteral("selections"));
            if (mode == QStringLiteral("evenodd")) selectionMode_->setCurrentIndex(1);
            else if (mode == QStringLiteral("individual")) selectionMode_->setCurrentIndex(2);
        }
        if (parser.isSet(QStringLiteral("exceptions"))) {
            exceptions_->setText(parser.value(QStringLiteral("exceptions")));
            applyExceptions();
        }
        if (parser.isSet(QStringLiteral("trim"))) {
            padding_->setValue(parser.value(QStringLiteral("trim-padding")).toDouble());
            trimCurrent(parser.value(QStringLiteral("trim-use")) == QStringLiteral("all"));
        }
        if (parser.isSet(QStringLiteral("whichpages"))) {
            scope_->setCurrentIndex(2);
            pages_->setText(parser.value(QStringLiteral("whichpages")));
        }
        if (parser.isSet(QStringLiteral("rotate")))
            outputRotation_->setCurrentIndex(parser.value(QStringLiteral("rotate")).toInt() / 90);
        if (parser.isSet(QStringLiteral("strict"))) mode_->setCurrentIndex(1);
        if (parser.isSet(QStringLiteral("dpi")))
            dpi_->setValue(parser.value(QStringLiteral("dpi")).toInt());
    }

private:
    QTranslator translator_;
    QString language_;
    bool darkThemeEnabled_ = false;
    QStringList recentFiles_;
    QMenu *applicationMenu_ = nullptr;
    QToolButton *applicationButton_ = nullptr;
    QMenu *selectionMenu_ = nullptr;
    QMenu *editMenu_ = nullptr;
    QMenu *trimMenu_ = nullptr;
    QMenu *exportMenu_ = nullptr;
    QMenu *fileMenu_ = nullptr;
    QMenu *historyMenu_ = nullptr;
    QMenu *themeMenu_ = nullptr;
    QMenu *languageMenu_ = nullptr;
    QAction *openAction_ = nullptr;
    QAction *closeAction_ = nullptr;
    QAction *previous_ = nullptr;
    QAction *next_ = nullptr;
    QAction *bookmarksAction_ = nullptr;
    QAction *selectionListAction_ = nullptr;
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
    QMenu *scopeMenu_ = nullptr;
    QVector<QAction *> scopeActions_;
    QLineEdit *pages_ = nullptr;
    QWidgetAction *pagesAction_ = nullptr;
    QLabel *selectionModeLabel_ = nullptr;
    QComboBox *selectionMode_ = nullptr;
    QLabel *exceptionsLabel_ = nullptr;
    QLineEdit *exceptions_ = nullptr;
    QPushButton *applyExceptionsButton_ = nullptr;
    QPushButton *fullPage_ = nullptr;
    QPushButton *grid_ = nullptr;
    QLabel *aspectLabel_ = nullptr;
    QComboBox *aspect_ = nullptr;
    QPushButton *trim_ = nullptr;
    QPushButton *trimGroup_ = nullptr;
    QLabel *paddingLabel_ = nullptr;
    QDoubleSpinBox *padding_ = nullptr;
    QPushButton *device_ = nullptr;
    QPushButton *removeSelection_ = nullptr;
    QComboBox *outputRotation_ = nullptr;
    QMenu *outputRotationMenu_ = nullptr;
    QVector<QAction *> outputRotationActions_;
    QComboBox *mode_ = nullptr;
    QMenu *modeMenu_ = nullptr;
    QVector<QAction *> modeActions_;
    QLabel *dpiLabel_ = nullptr;
    QSpinBox *dpi_ = nullptr;
    QWidgetAction *dpiAction_ = nullptr;
    QPushButton *save_ = nullptr;
    QPushButton *fullPages_ = nullptr;
    int fullPageScope_ = 1;
    QString fullPageRange_;
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

    void applyExceptions() {
        auto *view = currentView();
        if (!view) return;
        QSet<int> exceptions;
        if (!exceptions_->text().trimmed().isEmpty()) {
            QVector<int> pages;
            QString error;
            if (!parsePageRange(exceptions_->text(), view->pageCount(), pages, error)) {
                QMessageBox::warning(this, tr("Invalid exception pages"), error);
                return;
            }
            for (int page : pages) exceptions.insert(page);
        }
        view->setExceptions(exceptions);
    }

    void addGrid() {
        auto *view = currentView();
        if (!view) return;
        bool accepted = false;
        const QString input = QInputDialog::getText(this, tr("Selection grid"),
            tr("Columns x rows (for example 2x2):"), QLineEdit::Normal,
            QStringLiteral("2x2"), &accepted).trimmed();
        if (!accepted) return;
        const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$")).match(input);
        const int columns = match.hasMatch() ? match.captured(1).toInt() : 0;
        const int rows = match.hasMatch() ? match.captured(2).toInt() : 0;
        if (columns < 1 || rows < 1 || columns > 100 / rows) {
            QMessageBox::warning(this, tr("Invalid grid"), tr("Use a grid with 1 to 100 cells."));
            return;
        }
        view->canvas()->addGrid(columns, rows);
    }

    void applyAspect() {
        auto *view = currentView();
        if (!view) return;
        const double ratios[] = {0, 210.0 / 297.0, 4.0 / 3.0, 16.0 / 9.0};
        view->canvas()->setAspectRatio(ratios[aspect_->currentIndex()]);
    }

    void trimCurrent(bool useGroup) {
        auto *view = currentView();
        if (!view) return;
        if (!view->hasSelection()) view->canvas()->addFullPage();
        QVector<int> pages;
        if (useGroup && view->selectionMode() != DocumentView::SelectionMode::Individual &&
            !view->exceptions().contains(view->pageIndex())) {
            for (int page = 0; page < view->pageCount(); ++page) {
                if (view->exceptions().contains(page)) continue;
                if (view->selectionMode() == DocumentView::SelectionMode::OddEven &&
                    page % 2 != view->pageIndex() % 2) continue;
                pages.append(page);
            }
        } else {
            pages.append(view->pageIndex());
        }
        QVector<QRectF> trimmed;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = trimSelections(view->document(), pages,
                                      view->selectionsForExport(view->pageIndex()),
                                      padding_->value(), trimmed, error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::warning(this, tr("Cannot trim margins"), error);
            return;
        }
        view->setCurrentSelections(trimmed);
    }

    void splitCurrentForDevice() {
        auto *view = currentView();
        if (!view || !view->hasSelection()) return;
        bool accepted = false;
        const QString input = QInputDialog::getText(this, tr("Fit device screen"),
            tr("Screen width x height in pixels:"), QLineEdit::Normal,
            QStringLiteral("730x600"), &accepted).trimmed();
        if (!accepted) return;
        const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$")).match(input);
        const int width = match.hasMatch() ? match.captured(1).toInt() : 0;
        const int height = match.hasMatch() ? match.captured(2).toInt() : 0;
        if (width < 1 || height < 1) {
            QMessageBox::warning(this, tr("Invalid screen size"), tr("Enter a positive width and height."));
            return;
        }
        auto page = view->document().page(view->pageIndex());
        const auto slices = splitForDevice(view->selectionForExport(),
                                           page->pageSizeF(), double(width) / height);
        if (slices.isEmpty()) {
            QMessageBox::warning(this, tr("Cannot split page"),
                                 tr("The device size would create too many pages."));
            return;
        }
        QVector<QRectF> selections = view->selectionsForExport(view->pageIndex());
        const int active = view->canvas()->activeIndex();
        if (active < 0 || active >= selections.size()) return;
        selections.removeAt(active);
        for (int i = 0; i < slices.size(); ++i) selections.insert(active + i, slices.at(i));
        view->setCurrentSelections(selections);
    }

    bool buildJobs(const DocumentView &view, QVector<CropJob> &jobs, QString &error) const {
        QVector<int> pages;
        if (scope_->currentIndex() == 0) pages.append(view.pageIndex());
        else if (scope_->currentIndex() == 1) {
            for (int page = 0; page < view.pageCount(); ++page)
                if (view.selectionMode() != DocumentView::SelectionMode::Individual ||
                    !view.selectionsForExport(page).isEmpty())
                    pages.append(page);
        } else if (!parsePageRange(pages_->text(), view.pageCount(), pages, error)) return false;
        if (pages.isEmpty()) {
            error = tr("No pages have selections.");
            return false;
        }
        for (int page : pages) {
            const QVector<QRectF> selections = view.selectionsForExport(page);
            if (selections.isEmpty()) {
                error = tr("Page %1 has no selection.").arg(page + 1);
                return false;
            }
            for (const QRectF &selection : selections)
                jobs.append({page, selection});
        }
        return true;
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
        selectionListAction_->setEnabled(loaded);
        selectionMenu_->menuAction()->setEnabled(loaded);
        editMenu_->menuAction()->setEnabled(loaded);
        trimMenu_->menuAction()->setEnabled(loaded);
        exportMenu_->menuAction()->setEnabled(loaded);
        scope_->setEnabled(loaded);
        mode_->setEnabled(loaded);
        bool anySelections = false;
        if (loaded)
            for (int page = 0; page < view->pageCount(); ++page)
                if (!view->selectionsForExport(page).isEmpty()) {
                    anySelections = true;
                    break;
                }
        save_->setEnabled(anySelections);
        fullPages_->setEnabled(loaded);
        selectionMode_->setEnabled(loaded);
        const bool exceptionsAvailable = loaded &&
            view->selectionMode() != DocumentView::SelectionMode::Individual;
        exceptionsLabel_->setVisible(exceptionsAvailable);
        exceptions_->setVisible(exceptionsAvailable);
        applyExceptionsButton_->setVisible(exceptionsAvailable);
        fullPage_->setEnabled(loaded);
        grid_->setEnabled(loaded);
        aspect_->setEnabled(loaded);
        trim_->setEnabled(loaded);
        const bool groupTrimAvailable = exceptionsAvailable &&
            !view->exceptions().contains(view->pageIndex());
        trimGroup_->setVisible(groupTrimAvailable);
        trimGroup_->setText(loaded && view->selectionMode() == DocumentView::SelectionMode::OddEven
                                ? tr("Trim matching odd/even pages") : tr("Trim using all pages"));
        padding_->setEnabled(loaded);
        device_->setEnabled(loaded && view->hasSelection());
        const int selectedCount = loaded ? view->selectedSelectionCount() : 0;
        removeSelection_->setEnabled(loaded &&
            (selectedCount > 0 || view->canvas()->activeIndex() >= 0));
        removeSelection_->setText(selectedCount > 0
            ? tr("Delete selected selections (%1)").arg(selectedCount)
            : tr("Delete active selection"));
        scope_->setItemText(1, loaded && view->selectionMode() == DocumentView::SelectionMode::Individual
                                  ? tr("Pages with selections") : tr("All pages"));
        for (int index = 0; index < scopeActions_.size(); ++index) {
            scopeActions_[index]->setText(scope_->itemText(index));
            scopeActions_[index]->setChecked(index == scope_->currentIndex());
        }
        scopeMenu_->setTitle(tr("Pages for cropped export") + QStringLiteral(": ") +
                             scope_->currentText());
        pagesAction_->setVisible(scope_->currentIndex() == 2);
        const bool raster = mode_->currentIndex() == 1;
        for (int index = 0; index < modeActions_.size(); ++index) {
            modeActions_[index]->setText(mode_->itemText(index));
            modeActions_[index]->setChecked(index == mode_->currentIndex());
        }
        modeMenu_->setTitle(tr("Export mode") + QStringLiteral(": ") +
                            mode_->currentText());
        dpiAction_->setVisible(raster);
        for (int index = 0; index < outputRotationActions_.size(); ++index)
            outputRotationActions_[index]->setChecked(index == outputRotation_->currentIndex());
        outputRotationMenu_->setTitle(tr("Output rotation") + QStringLiteral(": ") +
                                      outputRotation_->currentText());
        for (const auto &[menu, panel] : {
                 std::pair{selectionMenu_, selectionMode_->parentWidget()},
                 std::pair{trimMenu_, trim_->parentWidget()}}) {
            panel->setMinimumHeight(0);
            panel->setMaximumHeight(QWIDGETSIZE_MAX);
            panel->layout()->invalidate();
            panel->layout()->activate();
            panel->setFixedHeight(panel->layout()->sizeHint().height());
            if (menu->isVisible()) {
                menu->updateGeometry();
                menu->resize(menu->sizeHint());
            }
        }
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
            const QSignalBlocker selectionBlocker(selectionMode_);
            selectionMode_->setCurrentIndex(loaded ? int(view->selectionMode()) : 0);
            const QSignalBlocker exceptionBlocker(exceptions_);
            QStringList numbers;
            if (loaded) {
                QList<int> sorted = view->exceptions().values();
                std::sort(sorted.begin(), sorted.end());
                for (int page : sorted) numbers.append(QString::number(page + 1));
            }
            exceptions_->setText(numbers.join(QLatin1Char(',')));
        }
        {
            const QSignalBlocker bookmarksBlocker(bookmarksAction_);
            bookmarksAction_->setChecked(loaded && view->bookmarksVisible());
        }
        {
            const QSignalBlocker selectionListBlocker(selectionListAction_);
            selectionListAction_->setChecked(loaded && view->selectionListVisible());
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
            statusBar()->showMessage(tr("Drag to add a selection; Ctrl+wheel zooms; Space changes pages."));
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
        tabs_->setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: 0; border-top: 1px solid %1; }")
                                 .arg(dark ? QStringLiteral("#535b65")
                                           : QStringLiteral("#bcc4cd")));
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
        selectionMenu_->setTitle(tr("1. Selection use"));
        editMenu_->setTitle(tr("2. Create and adjust"));
        trimMenu_->setTitle(tr("3. Auto trim"));
        exportMenu_->setTitle(tr("4. Export settings"));
        applicationButton_->setText(tr("More ▾"));
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
        selectionListAction_->setText(tr("Selection list"));
        lightTheme_->setText(tr("Light"));
        darkTheme_->setText(tr("Dark"));
        englishAction_->setText(tr("English"));
        chineseAction_->setText(tr("Simplified Chinese"));
        scope_->setItemText(0, tr("Current page only"));
        scope_->setItemText(2, tr("Specified pages"));
        pages_->setPlaceholderText(tr("e.g. 1-5,8,10-"));
        selectionModeLabel_->setText(tr("Selections"));
        selectionMode_->setItemText(0, tr("Shared"));
        selectionMode_->setItemText(1, tr("Odd / even"));
        selectionMode_->setItemText(2, tr("Individual pages"));
        exceptionsLabel_->setText(tr("Exceptions"));
        exceptions_->setPlaceholderText(tr("e.g. 1,5-7"));
        applyExceptionsButton_->setText(tr("Apply exception pages"));
        fullPage_->setText(tr("Full page"));
        grid_->setText(tr("Grid…"));
        aspectLabel_->setText(tr("Aspect"));
        aspect_->setItemText(0, tr("Free"));
        trim_->setText(tr("Trim using current page"));
        paddingLabel_->setText(tr("Padding"));
        device_->setText(tr("Fit device…"));
        mode_->setItemText(0, tr("Keep text and vectors (best effort)"));
        mode_->setItemText(1, tr("Remove strictly (rasterize)"));
        dpiLabel_->setText(tr("DPI"));
        save_->setText(tr("Export selection…"));
        fullPages_->setText(tr("Export complete pages…"));
        fullPages_->setToolTip(tr("Keep complete source pages; crop settings do not apply."));
        for (int i = 0; i < tabs_->count(); ++i)
            static_cast<DocumentView *>(tabs_->widget(i))->retranslateUi();
        scheduleHistoryMenuRefresh();
        syncControls();
    }

    void saveFile() {
        auto *view = currentView();
        if (!view)
            return;
        QVector<CropJob> jobs;
        QString error;
        if (!buildJobs(*view, jobs, error)) {
            QMessageBox::warning(this, tr("Cannot export"), error);
            return;
        }
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
        const bool ok = mode_->currentIndex() == 0 ? exportVector(*view, output, jobs, error)
                                                     : exportRaster(*view, output, jobs, error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::critical(this, tr("Export failed"), error);
            return;
        }
        statusBar()->showMessage(tr("Exported: %1").arg(output), 10000);
        QMessageBox::information(this, tr("Export complete"),
                                 tr("Saved to:\n%1").arg(output));
    }

    void saveCompletePages() {
        auto *view = currentView();
        if (!view) return;
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Export complete pages"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *explanation = new QLabel(
            tr("Complete pages keep their content and page size. Crop selections and export mode do not apply."),
            &dialog);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);
        auto *scope = new QComboBox(&dialog);
        scope->addItems({tr("Current page only"), tr("All pages"), tr("Specified pages")});
        scope->setCurrentIndex(fullPageScope_);
        layout->addWidget(scope);
        auto *range = new QLineEdit(&dialog);
        range->setPlaceholderText(tr("e.g. 1-5,8,10-"));
        range->setText(fullPageRange_);
        range->setVisible(scope->currentIndex() == 2);
        layout->addWidget(range);
        connect(scope, &QComboBox::currentIndexChanged, range,
                [range](int index) { range->setVisible(index == 2); });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                              &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) return;
        fullPageScope_ = scope->currentIndex();
        fullPageRange_ = range->text();
        QVector<int> pages;
        QString error;
        if (fullPageScope_ == 0) pages.append(view->pageIndex());
        else if (fullPageScope_ == 1) {
            for (int page = 0; page < view->pageCount(); ++page) pages.append(page);
        } else if (!parsePageRange(fullPageRange_, view->pageCount(), pages, error)) {
            QMessageBox::warning(this, tr("Cannot export"), error);
            return;
        }
        const QString suggested = QFileInfo(view->path()).completeBaseName()
                                  + QStringLiteral("-pages.pdf");
        const QString output = QFileDialog::getSaveFileName(
            this, tr("Export complete pages"),
            QFileInfo(view->path()).dir().filePath(suggested), tr("PDF files (*.pdf)"));
        if (output.isEmpty()) return;
        if (QFileInfo(output).absoluteFilePath() == view->path() ||
            (QFileInfo(output).exists() &&
             QFileInfo(output).canonicalFilePath() == view->path())) {
            QMessageBox::warning(this, tr("Cannot overwrite original"),
                                 tr("Choose a different output file."));
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = writeFullPagesPdf(view->path(), output, pages, error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            QMessageBox::critical(this, tr("Export failed"), error);
            return;
        }
        statusBar()->showMessage(tr("Exported: %1").arg(output), 10000);
        QMessageBox::information(this, tr("Export complete"),
                                 tr("Saved to:\n%1").arg(output));
    }

    bool exportVector(const DocumentView &view, const QString &output,
                      const QVector<CropJob> &jobs, QString &error) {
        return writeVectorPdf(view.path(), output, jobs,
                              outputRotation_->currentData().toInt(), error);
    }

    bool exportRaster(const DocumentView &view, const QString &output,
                      const QVector<CropJob> &jobs, QString &error) {
        return export_raster_pdf(view.document(), output, jobs, dpi_->value(),
                                 outputRotation_->currentData().toInt(), error);
    }
};

static int runBatch(const QCommandLineParser &parser) {
    const QStringList inputs = parser.positionalArguments();
    const QString output = parser.value(QStringLiteral("output"));
    auto fail = [](const QString &message) {
        fprintf(stderr, "%s\n", qPrintable(message));
        return 1;
    };
    if (inputs.size() != 1 || output.isEmpty())
        return fail(QStringLiteral("--go requires one input PDF and --output."));
    const QString input = inputs.constFirst();
    if (QFileInfo(input).absoluteFilePath() == QFileInfo(output).absoluteFilePath() ||
        (QFileInfo(output).exists() && QFileInfo(input).canonicalFilePath() ==
                                       QFileInfo(output).canonicalFilePath()))
        return fail(QStringLiteral("Output must differ from input."));
    auto document = Poppler::Document::load(input);
    if (!document || document->isLocked() || document->numPages() < 1)
        return fail(QStringLiteral("Cannot open input PDF or it is password-protected."));
    document->setRenderHint(Poppler::Document::Antialiasing, true);
    document->setRenderHint(Poppler::Document::TextAntialiasing, true);
    const int count = document->numPages();
    bool valid = false;
    const int initial = parser.value(QStringLiteral("initialpage")).toInt(&valid) - 1;
    if (!valid || initial < 0 || initial >= count)
        return fail(QStringLiteral("--initialpage is outside the document."));
    QVector<int> pages;
    QString error;
    if (parser.isSet(QStringLiteral("whichpages"))) {
        if (!parsePageRange(parser.value(QStringLiteral("whichpages")), count, pages, error))
            return fail(error);
    } else {
        for (int page = 0; page < count; ++page) pages.append(page);
    }
    if (parser.isSet(QStringLiteral("complete-pages"))) {
        if (parser.isSet(QStringLiteral("grid")) || parser.isSet(QStringLiteral("trim")) ||
            parser.isSet(QStringLiteral("device")) || parser.isSet(QStringLiteral("strict")) ||
            parser.value(QStringLiteral("rotate")) != QStringLiteral("0") ||
            parser.isSet(QStringLiteral("exceptions")) ||
            parser.value(QStringLiteral("selections")) != QStringLiteral("all"))
            return fail(QStringLiteral("--complete-pages cannot be combined with crop options."));
        if (!writeFullPagesPdf(input, output, pages, error)) return fail(error);
        fprintf(stdout, "%s\n", qPrintable(output));
        return 0;
    }
    int columns = 1, rows = 1;
    if (parser.isSet(QStringLiteral("grid"))) {
        const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$"))
                               .match(parser.value(QStringLiteral("grid")));
        columns = match.hasMatch() ? match.captured(1).toInt() : 0;
        rows = match.hasMatch() ? match.captured(2).toInt() : 0;
        if (columns < 1 || rows < 1 || columns > 100 / rows)
            return fail(QStringLiteral("--grid must be CxR with 1 to 100 cells."));
    }
    QVector<QRectF> selections;
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column)
            selections.append(QRectF(double(column) / columns, double(row) / rows,
                                     1.0 / columns, 1.0 / rows));
    if (parser.isSet(QStringLiteral("trim"))) {
        const QString use = parser.value(QStringLiteral("trim-use"));
        if (use != QStringLiteral("initial") && use != QStringLiteral("all"))
            return fail(QStringLiteral("--trim-use must be initial or all."));
        const double padding = parser.value(QStringLiteral("trim-padding")).toDouble(&valid);
        if (!valid || padding < 0 || padding > 50)
            return fail(QStringLiteral("--trim-padding must be between 0 and 50 mm."));
        QVector<int> inspect;
        if (use == QStringLiteral("all")) {
            for (int page = 0; page < count; ++page) inspect.append(page);
        } else inspect.append(initial);
        QVector<QRectF> trimmed;
        if (!trimSelections(*document, inspect, selections, padding, trimmed, error))
            return fail(error);
        selections = trimmed;
    }
    const int rotation = parser.value(QStringLiteral("rotate")).toInt(&valid);
    if (!valid || rotation < 0 || rotation > 270 || rotation % 90)
        return fail(QStringLiteral("--rotate must be 0, 90, 180, or 270."));
    const int dpi = parser.value(QStringLiteral("dpi")).toInt(&valid);
    if (!valid || dpi < 150 || dpi > 600)
        return fail(QStringLiteral("--dpi must be between 150 and 600."));
    double deviceRatio = 0;
    if (parser.isSet(QStringLiteral("device"))) {
        const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$"))
                               .match(parser.value(QStringLiteral("device")));
        const int width = match.hasMatch() ? match.captured(1).toInt() : 0;
        const int height = match.hasMatch() ? match.captured(2).toInt() : 0;
        if (width < 1 || height < 1)
            return fail(QStringLiteral("--device must be WIDTHxHEIGHT."));
        deviceRatio = double(width) / height;
    }
    if (parser.isSet(QStringLiteral("exceptions")) ||
        (parser.isSet(QStringLiteral("selections")) &&
         parser.value(QStringLiteral("selections")) != QStringLiteral("all")))
        return fail(QStringLiteral("--exceptions and per-page selections require the GUI."));
    QVector<CropJob> jobs;
    for (int pageIndex : pages) {
        auto page = document->page(pageIndex);
        for (const QRectF &selection : selections) {
            const QVector<QRectF> parts = deviceRatio > 0
                ? splitForDevice(selection, page->pageSizeF(), deviceRatio)
                : QVector<QRectF>{selection};
            if (parts.isEmpty())
                return fail(QStringLiteral("Device size would create too many pages."));
            for (const QRectF &part : parts) jobs.append({pageIndex, part});
        }
    }
    const bool ok = parser.isSet(QStringLiteral("strict"))
        ? export_raster_pdf(*document, output, jobs, dpi, rotation, error)
        : writeVectorPdf(input, output, jobs, rotation, error);
    if (!ok) return fail(error);
    fprintf(stdout, "%s\n", qPrintable(output));
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--go" || QByteArray(argv[i]) == "--help" ||
            QByteArray(argv[i]) == "--help-all" || QByteArray(argv[i]) == "-h" ||
            QByteArray(argv[i]) == "--version" || QByteArray(argv[i]) == "-v")
            qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pdf-select-crop"));
    app.setOrganizationName(QStringLiteral("pdf-select-crop"));
    app.setStyle(QStringLiteral("Fusion"));
    app.setApplicationVersion(QStringLiteral("0.5.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Crop selected PDF regions."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Input PDF file."));
    parser.addOption({{QStringLiteral("o"), QStringLiteral("output")},
                      QStringLiteral("Output PDF path (required with --go)."), QStringLiteral("file")});
    parser.addOption({QStringLiteral("whichpages"), QStringLiteral("Pages such as 1-5,8,10-."), QStringLiteral("range")});
    parser.addOption({QStringLiteral("initialpage"), QStringLiteral("Initial page to inspect."),
                      QStringLiteral("number"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("grid"), QStringLiteral("Create a selection grid such as 2x2."),
                      QStringLiteral("columnsxrows")});
    parser.addOption({QStringLiteral("trim"), QStringLiteral("Trim white margins around selections.")});
    parser.addOption({QStringLiteral("trim-use"), QStringLiteral("Inspect initial or all pages."),
                      QStringLiteral("mode"), QStringLiteral("initial")});
    parser.addOption({QStringLiteral("trim-padding"), QStringLiteral("Padding in millimetres."),
                      QStringLiteral("mm"), QStringLiteral("2")});
    parser.addOption({QStringLiteral("selections"), QStringLiteral("Selection mode: all, evenodd, individual."),
                      QStringLiteral("mode"), QStringLiteral("all")});
    parser.addOption({QStringLiteral("exceptions"), QStringLiteral("Pages with individual selections."),
                      QStringLiteral("range")});
    parser.addOption({QStringLiteral("device"), QStringLiteral("Split for a WIDTHxHEIGHT screen."),
                      QStringLiteral("pixels")});
    parser.addOption({QStringLiteral("rotate"), QStringLiteral("Clockwise output rotation."),
                      QStringLiteral("degrees"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("strict"), QStringLiteral("Rasterize crops for strict removal.")});
    parser.addOption({QStringLiteral("dpi"), QStringLiteral("Raster output resolution."),
                      QStringLiteral("dpi"), QStringLiteral("300")});
    parser.addOption({QStringLiteral("go"), QStringLiteral("Export without opening the GUI.")});
    parser.addOption({QStringLiteral("complete-pages"),
                      QStringLiteral("Export complete pages without cropping (with --go).")});
    parser.process(app);
    const QString selectionMode = parser.value(QStringLiteral("selections"));
    if (selectionMode != QStringLiteral("all") &&
        selectionMode != QStringLiteral("evenodd") &&
        selectionMode != QStringLiteral("individual")) {
        fprintf(stderr, "--selections must be all, evenodd, or individual.\n");
        return 1;
    }
    if (parser.value(QStringLiteral("trim-use")) != QStringLiteral("initial") &&
        parser.value(QStringLiteral("trim-use")) != QStringLiteral("all")) {
        fprintf(stderr, "--trim-use must be initial or all.\n");
        return 1;
    }
    bool valid = false;
    const int rotation = parser.value(QStringLiteral("rotate")).toInt(&valid);
    if (!valid || rotation < 0 || rotation > 270 || rotation % 90) {
        fprintf(stderr, "--rotate must be 0, 90, 180, or 270.\n");
        return 1;
    }
    const int dpi = parser.value(QStringLiteral("dpi")).toInt(&valid);
    if (!valid || dpi < 150 || dpi > 600) {
        fprintf(stderr, "--dpi must be between 150 and 600.\n");
        return 1;
    }
    const double padding = parser.value(QStringLiteral("trim-padding")).toDouble(&valid);
    if (!valid || padding < 0 || padding > 50) {
        fprintf(stderr, "--trim-padding must be between 0 and 50 mm.\n");
        return 1;
    }
    const int initial = parser.value(QStringLiteral("initialpage")).toInt(&valid);
    if (!valid || initial < 1) {
        fprintf(stderr, "--initialpage must be a positive page number.\n");
        return 1;
    }
    if (parser.isSet(QStringLiteral("grid"))) {
        const auto match = QRegularExpression(QStringLiteral("^(\\d+)\\s*[xX]\\s*(\\d+)$"))
                               .match(parser.value(QStringLiteral("grid")));
        const int columns = match.hasMatch() ? match.captured(1).toInt() : 0;
        const int rows = match.hasMatch() ? match.captured(2).toInt() : 0;
        if (columns < 1 || rows < 1 || columns > 100 / rows) {
            fprintf(stderr, "--grid must be CxR with 1 to 100 cells.\n");
            return 1;
        }
    }
    if (parser.isSet(QStringLiteral("go"))) return runBatch(parser);
    if (parser.isSet(QStringLiteral("complete-pages"))) {
        fprintf(stderr, "--complete-pages requires --go.\n");
        return 1;
    }
    MainWindow window;
    window.show();
    const QStringList inputs = parser.positionalArguments();
    for (const QString &input : inputs)
        if (window.loadPath(input) && input == inputs.constFirst())
            window.configureFromOptions(parser);
    return app.exec();
}
