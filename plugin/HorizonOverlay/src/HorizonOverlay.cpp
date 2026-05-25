#include "HorizonOverlay.hpp"

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelFileMgr.hpp"
#include "StelLocaleMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelOpenGL.hpp"
#include "StelTranslator.hpp"
#include "StelUtils.hpp"
#include "StelVertexArray.hpp"

#include <QDebug>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPushButton>
#include <QLocale>
#include <QSlider>
#include <QTranslator>
#include <QVector2D>
#include <QVector4D>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cmath>

namespace
{
constexpr double maxAzStepDeg = 0.5;
constexpr double screenSpaceFillFovThresholdDeg = 180.0;
constexpr int screenSpaceFillStepPx = 8;
constexpr int fullscreenQuadCoordsPerVertex = 2;
constexpr int fullscreenQuadVertexAttribIndex = 0;
constexpr int maxShaderObstructionSamples = 256;
constexpr double pi = 3.141592653589793238462643383279502884;

struct ScreenPoint
{
    float x;
    float y;
};

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseBool(const std::string& value, bool fallback)
{
    const std::string normalized = lowerAscii(trim(value));
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
        return true;
    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
        return false;
    return fallback;
}

double parseDouble(const std::string& value, double fallback)
{
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end && end != value.c_str() ? parsed : fallback;
}

std::string rgbToHex(const float rgb[3])
{
    QColor color;
    color.setRgbF(qBound(0.0f, rgb[0], 1.0f), qBound(0.0f, rgb[1], 1.0f), qBound(0.0f, rgb[2], 1.0f));
    return color.name(QColor::HexRgb).toStdString();
}

void setRgbFromHex(const std::string& hex, float rgb[3])
{
    const QColor color(QString::fromStdString(hex));
    if (!color.isValid())
        return;

    rgb[0] = static_cast<float>(color.redF());
    rgb[1] = static_cast<float>(color.greenF());
    rgb[2] = static_cast<float>(color.blueF());
}

void appendScreenTriangle(std::vector<float>& vertices, const ScreenPoint& a, const ScreenPoint& b, const ScreenPoint& c)
{
    vertices.push_back(a.x);
    vertices.push_back(a.y);
    vertices.push_back(b.x);
    vertices.push_back(b.y);
    vertices.push_back(c.x);
    vertices.push_back(c.y);
}

QString currentAppLocaleName()
{
    if (StelTranslator::globalTranslator)
        return StelTranslator::globalTranslator->getTrueLocaleName();

    return QLocale::system().name();
}

QString horizonOverlayTranslationDir()
{
    return StelFileMgr::getUserDir() + "/modules/HorizonOverlay/translations/horizonoverlay";
}

bool loadHorizonOverlayTranslator(QTranslator& translator, const QString& localeName)
{
    const QString translationDir = horizonOverlayTranslationDir();
    bool loaded = translator.load(localeName + ".qm", translationDir);
    if (!loaded)
    {
        const int separator = localeName.indexOf('_');
        if (separator > 0)
            loaded = translator.load(localeName.left(separator) + ".qm", translationDir);
    }

    return loaded && !translator.isEmpty();
}

QString translateHorizonOverlayInfo(const char* text)
{
    QTranslator translator;
    if (loadHorizonOverlayTranslator(translator, currentAppLocaleName()))
    {
        const QString translated = translator.translate("", text);
        if (!translated.isEmpty())
            return translated;
    }

    return QString::fromUtf8(text);
}
}

#ifndef HORIZONOVERLAY_PLUGIN_VERSION
#define HORIZONOVERLAY_PLUGIN_VERSION "0.1.0"
#endif

#ifndef HORIZONOVERLAY_PLUGIN_LICENSE
#define HORIZONOVERLAY_PLUGIN_LICENSE "GPL-2.0-or-later"
#endif

StelModule* HorizonOverlayStelPluginInterface::getStelModule() const
{
    return new HorizonOverlay();
}

StelPluginInfo HorizonOverlayStelPluginInterface::getPluginInfo() const
{
    StelPluginInfo info;
    info.id = "HorizonOverlay";
    info.displayedName = translateHorizonOverlayInfo(N_("Horizon Overlay"));
    info.authors = "Song Zihan / Codex";
    info.contact = "";
    info.description = translateHorizonOverlayInfo(N_("Draws a transparent local obstruction horizon overlay above the normal Stellarium landscape."));
    info.version = HORIZONOVERLAY_PLUGIN_VERSION;
    info.license = HORIZONOVERLAY_PLUGIN_LICENSE;
    return info;
}

HorizonOverlay::HorizonOverlay()
    : visible(true)
    , drawLine(true)
    , drawFill(true)
    , lineOpacity(0.95f)
    , fillOpacity(0.22f)
    , lineWidth(2.0f)
    , lineRgb{1.0f, 0.8f, 0.4f}
    , fillRgb{1.0f, 0.48f, 0.09f}
    , obstructionPath("obstructions.txt")
    , settingsDialog(nullptr)
    , fillShaderVars{ -1, -1, -1, -1 }
{
    setObjectName("HorizonOverlay");
}

HorizonOverlay::~HorizonOverlay()
{
    delete settingsDialog;
}

void HorizonOverlay::init()
{
    qDebug() << "[HorizonOverlay] init";

    loadTranslator();
    connect(&StelApp::getInstance(), &StelApp::languageChanged, this, [this]() {
        reloadTranslator();
    });
    loadSettings();
    reloadObstructionTable();
}

bool HorizonOverlay::configureGui(bool show)
{
    if (!show)
        return true;

    if (!settingsDialog)
        createSettingsDialog();

    settingsDialog->show();
    settingsDialog->raise();
    settingsDialog->activateWindow();
    return true;
}

void HorizonOverlay::reloadObstructionTable()
{
    const std::string path = resolveObstructionPath(obstructionPath);
    if (!loadObstructionTable(path))
    {
        qWarning() << "[HorizonOverlay] Could not read obstruction table, using fallback data.";
        useFallbackTable();
    }

    rebuildGeometry();
}

void HorizonOverlay::setupCurrentVAO()
{
    auto& gl = *QOpenGLContext::currentContext()->functions();
    vbo->bind();
    gl.glVertexAttribPointer(fullscreenQuadVertexAttribIndex, fullscreenQuadCoordsPerVertex, GL_FLOAT, false, 0, nullptr);
    vbo->release();
    gl.glEnableVertexAttribArray(fullscreenQuadVertexAttribIndex);
}

void HorizonOverlay::bindVAO()
{
    if (vao && vao->isCreated())
        vao->bind();
    else
        setupCurrentVAO();
}

void HorizonOverlay::releaseVAO()
{
    if (vao && vao->isCreated())
    {
        vao->release();
        return;
    }

    auto& gl = *QOpenGLContext::currentContext()->functions();
    gl.glDisableVertexAttribArray(fullscreenQuadVertexAttribIndex);
}

bool HorizonOverlay::ensureShaderProgram(const StelProjectorP& projector)
{
    if (samples.empty() || samples.size() > maxShaderObstructionSamples)
        return false;

    if (!vbo)
    {
        auto& gl = *QOpenGLContext::currentContext()->functions();
        vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        vbo->create();
        vbo->bind();
        const GLfloat vertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };
        gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
        vbo->release();

        vao = std::make_unique<QOpenGLVertexArrayObject>();
        vao->create();
        bindVAO();
        setupCurrentVAO();
        releaseVAO();
    }

    if (fillShaderProgram && fillShaderProjector && projector->isSameProjection(*fillShaderProjector))
        return fillShaderProgram->isLinked();

    fillShaderProgram = std::make_unique<QOpenGLShaderProgram>();
    fillShaderProjector = projector;

    const QByteArray vertexShader =
        StelOpenGL::globalShaderPrefix(StelOpenGL::VERTEX_SHADER) +
        R"(
ATTRIBUTE highp vec3 vertex;
VARYING highp vec3 ndcPos;
void main()
{
    gl_Position = vec4(vertex, 1.0);
    ndcPos = vertex;
}
)";

    bool ok = fillShaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    if (!fillShaderProgram->log().isEmpty())
        qWarning().noquote() << "HorizonOverlay: vertex shader log:\n" << fillShaderProgram->log();
    if (!ok)
        return false;

    QByteArray fragmentShader =
        StelOpenGL::globalShaderPrefix(StelOpenGL::FRAGMENT_SHADER) +
        projector->getUnProjectShader() +
        R"(

VARYING highp vec3 ndcPos;
uniform mat4 projectionMatrixInverse;
uniform vec4 fillColor;
uniform int obstructionSampleCount;
uniform vec2 obstructionSamples[)" + QByteArray::number(maxShaderObstructionSamples) + R"(];

float normalizeAz(float az)
{
    const float fullCircle = 360.0;
    az = mod(az, fullCircle);
    if (az < 0.0)
        az += fullCircle;
    return az;
}

float obstructionAltitudeAtShader(float az)
{
    az = normalizeAz(az);
    if (obstructionSampleCount <= 0)
        return 0.0;
    if (obstructionSampleCount == 1)
        return obstructionSamples[0].y;

    for (int i = 1; i < )" + QByteArray::number(maxShaderObstructionSamples) + R"(; ++i)
    {
        if (i >= obstructionSampleCount)
            break;

        vec2 previous = obstructionSamples[i - 1];
        vec2 current = obstructionSamples[i];
        if (az < previous.x || az > current.x)
            continue;

        float span = current.x - previous.x;
        if (span <= 0.0)
            return current.y;

        float t = (az - previous.x) / span;
        return mix(previous.y, current.y, t);
    }

    return obstructionSamples[obstructionSampleCount - 1].y;
}

void main(void)
{
    vec4 winPos = projectionMatrixInverse * vec4(ndcPos, 1.0);
    bool ok = false;
    vec3 dir = unProject(winPos.x, winPos.y, ok);
    if (!ok)
    {
        FRAG_COLOR = vec4(0.0);
        return;
    }

    dir = normalize(dir);
    float alt = degrees(asin(clamp(dir.z, -1.0, 1.0)));
    float stelLongitude = atan(dir.y, dir.x);
    float az = normalizeAz(degrees(3.14159265358979323846 - stelLongitude));
    float obstructionAlt = obstructionAltitudeAtShader(az);

    if (alt < 0.0 || alt > obstructionAlt)
    {
        FRAG_COLOR = vec4(0.0);
        return;
    }

    FRAG_COLOR = fillColor;
}
)";

    ok = fillShaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    if (!fillShaderProgram->log().isEmpty())
        qWarning().noquote() << "HorizonOverlay: fragment shader log:\n" << fillShaderProgram->log();
    if (!ok)
        return false;

    fillShaderProgram->bindAttributeLocation("vertex", fullscreenQuadVertexAttribIndex);
    if (!StelPainter::linkProg(fillShaderProgram.get(), "HorizonOverlay fill shader"))
        return false;

    fillShaderProgram->bind();
    fillShaderVars.projectionMatrixInverse = fillShaderProgram->uniformLocation("projectionMatrixInverse");
    fillShaderVars.fillColor = fillShaderProgram->uniformLocation("fillColor");
    fillShaderVars.sampleCount = fillShaderProgram->uniformLocation("obstructionSampleCount");
    fillShaderVars.samples = fillShaderProgram->uniformLocation("obstructionSamples");
    fillShaderProgram->release();

    return true;
}

bool HorizonOverlay::drawShaderFill(StelPainter& painter, const StelProjectorP& projector)
{
    if (!ensureShaderProgram(projector))
        return false;

    QVector<QVector2D> shaderSamples;
    shaderSamples.reserve(static_cast<int>(samples.size()));
    for (const Sample& sample : samples)
        shaderSamples.push_back(QVector2D(static_cast<float>(sample.azDeg), static_cast<float>(sample.altDeg)));

    fillShaderProgram->bind();
    fillShaderProgram->setUniformValue(fillShaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
    fillShaderProgram->setUniformValue(fillShaderVars.fillColor, QVector4D(fillRgb[0], fillRgb[1], fillRgb[2], fillOpacity));
    fillShaderProgram->setUniformValue(fillShaderVars.sampleCount, static_cast<int>(shaderSamples.size()));
    fillShaderProgram->setUniformValueArray(fillShaderVars.samples, shaderSamples.constData(), shaderSamples.size());
    projector->setUnProjectUniforms(*fillShaderProgram);

    painter.glFuncs()->glEnable(GL_BLEND);
    painter.glFuncs()->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    bindVAO();
    painter.glFuncs()->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    releaseVAO();
    fillShaderProgram->release();

    return true;
}

void HorizonOverlay::drawCpuScreenSpaceFill(StelPainter& painter, const StelProjectorP& projector) const
{
    const Vec4i& viewport = projector->getViewport();
    const int left = viewport[0];
    const int bottom = viewport[1];
    const int right = viewport[0] + viewport[2];
    const int top = viewport[1] + viewport[3];

    std::vector<float> fillVertices;
    fillVertices.reserve(static_cast<std::size_t>(viewport[2] / screenSpaceFillStepPx + 1) *
                         static_cast<std::size_t>(viewport[3] / screenSpaceFillStepPx + 1) * 3);

    for (int y = bottom; y < top; y += screenSpaceFillStepPx)
    {
        const int y1 = std::min(y + screenSpaceFillStepPx, top);
        const double sampleY = 0.5 * static_cast<double>(y + y1);

        for (int x = left; x < right; x += screenSpaceFillStepPx)
        {
            const int x1 = std::min(x + screenSpaceFillStepPx, right);
            const double sampleX = 0.5 * static_cast<double>(x + x1);

            Vec3d direction;
            if (!projector->unProject(sampleX, sampleY, direction))
                continue;

            double azDeg = 0.0;
            double altDeg = 0.0;
            if (!vectorToAltAz(direction, azDeg, altDeg))
                continue;

            const double obstructionAlt = obstructionAltitudeAt(azDeg);
            if (altDeg < 0.0 || altDeg > obstructionAlt)
                continue;

            const ScreenPoint p0{ static_cast<float>(x), static_cast<float>(y) };
            const ScreenPoint p1{ static_cast<float>(x1), static_cast<float>(y) };
            const ScreenPoint p2{ static_cast<float>(x1), static_cast<float>(y1) };
            const ScreenPoint p3{ static_cast<float>(x), static_cast<float>(y1) };
            appendScreenTriangle(fillVertices, p0, p1, p2);
            appendScreenTriangle(fillVertices, p0, p2, p3);
        }
    }

    if (fillVertices.empty())
        return;

    painter.setColor(fillRgb[0], fillRgb[1], fillRgb[2], fillOpacity);
    painter.enableClientStates(true);
    painter.setVertexPointer(2, GL_FLOAT, fillVertices.data());
    painter.drawFromArray(StelPainter::Triangles, static_cast<int>(fillVertices.size() / 2), 0, false);
    painter.enableClientStates(false);
}

void HorizonOverlay::drawLegacyFill(StelPainter& painter, const StelProjectorP& projector, const SphericalCap& cameraCap) const
{
    Q_UNUSED(projector)

    painter.setColor(fillRgb[0], fillRgb[1], fillRgb[2], fillOpacity);

    StelVertexArray fillArray(StelVertexArray::Triangles);
    fillArray.vertex.reserve(static_cast<int>((geometry.size() - 1) * 6));

    for (std::size_t i = 1; i < geometry.size(); ++i)
    {
        const RenderSample& previous = geometry[i - 1];
        const RenderSample& current = geometry[i];
        if (!cameraCap.contains(previous.horizon) &&
            !cameraCap.contains(previous.top) &&
            !cameraCap.contains(current.horizon) &&
            !cameraCap.contains(current.top))
        {
            continue;
        }

        fillArray.vertex.append(previous.horizon);
        fillArray.vertex.append(current.horizon);
        fillArray.vertex.append(current.top);
        fillArray.vertex.append(previous.horizon);
        fillArray.vertex.append(current.top);
        fillArray.vertex.append(previous.top);
    }

    if (!fillArray.vertex.isEmpty())
        painter.drawStelVertexArray(fillArray, true);
}

double HorizonOverlay::obstructionAltitudeAt(double azDeg) const
{
    if (samples.empty())
        return 0.0;

    azDeg = std::fmod(azDeg, 360.0);
    if (azDeg < 0.0)
        azDeg += 360.0;

    if (samples.size() == 1)
        return samples.front().altDeg;

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const Sample& previous = samples[i - 1];
        const Sample& current = samples[i];
        if (azDeg < previous.azDeg || azDeg > current.azDeg)
            continue;

        const double span = current.azDeg - previous.azDeg;
        if (span <= 0.0)
            return current.altDeg;

        const double t = (azDeg - previous.azDeg) / span;
        return previous.altDeg + (current.altDeg - previous.altDeg) * t;
    }

    return samples.back().altDeg;
}

bool HorizonOverlay::vectorToAltAz(const Vec3d& direction, double& azDeg, double& altDeg) const
{
    Vec3d normalized = direction;
    const double norm = normalized.norm();
    if (norm <= 0.0)
        return false;

    normalized /= norm;
    altDeg = qRadiansToDegrees(std::asin(qBound(-1.0, normalized[2], 1.0)));

    const double stelLongitude = std::atan2(normalized[1], normalized[0]);
    double azRad = pi - stelLongitude;
    azRad = std::fmod(azRad, 2.0 * pi);
    if (azRad < 0.0)
        azRad += 2.0 * pi;

    azDeg = qRadiansToDegrees(azRad);
    return true;
}

double HorizonOverlay::getCallOrder(StelModuleActionName actionName) const
{
    if (actionName == StelModule::ActionDraw)
        return 60.0;
    return 0.0;
}

void HorizonOverlay::draw(StelCore* core)
{
    if (!visible || geometry.empty())
        return;

    const StelProjectorP projector = core->getProjection(StelCore::FrameAltAz);
    StelPainter painter(projector);

    bool oldBlend = painter.getBlending();
    painter.setBlending(true);
    painter.setDepthTest(false);
    painter.setDepthMask(false);

    Vec3d viewDirection;
    const Vec2d viewportCenter = projector->getViewportCenter();
    if (!projector->unProject(viewportCenter[0], viewportCenter[1], viewDirection))
        viewDirection = Vec3d(0.0, 0.0, 1.0);
    viewDirection.normalize();

    const double apertureDeg = qBound(115.0, static_cast<double>(projector->getFov()) * 0.5 + 35.0, 170.0);
    const SphericalCap cameraCap(viewDirection, std::cos(qDegreesToRadians(apertureDeg)));

    if (drawFill && geometry.size() >= 2 && projector->getFov() > screenSpaceFillFovThresholdDeg)
    {
        if (!drawShaderFill(painter, projector))
            drawCpuScreenSpaceFill(painter, projector);
    }
    else if (drawFill && geometry.size() >= 2)
    {
        drawLegacyFill(painter, projector, cameraCap);
    }

    if (drawLine && geometry.size() >= 2)
    {
        painter.setColor(lineRgb[0], lineRgb[1], lineRgb[2], lineOpacity);
        painter.setLineSmooth(true);
        painter.setLineWidth(lineWidth);

        StelVertexArray lineArray(StelVertexArray::LineStrip);
        lineArray.vertex.reserve(static_cast<int>(geometry.size()));
        for (const RenderSample& sample : geometry)
            lineArray.vertex.append(sample.top);

        painter.drawGreatCircleArcs(lineArray, &cameraCap);

        painter.setLineWidth(1.0f);
        painter.setLineSmooth(false);
    }

    painter.setDepthMask(true);
    painter.setDepthTest(true);
    painter.setBlending(oldBlend);
}

std::string HorizonOverlay::defaultObstructionPath() const
{
    return moduleDirPath() + "/obstructions.txt";
}

std::string HorizonOverlay::configPath() const
{
    return moduleDirPath() + "/config.ini";
}

std::string HorizonOverlay::moduleDirPath() const
{
    return (StelFileMgr::getUserDir() + "/modules/HorizonOverlay").toStdString();
}

void HorizonOverlay::loadTranslator()
{
    localTranslator.reset();

    const QString localeName = StelApp::getInstance().getLocaleMgr().getAppLanguage();
    if (localeName.isEmpty())
        return;

    localTranslator = std::make_unique<QTranslator>();
    if (!loadHorizonOverlayTranslator(*localTranslator, localeName))
    {
        localTranslator.reset();
        return;
    }

    qDebug() << "[HorizonOverlay] Loaded translations for" << localeName;
}

void HorizonOverlay::reloadTranslator()
{
    loadTranslator();

    if (settingsDialog)
    {
        delete settingsDialog;
        settingsDialog = nullptr;
    }
}

QString HorizonOverlay::translateUi(const char* text) const
{
    if (localTranslator)
    {
        const QString translated = localTranslator->translate("", text);
        if (!translated.isEmpty())
            return translated;
    }

    return q_(text);
}

std::string HorizonOverlay::resolveObstructionPath(const std::string& path) const
{
    if (path.empty())
        return defaultObstructionPath();
    if (!path.empty() && path.front() == '/')
        return path;
    return moduleDirPath() + "/" + path;
}

void HorizonOverlay::loadSettings()
{
    std::ifstream file(configPath());
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line))
    {
        const std::size_t commentIndex = line.find('#');
        if (commentIndex != std::string::npos)
            line.erase(commentIndex);

        line = trim(line);
        if (line.empty() || line.front() == '[')
            continue;

        const std::size_t equalsIndex = line.find('=');
        if (equalsIndex == std::string::npos)
            continue;

        const std::string key = lowerAscii(trim(line.substr(0, equalsIndex)));
        const std::string value = trim(line.substr(equalsIndex + 1));

        if (key == "visible")
            visible = parseBool(value, visible);
        else if (key == "drawline")
            drawLine = parseBool(value, drawLine);
        else if (key == "drawfill")
            drawFill = parseBool(value, drawFill);
        else if (key == "lineopacity")
            lineOpacity = static_cast<float>(qBound(0.0, parseDouble(value, lineOpacity), 1.0));
        else if (key == "fillopacity")
            fillOpacity = static_cast<float>(qBound(0.0, parseDouble(value, fillOpacity), 1.0));
        else if (key == "linecolor")
            setLineColorFromHex(value);
        else if (key == "fillcolor")
            setFillColorFromHex(value);
        else if (key == "linewidth")
            lineWidth = static_cast<float>(qBound(0.5, parseDouble(value, lineWidth), 8.0));
        else if (key == "obstructionfile")
            obstructionPath = value;
    }
}

void HorizonOverlay::saveSettings() const
{
    std::ofstream file(configPath(), std::ios::trunc);
    if (!file.is_open())
        return;

    file << "[overlay]\n";
    file << "visible=" << (visible ? "true" : "false") << "\n";
    file << "drawLine=" << (drawLine ? "true" : "false") << "\n";
    file << "drawFill=" << (drawFill ? "true" : "false") << "\n";
    file << "lineColor=" << lineColorHex() << "\n";
    file << "fillColor=" << fillColorHex() << "\n";
    file << "lineOpacity=" << lineOpacity << "\n";
    file << "fillOpacity=" << fillOpacity << "\n";
    file << "lineWidth=" << lineWidth << "\n";
    file << "obstructionFile=" << obstructionPath << "\n";
}

void HorizonOverlay::setLineColorFromHex(const std::string& color)
{
    setRgbFromHex(color, lineRgb);
}

void HorizonOverlay::setFillColorFromHex(const std::string& color)
{
    setRgbFromHex(color, fillRgb);
}

std::string HorizonOverlay::lineColorHex() const
{
    return rgbToHex(lineRgb);
}

std::string HorizonOverlay::fillColorHex() const
{
    return rgbToHex(fillRgb);
}

void HorizonOverlay::createSettingsDialog()
{
    settingsDialog = new QDialog();
    settingsDialog->setWindowTitle(translateUi("Horizon Overlay Settings"));
    settingsDialog->setAttribute(Qt::WA_DeleteOnClose, false);
    settingsDialog->setStyleSheet(
        "QDialog { background-color: #2f3331; color: #e8e8e8; }"
        "QLabel, QCheckBox { color: #e8e8e8; }"
        "QLineEdit { background-color: #171817; color: #f2f2f2; border: 1px solid #4f5653; border-radius: 3px; padding: 4px; }"
        "QPushButton { background-color: #444b48; color: #f4f4f4; border: 1px solid #6a7470; border-radius: 4px; padding: 6px 10px; }"
        "QPushButton:hover { background-color: #56615c; }"
        "QPushButton:pressed { background-color: #38403c; }"
        "QPushButton:disabled { background-color: #343937; color: #8d9490; border-color: #4a504d; }"
        "QSlider::groove:horizontal { height: 6px; background: #1f2221; border: 1px solid #58615c; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #4aa3df; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 16px; margin: -5px 0; background: #eeeeee; border: 1px solid #202020; border-radius: 3px; }");

    auto* mainLayout = new QVBoxLayout(settingsDialog);
    auto* grid = new QGridLayout();
    mainLayout->addLayout(grid);

    auto* visibleBox = new QCheckBox(translateUi("Show overlay"));
    visibleBox->setChecked(visible);
    grid->addWidget(visibleBox, 0, 0, 1, 2);

    auto* fillBox = new QCheckBox(translateUi("Show filled obstruction area"));
    fillBox->setChecked(drawFill);
    grid->addWidget(fillBox, 1, 0, 1, 2);

    auto* lineBox = new QCheckBox(translateUi("Show outline"));
    lineBox->setChecked(drawLine);
    grid->addWidget(lineBox, 2, 0, 1, 2);

    auto makeSlider = [&](double value) {
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 100);
        slider->setValue(static_cast<int>(std::round(qBound(0.0, value, 1.0) * 100.0)));
        return slider;
    };

    auto* fillOpacityLabel = new QLabel(translateUi("Fill opacity: %1%").arg(static_cast<int>(std::round(fillOpacity * 100.0f))));
    auto* fillOpacitySlider = makeSlider(fillOpacity);
    grid->addWidget(fillOpacityLabel, 3, 0);
    grid->addWidget(fillOpacitySlider, 3, 1);

    auto* lineOpacityLabel = new QLabel(translateUi("Line opacity: %1%").arg(static_cast<int>(std::round(lineOpacity * 100.0f))));
    auto* lineOpacitySlider = makeSlider(lineOpacity);
    grid->addWidget(lineOpacityLabel, 4, 0);
    grid->addWidget(lineOpacitySlider, 4, 1);

    auto* fillColorButton = new QPushButton(translateUi("Fill color"));
    auto* lineColorButton = new QPushButton(translateUi("Line color"));
    auto applyButtonColor = [](QPushButton* button, const std::string& color) {
        const QColor background(QString::fromStdString(color));
        const QString textColor = background.lightness() > 150 ? "#101010" : "#f4f4f4";
        button->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; border: 1px solid #7c8580; border-radius: 4px; padding: 6px 10px; }")
            .arg(QString::fromStdString(color), textColor));
    };
    applyButtonColor(fillColorButton, fillColorHex());
    applyButtonColor(lineColorButton, lineColorHex());
    grid->addWidget(fillColorButton, 5, 0);
    grid->addWidget(lineColorButton, 5, 1);

    auto* fileEdit = new QLineEdit(QString::fromStdString(obstructionPath));
    auto* browseButton = new QPushButton(translateUi("Choose file..."));
    auto* reloadButton = new QPushButton(translateUi("Reload table"));
    grid->addWidget(new QLabel(translateUi("Obstruction table:")), 6, 0);
    grid->addWidget(fileEdit, 6, 1);
    grid->addWidget(browseButton, 7, 0);
    grid->addWidget(reloadButton, 7, 1);

    auto* closeButton = new QPushButton(translateUi("Close"));
    mainLayout->addWidget(closeButton);

    connect(visibleBox, &QCheckBox::toggled, settingsDialog, [this](bool checked) {
        visible = checked;
        saveSettings();
    });
    connect(fillBox, &QCheckBox::toggled, settingsDialog, [this](bool checked) {
        drawFill = checked;
        saveSettings();
    });
    connect(lineBox, &QCheckBox::toggled, settingsDialog, [this](bool checked) {
        drawLine = checked;
        saveSettings();
    });
    connect(fillOpacitySlider, &QSlider::valueChanged, settingsDialog, [this, fillOpacityLabel](int value) {
        fillOpacity = static_cast<float>(value) / 100.0f;
        fillOpacityLabel->setText(translateUi("Fill opacity: %1%").arg(value));
        saveSettings();
    });
    connect(lineOpacitySlider, &QSlider::valueChanged, settingsDialog, [this, lineOpacityLabel](int value) {
        lineOpacity = static_cast<float>(value) / 100.0f;
        lineOpacityLabel->setText(translateUi("Line opacity: %1%").arg(value));
        saveSettings();
    });
    connect(fillColorButton, &QPushButton::clicked, settingsDialog, [this, fillColorButton, applyButtonColor]() {
        const QColor initial(QString::fromStdString(fillColorHex()));
        const QColor color = QColorDialog::getColor(initial, settingsDialog, translateUi("Fill color"));
        if (!color.isValid())
            return;
        setFillColorFromHex(color.name(QColor::HexRgb).toStdString());
        applyButtonColor(fillColorButton, fillColorHex());
        saveSettings();
    });
    connect(lineColorButton, &QPushButton::clicked, settingsDialog, [this, lineColorButton, applyButtonColor]() {
        const QColor initial(QString::fromStdString(lineColorHex()));
        const QColor color = QColorDialog::getColor(initial, settingsDialog, translateUi("Line color"));
        if (!color.isValid())
            return;
        setLineColorFromHex(color.name(QColor::HexRgb).toStdString());
        applyButtonColor(lineColorButton, lineColorHex());
        saveSettings();
    });
    connect(fileEdit, &QLineEdit::editingFinished, settingsDialog, [this, fileEdit]() {
        obstructionPath = fileEdit->text().trimmed().toStdString();
        saveSettings();
    });
    connect(browseButton, &QPushButton::clicked, settingsDialog, [this, fileEdit]() {
        const QString selected = QFileDialog::getOpenFileName(
            settingsDialog,
            translateUi("Choose obstruction table"),
            QString::fromStdString(resolveObstructionPath(obstructionPath)),
            translateUi("Text files (*.txt *.hrz *.csv);;All files (*)"));
        if (selected.isEmpty())
            return;

        obstructionPath = selected.toStdString();
        fileEdit->setText(selected);
        saveSettings();
        reloadObstructionTable();
    });
    connect(reloadButton, &QPushButton::clicked, settingsDialog, [this, fileEdit]() {
        obstructionPath = fileEdit->text().trimmed().toStdString();
        saveSettings();
        reloadObstructionTable();
    });
    connect(closeButton, &QPushButton::clicked, settingsDialog, &QDialog::hide);

    settingsDialog->resize(520, 260);
}

bool HorizonOverlay::loadObstructionTable(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return false;

    std::vector<Sample> parsed;
    std::string line;

    while (std::getline(file, line))
    {
        const std::size_t commentIndex = line.find('#');
        if (commentIndex != std::string::npos)
            line.erase(commentIndex);
        std::replace(line.begin(), line.end(), ',', ' ');
        std::replace(line.begin(), line.end(), ';', ' ');

        std::istringstream stream(line);
        double az = 0.0;
        double alt = 0.0;
        if (!(stream >> az >> alt))
            continue;

        const double rawAz = az;
        const bool isExplicitFullCircle = rawAz > 0.0 && qFuzzyIsNull(std::fmod(rawAz, 360.0));

        az = std::fmod(rawAz, 360.0);
        if (az < 0.0)
            az += 360.0;
        if (isExplicitFullCircle)
            az = 360.0;

        const auto duplicate = std::find_if(parsed.begin(), parsed.end(), [az](const Sample& sample) {
            return qFuzzyCompare(sample.azDeg + 1.0, az + 1.0);
        });
        if (duplicate != parsed.end())
            *duplicate = { az, qBound(-90.0, alt, 90.0) };
        else
            parsed.push_back({ az, qBound(-90.0, alt, 90.0) });
    }

    if (parsed.size() < 2)
        return false;

    std::sort(parsed.begin(), parsed.end(), [](const Sample& a, const Sample& b) {
        return a.azDeg < b.azDeg;
    });

    samples.clear();
    for (const Sample& sample : parsed)
    {
        if (!samples.empty() && qFuzzyCompare(samples.back().azDeg + 1.0, sample.azDeg + 1.0))
            samples.back() = sample;
        else
            samples.push_back(sample);
    }

    if (!samples.empty() && samples.front().azDeg > 0.0)
        samples.insert(samples.begin(), { 0.0, samples.front().altDeg });
    if (!samples.empty() && samples.back().azDeg < 360.0)
        samples.push_back({ 360.0, samples.front().altDeg });

    return samples.size() >= 2;
}

void HorizonOverlay::useFallbackTable()
{
    samples = {
        {0.0, 16.0},
        {45.0, 24.0},
        {90.0, 34.0},
        {180.0, 10.0},
        {270.0, 46.0},
        {360.0, 16.0},
    };
}

void HorizonOverlay::rebuildGeometry()
{
    geometry.clear();

    if (samples.empty())
        return;

    appendGeometrySample(samples.front().azDeg, samples.front().altDeg);

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const Sample& previous = samples[i - 1];
        const Sample& current = samples[i];
        const double span = current.azDeg - previous.azDeg;
        if (span <= 0.0)
            continue;

        const int steps = std::max(1, static_cast<int>(std::ceil(span / maxAzStepDeg)));
        for (int step = 1; step <= steps; ++step)
        {
            const double t = static_cast<double>(step) / static_cast<double>(steps);
            const double az = previous.azDeg + span * t;
            const double alt = previous.altDeg + (current.altDeg - previous.altDeg) * t;
            appendGeometrySample(az, alt);
        }
    }
}

void HorizonOverlay::appendGeometrySample(double azDeg, double altDeg)
{
    const Vec3d top = altAzToVector(azDeg, altDeg);
    const Vec3d horizon = altAzToVector(azDeg, 0.0);

    geometry.push_back({ azDeg, horizon, top });
}

Vec3d HorizonOverlay::altAzToVector(double azDeg, double altDeg) const
{
    Vec3d result;

    const double azRad = qDegreesToRadians(azDeg);
    const double altRad = qDegreesToRadians(altDeg);

    // Stellarium FrameAltAz uses +x=south, +y=east, +z=zenith.
    // The input table uses standard compass azimuth: 0=north, 90=east.
    const double stelLongitude = pi - azRad;
    StelUtils::spheToRect(stelLongitude, altRad, result);
    return result;
}
