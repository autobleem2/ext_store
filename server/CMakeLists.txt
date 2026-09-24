# abstored - the AutoBleem Store's LAN server (README.md next to this file).
#
# Standalone, on any Linux:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DAB_CORE_DIR=<an autobleem-core checkout>]
#   cmake --build build -j
# Without AB_CORE_DIR, autobleem-core (develop) is fetched from GitHub. Only its SDL-free engine is built:
# a C++14 compiler, CMake 3.14 and pthreads are all it takes - no SDL, no other packages.
#
# Inside the launcher's build (ext_store is in AB_EXTENSION_DIRS) the launcher's own lib_ableem is used -
# its ableem_lanserver is the server; this folder is only its command line (src/main.cpp).
cmake_minimum_required(VERSION 3.14)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(abstored C CXX)
    set(CMAKE_CXX_STANDARD 14)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif ()
endif ()

if (NOT TARGET ableem_engine)
    set(AB_CORE_DIR "" CACHE PATH "An autobleem-core checkout; empty = fetched from GitHub")
    if (AB_CORE_DIR)
        set(core_dir "${AB_CORE_DIR}")
    else ()
        include(FetchContent)
        FetchContent_Declare(autobleem_core
                GIT_REPOSITORY https://github.com/autobleem2/autobleem-core.git
                GIT_TAG develop
                GIT_SHALLOW TRUE)
        FetchContent_GetProperties(autobleem_core)
        if (NOT autobleem_core_POPULATED)
            FetchContent_Populate(autobleem_core)
        endif ()
        set(core_dir "${autobleem_core_SOURCE_DIR}")
    endif ()
    # the engine alone: no SDL, no demo
    set(ABLEEM_BUILD_UI OFF CACHE BOOL "" FORCE)
    set(ABLEEM_BUILD_DEMO OFF CACHE BOOL "" FORCE)
    add_subdirectory("${core_dir}/lib_ableem" lib_ableem)
endif ()

find_package(Threads REQUIRED)
# the server itself - the scan, the HTTP server, the status page - is core's ableem_lanserver (lib_ableem,
# tested there as test_lan_server); pc-tools' LAN Share is its other front end. This is the command line.
add_executable(abstored src/main.cpp)
target_link_libraries(abstored PRIVATE ableem_lanserver)
if (WIN32)
    target_link_options(abstored PRIVATE -static-libgcc -static-libstdc++)
endif ()
install(TARGETS abstored RUNTIME DESTINATION bin)
