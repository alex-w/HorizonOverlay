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
class QDialog;
class QString;
class QTranslator;
class StelPainter;

class HorizonOverlay : public StelModule
{
public:
    HorizonOverlay();
    ~HorizonOverlay() override;

    void init() override;
    void draw(StelCore* core) override;
    double getCallOrder(StelModuleActionName actionName) const override;
    bool configureGui(bool show = true) override;

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
    void saveSettings() const;
    void useFallbackTable();
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
    void createSettingsDialog();
    void reloadObstructionTable();
    void loadTranslator();
    void reloadTranslator();
    QString currentTranslationLocale() const;
    QString translateUi(const char* text) const;
    bool drawShaderFill(StelPainter& painter, const StelProjectorP& projector);
    void drawCpuScreenSpaceFill(StelPainter& painter, const StelProjectorP& projector) const;
    void drawLegacyFill(StelPainter& painter, const StelProjectorP& projector, const SphericalCap& cameraCap) const;
    bool ensureShaderProgram(const StelProjectorP& projector);
    void setupCurrentVAO();
    void bindVAO();
    void releaseVAO();
    double obstructionAltitudeAt(double azDeg) const;
    bool vectorToAltAz(const Vec3d& direction, double& azDeg, double& altDeg) const;

    bool visible;
    bool drawLine;
    bool drawFill;
    float lineOpacity;
    float fillOpacity;
    float lineWidth;
    float lineRgb[3];
    float fillRgb[3];
    std::string obstructionPath;
    QDialog* settingsDialog;
    std::unique_ptr<QTranslator> localTranslator;

    std::vector<Sample> samples;
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
