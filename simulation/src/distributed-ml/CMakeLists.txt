check_include_file_cxx(stdint.h HAVE_STDINT_H)
if(HAVE_STDINT_H)
    add_definitions(-DHAVE_STDINT_H)
endif()

set(examples_as_tests_sources)
if(${ENABLE_EXAMPLES})
    set(examples_as_tests_sources
        #test/distributed-training-examples-test-suite.cc
        )
endif()

find_package(LibXml2)

if(LIBXML2_FOUND)
    add_definitions(-DHAVE_LIBXML2)
    include_directories(${LIBXML2_INCLUDE_DIR})
    set(EXTRA_LIBS ${LIBXML2_LIBRARIES})
else()
    message(WARNING "LibXml2 not found, XML features disabled")
endif()


build_lib(
    LIBNAME distributed-training
    SOURCE_FILES model/distributed-training.cc
								 model/gpu.cc
                 model/topo.cc
								 model/collectives.cc
								 model/msccl-header.cc
#								 model/msccl-channel.cc
								 helper/collectives-helper.cc
								 helper/distributed-training-helper.cc
								 model/collective-tests.cc
								 model/switch.cc
								 helper/switch-helper.cc
    HEADER_FILES model/distributed-training.h
								 model/msccl.h
								 model/gpu.h
                 model/topo.h
								 model/collectives.h
								 model/utils.h
								 model/msccl-header.h
#								 model/msccl-channel.h
								 helper/collectives-helper.h
								 helper/distributed-training-helper.h
								 model/topo_examples.h
								 model/collective-tests.h
								 model/switch.h
								 helper/switch-helper.h
    LIBRARIES_TO_LINK ${libcore} ${libnetwork} ${libinternet} ${libbridge} ${EXTRA_LIBS}
    TEST_SOURCES test/distributed-training-test-suite.cc
                 ${examples_as_tests_sources}
)
