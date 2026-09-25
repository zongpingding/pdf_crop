cmake_minimum_required(VERSION 3.21)
project(prop VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Gui LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(MUPDF REQUIRED IMPORTED_TARGET mupdf)

qt_add_executable(prop
    src/main.cpp
    src/document_view.cpp
    src/document_view.h
    src/crop_plan.cpp
    src/crop_plan.h
    src/pdf_backend.c
    src/pdf_backend.h
    src/raster_backend.cpp
    src/raster_backend.h
)

qt_add_translations(prop
    TS_FILES i18n/prop_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(prop PRIVATE
    Qt6::Widgets
    Qt6::Gui
    PkgConfig::POPPLER_QT6
    PkgConfig::MUPDF
)

install(TARGETS prop RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(PDFINFO_EXECUTABLE pdfinfo)
    find_program(PDFTOTEXT_EXECUTABLE pdftotext)
    if(Python3_Interpreter_FOUND AND PDFINFO_EXECUTABLE AND PDFTOTEXT_EXECUTABLE)
        add_test(NAME export_smoke
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/smoke.py
                         $<TARGET_FILE:prop>)
    endif()
    add_executable(view_smoke tests/view_smoke.cpp src/document_view.cpp src/crop_plan.cpp)
    target_include_directories(view_smoke PRIVATE src)
    target_link_libraries(view_smoke PRIVATE Qt6::Widgets Qt6::Gui PkgConfig::POPPLER_QT6)
    add_test(NAME view_smoke COMMAND $<TARGET_FILE:view_smoke>)
    set_tests_properties(view_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
