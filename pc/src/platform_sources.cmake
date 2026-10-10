# Sources of the disc, memory card, SIF and sound parts of
# chronicletwo_platform, kept here so the tests in pc/tests build the same set.

set(PLATFORM_IO_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/host/host_paths.cpp
    ${CMAKE_CURRENT_LIST_DIR}/iop/iop_memory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/iop/sifrpc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/disc/disc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/disc/libcdvd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/disc/sifdev.cpp
    ${CMAKE_CURRENT_LIST_DIR}/memcard/libmc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/spu2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/output.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/libsdr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/bank.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/synth.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/sequence.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/ezbgm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/ezmidi.cpp
    ${CMAKE_CURRENT_LIST_DIR}/audio/msin.cpp)
