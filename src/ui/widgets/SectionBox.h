#pragma once

#include <QGroupBox>

class QResizeEvent;

namespace ui {

// A QGroupBox that pins one small widget (an inline on/off toggle) to the
// top-right of its title band and keeps it there on resize. This gives a
// section header the "TITLE ............ [x]" shape — the section name stays
// where QGroupBox draws it, and the toggle floats over the right edge of the
// same band, instead of the checkable-groupbox's left-aligned indicator.
class SectionBox : public QGroupBox
{
public:
    explicit SectionBox(const QString &title, QWidget *parent = nullptr);

    // Reparents `w` onto the title band's right edge (nullptr detaches).
    void setCornerWidget(QWidget *w);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void repositionCorner();

    QWidget *m_corner = nullptr;
};

} // namespace ui
