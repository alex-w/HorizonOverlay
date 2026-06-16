#include "HorizonOverlayDialog.hpp"
#include "ui_horizonOverlayDialog.h"

#include "HorizonOverlay.hpp"
#include "StelApp.hpp"
#include "StelGui.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTextBrowser>

#ifndef HORIZONOVERLAY_PLUGIN_VERSION
#define HORIZONOVERLAY_PLUGIN_VERSION "0.1.0"
#endif

#ifndef HORIZONOVERLAY_PLUGIN_LICENSE
#define HORIZONOVERLAY_PLUGIN_LICENSE "GPL-2.0-or-later"
#endif

HorizonOverlayDialog::HorizonOverlayDialog(HorizonOverlay* module)
    : StelDialog("HorizonOverlay")
    , ui(new Ui_horizonOverlayDialogForm())
    , module(module)
    , updating(false)
{
}

HorizonOverlayDialog::~HorizonOverlayDialog()
{
    delete ui;
}

void HorizonOverlayDialog::retranslate()
{
    if (!dialog)
        return;

    ui->retranslateUi(dialog);
    translateStaticText();
    setAboutHtml();
    updateFromModule();
    updatePerformanceText();
}

void HorizonOverlayDialog::createDialogContent()
{
    ui->setupUi(dialog);
    dialog->resize(780, 520);
    translateStaticText();
    setAboutHtml();

    kineticScrollingList << ui->aboutTextBrowser;
    StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
    if (gui)
    {
        enableKineticScrolling(gui->getFlagUseKineticScrolling());
        connect(gui, SIGNAL(flagUseKineticScrollingChanged(bool)), this, SLOT(enableKineticScrolling(bool)));
    }

    connect(&StelApp::getInstance(), SIGNAL(languageChanged()), this, SLOT(retranslate()));
    connect(ui->titleBar, &TitleBar::closeClicked, this, &StelDialog::close);
    connect(ui->titleBar, SIGNAL(movedTo(QPoint)), this, SLOT(handleMovedTo(QPoint)));

    updateFromModule();
    updatePerformanceText();

    connect(ui->showOverlayCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating)
            return;
        module->setOverlayVisible(checked);
        markSaved();
    });
    connect(ui->showFillCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating)
            return;
        module->setDrawFill(checked);
        markSaved();
    });
    connect(ui->showLineCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating)
            return;
        module->setDrawLine(checked);
        markSaved();
    });
    connect(ui->editPointsCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating)
            return;
        module->setEditMode(checked);
        ui->startDrawingButton->setEnabled(!checked);
        ui->finishDrawingButton->setEnabled(checked);
        if (checked)
        {
            ui->showOverlayCheckBox->setChecked(true);
            ui->showLineCheckBox->setChecked(true);
        }
        markSaved(module->translateUi("Edit mode is active. Hold Shift and drag with the left mouse button to add points; hold Shift and drag with the right mouse button to erase nearby points."));
    });
    connect(ui->startDrawingButton, &QPushButton::clicked, this, [this]() {
        if (updating)
            return;

        module->setOverlayVisible(true);
        module->setDrawLine(true);
        module->setEditMode(true);
        ui->showOverlayCheckBox->setChecked(true);
        ui->showLineCheckBox->setChecked(true);
        ui->editPointsCheckBox->setChecked(true);
        ui->startDrawingButton->setEnabled(false);
        ui->finishDrawingButton->setEnabled(true);
        markSaved(module->translateUi("Drawing mode is active. Hold Shift and drag with the left mouse button in the sky to add points; hold Shift and drag with the right mouse button to erase nearby points."));
    });
    connect(ui->finishDrawingButton, &QPushButton::clicked, this, [this]() {
        if (updating)
            return;

        module->setEditMode(false);
        ui->editPointsCheckBox->setChecked(false);
        ui->startDrawingButton->setEnabled(true);
        ui->finishDrawingButton->setEnabled(false);
        markSaved(module->saveObstructionTable() ? module->translateUi("Drawing finished and table saved.") : module->translateUi("Drawing finished, but the table could not be saved."));
    });
    connect(ui->showToolbarButtonCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (updating)
            return;
        module->setFlagShowToolbarButton(checked);
        markSaved();
    });
    connect(ui->fillOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (updating)
            return;
        module->setFillOpacity(static_cast<float>(value) / 100.0f);
        ui->fillOpacityValueLabel->setText(QString("%1%").arg(value));
        markSaved();
    });
    connect(ui->lineOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        if (updating)
            return;
        module->setLineOpacity(static_cast<float>(value) / 100.0f);
        ui->lineOpacityValueLabel->setText(QString("%1%").arg(value));
        markSaved();
    });
    connect(ui->lineWidthSlider, &QSlider::valueChanged, this, [this](int value) {
        if (updating)
            return;
        module->setLineWidth(static_cast<float>(value) / 10.0f);
        ui->lineWidthValueLabel->setText(QString::number(module->getLineWidth(), 'f', 1));
        markSaved();
    });
    connect(ui->fillColorButton, &QPushButton::clicked, this, [this]() {
        const QColor color = QColorDialog::getColor(module->getFillColor(), dialog, module->translateUi("Fill color"));
        if (!color.isValid())
            return;
        module->setFillColor(color);
        applyButtonColor(ui->fillColorButton, module->getFillColor());
        markSaved();
    });
    connect(ui->lineColorButton, &QPushButton::clicked, this, [this]() {
        const QColor color = QColorDialog::getColor(module->getLineColor(), dialog, module->translateUi("Line color"));
        if (!color.isValid())
            return;
        module->setLineColor(color);
        applyButtonColor(ui->lineColorButton, module->getLineColor());
        markSaved();
    });
    connect(ui->saveSettingsButton, &QPushButton::clicked, this, [this]() {
        module->saveSettings();
        markSaved();
    });
    connect(ui->obstructionPathEdit, &QLineEdit::editingFinished, this, [this]() {
        if (updating)
            return;
        module->setObstructionPath(ui->obstructionPathEdit->text());
        markSaved();
    });
    connect(ui->chooseFileButton, &QPushButton::clicked, this, [this]() {
        const QString selected = QFileDialog::getOpenFileName(
            dialog,
            module->translateUi("Choose obstruction table"),
            module->getResolvedObstructionPath(),
            module->translateUi("Text files (*.txt *.hrz *.csv);;All files (*)"));
        if (selected.isEmpty())
            return;

        module->setObstructionPath(selected);
        ui->obstructionPathEdit->setText(selected);
        module->reloadObstructionTable();
        markSaved(module->translateUi("Table reloaded."));
    });
    connect(ui->reloadTableButton, &QPushButton::clicked, this, [this]() {
        module->setObstructionPath(ui->obstructionPathEdit->text());
        module->reloadObstructionTable();
        markSaved(module->translateUi("Table reloaded."));
    });
    connect(ui->clearPointsButton, &QPushButton::clicked, this, [this]() {
        module->clearSamples();
        markSaved(module->translateUi("Points cleared."));
    });
    connect(ui->saveTableButton, &QPushButton::clicked, this, [this]() {
        module->setObstructionPath(ui->obstructionPathEdit->text());
        markSaved(module->saveObstructionTable() ? module->translateUi("Table saved.") : module->translateUi("Could not save table."));
    });
}

void HorizonOverlayDialog::translateStaticText()
{
    if (!module || !dialog)
        return;

    dialog->setWindowTitle(module->translateUi("Horizon Overlay Settings"));
    ui->titleBar->setTitle(module->translateUi("Horizon Overlay Settings"));
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->settingsTab), module->translateUi("Settings"));
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->aboutTab), module->translateUi("About"));
    ui->tabWidget->setTabText(ui->tabWidget->indexOf(ui->performanceTab), module->translateUi("Performance monitor"));
    ui->profileGroup->setTitle(module->translateUi("Obstruction profile"));
    ui->obstructionPathLabel->setText(module->translateUi("Obstruction table:"));
    ui->chooseFileButton->setText(module->translateUi("Choose file..."));
    ui->reloadTableButton->setText(module->translateUi("Reload table"));
    ui->clearPointsButton->setText(module->translateUi("Clear points"));
    ui->saveTableButton->setText(module->translateUi("Save table"));
    ui->overlayControlsGroup->setTitle(module->translateUi("Overlay controls"));
    ui->showOverlayCheckBox->setText(module->translateUi("Show overlay"));
    ui->showFillCheckBox->setText(module->translateUi("Show filled obstruction area"));
    ui->showLineCheckBox->setText(module->translateUi("Show outline"));
    ui->editPointsCheckBox->setText(module->translateUi("Edit points"));
    ui->showToolbarButtonCheckBox->setText(module->translateUi("Show button on bottom toolbar"));
    ui->drawingHelpLabel->setText(module->translateUi("Draw a profile directly in the sky: hold Shift and drag with the left mouse button to add points; hold Shift and drag with the right mouse button to erase nearby points."));
    ui->startDrawingButton->setText(module->translateUi("Start drawing profile"));
    ui->finishDrawingButton->setText(module->translateUi("Finish and save drawing"));
    ui->appearanceGroup->setTitle(module->translateUi("Appearance"));
    ui->fillOpacityLabel->setText(module->translateUi("Fill opacity:"));
    ui->lineOpacityLabel->setText(module->translateUi("Line opacity:"));
    ui->lineWidthLabel->setText(module->translateUi("Line width:"));
    ui->fillColorButton->setText(module->translateUi("Fill color"));
    ui->lineColorButton->setText(module->translateUi("Line color"));
    ui->saveSettingsButton->setText(module->translateUi("Save settings"));
    ui->performanceGroup->setTitle(module->translateUi("Performance monitor"));
}

void HorizonOverlayDialog::setAboutHtml()
{
    if (!module || !dialog)
        return;

    QString html = "<html><head></head><body>";
    html += "<h2>" + module->translateUi("Horizon Overlay") + "</h2>";
    html += "<table class='layout' width='90%'>";
    html += "<tr><td><strong>" + module->translateUi("Version:") + "</strong></td><td>" + QString::fromLatin1(HORIZONOVERLAY_PLUGIN_VERSION) + "</td></tr>";
    html += "<tr><td><strong>" + module->translateUi("License:") + "</strong></td><td>" + QString::fromLatin1(HORIZONOVERLAY_PLUGIN_LICENSE) + "</td></tr>";
    html += "<tr><td><strong>" + module->translateUi("Authors:") + "</strong></td><td>Song Zihan / Codex</td></tr>";
    html += "</table>";
    html += "<p>" + module->translateUi("Draws a transparent local obstruction horizon overlay above the normal Stellarium landscape.") + "</p>";
    html += "<p>" + module->translateUi("Use an Az/Alt obstruction table to visualize buildings, trees, terrain, or other local horizon blockers without replacing the active Stellarium landscape.") + "</p>";
    html += "<p>" + module->translateUi("Wide-field rendering uses a shader mask when available and falls back to a CPU screen mask when needed.") + "</p>";
    html += "</body></html>";

    StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
    if (gui)
        ui->aboutTextBrowser->document()->setDefaultStyleSheet(QString(gui->getStelStyle().htmlStyleSheet));
    ui->aboutTextBrowser->setHtml(html);
}

void HorizonOverlayDialog::updateFromModule()
{
    if (!module || !dialog)
        return;

    updating = true;
    ui->showOverlayCheckBox->setChecked(module->getOverlayVisible());
    ui->showFillCheckBox->setChecked(module->getDrawFill());
    ui->showLineCheckBox->setChecked(module->getDrawLine());
    ui->editPointsCheckBox->setChecked(module->getEditMode());
    ui->startDrawingButton->setEnabled(!module->getEditMode());
    ui->finishDrawingButton->setEnabled(module->getEditMode());
    ui->showToolbarButtonCheckBox->setChecked(module->getFlagShowToolbarButton());
    ui->fillOpacitySlider->setValue(qRound(module->getFillOpacity() * 100.0f));
    ui->lineOpacitySlider->setValue(qRound(module->getLineOpacity() * 100.0f));
    ui->lineWidthSlider->setValue(qRound(module->getLineWidth() * 10.0f));
    ui->fillOpacityValueLabel->setText(QString("%1%").arg(ui->fillOpacitySlider->value()));
    ui->lineOpacityValueLabel->setText(QString("%1%").arg(ui->lineOpacitySlider->value()));
    ui->lineWidthValueLabel->setText(QString::number(module->getLineWidth(), 'f', 1));
    ui->obstructionPathEdit->setText(module->getObstructionPath());
    applyButtonColor(ui->fillColorButton, module->getFillColor());
    applyButtonColor(ui->lineColorButton, module->getLineColor());
    markSaved(module->translateUi("Settings are saved automatically."));
    updating = false;
}

void HorizonOverlayDialog::updatePerformanceText()
{
    if (!module || !dialog)
        return;

    ui->performanceLabel->setText(module->getPerformanceText());
}

void HorizonOverlayDialog::applyButtonColor(QPushButton* button, const QColor& color) const
{
    if (!button || !color.isValid())
        return;

    const QString textColor = color.lightness() > 150 ? "#101010" : "#f4f4f4";
    button->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; }")
        .arg(color.name(QColor::HexRgb), textColor));
}

void HorizonOverlayDialog::markSaved(const QString& message)
{
    if (!dialog)
        return;

    ui->saveStatusLabel->setText(message.isEmpty() ? module->translateUi("Settings saved.") : message);
}
