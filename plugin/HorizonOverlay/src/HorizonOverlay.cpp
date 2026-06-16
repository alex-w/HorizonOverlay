#include "HorizonOverlay.hpp"

#include "StelApp.hpp"
#include "StelCore.hpp"
#include "StelFileMgr.hpp"
#include "StelGui.hpp"
#include "StelGuiItems.hpp"
#include "StelLocaleMgr.hpp"
#include "StelPainter.hpp"
#include "StelProjector.hpp"
#include "StelOpenGL.hpp"
#include "StelTranslator.hpp"
#include "StelUtils.hpp"
#include "StelVertexArray.hpp"
#include "gui/HorizonOverlayDialog.hpp"

#include <QDebug>
#include <QCoreApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPoint>
#include <QPixmap>
#include <QStringList>
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
#include <functional>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <utility>

namespace
{
constexpr double maxAzStepDeg = 0.5;
constexpr double editMinAzSpacingDeg = 3.0;
constexpr double autoSimplifyToleranceDeg = 0.45;
constexpr double autoSmoothResampleStepDeg = 3.0;
// Legacy 3D fill can cross projection boundaries before the nominal 180 degree
// limit in wide cylindrical/fisheye views, so switch to the screen mask early.
constexpr double screenSpaceFillFovThresholdDeg = 150.0;
constexpr int screenSpaceFillStepPx = 8;
constexpr double screenSpaceLineMaxJumpPx = 180.0;
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

std::string stripQuotes(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
        return value.substr(1, value.size() - 2);
    return value;
}

std::string normalizeColumnName(std::string value)
{
    value = lowerAscii(stripQuotes(value));
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char c : value)
    {
        if (std::isalnum(c))
            normalized.push_back(static_cast<char>(c));
    }
    return normalized;
}

std::vector<std::string> splitObstructionRow(std::string line)
{
    const std::size_t commentIndex = line.find('#');
    if (commentIndex != std::string::npos)
        line.erase(commentIndex);

    const char delimiter = line.find(',') != std::string::npos ? ',' : (line.find(';') != std::string::npos ? ';' : '\0');
    if (delimiter != '\0')
    {
        std::vector<std::string> tokens;
        std::string token;
        bool inQuotes = false;
        char quoteChar = '\0';

        for (const char c : line)
        {
            if ((c == '"' || c == '\'') && (!inQuotes || c == quoteChar))
            {
                inQuotes = !inQuotes;
                quoteChar = inQuotes ? c : '\0';
                token.push_back(c);
                continue;
            }
            if (c == delimiter && !inQuotes)
            {
                tokens.push_back(stripQuotes(token));
                token.clear();
                continue;
            }
            token.push_back(c);
        }
        tokens.push_back(stripQuotes(token));
        return tokens;
    }

    for (char& c : line)
    {
        if (c == '\t')
            c = ' ';
    }

    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token)
        tokens.push_back(stripQuotes(token));
    return tokens;
}

bool parseNumericToken(const std::string& token, double& value)
{
    const std::string trimmed = stripQuotes(token);
    if (trimmed.empty())
        return false;

    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(trimmed.c_str(), &end);
    if (!end || end == trimmed.c_str() || errno == ERANGE)
        return false;

    value = parsed;
    return true;
}

bool isAzimuthColumn(const std::string& normalized)
{
    return normalized == "az" ||
           normalized == "azi" ||
           normalized == "azimuth" ||
           normalized == "bearing" ||
           normalized == "direction" ||
           normalized == "compass" ||
           normalized == "heading";
}

bool isAltitudeColumn(const std::string& normalized)
{
    return normalized == "alt" ||
           normalized == "altitude" ||
           normalized == "elevation" ||
           normalized == "elev" ||
           normalized == "height" ||
           normalized == "horizon" ||
           normalized == "horizonalt" ||
           normalized == "obstruction" ||
           normalized == "obstructionalt" ||
           normalized == "obstructionaltitude";
}

bool detectObstructionHeader(const std::vector<std::string>& tokens, int& azColumn, int& altColumn)
{
    int detectedAz = -1;
    int detectedAlt = -1;
    for (int i = 0; i < static_cast<int>(tokens.size()); ++i)
    {
        const std::string normalized = normalizeColumnName(tokens[static_cast<std::size_t>(i)]);
        if (detectedAz < 0 && isAzimuthColumn(normalized))
            detectedAz = i;
        if (detectedAlt < 0 && isAltitudeColumn(normalized))
            detectedAlt = i;
    }

    if (detectedAz < 0 || detectedAlt < 0 || detectedAz == detectedAlt)
        return false;

    azColumn = detectedAz;
    altColumn = detectedAlt;
    return true;
}

bool parseObstructionSample(const std::vector<std::string>& tokens, int azColumn, int altColumn, double& az, double& alt)
{
    if (tokens.empty())
        return false;

    if (azColumn >= 0 && altColumn >= 0)
    {
        const int maxColumn = std::max(azColumn, altColumn);
        if (static_cast<int>(tokens.size()) <= maxColumn)
            return false;
        return parseNumericToken(tokens[static_cast<std::size_t>(azColumn)], az) &&
               parseNumericToken(tokens[static_cast<std::size_t>(altColumn)], alt);
    }

    bool foundAz = false;
    for (const std::string& token : tokens)
    {
        double value = 0.0;
        if (!parseNumericToken(token, value))
            continue;

        if (!foundAz)
        {
            az = value;
            foundAz = true;
        }
        else
        {
            alt = value;
            return true;
        }
    }

    return false;
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

QString normalizedSupportedLocale(QString localeName)
{
    localeName.replace('-', '_');
    const QString baseName = localeName.section('_', 0, 0);

    if (localeName == "zh_CN" || localeName == "zh_Hans" || localeName == "zh_SG" || localeName == "zh")
        return "zh_CN";
    if (localeName == "zh_TW" || localeName == "zh_Hant" || localeName == "zh_HK" || localeName == "zh_MO")
        return "zh_TW";
    if (localeName == "pt_BR" || localeName == "pt")
        return "pt_BR";

    static const QStringList supportedBaseLocales = {
        "de",
        "es",
        "fr",
        "it",
        "ja",
        "ko",
        "ru",
    };

    if (supportedBaseLocales.contains(baseName))
        return baseName;

    return {};
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

double normalizeAzimuth(double azDeg, bool preserveExplicitFullCircle)
{
    const bool isExplicitFullCircle = azDeg > 0.0 && qFuzzyIsNull(std::fmod(azDeg, 360.0));

    azDeg = std::fmod(azDeg, 360.0);
    if (azDeg < 0.0)
        azDeg += 360.0;
    if (preserveExplicitFullCircle && isExplicitFullCircle)
        azDeg = 360.0;

    return azDeg;
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
    Q_INIT_RESOURCE(HorizonOverlay);

    StelPluginInfo info;
    info.id = "HorizonOverlay";
    info.displayedName = N_("Horizon Overlay");
    info.authors = "Song Zihan / Codex";
    info.contact = "";
    info.description = N_("Draws a transparent local obstruction horizon overlay above the normal Stellarium landscape.");
    info.version = HORIZONOVERLAY_PLUGIN_VERSION;
    info.license = HORIZONOVERLAY_PLUGIN_LICENSE;
    return info;
}

HorizonOverlay::HorizonOverlay()
    : visible(true)
    , showToolbarButton(true)
    , toolbarButtonInBar(false)
    , editMode(false)
    , drawLine(true)
    , drawFill(true)
    , lineOpacity(0.95f)
    , fillOpacity(0.22f)
    , lineWidth(2.0f)
    , lineRgb{1.0f, 0.8f, 0.4f}
    , fillRgb{1.0f, 0.48f, 0.09f}
    , obstructionPath("obstructions.txt")
    , settingsDialog(nullptr)
    , toolbarButton(nullptr)
    , localTranslatorInstalled(false)
    , fillShaderVars{ -1, -1, -1, -1 }
{
    resetPerformanceStats();
    setObjectName("HorizonOverlay");
}

HorizonOverlay::~HorizonOverlay()
{
    delete settingsDialog;
    if (localTranslatorInstalled && localTranslator)
        QCoreApplication::removeTranslator(localTranslator.get());
}

void HorizonOverlay::init()
{
    Q_INIT_RESOURCE(HorizonOverlay);

    qDebug() << "[HorizonOverlay] init";

    loadTranslator();
    connect(&StelApp::getInstance(), &StelApp::languageChanged, this, [this]() {
        reloadTranslator();
    });
    loadSettings();
    reloadObstructionTable();

    addAction("actionShow_HorizonOverlay", N_("Horizon Overlay"), N_("Show local obstruction horizon overlay"), "horizonOverlayVisible");
    addAction("actionShow_HorizonOverlay_dialog", N_("Horizon Overlay"), N_("Show settings dialog"), this, [this]() {
        configureGui(true);
    });

    setFlagShowToolbarButton(showToolbarButton);
}

void HorizonOverlay::update(double deltaTime)
{
    Q_UNUSED(deltaTime)

    if (showToolbarButton && !toolbarButtonInBar)
        setFlagShowToolbarButton(true);
}

bool HorizonOverlay::getOverlayVisible() const
{
    return visible;
}

void HorizonOverlay::setOverlayVisible(bool value)
{
    if (visible == value)
        return;

    visible = value;
    saveSettings();
    emit overlayVisibleChanged(visible);
}

bool HorizonOverlay::getFlagShowToolbarButton() const
{
    return showToolbarButton;
}

void HorizonOverlay::setFlagShowToolbarButton(bool value)
{
    StelGui* gui = dynamic_cast<StelGui*>(StelApp::getInstance().getGui());
    if (gui)
    {
        if (value)
        {
            ensureToolbarButton();
            if (toolbarButton && !toolbarButtonInBar)
            {
                gui->getButtonBar()->addButton(toolbarButton, "065-pluginsGroup");
                toolbarButtonInBar = true;
            }
        }
        else if (toolbarButtonInBar)
        {
            gui->getButtonBar()->hideButton("actionShow_HorizonOverlay");
            toolbarButtonInBar = false;
        }
    }

    if (showToolbarButton == value)
        return;

    showToolbarButton = value;
    saveSettings();
    emit flagShowToolbarButtonChanged(showToolbarButton);
}

bool HorizonOverlay::getDrawLine() const
{
    return drawLine;
}

void HorizonOverlay::setDrawLine(bool value)
{
    if (drawLine == value)
        return;

    drawLine = value;
    saveSettings();
    emit drawLineChanged(drawLine);
}

bool HorizonOverlay::getDrawFill() const
{
    return drawFill;
}

void HorizonOverlay::setDrawFill(bool value)
{
    if (drawFill == value)
        return;

    drawFill = value;
    saveSettings();
    emit drawFillChanged(drawFill);
}

bool HorizonOverlay::getEditMode() const
{
    return editMode;
}

void HorizonOverlay::setEditMode(bool value)
{
    if (editMode == value)
        return;

    editMode = value;
    emit editModeChanged(editMode);
}

float HorizonOverlay::getLineOpacity() const
{
    return lineOpacity;
}

void HorizonOverlay::setLineOpacity(float value)
{
    value = qBound(0.0f, value, 1.0f);
    if (qFuzzyCompare(lineOpacity, value))
        return;

    lineOpacity = value;
    saveSettings();
    emit lineOpacityChanged(lineOpacity);
}

float HorizonOverlay::getFillOpacity() const
{
    return fillOpacity;
}

void HorizonOverlay::setFillOpacity(float value)
{
    value = qBound(0.0f, value, 1.0f);
    if (qFuzzyCompare(fillOpacity, value))
        return;

    fillOpacity = value;
    saveSettings();
    emit fillOpacityChanged(fillOpacity);
}

float HorizonOverlay::getLineWidth() const
{
    return lineWidth;
}

void HorizonOverlay::setLineWidth(float value)
{
    value = qBound(0.5f, value, 8.0f);
    if (qFuzzyCompare(lineWidth, value))
        return;

    lineWidth = value;
    saveSettings();
    emit lineWidthChanged(lineWidth);
}

QColor HorizonOverlay::getLineColor() const
{
    return QColor(QString::fromStdString(lineColorHex()));
}

void HorizonOverlay::setLineColor(const QColor& color)
{
    if (!color.isValid())
        return;

    const std::string hex = color.name(QColor::HexRgb).toStdString();
    if (hex == lineColorHex())
        return;

    setLineColorFromHex(hex);
    saveSettings();
    emit lineColorChanged(getLineColor());
}

QColor HorizonOverlay::getFillColor() const
{
    return QColor(QString::fromStdString(fillColorHex()));
}

void HorizonOverlay::setFillColor(const QColor& color)
{
    if (!color.isValid())
        return;

    const std::string hex = color.name(QColor::HexRgb).toStdString();
    if (hex == fillColorHex())
        return;

    setFillColorFromHex(hex);
    saveSettings();
    emit fillColorChanged(getFillColor());
}

QString HorizonOverlay::getObstructionPath() const
{
    return QString::fromStdString(obstructionPath);
}

QString HorizonOverlay::getResolvedObstructionPath() const
{
    return QString::fromStdString(resolveObstructionPath(obstructionPath));
}

void HorizonOverlay::setObstructionPath(const QString& path)
{
    const std::string normalizedPath = path.trimmed().toStdString();
    if (obstructionPath == normalizedPath)
        return;

    obstructionPath = normalizedPath;
    saveSettings();
    emit obstructionPathChanged(QString::fromStdString(obstructionPath));
}

QString HorizonOverlay::getPerformanceText() const
{
    return translateUi("Performance: %1 ms last, %2 ms avg, %3 ms max | FoV %4 deg | %5 | %6 samples")
        .arg(performanceLastMs, 0, 'f', 3)
        .arg(performanceLastAvgMs, 0, 'f', 3)
        .arg(performanceLastMaxMs, 0, 'f', 3)
        .arg(performanceLastFovDeg, 0, 'f', 1)
        .arg(QString::fromLatin1(performanceLastFillPath))
        .arg(static_cast<qulonglong>(performanceLastSampleCount));
}

void HorizonOverlay::createSettingsDialog()
{
    if (!settingsDialog)
        settingsDialog = new HorizonOverlayDialog(this);
}

void HorizonOverlay::ensureToolbarButton()
{
    if (toolbarButton)
        return;

    toolbarButton = new StelButton(
        nullptr,
        QPixmap(":/HorizonOverlay/bt_HorizonOverlay_On.png"),
        QPixmap(":/HorizonOverlay/bt_HorizonOverlay_Off.png"),
        QPixmap(":/graphicGui/miscGlow32x32.png"),
        "actionShow_HorizonOverlay",
        false,
        "actionShow_HorizonOverlay_dialog");
}

bool HorizonOverlay::configureGui(bool show)
{
    if (!show)
    {
        if (settingsDialog)
            settingsDialog->setVisible(false);
        return true;
    }

    if (!settingsDialog)
        createSettingsDialog();

    settingsDialog->setVisible(true);
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

bool HorizonOverlay::setupCurrentVAO()
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context || !context->functions() || !vbo || !vbo->isCreated())
        return false;

    auto& gl = *context->functions();
    vbo->bind();
    gl.glVertexAttribPointer(fullscreenQuadVertexAttribIndex, fullscreenQuadCoordsPerVertex, GL_FLOAT, false, 0, nullptr);
    vbo->release();
    gl.glEnableVertexAttribArray(fullscreenQuadVertexAttribIndex);
    return true;
}

bool HorizonOverlay::bindVAO()
{
    if (vao && vao->isCreated())
    {
        vao->bind();
        return true;
    }

    return setupCurrentVAO();
}

void HorizonOverlay::releaseVAO()
{
    if (vao && vao->isCreated())
    {
        vao->release();
        return;
    }

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context || !context->functions())
        return;

    auto& gl = *context->functions();
    gl.glDisableVertexAttribArray(fullscreenQuadVertexAttribIndex);
}

bool HorizonOverlay::ensureShaderProgram(const StelProjectorP& projector)
{
    if (displaySamples.empty() || displaySamples.size() > maxShaderObstructionSamples)
        return false;

    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context || !context->functions())
        return false;

    if (!vbo)
    {
        auto& gl = *context->functions();
        vbo = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!vbo->create() || !vbo->bind())
        {
            vbo.reset();
            return false;
        }

        const GLfloat vertices[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f,
        };
        gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
        vbo->release();

        vao = std::make_unique<QOpenGLVertexArrayObject>();
        if (!vao->create())
            vao.reset();

        if (vao && vao->isCreated())
            vao->bind();

        if (!setupCurrentVAO())
        {
            if (vao && vao->isCreated())
                vao->release();
            vao.reset();
            vbo.reset();
            return false;
        }

        if (vao && vao->isCreated())
            vao->release();
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
    {
        fillShaderProgram.reset();
        return false;
    }

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
    {
        fillShaderProgram.reset();
        return false;
    }

    fillShaderProgram->bindAttributeLocation("vertex", fullscreenQuadVertexAttribIndex);
    if (!StelPainter::linkProg(fillShaderProgram.get(), "HorizonOverlay fill shader"))
    {
        fillShaderProgram.reset();
        return false;
    }

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
    shaderSamples.reserve(static_cast<int>(displaySamples.size()));
    for (const Sample& sample : displaySamples)
        shaderSamples.push_back(QVector2D(static_cast<float>(sample.azDeg), static_cast<float>(sample.altDeg)));

    fillShaderProgram->bind();
    fillShaderProgram->setUniformValue(fillShaderVars.projectionMatrixInverse, projector->getProjectionMatrix().toQMatrix().inverted());
    fillShaderProgram->setUniformValue(fillShaderVars.fillColor, QVector4D(fillRgb[0], fillRgb[1], fillRgb[2], fillOpacity));
    fillShaderProgram->setUniformValue(fillShaderVars.sampleCount, static_cast<int>(shaderSamples.size()));
    fillShaderProgram->setUniformValueArray(fillShaderVars.samples, shaderSamples.constData(), shaderSamples.size());
    projector->setUnProjectUniforms(*fillShaderProgram);

    painter.glFuncs()->glEnable(GL_BLEND);
    painter.glFuncs()->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!bindVAO())
    {
        fillShaderProgram->release();
        return false;
    }
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

void HorizonOverlay::drawScreenSpaceLine(StelPainter& painter, const StelProjectorP& projector) const
{
    if (geometry.size() < 2)
        return;

    std::vector<float> lineVertices;
    lineVertices.reserve(geometry.size() * 4);

    bool hasPrevious = false;
    ScreenPoint previous{ 0.0f, 0.0f };
    const double maxJumpSquared = screenSpaceLineMaxJumpPx * screenSpaceLineMaxJumpPx;

    for (const RenderSample& sample : geometry)
    {
        Vec3d screenPos;
        if (!projector->project(sample.top, screenPos) ||
            !std::isfinite(screenPos[0]) ||
            !std::isfinite(screenPos[1]))
        {
            hasPrevious = false;
            continue;
        }

        const ScreenPoint current{ static_cast<float>(screenPos[0]), static_cast<float>(screenPos[1]) };
        if (hasPrevious)
        {
            const double dx = static_cast<double>(current.x - previous.x);
            const double dy = static_cast<double>(current.y - previous.y);
            if (dx * dx + dy * dy <= maxJumpSquared)
            {
                lineVertices.push_back(previous.x);
                lineVertices.push_back(previous.y);
                lineVertices.push_back(current.x);
                lineVertices.push_back(current.y);
            }
        }

        previous = current;
        hasPrevious = true;
    }

    if (lineVertices.empty())
        return;

    painter.setColor(lineRgb[0], lineRgb[1], lineRgb[2], lineOpacity);
    painter.setLineSmooth(true);
    painter.setLineWidth(lineWidth);
    painter.enableClientStates(true);
    painter.setVertexPointer(2, GL_FLOAT, lineVertices.data());
    painter.drawFromArray(StelPainter::Lines, static_cast<int>(lineVertices.size() / 2), 0, false);
    painter.enableClientStates(false);
    painter.setLineWidth(1.0f);
    painter.setLineSmooth(false);
}

double HorizonOverlay::obstructionAltitudeAt(double azDeg) const
{
    if (displaySamples.empty())
        return 0.0;

    azDeg = std::fmod(azDeg, 360.0);
    if (azDeg < 0.0)
        azDeg += 360.0;

    if (displaySamples.size() == 1)
        return displaySamples.front().altDeg;

    for (std::size_t i = 1; i < displaySamples.size(); ++i)
    {
        const Sample& previous = displaySamples[i - 1];
        const Sample& current = displaySamples[i];
        if (azDeg < previous.azDeg || azDeg > current.azDeg)
            continue;

        const double span = current.azDeg - previous.azDeg;
        if (span <= 0.0)
            return current.altDeg;

        const double t = (azDeg - previous.azDeg) / span;
        return previous.altDeg + (current.altDeg - previous.altDeg) * t;
    }

    return displaySamples.back().altDeg;
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

void HorizonOverlay::resetPerformanceStats()
{
    performanceWindowFrames = 0;
    performanceWindowMs = 0.0;
    performanceWindowMaxMs = 0.0;
    performanceLastMs = 0.0;
    performanceLastAvgMs = 0.0;
    performanceLastMaxMs = 0.0;
    performanceLastFovDeg = 0.0;
    performanceLastFillPath = "not drawn";
    performanceLastSampleCount = 0;
}

void HorizonOverlay::recordPerformanceSample(double elapsedMs, const char* fillPath, double fovDeg)
{
    performanceLastMs = elapsedMs;
    performanceLastFovDeg = fovDeg;
    performanceLastFillPath = fillPath;
    performanceLastSampleCount = displaySamples.size();
    performanceWindowMs += elapsedMs;
    performanceWindowMaxMs = std::max(performanceWindowMaxMs, elapsedMs);
    ++performanceWindowFrames;

    if (performanceWindowFrames < 30)
        return;

    performanceLastAvgMs = performanceWindowMs / static_cast<double>(performanceWindowFrames);
    performanceLastMaxMs = performanceWindowMaxMs;
    performanceWindowFrames = 0;
    performanceWindowMs = 0.0;
    performanceWindowMaxMs = 0.0;
    updatePerformanceLabel();
}

void HorizonOverlay::updatePerformanceLabel()
{
    if (!settingsDialog)
        return;

    settingsDialog->updatePerformanceText();
}

double HorizonOverlay::getCallOrder(StelModuleActionName actionName) const
{
    if (actionName == StelModule::ActionDraw)
        return 60.0;
    if (actionName == StelModule::ActionHandleMouseClicks || actionName == StelModule::ActionHandleMouseMoves)
        return -11.0;
    return 0.0;
}

void HorizonOverlay::handleMouseClicks(QMouseEvent* event)
{
    if (!event || !editMode)
    {
        if (event)
            event->setAccepted(false);
        return;
    }

    const bool isEditButton = event->button() == Qt::LeftButton || event->button() == Qt::RightButton;
    const bool hasEditModifier = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!isEditButton || !hasEditModifier)
    {
        event->setAccepted(false);
        return;
    }

    if (event->type() != QEvent::MouseButtonPress)
    {
        event->setAccepted(true);
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const double x = event->position().x();
    const double y = event->position().y();
#else
    const double x = event->x();
    const double y = event->y();
#endif

    if (event->button() == Qt::LeftButton)
    {
        addSampleFromScreen(x, y);
        event->setAccepted(true);
        return;
    }
    else if (event->button() == Qt::RightButton)
    {
        removeNearestSample(x, y);
        event->setAccepted(true);
        return;
    }

    event->setAccepted(true);
}

bool HorizonOverlay::handleMouseMoves(int x, int y, Qt::MouseButtons buttons)
{
    if (!editMode)
        return false;

    const bool hasEditModifier = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    if (!hasEditModifier)
        return false;

    if (buttons.testFlag(Qt::LeftButton))
    {
        addSampleFromScreen(x, y);
        return true;
    }

    if (buttons.testFlag(Qt::RightButton))
    {
        removeNearestSample(x, y);
        return true;
    }

    return false;
}

void HorizonOverlay::draw(StelCore* core)
{
    if (!visible || geometry.empty())
        return;

    const StelProjectorP projector = core->getProjection(StelCore::FrameAltAz);
    if (!projector)
        return;

    QElapsedTimer performanceTimer;
    performanceTimer.start();

    StelPainter painter(projector);
    const double fovDeg = projector->getFov();
    const bool useScreenSpaceOverlay = fovDeg > screenSpaceFillFovThresholdDeg;
    const char* fillPath = "line only";

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

    if (drawFill && geometry.size() >= 2 && useScreenSpaceOverlay)
    {
        if (drawShaderFill(painter, projector))
            fillPath = "shader mask";
        else
        {
            drawCpuScreenSpaceFill(painter, projector);
            fillPath = "cpu mask";
        }
    }
    else if (drawFill && geometry.size() >= 2)
    {
        drawLegacyFill(painter, projector, cameraCap);
        fillPath = "legacy mesh";
    }
    else if (!drawLine)
    {
        fillPath = "disabled";
    }

    if (drawLine && geometry.size() >= 2)
    {
        if (useScreenSpaceOverlay)
        {
            drawScreenSpaceLine(painter, projector);
        }
        else
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
    }

    painter.setDepthMask(true);
    painter.setDepthTest(true);
    painter.setBlending(oldBlend);

    recordPerformanceSample(static_cast<double>(performanceTimer.nsecsElapsed()) / 1000000.0, fillPath, fovDeg);
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
    if (localTranslatorInstalled && localTranslator)
    {
        QCoreApplication::removeTranslator(localTranslator.get());
        localTranslatorInstalled = false;
    }

    localTranslator.reset();

    const QString localeName = currentTranslationLocale();
    if (localeName.isEmpty())
        return;

    localTranslator = std::make_unique<QTranslator>();
    if (!loadHorizonOverlayTranslator(*localTranslator, localeName))
    {
        localTranslator.reset();
        return;
    }

    QCoreApplication::installTranslator(localTranslator.get());
    localTranslatorInstalled = true;
    qDebug() << "[HorizonOverlay] Loaded translations for" << localeName;
}

void HorizonOverlay::reloadTranslator()
{
    loadTranslator();

    if (settingsDialog)
        settingsDialog->retranslate();
}

QString HorizonOverlay::currentTranslationLocale() const
{
    return normalizedSupportedLocale(StelApp::getInstance().getLocaleMgr().getAppLanguage());
}

QString HorizonOverlay::translateUi(const char* text) const
{
    if (localTranslator)
    {
        const QString translated = localTranslator->translate("", text);
        if (!translated.isEmpty())
            return translated;
    }

    return QString::fromUtf8(text);
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
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == '[')
            continue;

        const std::size_t equalsIndex = line.find('=');
        if (equalsIndex == std::string::npos)
            continue;

        const std::string key = lowerAscii(trim(line.substr(0, equalsIndex)));
        const std::string value = trim(line.substr(equalsIndex + 1));

        if (key == "visible")
            visible = parseBool(value, visible);
        else if (key == "showtoolbarbutton")
            showToolbarButton = parseBool(value, showToolbarButton);
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
    {
        qWarning() << "[HorizonOverlay] Could not save settings:" << QString::fromStdString(configPath());
        return;
    }

    file << "[overlay]\n";
    file << "visible=" << (visible ? "true" : "false") << "\n";
    file << "showToolbarButton=" << (showToolbarButton ? "true" : "false") << "\n";
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

void HorizonOverlay::sortAndNormalizeSamples()
{
    for (Sample& sample : samples)
    {
        sample.azDeg = normalizeAzimuth(sample.azDeg, true);
        sample.altDeg = qBound(-90.0, sample.altDeg, 90.0);
    }

    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) {
        return a.azDeg < b.azDeg;
    });

    std::vector<Sample> normalized;
    normalized.reserve(samples.size() + 2);
    for (const Sample& sample : samples)
    {
        if (!normalized.empty() && qFuzzyCompare(normalized.back().azDeg + 1.0, sample.azDeg + 1.0))
            normalized.back() = sample;
        else
            normalized.push_back(sample);
    }

    samples = std::move(normalized);
    if (!samples.empty() && samples.front().azDeg > 0.0)
        samples.insert(samples.begin(), { 0.0, samples.front().altDeg });
    if (!samples.empty() && samples.back().azDeg < 360.0)
        samples.push_back({ 360.0, samples.front().altDeg });
}

void HorizonOverlay::addSample(double azDeg, double altDeg)
{
    azDeg = normalizeAzimuth(azDeg, false);
    altDeg = qBound(-90.0, altDeg, 90.0);

    const auto duplicateEnd = std::remove_if(samples.begin(), samples.end(), [azDeg](const Sample& sample) {
        const double sampleAz = normalizeAzimuth(sample.azDeg, false);
        return std::abs(sampleAz - azDeg) < editMinAzSpacingDeg ||
               std::abs(sampleAz - azDeg + 360.0) < editMinAzSpacingDeg ||
               std::abs(sampleAz - azDeg - 360.0) < editMinAzSpacingDeg;
    });
    samples.erase(duplicateEnd, samples.end());
    samples.push_back({ azDeg, altDeg });

    sortAndNormalizeSamples();
    rebuildGeometry();
}

bool HorizonOverlay::addSampleFromScreen(double screenX, double screenY)
{
    const StelProjectorP projector = StelApp::getInstance().getCore()->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
    Vec3d direction;
    if (!projector || !projector->unProject(screenX, screenY, direction))
        return false;

    double azDeg = 0.0;
    double altDeg = 0.0;
    if (!vectorToAltAz(direction, azDeg, altDeg))
        return false;

    addSample(azDeg, altDeg);
    return true;
}

bool HorizonOverlay::removeNearestSample(double screenX, double screenY)
{
    if (samples.empty())
        return false;

    const StelProjectorP projector = StelApp::getInstance().getCore()->getProjection(StelCore::FrameAltAz, StelCore::RefractionOff);
    if (!projector)
        return false;

    double bestDistanceSquared = 80.0 * 80.0;
    double targetAz = -1.0;

    for (const Sample& sample : samples)
    {
        Vec3d screenPos;
        if (!projector->project(altAzToVector(sample.azDeg, sample.altDeg), screenPos))
            continue;

        const double dx = screenPos[0] - screenX;
        const double dy = screenPos[1] - screenY;
        const double distanceSquared = dx * dx + dy * dy;
        if (distanceSquared < bestDistanceSquared)
        {
            bestDistanceSquared = distanceSquared;
            targetAz = normalizeAzimuth(sample.azDeg, false);
        }
    }

    if (targetAz < 0.0)
        return false;

    const auto newEnd = std::remove_if(samples.begin(), samples.end(), [targetAz](const Sample& sample) {
        const double sampleAz = normalizeAzimuth(sample.azDeg, false);
        return std::abs(sampleAz - targetAz) < 0.25 || std::abs(sampleAz - targetAz + 360.0) < 0.25 || std::abs(sampleAz - targetAz - 360.0) < 0.25;
    });

    if (newEnd == samples.end())
        return false;

    samples.erase(newEnd, samples.end());
    sortAndNormalizeSamples();
    rebuildGeometry();
    return true;
}

bool HorizonOverlay::saveObstructionTable() const
{
    const std::string path = resolveObstructionPath(obstructionPath);
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        qWarning() << "[HorizonOverlay] Could not save obstruction table:" << QString::fromStdString(path);
        return false;
    }

    file << "# HorizonOverlay obstruction table\n";
    file << "# Az Alt\n";
    file << std::fixed << std::setprecision(3);
    const std::vector<Sample>& samplesToSave = displaySamples.empty() ? samples : displaySamples;
    for (const Sample& sample : samplesToSave)
        file << sample.azDeg << ' ' << sample.altDeg << '\n';

    qInfo() << "[HorizonOverlay] Saved obstruction table:" << QString::fromStdString(path);
    return true;
}

void HorizonOverlay::clearSamples()
{
    samples.clear();
    displaySamples.clear();
    geometry.clear();
}

bool HorizonOverlay::loadObstructionTable(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return false;

    std::vector<Sample> parsed;
    std::string line;
    int azColumn = -1;
    int altColumn = -1;

    while (std::getline(file, line))
    {
        const std::vector<std::string> tokens = splitObstructionRow(line);
        if (tokens.empty())
            continue;

        if (detectObstructionHeader(tokens, azColumn, altColumn))
            continue;

        double az = 0.0;
        double alt = 0.0;
        if (!parseObstructionSample(tokens, azColumn, altColumn, az, alt))
            continue;

        az = normalizeAzimuth(az, true);

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

    samples = std::move(parsed);
    sortAndNormalizeSamples();

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

void HorizonOverlay::rebuildDisplaySamples()
{
    displaySamples.clear();
    if (samples.empty())
        return;

    std::vector<Sample> cleaned;
    cleaned.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const Sample& sample = samples[i];
        if (cleaned.empty())
        {
            cleaned.push_back(sample);
            continue;
        }

        const bool isLast = i + 1 == samples.size();
        const double span = sample.azDeg - cleaned.back().azDeg;
        if (!isLast && span > 0.0 && span < editMinAzSpacingDeg)
        {
            cleaned.back().altDeg = (cleaned.back().altDeg + sample.altDeg) * 0.5;
            continue;
        }

        cleaned.push_back(sample);
    }

    if (cleaned.size() <= 3)
    {
        displaySamples = std::move(cleaned);
        return;
    }

    std::vector<bool> keep(cleaned.size(), false);
    keep.front() = true;
    keep.back() = true;

    std::function<void(std::size_t, std::size_t)> simplify = [&](std::size_t first, std::size_t last) {
        if (last <= first + 1)
            return;

        const Sample& a = cleaned[first];
        const Sample& b = cleaned[last];
        const double span = b.azDeg - a.azDeg;
        if (span <= 0.0)
            return;

        double maxError = 0.0;
        std::size_t maxIndex = first;
        for (std::size_t i = first + 1; i < last; ++i)
        {
            const double t = (cleaned[i].azDeg - a.azDeg) / span;
            const double expectedAlt = a.altDeg + (b.altDeg - a.altDeg) * t;
            const double error = std::abs(cleaned[i].altDeg - expectedAlt);
            if (error > maxError)
            {
                maxError = error;
                maxIndex = i;
            }
        }

        if (maxError <= autoSimplifyToleranceDeg)
            return;

        keep[maxIndex] = true;
        simplify(first, maxIndex);
        simplify(maxIndex, last);
    };
    simplify(0, cleaned.size() - 1);

    std::vector<Sample> simplified;
    simplified.reserve(cleaned.size());
    for (std::size_t i = 0; i < cleaned.size(); ++i)
    {
        if (keep[i])
            simplified.push_back(cleaned[i]);
    }

    displaySamples = simplified;
    if (displaySamples.size() <= 3)
        return;

    std::vector<Sample> smoothed = displaySamples;
    for (std::size_t i = 1; i + 1 < displaySamples.size(); ++i)
    {
        const double leftSpan = displaySamples[i].azDeg - displaySamples[i - 1].azDeg;
        const double rightSpan = displaySamples[i + 1].azDeg - displaySamples[i].azDeg;
        if (leftSpan <= 0.0 || rightSpan <= 0.0 || leftSpan > 8.0 || rightSpan > 8.0)
            continue;

        const double prevAlt = displaySamples[i - 1].altDeg;
        const double currAlt = displaySamples[i].altDeg;
        const double nextAlt = displaySamples[i + 1].altDeg;
        const double medianAlt = std::max(std::min(prevAlt, currAlt), std::min(std::max(prevAlt, currAlt), nextAlt));

        if (std::abs(currAlt - medianAlt) > 3.0)
            smoothed[i].altDeg = medianAlt * 0.7 + currAlt * 0.3;
        else
            smoothed[i].altDeg = prevAlt * 0.25 + currAlt * 0.5 + nextAlt * 0.25;
    }

    std::vector<Sample> resampled;
    resampled.reserve(static_cast<std::size_t>(std::ceil(360.0 / autoSmoothResampleStepDeg)) + 2);
    resampled.push_back(smoothed.front());

    for (std::size_t i = 1; i < smoothed.size(); ++i)
    {
        const Sample& previous = smoothed[i - 1];
        const Sample& current = smoothed[i];
        const double span = current.azDeg - previous.azDeg;
        if (span <= 0.0)
            continue;

        const Sample& p0 = i >= 2 ? smoothed[i - 2] : previous;
        const Sample& p3 = i + 1 < smoothed.size() ? smoothed[i + 1] : current;
        const int steps = std::max(1, static_cast<int>(std::ceil(span / autoSmoothResampleStepDeg)));

        for (int step = 1; step <= steps; ++step)
        {
            const double t = static_cast<double>(step) / static_cast<double>(steps);
            const double t2 = t * t;
            const double t3 = t2 * t;
            double alt = 0.5 * ((2.0 * previous.altDeg) +
                                (-p0.altDeg + current.altDeg) * t +
                                (2.0 * p0.altDeg - 5.0 * previous.altDeg + 4.0 * current.altDeg - p3.altDeg) * t2 +
                                (-p0.altDeg + 3.0 * previous.altDeg - 3.0 * current.altDeg + p3.altDeg) * t3);

            const double segmentMin = std::min(previous.altDeg, current.altDeg);
            const double segmentMax = std::max(previous.altDeg, current.altDeg);
            alt = qBound(segmentMin, alt, segmentMax);

            const double az = previous.azDeg + span * t;
            resampled.push_back({ az, alt });
        }
    }

    displaySamples = std::move(resampled);
}

void HorizonOverlay::rebuildGeometry()
{
    geometry.clear();
    rebuildDisplaySamples();

    if (displaySamples.empty())
        return;

    appendGeometrySample(displaySamples.front().azDeg, displaySamples.front().altDeg);

    for (std::size_t i = 1; i < displaySamples.size(); ++i)
    {
        const Sample& previous = displaySamples[i - 1];
        const Sample& current = displaySamples[i];
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
