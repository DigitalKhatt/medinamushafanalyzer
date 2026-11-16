cmake_minimum_required(VERSION 3.14.0 FATAL_ERROR)

project(medinamushafanalyzer)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus;/permissive->")

find_package(Qt5 COMPONENTS Core Widgets Xml REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)
# The 4 following package are needed by Poppler
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(OPENJPEG REQUIRED)


pkg_check_modules(Poppler REQUIRED poppler IMPORTED_TARGET)




set(ALL_FILES   mainwindow.h mainwindow.cpp CustomOutputDev.h CustomOutputDev.cpp application.qrc renderarea.h renderarea.cpp graphicsview.h graphicsview.cpp
constants.h aglyph.h aglyph.cpp afont.h afont.cpp pageanalysisresult.h pageanalysisresult.cpp quran.h quran.cpp
shapeItem.h shapeitem.cpp quransearch.h quransearch.cpp "pathdelegate.h" worditem.h worditem.cpp segmentsubword.cpp lettersegmentationcache.cpp lettersegmentationcache.h
letterforminspection.h bodycutpanel.h bodycutpanel.cpp
)
set(LIBRARIES  Qt5::Core  Qt5::CorePrivate Qt5::GuiPrivate Qt5::Xml Qt5::Widgets  PkgConfig::Poppler JPEG::JPEG PNG::PNG  ${OPENJPEG_LIBRARIES})


set(TFMatcherCpp TFMatcherCpp/AlignedTF.cpp TFMatcherCpp/AlignedTF.h TFMatcherCpp/Matcher.cpp TFMatcherCpp/Matcher.h 
	TFMatcherCpp/Polygon.cpp TFMatcherCpp/Polygon.h TFMatcherCpp/SharedDefinitions.h 
	TFMatcherCpp/StringUtils.cpp TFMatcherCpp/StringUtils.h TFMatcherCpp/TurningFunction.cpp TFMatcherCpp/TurningFunction.h
	TFMatcherCpp/Vector2.cpp TFMatcherCpp/Vector2.h)


add_executable(${PROJECT_NAME} ${ALL_FILES} ${TFMatcherCpp} main.cpp proposalexport.cpp proposalexport.h
    markassignment.cpp markassignment.h markassignment_p.h markgeometry.cpp markcharacterlinks.cpp waqfrecognition.cpp)

target_sources(${PROJECT_NAME} PRIVATE
    cutter/detail.h
    cutter/engine/anchor_search.cpp
    cutter/engine/component_assembly.cpp
    cutter/engine/diagnostics.cpp
    cutter/engine/partition_assembly.cpp
    cutter/forms/anchor_candidates.cpp
    cutter/forms/configuration.cpp
    cutter/forms/counters_and_bowls.cpp
    cutter/forms/dispatch.cpp
    cutter/forms/heads_and_ligatures.cpp
    cutter/forms/stem_boundaries.cpp
    cutter/forms/stems_and_routes.cpp
    cutter/forms/teeth.cpp
    cutter/geometry/contours.cpp
    cutter/geometry/intersections.cpp
    cutter/geometry/outline.cpp
    cutter/geometry/partition.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE CommonPCH.h)

# Keep cross-file optimization for the separately compiled cutter in release builds.
# MSVC currently uses Edit and Continue and incremental linking instead.
if(NOT MSVC)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported LANGUAGES CXX)
    if(ipo_supported)
        set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endif()

# The resolved AI input is independent of letter templates and the cutter.
# Rebuilding any upstream parser/assignment dependency invalidates that cache.
set(NATIVE_INPUT_FILES ${ALL_FILES} ${TFMatcherCpp} CommonPCH.h pathmask.h proposalexport.cpp
    markassignment.cpp markassignment.h markassignment_p.h markgeometry.cpp
    markcharacterlinks.cpp waqfrecognition.cpp templates/mark-shapes.json tools/waqf_templates.json)
list(FILTER NATIVE_INPUT_FILES EXCLUDE REGEX "^(segmentsubword|lettersegmentationcache)\\.")
list(FILTER NATIVE_INPUT_FILES EXCLUDE REGEX "(application\\.qrc|\\.natvis)$")
set(NATIVE_INPUT_HASHES "native-input-v1;Qt=${Qt5Core_VERSION};Poppler=${Poppler_VERSION}")
foreach(INPUT_FILE IN LISTS NATIVE_INPUT_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${INPUT_FILE}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${INPUT_FILE}" INPUT_HASH)
    string(APPEND NATIVE_INPUT_HASHES ";${INPUT_FILE}:${INPUT_HASH}")
endforeach()
string(SHA256 NATIVE_INPUT_VERSION "${NATIVE_INPUT_HASHES}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/nativeinputversion.h"
    CONTENT "#pragma once\nconstexpr const char* NativeInputVersion = \"${NATIVE_INPUT_VERSION}\";\n")
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")


target_link_libraries(${PROJECT_NAME} PUBLIC  ${LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)


target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)


if(MSVC)
  target_compile_options(${PROJECT_NAME} PUBLIC "/ZI")
  target_link_options(${PROJECT_NAME} PUBLIC "/INCREMENTAL")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:MYDEBUG> )
target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:RelWithDebInfo>:MYDEBUG> )
