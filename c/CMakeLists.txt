cmake_minimum_required(VERSION 3.16)
project(nxs VERSION 1.0.0 LANGUAGES C)

# ── Options ──────────────────────────────────────────────────────────────────
option(NXS_BUILD_TESTS "Build the NXS test executable" ON)
option(NXS_BUILD_BENCH "Build the NXS benchmark executable" ON)
option(NXS_INSTALL     "Install NXS headers and library" ON)

# ── Library ───────────────────────────────────────────────────────────────────
add_library(nxs STATIC nxs.c)
target_include_directories(nxs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(nxs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    PUBLIC_HEADER nxs.h
)

# ── Test executable ───────────────────────────────────────────────────────────
if(NXS_BUILD_TESTS)
    add_executable(nxs_test test/test.c)
    target_link_libraries(nxs_test PRIVATE nxs m)
    set_target_properties(nxs_test PROPERTIES C_STANDARD 11)
endif()

# ── Bench executable ──────────────────────────────────────────────────────────
if(NXS_BUILD_BENCH)
    add_executable(nxs_bench bench/bench.c)
    target_link_libraries(nxs_bench PRIVATE nxs m)
    set_target_properties(nxs_bench PROPERTIES C_STANDARD 11)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
if(NXS_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS nxs
        ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES nxs.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# ── Package config ────────────────────────────────────────────────────────────
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/nxs-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
