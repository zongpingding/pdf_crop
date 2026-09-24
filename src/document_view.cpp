// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_view.h"

#include <poppler-qt6.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSplitter>
#include <QTreeWidget>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

QPointF rotatedPoint(QPointF point, int turns) {
    switch (turns & 3) {
    case 1: return {1.0 - point.y(), point.x()};
    case 2: return {1.0 - point.x(), 1.0 - point.y()};
    case 3: return {point.y(), 1.0 - point.x()};
    default: return point;
    }
}

QRectF rotatedRect(QRectF rect, int turns) {
    const QPointF a = rotatedPoint(rect.topLeft(), turns);
    const QPointF b = rotatedPoint(rect.bottomRight(), turns);
    return QRectF(a, b).normalized();
}

bool pageStep(QKeyEvent *event, const std::function<void(int)> &callback) {
    if (event->key() != Qt::Key_Space || !callback ||
        (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return false;
    callback(event->modifiers() & Qt::ShiftModifier ? -1 : 1);
    event->accept();
    return true;
}

} // namespace

PageCanvas::PageCanvas(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setMinimumSize(600, 450);
}

void PageCanvas::setPage(Poppler::Document *document, int pageIndex,
                         int zoomPercent, int quarterTurns, bool keepSelection) {
    document_ = document;
    pageIndex_ = pageIndex;
    zoomPercent_ = zoomPercent;
    quarterTurns_ = quarterTurns & 3;
    cache_ = {};
    cacheRect_ = {};
    if (!keepSelection) {
        selection_ = {};
        hasSelection_ = false;
    }
    auto page = document_ ? document_->page(pageIndex_) : nullptr;
    if (page) {
        QSizeF points = page->pageSizeF();
        if (quarterTurns_ & 1)
            points.transpose();
        const double scale = zoomPercent_ / 100.0;
        pagePixels_ = QSize(std::max(1, qRound(points.width() * scale)),
                            std::max(1, qRound(points.height() * scale)));
        setMinimumSize(pagePixels_ + QSize(padding * 2, padding * 2));
    } else {
        pagePixels_ = {};
        setMinimumSize(600, 450);
    }
    resize(minimumSize());
    update();
}

void PageCanvas::setSelection(const QRectF &selection) {
    selection_ = selection.normalized();
    hasSelection_ = !selection_.isEmpty();
    update();
    if (onSelectionChanged)
        onSelectionChanged();
}

void PageCanvas::setDarkTheme(bool dark) {
    darkTheme_ = dark;
    update();
}

QRect PageCanvas::pageBounds() const {
    return QRect(QPoint(padding, padding), pagePixels_);
}

QPointF PageCanvas::toNormalized(QPointF point) const {
    const QRect page = pageBounds();
    return {std::clamp((point.x() - page.left()) / page.width(), 0.0, 1.0),
            std::clamp((point.y() - page.top()) / page.height(), 0.0, 1.0)};
}

QRectF PageCanvas::selectedPixels() const {
    const QRectF s = dragging_ ? QRectF(start_, end_).normalized() : selection_;
    const QRect page = pageBounds();
    return QRectF(page.left() + s.left() * page.width(),
                  page.top() + s.top() * page.height(),
                  s.width() * page.width(), s.height() * page.height());
}

void PageCanvas::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.fillRect(event->rect(), darkTheme_ ? QColor(39, 43, 49)
                                               : QColor(215, 221, 228));
    if (!document_ || pagePixels_.isEmpty()) {
        painter.setPen(darkTheme_ ? QColor(215, 220, 227) : QColor(48, 55, 63));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open a PDF to begin"));
        return;
    }

    const QRect page = pageBounds();
    painter.fillRect(page.adjusted(-2, -2, 3, 3),
                     darkTheme_ ? QColor(15, 17, 20) : QColor(150, 158, 168));
    painter.fillRect(page, Qt::white);

    // Only render pixels that can appear in the scroll area's viewport.
    QRect visible = event->rect().intersected(page);
    if (parentWidget()) {
        const QRect viewport(mapFrom(parentWidget(), QPoint(0, 0)),
                             parentWidget()->size());
        visible = visible.intersected(viewport);
    }
    if (!visible.isEmpty()) {
        if (cache_.isNull() || !cacheRect_.contains(visible)) {
            const QRect tile = visible.adjusted(-128, -128, 128, 128).intersected(page);
            auto pdfPage = document_->page(pageIndex_);
            if (pdfPage) {
                const double dpi = 72.0 * zoomPercent_ / 100.0;
                QImage rendered = pdfPage->renderToImage(
                    dpi, dpi, tile.x() - page.x(), tile.y() - page.y(),
                    tile.width(), tile.height(),
                    static_cast<Poppler::Page::Rotation>(quarterTurns_));
                if (!rendered.isNull()) {
                    cache_ = std::move(rendered);
                    cacheRect_ = tile;
                }
            }
        }
        if (!cache_.isNull() && cacheRect_.contains(visible)) {
            const QRectF source(visible.x() - cacheRect_.x(),
                                visible.y() - cacheRect_.y(),
                                visible.width(), visible.height());
            painter.drawImage(QRectF(visible), cache_, source);
        }
    }

    if (hasSelection_ || dragging_) {
        const QRectF region = selectedPixels();
        const QColor shade(0, 0, 0, 105);
        painter.fillRect(QRectF(page.left(), page.top(), page.width(),
                                region.top() - page.top()), shade);
        painter.fillRect(QRectF(page.left(), region.bottom(), page.width(),
                                page.bottom() - region.bottom()), shade);
        painter.fillRect(QRectF(page.left(), region.top(),
                                region.left() - page.left(), region.height()), shade);
        painter.fillRect(QRectF(region.right(), region.top(),
                                page.right() - region.right(), region.height()), shade);
        QPen border(QColor(39, 208, 190), 2);
        border.setCosmetic(true);
        painter.setPen(border);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(region);
    }
}

void PageCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !pageBounds().contains(event->position().toPoint()))
        return;
    setFocus(Qt::MouseFocusReason);
    dragging_ = true;
    hasSelection_ = false;
    start_ = toNormalized(event->position());
    end_ = start_;
    update();
}

void PageCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_)
        return;
    end_ = toNormalized(event->position());
    update();
}

void PageCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_ || event->button() != Qt::LeftButton)
        return;
    end_ = toNormalized(event->position());
    dragging_ = false;
    selection_ = QRectF(start_, end_).normalized();
    hasSelection_ = selection_.width() * pagePixels_.width() >= 5 &&
                    selection_.height() * pagePixels_.height() >= 5;
    update();
    if (onSelectionChanged)
        onSelectionChanged();
}

void PageCanvas::wheelEvent(QWheelEvent *event) {
    if (onWheel && onWheel(event))
        event->accept();
    else
        QWidget::wheelEvent(event);
}

void PageCanvas::keyPressEvent(QKeyEvent *event) {
    if (!pageStep(event, onPageStep))
        QWidget::keyPressEvent(event);
}

void PreviewScrollArea::wheelEvent(QWheelEvent *event) {
    if (onWheel && onWheel(event))
        event->accept();
    else
        QScrollArea::wheelEvent(event);
}

void PreviewScrollArea::keyPressEvent(QKeyEvent *event) {
    if (!pageStep(event, onPageStep))
        QScrollArea::keyPressEvent(event);
}

DocumentView::DocumentView(std::unique_ptr<Poppler::Document> document,
                           QString path, QWidget *parent)
    : QWidget(parent), document_(std::move(document)), path_(std::move(path)) {
    document_->setRenderHint(Poppler::Document::Antialiasing, true);
    document_->setRenderHint(Poppler::Document::TextAntialiasing, true);
    document_->setRenderHint(Poppler::Document::TextHinting, true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    bookmarks_ = new QTreeWidget(splitter);
    bookmarks_->setObjectName(QStringLiteral("bookmarkTree"));
    bookmarks_->setHeaderHidden(true);
    bookmarks_->setMinimumWidth(150);
    scroll_ = new PreviewScrollArea(splitter);
    scroll_->setObjectName(QStringLiteral("previewScroll"));
    scroll_->setFocusPolicy(Qt::StrongFocus);
    canvas_ = new PageCanvas(scroll_);
    scroll_->setWidget(canvas_);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignCenter);
    splitter->addWidget(bookmarks_);
    splitter->addWidget(scroll_);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter);

    canvas_->onSelectionChanged = [this] { if (onStateChanged) onStateChanged(); };
    const auto pageStepCallback = [this](int step) { setPageIndex(pageIndex_ + step); };
    canvas_->onPageStep = pageStepCallback;
    scroll_->onPageStep = pageStepCallback;
    const auto wheelCallback = [this](QWheelEvent *event) { return handleWheel(event); };
    canvas_->onWheel = wheelCallback;
    scroll_->onWheel = wheelCallback;

    const auto outlines = document_->outline();
    hasBookmarks_ = !outlines.isEmpty();
    if (hasBookmarks_) {
        addBookmarks(outlines, nullptr);
        splitter->setSizes({240, 800});
    } else {
        bookmarks_->hide();
    }
    const auto jump = [this](QTreeWidgetItem *item) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0)
            setPageIndex(index);
    };
    connect(bookmarks_, &QTreeWidget::itemClicked, this,
            [jump](QTreeWidgetItem *item, int) { jump(item); });
    connect(bookmarks_, &QTreeWidget::itemActivated, this,
            [jump](QTreeWidgetItem *item, int) { jump(item); });
    renderPage(false);
}

DocumentView::~DocumentView() = default;

int DocumentView::pageCount() const {
    return document_->numPages();
}

void DocumentView::addBookmarks(const QVector<Poppler::OutlineItem> &items,
                                QTreeWidgetItem *parent) {
    for (const auto &outline : items) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(bookmarks_);
        item->setText(0, outline.name());
        int page = -1;
        auto destination = outline.destination();
        if (destination && outline.externalFileName().isEmpty() && outline.uri().isEmpty()) {
            page = destination->pageNumber() - 1;
            if (page < 0 && !destination->destinationName().isEmpty()) {
                auto resolved = document_->linkDestination(destination->destinationName());
                if (resolved)
                    page = resolved->pageNumber() - 1;
            }
        }
        if (page < 0 || page >= pageCount())
            page = -1;
        item->setData(0, Qt::UserRole, page);
        if (outline.hasChildren())
            addBookmarks(outline.children(), item);
        item->setExpanded(outline.isOpen());
    }
}

QRectF DocumentView::selectionForExport() const {
    return rotatedRect(canvas_->selection(), (4 - quarterTurns_) & 3);
}

bool DocumentView::bookmarksVisible() const {
    return hasBookmarks_ && !bookmarks_->isHidden();
}

void DocumentView::setBookmarksVisible(bool visible) {
    if (!hasBookmarks_)
        return;
    bookmarks_->setVisible(visible);
    if (onStateChanged)
        onStateChanged();
}

void DocumentView::renderPage(bool keepSelection) {
    canvas_->setPage(document_.get(), pageIndex_, zoomPercent_, quarterTurns_, keepSelection);
}

void DocumentView::setPageIndex(int index) {
    if (index < 0 || index >= pageCount() || index == pageIndex_)
        return;
    pageIndex_ = index;
    renderPage(false);
    scroll_->horizontalScrollBar()->setValue(0);
    scroll_->verticalScrollBar()->setValue(0);
    canvas_->setFocus(Qt::OtherFocusReason);
    if (onStateChanged)
        onStateChanged();
}

void DocumentView::setZoomPercent(int percent, QPoint viewportAnchor) {
    percent = std::clamp(percent, 25, 1600);
    if (percent == zoomPercent_)
        return;
    if (viewportAnchor.x() < 0 || viewportAnchor.y() < 0)
        viewportAnchor = scroll_->viewport()->rect().center();
    const QPoint canvasPoint = canvas_->mapFrom(scroll_->viewport(), viewportAnchor);
    const QRect oldPage = canvas_->pageBounds();
    const QPointF relative(double(canvasPoint.x() - oldPage.left()) / oldPage.width(),
                           double(canvasPoint.y() - oldPage.top()) / oldPage.height());
    zoomPercent_ = percent;
    renderPage(true);
    const QRect newPage = canvas_->pageBounds();
    scroll_->horizontalScrollBar()->setValue(
        qRound(newPage.left() + relative.x() * newPage.width() - viewportAnchor.x()));
    scroll_->verticalScrollBar()->setValue(
        qRound(newPage.top() + relative.y() * newPage.height() - viewportAnchor.y()));
    if (onStateChanged)
        onStateChanged();
}

void DocumentView::setQuarterTurns(int turns) {
    turns &= 3;
    if (turns == quarterTurns_)
        return;
    const bool selected = canvas_->hasSelection();
    const QRectF originalSelection = selectionForExport();
    quarterTurns_ = turns;
    renderPage(false);
    if (selected)
        canvas_->setSelection(rotatedRect(originalSelection, turns));
    if (onStateChanged)
        onStateChanged();
}

bool DocumentView::handleWheel(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        const int angle = event->angleDelta().y();
        const int delta = angle != 0 ? angle : event->pixelDelta().y() * 3;
        if (delta == 0)
            return false;
        wheelRemainder_ += delta;
        const int steps = wheelRemainder_ / 120;
        wheelRemainder_ %= 120;
        if (steps != 0) {
            const QPoint anchor = scroll_->viewport()->mapFromGlobal(
                event->globalPosition().toPoint());
            setZoomPercent(zoomPercent_ + steps * 10, anchor);
        }
        return true;
    }

    const bool horizontal = (event->modifiers() & Qt::ShiftModifier) ||
                            (event->pixelDelta().y() == 0 && event->angleDelta().y() == 0 &&
                             (event->pixelDelta().x() != 0 || event->angleDelta().x() != 0));
    auto *bar = horizontal ? scroll_->horizontalScrollBar() : scroll_->verticalScrollBar();
    int distance = event->pixelDelta().y();
    if (distance == 0 && horizontal)
        distance = event->pixelDelta().x();
    if (distance == 0) {
        const int angle = event->angleDelta().y() != 0
                              ? event->angleDelta().y() : event->angleDelta().x();
        distance = qRound(double(angle) / 120.0 *
                          QApplication::wheelScrollLines() * bar->singleStep());
    }
    if (distance == 0)
        return false;
    bar->setValue(bar->value() - distance);
    return true;
}
