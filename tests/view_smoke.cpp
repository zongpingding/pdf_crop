// SPDX-License-Identifier: AGPL-3.0-or-later
#include "document_view.h"
#include "crop_plan.h"

#include <poppler-qt6.h>

#include <QApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cstdio>
#include <memory>

static bool check(bool condition, const char *message) {
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString path = directory.filePath(QStringLiteral("source.pdf"));
    {
        QPdfWriter writer(path);
        QPainter painter(&writer);
        for (int page = 0; page < 3; ++page) {
            if (page && !writer.newPage()) return 1;
            painter.drawText(100, 100, QStringLiteral("Page %1").arg(page + 1));
        }
        painter.end();
    }
    auto document = Poppler::Document::load(path);
    if (!check(document && document->numPages() == 3, "Cannot load fixture")) return 1;
    DocumentView view(std::move(document), path);
    view.canvas()->addGrid(2, 1);
    if (!check(view.selectionsForExport(0).size() == 2, "Shared grid missing")) return 1;
    view.setPageIndex(1);
    if (!check(view.hasSelection(), "Shared selections lost on page change")) return 1;
    view.setSelectionMode(DocumentView::SelectionMode::OddEven);
    view.canvas()->addFullPage();
    if (!check(view.selectionsForExport(1).size() == 3, "Even selection missing")) return 1;
    view.setPageIndex(0);
    if (!check(view.selectionsForExport(0).size() == 2, "Odd selection changed")) return 1;
    view.setExceptions(QSet<int>{0});
    view.canvas()->addFullPage();
    if (!check(view.selectionsForExport(0).size() == 3, "Exception selection missing")) return 1;
    view.setPageIndex(2);
    if (!check(view.selectionsForExport(2).size() == 2, "Odd template changed by exception")) return 1;
    view.setQuarterTurns(1);
    if (!check(view.selectionsForExport(2).size() == 2, "Rotation lost selections")) return 1;
    view.setSelectionMode(DocumentView::SelectionMode::Individual);
    view.setPageIndex(1);
    if (!check(view.selectionsForExport(1).isEmpty(), "Individual page inherited a template")) return 1;
    view.canvas()->addFullPage();
    if (!check(view.selectionsForExport(1).size() == 1, "Individual selection missing")) return 1;
    view.setPageIndex(0);
    if (!check(view.selectionsForExport(0).size() == 3, "Exception selection lost")) return 1;
    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("selectionTree"));
    if (!check(tree && tree->topLevelItemCount() == 3,
               "Page and selection sidebar missing")) return 1;
    view.canvas()->setActiveIndex(2);
    view.moveActiveSelection(-1);
    if (!check(view.canvas()->activeIndex() == 1 &&
               view.selectionsForExport(0).at(1) == QRectF(0, 0, 1, 1),
               "Selection reorder failed")) return 1;
    if (!check(tree->selectionMode() == QAbstractItemView::ExtendedSelection &&
               !(tree->topLevelItem(0)->flags() & Qt::ItemIsSelectable),
               "Sidebar multi-selection is unavailable")) return 1;
    tree->topLevelItem(0)->child(0)->setSelected(true);
    tree->topLevelItem(0)->child(2)->setSelected(true);
    tree->topLevelItem(1)->child(0)->setSelected(true);
    if (!check(view.selectedSelectionCount() == 3, "Cross-page selection failed")) return 1;
    view.setPageIndex(2);
    if (!check(view.selectedSelectionCount() == 3,
               "Sidebar selection lost on page change")) return 1;
    view.deleteSelectedOrActive();
    if (!check(view.selectionsForExport(0).size() == 1 &&
               view.selectionsForExport(1).isEmpty() &&
               view.selectionsForExport(2).size() == 2 &&
               view.selectedSelectionCount() == 0,
               "Cross-page deletion failed")) return 1;
    view.deleteSelectedOrActive();
    if (!check(view.selectionsForExport(2).size() == 1,
               "Preview selection deletion failed")) return 1;
    view.setExceptions({});
    view.setSelectionMode(DocumentView::SelectionMode::Shared);
    view.canvas()->addGrid(2, 1);
    tree->topLevelItem(0)->child(0)->setSelected(true);
    tree->topLevelItem(1)->child(0)->setSelected(true);
    view.deleteSelectedOrActive();
    if (!check(view.selectionsForExport(0).size() == 1 &&
               view.selectionsForExport(1).size() == 1,
               "Shared selection was deleted more than once")) return 1;
    tree->topLevelItem(0)->child(0)->setSelected(true);
    view.setSelectionListVisible(false);
    if (!check(!view.selectionListVisible() && view.selectedSelectionCount() == 0,
               "Hiding selection list kept invisible selections")) return 1;
    view.setSelectionListVisible(true);
    if (!check(view.selectionListVisible(), "Selection list did not reopen")) return 1;
    QVector<int> pages;
    QString rangeError;
    if (!check(parsePageRange(QStringLiteral("2x+1"), 10, pages, rangeError) &&
               pages == QVector<int>({2, 4, 6, 8}), "2x+1 page progression failed")) return 1;
    if (!check(parsePageRange(QStringLiteral("3x+3"), 12, pages, rangeError) &&
               pages == QVector<int>({5, 8, 11}), "3x+3 page progression failed")) return 1;
    if (!check(parsePageRange(QStringLiteral("1-2,2x+1"), 7, pages, rangeError) &&
               pages == QVector<int>({0, 1, 2, 4, 6}), "Mixed page range failed")) return 1;
    if (!check(!parsePageRange(QStringLiteral("0x+1"), 10, pages, rangeError) &&
               !parsePageRange(QStringLiteral("2x+99"), 10, pages, rangeError),
               "Invalid page progression accepted")) return 1;
    return 0;
}
