cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_git VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_git
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "Git Preview"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.git.preview"
)

huxerui_configure_project_app(example_git)

huxerui_use_library(example_git
        TARGET HuxerUI::Git
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    get_target_property(GIT_DEPENDENCY_LICENSE HuxerUI::Git HUXERUI_GIT_LIBGIT2_LICENSE)
    if (GIT_DEPENDENCY_LICENSE)
        if (APPLE AND NOT IOS)
            add_custom_command(TARGET example_git POST_BUILD
                    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GIT_DEPENDENCY_LICENSE}"
                            "$<TARGET_BUNDLE_DIR:example_git>/Contents/Resources/libgit2-COPYING"
                    VERBATIM)
        elseif (IOS)
            configure_file("${GIT_DEPENDENCY_LICENSE}" "${CMAKE_BINARY_DIR}/licenses/libgit2-COPYING" COPYONLY)
        elseif (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
            install(FILES "${GIT_DEPENDENCY_LICENSE}" DESTINATION "licenses" RENAME "libgit2-COPYING"
                    COMPONENT HuxerUIApplication)
        elseif (ANDROID AND HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT)
            set(GIT_LICENSE_ASSETS "${HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT}/${ANDROID_ABI}/package/licenses")
            file(MAKE_DIRECTORY "${GIT_LICENSE_ASSETS}")
            configure_file("${GIT_DEPENDENCY_LICENSE}" "${GIT_LICENSE_ASSETS}/libgit2-COPYING" COPYONLY)
            get_target_property(GIT_TLS_LICENSE HuxerUI::Git HUXERUI_GIT_MBEDTLS_LICENSE)
            if (GIT_TLS_LICENSE)
                configure_file("${GIT_TLS_LICENSE}" "${GIT_LICENSE_ASSETS}/mbedtls-LICENSE" COPYONLY)
            endif ()
        endif ()
    endif ()
endif ()
