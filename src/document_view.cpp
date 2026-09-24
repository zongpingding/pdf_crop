// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_view.h"

#include <poppler-qt6.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QLabel>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QTreeWidget>
#include <QTimer>
#include <QWheelEvent>
#include <QRegion>

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
        selections_.clear();
        active_ = -1;
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

void PageCanvas::setSelections(const QVector<QRectF> &selections, int active) {
    selections_ = selections;
    active_ = selections_.isEmpty() ? -1 : std::clamp(active, 0, int(selections_.size()) - 1);
    update();
}

void PageCanvas::notifySelectionChanged() {
    update();
    if (onSelectionChanged) onSelectionChanged();
}

void PageCanvas::addFullPage() {
    selections_.append(QRectF(0, 0, 1, 1));
    active_ = selections_.size() - 1;
    notifySelectionChanged();
}

void PageCanvas::addGrid(int columns, int rows) {
    if (columns < 1 || rows < 1) return;
    selections_.clear();
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column)
            selections_.append(QRectF(double(column) / columns, double(row) / rows,
                                      1.0 / columns, 1.0 / rows));
    active_ = selections_.isEmpty() ? -1 : 0;
    notifySelectionChanged();
}

void PageCanvas::replaceActive(const QRectF &selection) {
    if (active_ < 0) return;
    selections_[active_] = selection.normalized().intersected(QRectF(0, 0, 1, 1));
    notifySelectionChanged();
}

void PageCanvas::removeActive() {
    if (active_ < 0) return;
    selections_.removeAt(active_);
    active_ = selections_.isEmpty() ? -1 : std::min(active_, int(selections_.size()) - 1);
    notifySelectionChanged();
}

void PageCanvas::setActiveIndex(int index) {
    if (index < 0 || index >= selections_.size()) return;
    active_ = index;
    update();
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

QRectF PageCanvas::selectedPixels(const QRectF &s) const {
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

    if (!selections_.isEmpty() || dragging_) {
        const QColor shade(0, 0, 0, 105);
        QRegion outside(page);
        for (const auto &selection : selections_)
            outside -= selectedPixels(selection).toAlignedRect();
        if (dragging_)
            outside -= selectedPixels(QRectF(start_, end_).normalized()).toAlignedRect();
        painter.setClipRegion(outside);
        painter.fillRect(page, shade);
        painter.setClipping(false);
        for (int i = 0; i < selections_.size(); ++i) {
            QPen border(i == active_ ? QColor(39, 208, 190) : QColor(255, 190, 55),
                        i == active_ ? 3 : 2);
            border.setCosmetic(true);
            painter.setPen(border);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selectedPixels(selections_.at(i)));
        }
        if (dragging_) {
            painter.setPen(QPen(QColor(39, 208, 190), 2));
            painter.drawRect(selectedPixels(QRectF(start_, end_).normalized()));
        }
    }
}

void PageCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !pageBounds().contains(event->position().toPoint()))
        return;
    if (onPreviewInteracted) onPreviewInteracted();
    setFocus(Qt::MouseFocusReason);
    const QPointF point = toNormalized(event->position());
    if (!(event->modifiers() & Qt::ControlModifier)) {
        for (int i = selections_.size() - 1; i >= 0; --i) {
            if (selections_.at(i).contains(point)) {
                active_ = i;
                moving_ = true;
                moveStart_ = point;
                moveOriginal_ = selections_.at(i);
                notifySelectionChanged();
                return;
            }
        }
    }
    dragging_ = true;
    start_ = point;
    end_ = start_;
    update();
}

void PageCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (moving_) {
        const QPointF delta = toNormalized(event->position()) - moveStart_;
        const double x = std::clamp(moveOriginal_.x() + delta.x(), 0.0,
                                    1.0 - moveOriginal_.width());
        const double y = std::clamp(moveOriginal_.y() + delta.y(), 0.0,
                                    1.0 - moveOriginal_.height());
        selections_[active_].moveTo(x, y);
        update();
        return;
    }
    if (!dragging_)
        return;
    end_ = toNormalized(event->position());
    if (aspectRatio_ > 0 && !pagePixels_.isEmpty()) {
        double dx = end_.x() - start_.x();
        double dy = std::copysign(std::abs(dx) * pagePixels_.width() /
                                  (aspectRatio_ * pagePixels_.height()),
                                  end_.y() < start_.y() ? -1.0 : 1.0);
        if (start_.y() + dy < 0 || start_.y() + dy > 1) {
            dy = std::clamp(start_.y() + dy, 0.0, 1.0) - start_.y();
            dx = std::copysign(std::abs(dy) * aspectRatio_ * pagePixels_.height() /
                               pagePixels_.width(), dx);
        }
        end_ = {std::clamp(start_.x() + dx, 0.0, 1.0), start_.y() + dy};
    }
    update();
}

void PageCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (moving_ && event->button() == Qt::LeftButton) {
        moving_ = false;
        notifySelectionChanged();
        return;
    }
    if (!dragging_ || event->button() != Qt::LeftButton)
        return;
    mouseMoveEvent(event);
    dragging_ = false;
    const QRectF selection = QRectF(start_, end_).normalized();
    if (selection.width() * pagePixels_.width() >= 5 &&
        selection.height() * pagePixels_.height() >= 5) {
        selections_.append(selection);
        active_ = selections_.size() - 1;
    }
    notifySelectionChanged();
}

void PageCanvas::wheelEvent(QWheelEvent *event) {
    if (onWheel && onWheel(event))
        event->accept();
    else
        QWidget::wheelEvent(event);
}

void PageCanvas::keyPressEvent(QKeyEvent *event) {
    if (pageStep(event, onPageStep)) return;
    if (event->key() == Qt::Key_Insert) { addFullPage(); return; }
    if (event->key() == Qt::Key_Delete && onDelete) {
        onDelete();
        return;
    }
    if (active_ >= 0 && (event->modifiers() & Qt::ShiftModifier)) {
        QPointF delta;
        if (event->key() == Qt::Key_Left) delta.setX(-1.0 / pagePixels_.width());
        if (event->key() == Qt::Key_Right) delta.setX(1.0 / pagePixels_.width());
        if (event->key() == Qt::Key_Up) delta.setY(-1.0 / pagePixels_.height());
        if (event->key() == Qt::Key_Down) delta.setY(1.0 / pagePixels_.height());
        if (!delta.isNull()) {
            QRectF moved = selections_.at(active_).translated(delta);
            moved.moveTo(std::clamp(moved.x(), 0.0, 1.0 - moved.width()),
                         std::clamp(moved.y(), 0.0, 1.0 - moved.height()));
            selections_[active_] = moved;
            notifySelectionChanged();
            return;
        }
    }
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

    auto *selectionPanel = selectionPanel_ = new QWidget(splitter);
    selectionPanel->setMinimumWidth(190);
    auto *selectionLayout = new QVBoxLayout(selectionPanel);
    selectionLayout->setContentsMargins(6, 6, 6, 6);
    selectionSummary_ = new QLabel(selectionPanel);
    selectionSummary_->setWordWrap(true);
    selectionLayout->addWidget(selectionSummary_);
    selectionTree_ = new QTreeWidget(selectionPanel);
    selectionTree_->setObjectName(QStringLiteral("selectionTree"));
    selectionTree_->setHeaderHidden(true);
    selectionTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    selectionLayout->addWidget(selectionTree_);
    auto *orderButtons = new QHBoxLayout();
    moveUp_ = new QPushButton(selectionPanel);
    moveDown_ = new QPushButton(selectionPanel);
    orderButtons->addWidget(moveUp_);
    orderButtons->addWidget(moveDown_);
    selectionLayout->addLayout(orderButtons);
    splitter->addWidget(selectionPanel);
    splitter->setStretchFactor(2, 0);
    layout->addWidget(splitter);

    canvas_->onSelectionChanged = [this] {
        selectionTree_->clearSelection();
        selectionAnchorPage_ = -1;
        selectionAnchorIndex_ = -1;
        storeCurrentSelections();
        refreshSelectionList();
        if (onStateChanged) onStateChanged();
    };
    canvas_->onPreviewInteracted = [this] {
        selectionTree_->clearSelection();
        selectionAnchorPage_ = -1;
        selectionAnchorIndex_ = -1;
    };
    canvas_->onDelete = [this] { deleteSelectedOrActive(); };
    auto *deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), selectionTree_);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this,
            [this] { deleteSelectedOrActive(); });
    connect(selectionTree_, &QTreeWidget::itemSelectionChanged, this,
            [this] {
        const bool oneOrNone = selectedSelectionCount() <= 1;
        const int active = canvas_->activeIndex();
        moveUp_->setEnabled(oneOrNone && active > 0);
        moveDown_->setEnabled(oneOrNone && active >= 0 &&
                              active + 1 < canvas_->selections().size());
        if (onStateChanged) onStateChanged();
    });
    connect(selectionTree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) {
        const int page = item->data(0, Qt::UserRole).toInt();
        const int selection = item->data(0, Qt::UserRole + 1).toInt();
        if (selection >= 0) {
            if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) &&
                selectionAnchorPage_ >= 0) {
                const auto before = [](int aPage, int aSelection,
                                       int bPage, int bSelection) {
                    return aPage < bPage || (aPage == bPage && aSelection <= bSelection);
                };
                const bool forward = before(selectionAnchorPage_, selectionAnchorIndex_,
                                            page, selection);
                const int firstPage = forward ? selectionAnchorPage_ : page;
                const int firstSelection = forward ? selectionAnchorIndex_ : selection;
                const int lastPage = forward ? page : selectionAnchorPage_;
                const int lastSelection = forward ? selection : selectionAnchorIndex_;
                const QSignalBlocker blocker(selectionTree_);
                selectionTree_->clearSelection();
                for (int p = firstPage; p <= lastPage; ++p) {
                    auto *parent = selectionTree_->topLevelItem(p);
                    for (int s = 0; s < parent->childCount(); ++s)
                        if (before(firstPage, firstSelection, p, s) &&
                            before(p, s, lastPage, lastSelection))
                            parent->child(s)->setSelected(true);
                }
            } else {
                selectionAnchorPage_ = page;
                selectionAnchorIndex_ = selection;
            }
        }
        QTimer::singleShot(0, this, [this, page, selection] {
            setPageIndex(page);
            if (selection >= 0) {
                canvas_->setActiveIndex(selection);
                refreshSelectionList();
            }
            if (onStateChanged) onStateChanged();
        });
    });
    connect(moveUp_, &QPushButton::clicked, this, [this] { moveActiveSelection(-1); });
    connect(moveDown_, &QPushButton::clicked, this, [this] { moveActiveSelection(1); });
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
        splitter->setSizes({220, 650, 230});
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
    retranslateUi();
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

QVector<QRectF> DocumentView::storedSelections(int pageIndex) const {
    if (selectionMode_ == SelectionMode::Individual || exceptions_.contains(pageIndex))
        return individualSelections_.value(pageIndex);
    if (selectionMode_ == SelectionMode::OddEven)
        return pageIndex % 2 == 0 ? oddSelections_ : evenSelections_;
    return sharedSelections_;
}

QVector<QRectF> DocumentView::selectionsForExport(int pageIndex) const {
    return storedSelections(pageIndex);
}

void DocumentView::storeCurrentSelections() {
    QVector<QRectF> selections;
    for (const QRectF &selection : canvas_->selections())
        selections.append(rotatedRect(selection, (4 - quarterTurns_) & 3));
    if (selectionMode_ == SelectionMode::Individual || exceptions_.contains(pageIndex_))
        individualSelections_[pageIndex_] = selections;
    else if (selectionMode_ == SelectionMode::OddEven)
        (pageIndex_ % 2 == 0 ? oddSelections_ : evenSelections_) = selections;
    else
        sharedSelections_ = selections;
}

void DocumentView::refreshSelectionList() {
    if (!selectionTree_) return;
    auto key = [](int page, int selection) {
        return (quint64(quint32(page)) << 32) | quint32(selection);
    };
    QSet<quint64> selected;
    for (auto *item : selectionTree_->selectedItems()) {
        const int selection = item->data(0, Qt::UserRole + 1).toInt();
        if (selection >= 0)
            selected.insert(key(item->data(0, Qt::UserRole).toInt(), selection));
    }
    const QSignalBlocker blocker(selectionTree_);
    selectionTree_->clear();
    int pagesWithSelections = 0;
    for (int page = 0; page < pageCount(); ++page) {
        const int count = storedSelections(page).size();
        if (count) ++pagesWithSelections;
        auto *item = new QTreeWidgetItem(selectionTree_);
        item->setText(0, tr("Page %1 — %2 selections").arg(page + 1).arg(count));
        item->setData(0, Qt::UserRole, page);
        item->setData(0, Qt::UserRole + 1, -1);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        if (!count) item->setForeground(0, palette().color(QPalette::Disabled, QPalette::Text));
        for (int index = 0; index < count; ++index) {
            auto *child = new QTreeWidgetItem(item);
            child->setFlags(child->flags() | Qt::ItemIsSelectable);
            child->setText(0, tr("Selection %1").arg(index + 1));
            child->setData(0, Qt::UserRole, page);
            child->setData(0, Qt::UserRole + 1, index);
            if (page == pageIndex_ && index == canvas_->activeIndex())
                selectionTree_->setCurrentItem(child);
        }
        item->setExpanded(count > 0);
        if (page == pageIndex_ && canvas_->activeIndex() < 0)
            selectionTree_->setCurrentItem(item);
    }
    selectionTree_->clearSelection();
    for (quint64 selectedKey : selected) {
        const int page = int(selectedKey >> 32);
        const int index = int(selectedKey & 0xffffffffu);
        if (page >= 0 && page < selectionTree_->topLevelItemCount()) {
            auto *parent = selectionTree_->topLevelItem(page);
            if (index >= 0 && index < parent->childCount())
                parent->child(index)->setSelected(true);
        }
    }
    selectionSummary_->setText(tr("%1 of %2 pages have selections")
                                   .arg(pagesWithSelections).arg(pageCount()));
    const int active = canvas_->activeIndex();
    const bool singleSelection = selectionTree_->selectedItems().size() <= 1;
    moveUp_->setEnabled(singleSelection && active > 0);
    moveDown_->setEnabled(singleSelection && active >= 0 &&
                          active + 1 < canvas_->selections().size());
}

int DocumentView::selectedSelectionCount() const {
    return selectionTree_ ? selectionTree_->selectedItems().size() : 0;
}

void DocumentView::deleteSelectedOrActive() {
    QHash<int, QSet<int>> groups;
    for (auto *item : selectionTree_->selectedItems()) {
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (index < 0) continue;
        const int page = item->data(0, Qt::UserRole).toInt();
        int group = page;
        if (selectionMode_ != SelectionMode::Individual && !exceptions_.contains(page)) {
            group = selectionMode_ == SelectionMode::Shared ? -1 : (page % 2 ? -3 : -2);
        }
        groups[group].insert(index);
    }
    if (groups.isEmpty()) {
        canvas_->removeActive();
        return;
    }
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        QVector<QRectF> *target = it.key() == -1 ? &sharedSelections_ :
                                  it.key() == -2 ? &oddSelections_ :
                                  it.key() == -3 ? &evenSelections_ :
                                                   &individualSelections_[it.key()];
        QList<int> indices = it.value().values();
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int index : indices)
            if (index >= 0 && index < target->size()) target->removeAt(index);
    }
    selectionTree_->clearSelection();
    selectionAnchorPage_ = -1;
    selectionAnchorIndex_ = -1;
    renderPage(false);
    refreshSelectionList();
    if (onStateChanged) onStateChanged();
}

void DocumentView::retranslateUi() {
    moveUp_->setText(tr("Move up"));
    moveDown_->setText(tr("Move down"));
    refreshSelectionList();
}

void DocumentView::moveActiveSelection(int step) {
    const int active = canvas_->activeIndex();
    QVector<QRectF> selections = storedSelections(pageIndex_);
    const int target = active + step;
    if (active < 0 || target < 0 || target >= selections.size()) return;
    const bool keepSelected = selectedSelectionCount() == 1;
    selectionTree_->clearSelection();
    selections.swapItemsAt(active, target);
    setCurrentSelections(selections, target);
    if (keepSelected)
        selectionTree_->topLevelItem(pageIndex_)->child(target)->setSelected(true);
}

void DocumentView::setSelectionMode(SelectionMode mode) {
    if (mode == selectionMode_) return;
    const QVector<QRectF> current = storedSelections(pageIndex_);
    if (mode == SelectionMode::OddEven) {
        if (oddSelections_.isEmpty()) oddSelections_ = current;
        if (evenSelections_.isEmpty()) evenSelections_ = current;
    } else if (mode == SelectionMode::Individual) {
        if (!individualSelections_.contains(pageIndex_))
            individualSelections_[pageIndex_] = current;
    } else if (sharedSelections_.isEmpty()) {
        sharedSelections_ = current;
    }
    selectionMode_ = mode;
    renderPage(false);
    refreshSelectionList();
    if (onStateChanged) onStateChanged();
}

void DocumentView::setExceptions(const QSet<int> &pages) {
    for (int page : pages) {
        if (!exceptions_.contains(page) && !individualSelections_.contains(page))
            individualSelections_[page] = storedSelections(page);
    }
    exceptions_ = pages;
    renderPage(false);
    refreshSelectionList();
    if (onStateChanged) onStateChanged();
}

void DocumentView::setCurrentSelections(const QVector<QRectF> &selections, int active) {
    QVector<QRectF> rotated;
    for (const QRectF &selection : selections)
        rotated.append(rotatedRect(selection, quarterTurns_));
    canvas_->setSelections(rotated, active);
    storeCurrentSelections();
    refreshSelectionList();
    if (onStateChanged) onStateChanged();
}

bool DocumentView::bookmarksVisible() const {
    return hasBookmarks_ && !bookmarks_->isHidden();
}

bool DocumentView::selectionListVisible() const {
    return !selectionPanel_->isHidden();
}

void DocumentView::setSelectionListVisible(bool visible) {
    if (!visible) {
        selectionTree_->clearSelection();
        selectionAnchorPage_ = -1;
        selectionAnchorIndex_ = -1;
    }
    selectionPanel_->setVisible(visible);
    if (onStateChanged) onStateChanged();
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
    if (!keepSelection) {
        QVector<QRectF> rotated;
        for (const QRectF &selection : storedSelections(pageIndex_))
            rotated.append(rotatedRect(selection, quarterTurns_));
        canvas_->setSelections(rotated, 0);
    }
}

void DocumentView::setPageIndex(int index) {
    if (index < 0 || index >= pageCount() || index == pageIndex_)
        return;
    pageIndex_ = index;
    renderPage(false);
    refreshSelectionList();
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
    quarterTurns_ = turns;
    renderPage(false);
    refreshSelectionList();
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
