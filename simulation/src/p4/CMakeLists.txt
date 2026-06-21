check_include_file_cxx(stdint.h HAVE_STDINT_H)
if(HAVE_STDINT_H)
    add_definitions(-DHAVE_STDINT_H)
endif()

set(examples_as_tests_sources)
if(${ENABLE_EXAMPLES})
    set(examples_as_tests_sources
        #test/ethernet-switch-examples-test-suite.cc
        )
endif()

# NOTE: this module ("p4", LIBNAME "ethernet-switch") is currently excluded
# from the build entirely -- see the `list(REMOVE_ITEM libs_to_build p4)` in
# build-support/macros-and-definitions.cmake. This CMakeLists.txt is unreached
# as long as that exclusion is in place.
set(libethernet-switch libethernet-switch CACHE INTERNAL "")
set(libethernet-switch-obj libethernet-switch-obj CACHE INTERNAL "")

build_lib(
    LIBNAME ethernet-switch
    SOURCE_FILES model/switch-net-device.cc
								 model/switched-ethernet-host-device.cc
								 model/switched-ethernet-channel.cc
								 model/custom-switch-impl.cc
                 helper/p4-helper.cc
								 helper/switched-ethernet-helper.cc
    HEADER_FILES model/switch-net-device.h
								 model/switched-ethernet-host-device.h
								 model/switched-ethernet-channel.h
								 model/custom-switch-impl.h
								 helper/p4-helper.h
                 helper/switched-ethernet-helper.h
    LIBRARIES_TO_LINK ${libcore} ${libnetwork}
    TEST_SOURCES test/ethernet-switch-test-suite.cc
                 ${examples_as_tests_sources}
)
