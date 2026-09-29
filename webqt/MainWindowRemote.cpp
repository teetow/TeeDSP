#include "ui/MainWindow.h"
#include "editor/DspController.h"
#include "Client.h"
#include "ui/widgets/EqCurve.h"
#include "ui/widgets/Knob.h"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QStatusBar>

QWidget *MainWindow::buildIoSection() {
    auto *section=new QWidget;
    auto *row=new QHBoxLayout(section);row->setContentsMargins(0,0,0,4);
    m_captureDevice=new QComboBox;
    m_captureDevice->addItem("CM3588 · AirPlay → UA-25");m_captureDevice->setEnabled(false);
    row->addWidget(m_captureDevice);
    m_globalBypass=new QCheckBox("Bypass");row->addWidget(m_globalBypass);
    row->addStretch();row->addWidget(new QLabel("Master"));
    auto *slider=new QSlider(Qt::Horizontal);slider->setRange(0,100);slider->setMaximumWidth(180);
    slider->setObjectName("masterVolume");
    auto *value=new QLabel("0%");value->setMinimumWidth(40);
    auto *mute=new QPushButton("Mute");mute->setCheckable(true);mute->setObjectName("masterMute");
    row->addWidget(slider);row->addWidget(value);row->addWidget(mute);
    auto *client=static_cast<remote::Client*>(m_dspController->transport());
    connect(slider,&QSlider::valueChanged,this,[client,value](int v) {
        value->setText(QString::number(v)+"%");client->setVolume({{"volume",v}});
    });
    connect(mute,&QPushButton::toggled,this,[client](bool v) {client->setVolume({{"muted",v}});});
    connect(client,&remote::Client::volumeReceived,this,[=] {
        QSignalBlocker blockSlider(slider),blockMute(mute);
        if(!slider->isSliderDown()) {
            slider->setValue(client->volume["volume"].toInt());
            value->setText(QString::number(slider->value())+"%");
        }
        mute->setChecked(client->volume["muted"].toBool());
        mute->setText(mute->isChecked()?"Unmute":"Mute");
    });
    m_statusLabel=new QLabel("Connecting to CM3588…");statusBar()->addWidget(m_statusLabel,1);
    m_dspBuildLabel=new QLabel("Qt web editor");statusBar()->addPermanentWidget(m_dspBuildLabel);
    return section;
}
void MainWindow::refreshEngineStatus() {
    auto *client=static_cast<remote::Client*>(m_dspController->transport());
    m_statusLabel->setText(client->statusText());
    m_central->setEnabled(client->ready() && client->connected());
}

void MainWindow::platformSetup() {
    // Server/CLAP ranges are the contract; include valid values saved by the
    // simple UI even where the original Windows knobs had narrower travel.
    const auto range=[](ui::Knob *knob,unsigned id,ui::Knob::Scale scale=ui::Knob::Scale::Linear) {
        const auto *d=teedsp::findParam(id);
        knob->setRange(d->minVal,d->maxVal,scale);
    };
    range(m_inputTrim,teedsp::PID_InputTrim);
    range(m_outputTrim,teedsp::PID_OutputTrim);
    range(m_compRatio,teedsp::PID_CompRatio,ui::Knob::Scale::Log);
    range(m_compMakeup,teedsp::PID_CompMakeup);
    range(m_exciterTone,teedsp::PID_ExciterTone,ui::Knob::Scale::Log);
}

void MainWindow::initializePlatform() {}
void MainWindow::connectPlatformSignals() {}
void MainWindow::savePlatformState() const {}
void MainWindow::cleanupPlatform() {}
bool MainWindow::handleClose(QCloseEvent *) {return false;}
