#ifndef HORIZONOVERLAYDIALOG_HPP
#define HORIZONOVERLAYDIALOG_HPP

#include "StelDialog.hpp"

#include <QString>

class HorizonOverlay;
class QColor;
class QPushButton;
class Ui_horizonOverlayDialogForm;

class HorizonOverlayDialog : public StelDialog
{
    Q_OBJECT

public:
    explicit HorizonOverlayDialog(HorizonOverlay* module);
    ~HorizonOverlayDialog() override;

public slots:
    void retranslate() override;
    void updatePerformanceText();

protected:
    void createDialogContent() override;

private:
    void setAboutHtml();
    void translateStaticText();
    void updateFromModule();
    void applyButtonColor(QPushButton* button, const QColor& color) const;
    void markSaved(const QString& message = QString());

    Ui_horizonOverlayDialogForm* ui;
    HorizonOverlay* module;
    bool updating;
};

#endif
