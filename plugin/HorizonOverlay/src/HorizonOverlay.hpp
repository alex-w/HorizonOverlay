#ifndef HORIZONOVERLAY_HPP
#define HORIZONOVERLAY_HPP

#include "StelModule.hpp"
#include "StelProjectorType.hpp"
#include "StelSphereGeometry.hpp"
#include "VecMath.hpp"

#include <QObject>

#include <memory>
#include <string>
#include <vector>

class QOpenGLBuffer;
class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;
class QColor;
class QMouseEvent;
class QString;
class QTranslator;
class HorizonOverlayDialog;
class StelPainter;
class StelButton;

class HorizonOverlay : public StelModule
{
    Q_OBJECT
    Q_PROPERTY(bool horizonOverlayVisible READ getOverlayVisible WRITE setOverlayVisible NOTIFY overlayVisibleChanged)
    Q_PROPERTY(bool showToolbarButton READ getFlagShowToolbarButton WRITE setFlagShowToolbarButton NOTIFY flagShowToolbarButtonChanged)

public:
    HorizonOverlay();
    ~HorizonOverlay() override;

    void init() override;
    void update(double deltaTime) override;
    void draw(StelCore* core) override;
    void handleMouseClicks(QMouseEvent* event) override;
    bool handleMouseMoves(int x, int y, Qt::MouseButtons buttons) override;
    double getCallOrder(StelModuleActionName actionName) const override;
    bool configureGui(bool show = true) override;

    bool getOverlayVisible() const;
    void setOverlayVisible(bool value);
    bool getFlagShowToolbarButton() const;
    void setFlagShowToolbarButton(bool value);
    bool getDrawLine() const;
    void setDrawLine(bool value);
    bool getDrawFill() const;
    void setDrawFill(bool value);
    bool getEditMode() const;
    void setEditMode(bool value);
    float getLineOpacity() const;
    void setLineOpacity(float value);
    float getFillOpacity() const;
    void setFillOpacity(float value);
    float getLineWidth() const;
    void setLineWidth(float value);
    QColor getLineColor() const;
    void setLineColor(const QColor& color);
    QColor getFillColor() const;
    void setFillColor(const QColor& color);
    QString getObstructionPath() const;
    QString getResolvedObstructionPath() const;
    void setObstructionPath(const QString& path);
    QString getPerformanceText() const;
    QString translateUi(const char* text) const;

    void reloadObstructionTable();
    bool saveObstructionTable() const;
    void clearSamples();
    void saveSettings() const;

signals:
    void overlayVisibleChanged(bool visible);
    void flagShowToolbarButtonChanged(bool visible);
    void drawLineChanged(bool visible);
    void drawFillChanged(bool visible);
    void editModeChanged(bool enabled);
    void lineOpacityChanged(float opacity);
    void fillOpacityChanged(float opacity);
    void lineWidthChanged(float width);
    void lineColorChanged(const QColor& color);
    void fillColorChanged(const QColor& color);
    void obstructionPathChanged(const QString& path);

private:
    struct Sample
    {
        double azDeg;
        double altDeg;
    };

    struct RenderSample
    {
        double azDeg;
        Vec3d horizon;
        Vec3d top;
    };

    bool loadObstructionTable(const std::string& path);
    void loadSettings();
    void useFallbackTable();
    void rebuildDisplaySamples();
    void rebuildGeometry();
    void appendGeometrySample(double azDeg, double altDeg);
    Vec3d altAzToVector(double azDeg, double altDeg) const;
    std::string configPath() const;
    std::string defaultObstructionPath() const;
    std::string moduleDirPath() const;
    std::string resolveObstructionPath(const std::string& path) const;
    void setLineColorFromHex(const std::string& color);
    void setFillColorFromHex(const std::string& color);
    std::string lineColorHex() const;
    std::string fillColorHex() const;
    void sortAndNormalizeSamples();
    void addSample(double azDeg, double altDeg);
    bool addSampleFromScreen(double screenX, double screenY);
    bool removeNearestSample(double screenX, double screenY);
    void createSettingsDialog();
    void loadTranslator();
    void reloadTranslator();
    QString currentTranslationLocale() const;
    bool drawShaderFill(StelPainter& painter, const StelProjectorP& projector);
    void drawCpuScreenSpaceFill(StelPainter& painter, const StelProjectorP& projector) const;
    void drawLegacyFill(StelPainter& painter, const StelProjectorP& projector, const SphericalCap& cameraCap) const;
    void drawScreenSpaceLine(StelPainter& painter, const StelProjectorP& projector) const;
    bool ensureShaderProgram(const StelProjectorP& projector);
    bool setupCurrentVAO();
    bool bindVAO();
    void releaseVAO();
    void resetPerformanceStats();
    void recordPerformanceSample(double elapsedMs, const char* fillPath, double fovDeg);
    void updatePerformanceLabel();
    void ensureToolbarButton();
    double obstructionAltitudeAt(double azDeg) const;
    bool vectorToAltAz(const Vec3d& direction, double& azDeg, double& altDeg) const;

    bool visible;
    bool showToolbarButton;
    bool toolbarButtonInBar;
    bool editMode;
    bool drawLine;
    bool drawFill;
    float lineOpacity;
    float fillOpacity;
    float lineWidth;
    float lineRgb[3];
    float fillRgb[3];
    std::string obstructionPath;
    HorizonOverlayDialog* settingsDialog;
    StelButton* toolbarButton;
    std::unique_ptr<QTranslator> localTranslator;
    bool localTranslatorInstalled;
    int performanceWindowFrames;
    double performanceWindowMs;
    double performanceWindowMaxMs;
    double performanceLastMs;
    double performanceLastAvgMs;
    double performanceLastMaxMs;
    double performanceLastFovDeg;
    const char* performanceLastFillPath;
    std::size_t performanceLastSampleCount;

    std::vector<Sample> samples;
    std::vector<Sample> displaySamples;
    std::vector<RenderSample> geometry;
    std::unique_ptr<QOpenGLVertexArrayObject> vao;
    std::unique_ptr<QOpenGLBuffer> vbo;
    std::unique_ptr<QOpenGLShaderProgram> fillShaderProgram;
    StelProjectorP fillShaderProjector;
    struct
    {
        int projectionMatrixInverse;
        int fillColor;
        int sampleCount;
        int samples;
    } fillShaderVars;
};

#include "StelPluginInterface.hpp"

class HorizonOverlayStelPluginInterface : public QObject, public StelPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID StelPluginInterface_iid)
    Q_INTERFACES(StelPluginInterface)

public:
    StelModule* getStelModule() const override;
    StelPluginInfo getPluginInfo() const override;
    QObjectList getExtensionList() const override { return QObjectList(); }
};

#endif
