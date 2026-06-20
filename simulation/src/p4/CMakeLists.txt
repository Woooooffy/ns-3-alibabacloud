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
