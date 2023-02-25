cmake_minimum_required(VERSION 3.8.0 FATAL_ERROR)

project(medinamushafanalyzer)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

#add_compile_options($<$<C_COMPILER_ID:MSVC>:/utf-8>)
#add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

#file(GLOB OTHERS files/*.mp files/*.fea  files/images/*.jpg files/*.json)

#add_custom_target(files SOURCES ${OTHERS})



add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus;/permissive->")





find_package(Qt5 COMPONENTS Core Widgets Xml REQUIRED)
# The 4 following package are needed by Poppler
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(OPENJPEG REQUIRED)


pkg_check_modules(Poppler REQUIRED poppler IMPORTED_TARGET)




set(ALL_FILES   mainwindow.h mainwindow.cpp CustomOutputDev.h CustomOutputDev.cpp application.qrc renderarea.h renderarea.cpp graphicsview.h graphicsview.cpp
constants.h aglyph.h aglyph.cpp afont.h afont.cpp pageanalysisresult.h pageanalysisresult.cpp quran.h quran.cpp
shapeitem.h shapeitem.cpp quransearch.h quransearch.cpp "pathdelegate.h"
test.natvis qt5.natvis
)
set(LIBRARIES  Qt5::Core  Qt5::CorePrivate Qt5::GuiPrivate Qt5::Xml Qt5::Widgets  PkgConfig::Poppler JPEG::JPEG PNG::PNG  ${OPENJPEG_LIBRARIES})

set(TFMatcherCpp TFMatcherCpp/AlignedTF.cpp TFMatcherCpp/AlignedTF.h TFMatcherCpp/Matcher.cpp TFMatcherCpp/Matcher.h 
	TFMatcherCpp/Polygon.cpp TFMatcherCpp/Polygon.h TFMatcherCpp/SharedDefinitions.h 
	TFMatcherCpp/StringUtils.cpp TFMatcherCpp/StringUtils.h TFMatcherCpp/TurningFunction.cpp TFMatcherCpp/TurningFunction.h
	TFMatcherCpp/Vector2.cpp TFMatcherCpp/Vector2.h)


add_executable(${PROJECT_NAME} ${ALL_FILES} ${TFMatcherCpp} main.cpp)

target_precompile_headers(${PROJECT_NAME} PRIVATE CommonPCH.h)


target_link_libraries(${PROJECT_NAME} PUBLIC  ${LIBRARIES})


target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:MYDEBUG> )
target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:RelWithDebInfo>:MYDEBUG> )