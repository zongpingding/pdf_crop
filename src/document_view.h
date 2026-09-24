// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QScrollArea>
#include <QCoreApplication>
#include <QImage>
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

namespace Poppler { class Document; class OutlineItem; }

class PageCanvas final : public QWidget {
public:
    Q_DECLARE_TR_FUNCTIONS(PageCanvas)
public:
    explicit PageCanvas(QWidget *parent = nullptr);
    void setPage(Poppler::Document *document, int pageIndex, int zoomPercent,
                 int quarterTurns, bool keepSelection);
    void setSelection(const QRectF &selection);
    void setDarkTheme(bool dark);
    bool hasSelection() const { return hasSelection_; }
    QRectF selection() const { return selection_.normalized(); }
    QRect pageBounds() const;
    QSize cachedImageSize() const { return cache_.size(); }

    std::function<void()> onSelectionChanged;
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
    QRectF selection_;
    QPointF start_;
    QPointF end_;
    bool dragging_ = false;
    bool hasSelection_ = false;
    bool darkTheme_ = false;

    QPointF toNormalized(QPointF point) const;
    QRectF selectedPixels() const;
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
    bool hasBookmarks() const { return hasBookmarks_; }
    bool bookmarksVisible() const;
    void setBookmarksVisible(bool visible);
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
    bool hasBookmarks_ = false;
    PageCanvas *canvas_ = nullptr;
    PreviewScrollArea *scroll_ = nullptr;
    QTreeWidget *bookmarks_ = nullptr;

    void renderPage(bool keepSelection);
    bool handleWheel(QWheelEvent *event);
    void addBookmarks(const QVector<Poppler::OutlineItem> &items, QTreeWidgetItem *parent);
};
