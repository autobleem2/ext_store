# ext_store: the AutoBleem Store, an extension of the AutoBleem launcher (the launcher's
# docs/extensions-plan.md and docs/store-plan.md). Built with the launcher - its AB_EXTENSION_DIRS names this
# folder - until the AutoBleem SDK package exists; ab_add_extension() is autobleem-core's.
if (NOT COMMAND ab_add_extension)
    message(FATAL_ERROR "ext_store is built with the AutoBleem launcher: -DAB_EXTENSION_DIRS=<this folder>")
endif()

ab_add_extension(store HOST ${AB_EXTENSION_HOST}
        SOURCES src/store_extension.cpp src/store_service.cpp src/gui_store.cpp
        INI ${CMAKE_CURRENT_SOURCE_DIR}/extension.ini
        LANG ${CMAKE_CURRENT_SOURCE_DIR}/lang)

# the service is tested on its own, against a fake site, on the SDK's test harness
if (AB_BUILD_TESTS AND COMMAND ab_add_test)
    ab_add_test(test_store_service tests/test_store_service.cpp src/store_service.cpp)
endif()
