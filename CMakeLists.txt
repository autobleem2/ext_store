# ext_store: the AutoBleem Store, an extension of the AutoBleem launcher (the launcher's
# docs/extensions-plan.md and docs/store-plan.md). Built with the launcher - its AB_EXTENSION_DIRS names this
# folder - until the AutoBleem SDK package exists; ab_add_extension() is autobleem-core's.
if (NOT COMMAND ab_add_extension)
    message(FATAL_ERROR "ext_store is built with the AutoBleem launcher: -DAB_EXTENSION_DIRS=<this folder>")
endif()

ab_add_extension(store HOST ${AB_EXTENSION_HOST}
        SOURCES src/store_extension.cpp src/store_service.cpp src/store_pictures.cpp src/gui_store.cpp
        INI ${CMAKE_CURRENT_SOURCE_DIR}/extension.ini
        LANG ${CMAKE_CURRENT_SOURCE_DIR}/lang)
# data/ goes into the staged folder with the plugin (psn_serials.tsv - data/README.md); the package takes the
# folder whole
add_custom_command(TARGET store POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/extensions/store/data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/data/psn_serials.tsv"
                "${CMAKE_BINARY_DIR}/extensions/store/data/psn_serials.tsv")

# abstored, the LAN server (server/README.md): its own program, built here with the launcher's engine for the
# tests and our own packages; it also builds on its own, on any Linux (server/CMakeLists.txt). Not for the
# console, which serves nothing.
if (NOT AB_TARGET STREQUAL "psc")
    option(AB_BUILD_STORE_SERVER "Build abstored, the Store's LAN server" ON)
else ()
    option(AB_BUILD_STORE_SERVER "Build abstored, the Store's LAN server" OFF)
endif ()
if (AB_BUILD_STORE_SERVER)
    add_subdirectory(server)
    set_target_properties(abstored PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extensions/store-server")
endif ()

# the service is tested on its own, against a fake site, on the SDK's test harness
if (AB_BUILD_TESTS AND COMMAND ab_add_test)
    ab_add_test(test_store_service tests/test_store_service.cpp src/store_service.cpp)
    ab_add_test(test_store_pictures tests/test_store_pictures.cpp src/store_pictures.cpp)
    # abstored's server is core's ableem_lanserver, tested there (test_lan_server)
endif()
