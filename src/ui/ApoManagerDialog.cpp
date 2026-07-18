#include "ApoManagerDialog.h"

#include "../host/ApoBindingStatus.h"
#include "../host/WasapiDevices.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ui {

namespace {
QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}
} // namespace

ApoManagerDialog::ApoManagerDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Manage APO"));
    resize(600, 440);

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(QStringLiteral(
        "Every teedsp*.inf package pnputil currently has published in the "
        "Driver Store (including stale or unexpected ones — this is not "
        "filtered to a fixed list), and which live output endpoints "
        "currently have the APO bound. Install/uninstall is still a "
        "scripted operation (scripts\\deploy-apo.ps1) — this view only "
        "reports current state."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    layout->addWidget(new QLabel(QStringLiteral("<b>Installed driver packages</b>"), this));
    m_packagesTable = new QTableWidget(0, 3, this);
    m_packagesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Package"), QStringLiteral("Published name"),
         QStringLiteral("Driver version")});
    m_packagesTable->horizontalHeader()->setStretchLastSection(true);
    m_packagesTable->verticalHeader()->setVisible(false);
    m_packagesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_packagesTable->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_packagesTable);

    layout->addWidget(new QLabel(QStringLiteral("<b>Live endpoint bindings</b>"), this));
    m_bindingsTable = new QTableWidget(0, 3, this);
    m_bindingsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Output endpoint"), QStringLiteral("APO bound"), QStringLiteral("Slot")});
    m_bindingsTable->horizontalHeader()->setStretchLastSection(true);
    m_bindingsTable->verticalHeader()->setVisible(false);
    m_bindingsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bindingsTable->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_bindingsTable);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
    buttons->addButton(m_refreshButton, QDialogButtonBox::ActionRole);
    connect(m_refreshButton, &QPushButton::clicked, this, &ApoManagerDialog::refresh);
    connect(buttons, &QDialogButtonBox::rejected, this, &ApoManagerDialog::reject);
    layout->addWidget(buttons);

    refresh();
}

void ApoManagerDialog::refresh()
{
    const auto packages = host::queryInstalledApoPackages();
    m_packagesTable->setRowCount(packages.size());
    for (int row = 0; row < packages.size(); ++row) {
        const auto &pkg = packages[row];
        m_packagesTable->setItem(row, 0, readOnlyItem(pkg.label));
        m_packagesTable->setItem(row, 1, readOnlyItem(pkg.publishedName));
        m_packagesTable->setItem(row, 2, readOnlyItem(pkg.driverVersion));
    }

    const auto endpoints = host::WasapiDevices::enumerateRender();
    m_bindingsTable->setRowCount(endpoints.size());
    for (int row = 0; row < endpoints.size(); ++row) {
        const auto &ep = endpoints[row];
        const host::ApoBindingInfo binding = host::queryApoBinding(ep.id);
        m_bindingsTable->setItem(row, 0, readOnlyItem(ep.name));
        m_bindingsTable->setItem(row, 1,
            readOnlyItem(binding.bound ? QStringLiteral("Yes") : QStringLiteral("No")));
        m_bindingsTable->setItem(row, 2, readOnlyItem(binding.slot));
    }
}

} // namespace ui
