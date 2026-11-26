cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(git VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_git
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_git
)

add_library(HuxerUI::Git ALIAS huxerui_git)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_git PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(FetchContent)

set(HUXERUI_GIT_SSH "OFF" CACHE STRING "libgit2 SSH provider, such as libssh2 or exec")
set(HUXERUI_GIT_HTTPS "" CACHE STRING "libgit2 HTTPS provider, or empty for the platform default")

function(huxerui_git_add_libgit2)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_CLI OFF)
    set(BUILD_EXAMPLES OFF)
    set(EXPERIMENTAL_SHA256 OFF)
    set(STATIC_CRT OFF)
    set(USE_SSH "${HUXERUI_GIT_SSH}")
    set(USE_HTTPS "${HUXERUI_GIT_HTTPS}")
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()
    if (EMSCRIPTEN)
        set(USE_THREADS OFF)
        set(USE_SSH OFF)
        set(USE_HTTPS OFF)
    elseif (ANDROID AND NOT USE_HTTPS)
        # libgit2's mbedTLS finder accepts archive paths before the archives are built.
        set(ENABLE_PROGRAMS OFF)
        set(ENABLE_TESTING OFF)
        set(USE_SHARED_MBEDTLS_LIBRARY OFF)
        set(USE_STATIC_MBEDTLS_LIBRARY ON)
        FetchContent_Declare(mbedtls
                URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2"
                URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
        )
        FetchContent_MakeAvailable(mbedtls)
        set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include")
        set(MBEDTLS_LIBRARY "${mbedtls_BINARY_DIR}/library/libmbedtls.a")
        set(MBEDX509_LIBRARY "${mbedtls_BINARY_DIR}/library/libmbedx509.a")
        set(MBEDCRYPTO_LIBRARY "${mbedtls_BINARY_DIR}/library/libmbedcrypto.a")
        set(USE_HTTPS mbedTLS)
        set(GIT_BUNDLED_MBEDTLS TRUE)
    endif ()
    FetchContent_Declare(libgit2
            URL "https://codeload.github.com/libgit2/libgit2/tar.gz/refs/tags/v1.9.7"
            URL_HASH SHA256=1a4fbe7589e814777ae76b64734ad80f4ecad22cd33a22682a2aaea4ae5375e7
    )
    FetchContent_MakeAvailable(libgit2)
    if (GIT_BUNDLED_MBEDTLS)
        add_dependencies(libgit2package mbedtls mbedx509 mbedcrypto)
        target_compile_definitions(huxerui_git PRIVATE HUXERUI_GIT_ANDROID_MBEDTLS=1)
        set_property(TARGET huxerui_git PROPERTY HUXERUI_GIT_MBEDTLS_LICENSE "${mbedtls_SOURCE_DIR}/LICENSE")
    endif ()
    target_include_directories(huxerui_git SYSTEM PRIVATE "${libgit2_SOURCE_DIR}/include")
    set_property(TARGET huxerui_git PROPERTY HUXERUI_GIT_LIBGIT2_LICENSE "${libgit2_SOURCE_DIR}/COPYING")
endfunction()

if (NOT TARGET libgit2package)
    huxerui_git_add_libgit2()
else ()
    target_include_directories(huxerui_git SYSTEM PRIVATE "$<TARGET_PROPERTY:libgit2,INTERFACE_INCLUDE_DIRECTORIES>")
endif ()
if (IOS)
    # The SDK's iOS application core combines HuxerUI library archives. Include the pinned
    # libgit2 package's objects in this archive so they reach the application's Xcode link.
    get_target_property(GIT_PACKAGE_OBJECTS libgit2package SOURCES)
    target_sources(huxerui_git PRIVATE ${GIT_PACKAGE_OBJECTS})
    target_link_libraries(huxerui_git PRIVATE "$<TARGET_PROPERTY:libgit2package,INTERFACE_LINK_LIBRARIES>")
else ()
    target_link_libraries(huxerui_git PRIVATE libgit2package)
endif ()
target_compile_features(huxerui_git PUBLIC cxx_std_20)

option(HUXERUI_GIT_BUILD_TESTS "Build Lib-Git repository integration tests" OFF)
if (HUXERUI_GIT_BUILD_TESTS)
    enable_testing()
    if (NOT TARGET HuxerUI::testing)
        find_package(HuxerUI CONFIG REQUIRED COMPONENTS testing)
    endif ()
    find_package(Git REQUIRED)
    add_executable(huxerui_git_repository_test tests/repository.cpp examples/preview/src/app.cpp
                   examples/preview/src/workspace_session.cpp examples/preview/src/credentials.cpp)
    target_include_directories(huxerui_git_repository_test SYSTEM PRIVATE
                               "$<TARGET_PROPERTY:libgit2,INTERFACE_INCLUDE_DIRECTORIES>")
    if (APPLE)
        target_link_libraries(huxerui_git_repository_test PRIVATE "-framework Security")
    endif ()
    if (TARGET HuxerUI::huxerui_static)
        set(HUXERUI_GIT_TEST_CORE HuxerUI::huxerui_static)
    else ()
        set(HUXERUI_GIT_TEST_CORE HuxerUI::huxerui)
    endif ()
    target_compile_definitions(huxerui_git_repository_test PRIVATE
            GIT_TEST_EXECUTABLE="${GIT_EXECUTABLE}"
            GIT_TEST_RESOURCE_PACKAGE="$<TARGET_PROPERTY:${HUXERUI_GIT_TEST_CORE},HUXERUI_RESOURCE_PACKAGE>")
    target_link_libraries(huxerui_git_repository_test PRIVATE HuxerUI::Git HuxerUI::testing)
    huxerui_enable_codegen(huxerui_git_repository_test)
    add_test(NAME huxerui_git_repository COMMAND huxerui_git_repository_test)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        add_test(NAME huxerui_git_smart_http
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/smart_http.py"
                        "$<TARGET_FILE:huxerui_git_repository_test>" "${GIT_EXECUTABLE}")
        set_tests_properties(huxerui_git_smart_http PROPERTIES TIMEOUT 60)
        find_program(HUXERUI_GIT_TEST_OPENSSL openssl)
        if (HUXERUI_GIT_TEST_OPENSSL)
            add_test(NAME huxerui_git_credentials
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/smart_http.py"
                            "$<TARGET_FILE:huxerui_git_repository_test>" "${GIT_EXECUTABLE}" "${HUXERUI_GIT_TEST_OPENSSL}")
            set_tests_properties(huxerui_git_credentials PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif ()
    endif ()
endif ()
