// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QScrollArea>
#include <QCoreApplication>
#include <QImage>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QWheelEvent;
class QLabel;
class QPushButton;

namespace Poppler { class Document; class OutlineItem; }

class PageCanvas final : public QWidget {
public:
    Q_DECLARE_TR_FUNCTIONS(PageCanvas)
public:
    explicit PageCanvas(QWidget *parent = nullptr);
    void setPage(Poppler::Document *document, int pageIndex, int zoomPercent,
                 int quarterTurns, bool keepSelection);
    void setSelections(const QVector<QRectF> &selections, int active = -1);
    void setAspectRatio(double ratio) { aspectRatio_ = ratio; }
    void addFullPage();
    void addGrid(int columns, int rows);
    void replaceActive(const QRectF &selection);
    void removeActive();
    void setActiveIndex(int index);
    void setDarkTheme(bool dark);
    bool hasSelection() const { return !selections_.isEmpty(); }
    QRectF selection() const { return active_ >= 0 ? selections_.at(active_) : QRectF(); }
    QVector<QRectF> selections() const { return selections_; }
    int activeIndex() const { return active_; }
    QRect pageBounds() const;
    QSize cachedImageSize() const { return cache_.size(); }

    std::function<void()> onSelectionChanged;
    std::function<void()> onPreviewInteracted;
    std::function<void()> onDelete;
    std::function<bool(QWheelEvent *)> onWheel;
    std::function<void(int)> onPageStep;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    static constexpr int padding = 24;
    Poppler::Document *document_ = nullptr;
    int pageIndex_ = 0;
    int zoomPercent_ = 100;
    int quarterTurns_ = 0;
    QSize pagePixels_;
    QImage cache_;
    QRect cacheRect_;
    QVector<QRectF> selections_;
    int active_ = -1;
    double aspectRatio_ = 0;
    QPointF start_;
    QPointF end_;
    bool dragging_ = false;
    bool moving_ = false;
    QPointF moveStart_;
    QRectF moveOriginal_;
    bool darkTheme_ = false;

    QPointF toNormalized(QPointF point) const;
    QRectF selectedPixels(const QRectF &selection) const;
    void notifySelectionChanged();
};

class PreviewScrollArea final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;
    std::function<bool(QWheelEvent *)> onWheel;
    std::function<void(int)> onPageStep;

protected:
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
};

class DocumentView final : public QWidget {
public:
    Q_DECLARE_TR_FUNCTIONS(DocumentView)
public:
    enum class SelectionMode { Shared, OddEven, Individual };
    DocumentView(std::unique_ptr<Poppler::Document> document, QString path,
                 QWidget *parent = nullptr);
    ~DocumentView() override;

    Poppler::Document &document() const { return *document_; }
    const QString &path() const { return path_; }
    int pageIndex() const { return pageIndex_; }
    int pageCount() const;
    int zoomPercent() const { return zoomPercent_; }
    int quarterTurns() const { return quarterTurns_; }
    bool hasSelection() const { return canvas_->hasSelection(); }
    QRectF selectionForExport() const;
    QVector<QRectF> selectionsForExport(int pageIndex) const;
    void setSelectionMode(SelectionMode mode);
    void setExceptions(const QSet<int> &pages);
    void setCurrentSelections(const QVector<QRectF> &selections, int active = 0);
    void moveActiveSelection(int step);
    int selectedSelectionCount() const;
    void deleteSelectedOrActive();
    void retranslateUi();
    SelectionMode selectionMode() const { return selectionMode_; }
    QSet<int> exceptions() const { return exceptions_; }
    bool hasBookmarks() const { return hasBookmarks_; }
    bool bookmarksVisible() const;
    void setBookmarksVisible(bool visible);
    bool selectionListVisible() const;
    void setSelectionListVisible(bool visible);
    void setDarkTheme(bool dark) { canvas_->setDarkTheme(dark); }
    PageCanvas *canvas() const { return canvas_; }
    PreviewScrollArea *scrollArea() const { return scroll_; }

    void setPageIndex(int index);
    void setZoomPercent(int percent, QPoint viewportAnchor = QPoint(-1, -1));
    void setQuarterTurns(int turns);

    std::function<void()> onStateChanged;

private:
    std::unique_ptr<Poppler::Document> document_;
    QString path_;
    int pageIndex_ = 0;
    int zoomPercent_ = 100;
    int quarterTurns_ = 0;
    int wheelRemainder_ = 0;
    SelectionMode selectionMode_ = SelectionMode::Shared;
    QSet<int> exceptions_;
    QVector<QRectF> sharedSelections_;
    QVector<QRectF> oddSelections_;
    QVector<QRectF> evenSelections_;
    QHash<int, QVector<QRectF>> individualSelections_;
    bool hasBookmarks_ = false;
    PageCanvas *canvas_ = nullptr;
    PreviewScrollArea *scroll_ = nullptr;
    QTreeWidget *bookmarks_ = nullptr;
    QWidget *selectionPanel_ = nullptr;
    QTreeWidget *selectionTree_ = nullptr;
    QLabel *selectionSummary_ = nullptr;
    QPushButton *moveUp_ = nullptr;
    QPushButton *moveDown_ = nullptr;
    int selectionAnchorPage_ = -1;
    int selectionAnchorIndex_ = -1;

    void renderPage(bool keepSelection);
    QVector<QRectF> storedSelections(int pageIndex) const;
    void storeCurrentSelections();
    void refreshSelectionList();
    bool handleWheel(QWheelEvent *event);
    void addBookmarks(const QVector<Poppler::OutlineItem> &items, QTreeWidgetItem *parent);
};
