cmake_minimum_required(VERSION 3.21)
project(pdf_select_crop VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Gui LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(MUPDF REQUIRED IMPORTED_TARGET mupdf)

qt_add_executable(pdf-select-crop
    src/main.cpp
    src/document_view.cpp
    src/document_view.h
    src/pdf_backend.c
    src/pdf_backend.h
    src/raster_backend.cpp
    src/raster_backend.h
)

qt_add_translations(pdf-select-crop
    TS_FILES i18n/pdf-select-crop_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(pdf-select-crop PRIVATE
    Qt6::Widgets
    Qt6::Gui
    PkgConfig::POPPLER_QT6
    PkgConfig::MUPDF
)

install(TARGETS pdf-select-crop RUNTIME DESTINATION bin)
