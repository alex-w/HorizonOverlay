INCLUDE_DIRECTORIES(
    .
    gui
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/gui
)

FIND_PACKAGE(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets OpenGL)

IF(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    # Qt 6.8's WrapOpenGL can fall back to "-framework AGL" even when the
    # current macOS SDK no longer ships AGL.framework. Stellarium itself only
    # needs the OpenGL framework here, so remove the missing legacy framework
    # from the imported wrapper target instead of adding local SDK paths.
    GET_TARGET_PROPERTY(HorizonOverlay_WrapOpenGL_Libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    IF(HorizonOverlay_WrapOpenGL_Libs)
        SET(HorizonOverlay_FilteredWrapOpenGL_Libs)
        FOREACH(HorizonOverlay_WrapOpenGL_Lib IN LISTS HorizonOverlay_WrapOpenGL_Libs)
            IF(NOT HorizonOverlay_WrapOpenGL_Lib MATCHES "(^|/)AGL(\\.framework)?$")
                LIST(APPEND HorizonOverlay_FilteredWrapOpenGL_Libs "${HorizonOverlay_WrapOpenGL_Lib}")
            ENDIF()
        ENDFOREACH()
        LIST(REMOVE_ITEM HorizonOverlay_FilteredWrapOpenGL_Libs "-framework AGL" "AGL")
        SET_TARGET_PROPERTIES(WrapOpenGL::WrapOpenGL PROPERTIES
            INTERFACE_LINK_LIBRARIES "${HorizonOverlay_FilteredWrapOpenGL_Libs}")
    ENDIF()
ENDIF()

SET(HorizonOverlay_SRCS
    HorizonOverlay.hpp
    HorizonOverlay.cpp
    gui/HorizonOverlayDialog.hpp
    gui/HorizonOverlayDialog.cpp
)

SET(HorizonOverlay_UIS
    gui/horizonOverlayDialog.ui
)

SET(HorizonOverlay_RES ../resources/HorizonOverlay.qrc)
IF (${QT_VERSION_MAJOR} EQUAL "5")
    QT5_WRAP_UI(HorizonOverlay_UIS_H ${HorizonOverlay_UIS})
    QT5_ADD_RESOURCES(HorizonOverlay_RES_CXX ${HorizonOverlay_RES})
ELSE()
    QT_WRAP_UI(HorizonOverlay_UIS_H ${HorizonOverlay_UIS})
    QT_ADD_RESOURCES(HorizonOverlay_RES_CXX ${HorizonOverlay_RES})
ENDIF()

SET(HorizonOverlay_Qt_LIBRARIES
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::OpenGL
)
SET(StelMain)

IF(BUILD_DYNAMIC_PLUGIN)
    ADD_LIBRARY(HorizonOverlay MODULE ${HorizonOverlay_SRCS} ${HorizonOverlay_UIS_H} ${HorizonOverlay_RES_CXX})
    SET_TARGET_PROPERTIES(HorizonOverlay PROPERTIES PREFIX "lib")
    IF(APPLE)
        # Stellarium loads dynamic plug-ins at runtime and provides the StelMain
        # symbols from the host process, so unresolved host symbols are expected.
        # Keep the macOS module filename in Stellarium's conventional .dylib form.
        SET_TARGET_PROPERTIES(HorizonOverlay PROPERTIES LINK_FLAGS "-undefined dynamic_lookup" SUFFIX ".dylib")
    ENDIF()

    IF(WIN32 AND NOT MSVC)
        SET_TARGET_PROPERTIES(HorizonOverlay PROPERTIES LINK_FLAGS "-enable-runtime-pseudo-reloc -Wl,--allow-multiple-definition")
    ENDIF()
    IF(WIN32)
        SET(StelMain stelMain)
    ELSE(WIN32)
        SET(StelMain)
    ENDIF(WIN32)

    TARGET_LINK_LIBRARIES(HorizonOverlay ${StelMain} ${HorizonOverlay_Qt_LIBRARIES})
    INSTALL(TARGETS HorizonOverlay DESTINATION "modules/HorizonOverlay")
ELSE()
    ADD_LIBRARY(HorizonOverlay-static STATIC ${HorizonOverlay_SRCS} ${HorizonOverlay_UIS_H} ${HorizonOverlay_RES_CXX})
    SET_TARGET_PROPERTIES(HorizonOverlay-static PROPERTIES OUTPUT_NAME "HorizonOverlay")
    TARGET_LINK_LIBRARIES(HorizonOverlay-static ${StelMain} ${HorizonOverlay_Qt_LIBRARIES})
    SET_TARGET_PROPERTIES(HorizonOverlay-static PROPERTIES COMPILE_FLAGS "-DQT_STATICPLUGIN")
    ADD_DEPENDENCIES(AllStaticPlugins HorizonOverlay-static)
ENDIF()

IF(TARGET HorizonOverlay-static)
    SET_TARGET_PROPERTIES(HorizonOverlay-static PROPERTIES FOLDER "plugins/HorizonOverlay")
ENDIF()
