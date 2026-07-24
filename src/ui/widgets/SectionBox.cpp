#include "SectionBox.h"

#include <QResizeEvent>
#include <QWidget>

#include <algorithm>

namespace ui {

namespace {
// The QSS positions the title subcontrol at top:7px (QGroupBox::title) and
// gives the frame padding-top:16 (QGroupBox). Empirically the title text's
// vertical centre sits a few px below that 7px offset, hence the nudge past
// it here rather than centering the toggle over the 0..16 padding band.
constexpr int kRightMarginPx  = 8;
constexpr int kTitleCenterYPx = 17;
} // namespace

SectionBox::SectionBox(const QString &title, QWidget *parent)
    : QGroupBox(title, parent)
{
}

void SectionBox::setCornerWidget(QWidget *w)
{
    if (m_corner == w)
        return;
    m_corner = w;
    if (m_corner) {
        m_corner->setParent(this);
        m_corner->show();
        repositionCorner();
    }
}

void SectionBox::resizeEvent(QResizeEvent *event)
{
    QGroupBox::resizeEvent(event);
    repositionCorner();
}

void SectionBox::repositionCorner()
{
    if (!m_corner)
        return;
    const QSize hint = m_corner->sizeHint();
    const int x = width() - hint.width() - kRightMarginPx;
    const int y = std::max(0, kTitleCenterYPx - hint.height() / 2);
    m_corner->setGeometry(x, y, hint.width(), hint.height());
    m_corner->raise();
}

} // namespace ui
