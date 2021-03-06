cmake_minimum_required(VERSION 3.10)
project(cheri_compressed_cap C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

include(CheckCXXCompilerFlag)
# CMAKE_REQUIRED_LINK_OPTIONS needs cmake 3.13
set(CMAKE_REQUIRED_LIBRARIES -fsanitize=undefined)
check_cxx_compiler_flag(-fsanitize=undefined HAVE_UBSAN)
set(CMAKE_REQUIRED_LIBRARIES -fsanitize=address)
check_cxx_compiler_flag(-fsanitize=address HAVE_ASAN)
set(CMAKE_REQUIRED_LIBRARIES -fsanitize=memory)
check_cxx_compiler_flag(-fsanitize=memory HAVE_MSAN)
set(CMAKE_REQUIRED_LIBRARIES -fsanitize=fuzzer)
check_cxx_compiler_flag(-fsanitize=fuzzer HAVE_LIBFUZZER)

if (HAVE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    if (${CMAKE_VERSION} VERSION_GREATER 3.12)
        add_link_options(-fsanitize=undefined)
    else()
        link_libraries(-fsanitize=undefined)
    endif()
endif()

add_compile_options(-pedantic -Wall -Wextra -Wno-old-style-cast -Wno-c++98-compat-pedantic -Wno-c++98-c++11-compat-binary-literal -Wno-c11-extensions -Wno-padded)
# TODO: -Weverything
add_library(cheri_compressed_cap14 cheri_compressed_cap.c cheri_compressed_cap.h)

add_executable(decompress_c128_cap decompress_c128_cap.c)
add_executable(decompress_c128_cap23 decompress_c128_cap.c)
target_compile_definitions(decompress_c128_cap23 PRIVATE -DCC128_OLD_FORMAT)
add_executable(decompress_c128_cap14 decompress_c128_cap.c)
include(GNUInstallDirs)
install(TARGETS decompress_c128_cap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS decompress_c128_cap23 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS decompress_c128_cap14 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()

function(add_cc_test _tgt _src)
    if (HAVE_ASAN)
        add_executable(${_tgt}-asan-ubsan ${_src})
        target_compile_options(${_tgt}-asan-ubsan PRIVATE -fsanitize=undefined,address)
        target_link_libraries(${_tgt}-asan-ubsan PRIVATE -fsanitize=undefined,address)
        add_test(NAME test-${_tgt}-asan-ubsan COMMAND ${_tgt}-asan-ubsan)
    endif()
    if (HAVE_MSAN)
        add_executable(${_tgt}-msan-ubsan ${_src})
        target_compile_options(${_tgt}-msan-ubsan PRIVATE -fsanitize=undefined,address)
        target_link_libraries(${_tgt}-msan-ubsan PRIVATE -fsanitize=undefined,address)
        add_test(NAME test-${_tgt}-msan-ubsan COMMAND ${_tgt}-msan-ubsan)
    endif()
    # UBSan will be added if available -> no suffix
    add_executable(${_tgt} ${_src})
    add_test(NAME test-${_tgt} COMMAND ${_tgt})
endfunction()

add_cc_test(simple_test test/simple_test.cpp)

add_cc_test(setbounds_test test/setbounds_test.cpp)

add_cc_test(random_inputs_test test/random_inputs_test.cpp)

if (HAVE_LIBFUZZER)
    if (HAVE_ASAN)
        add_executable(fuzz_decompress_asan test/fuzz-decompress.cpp)
        target_compile_options(fuzz_decompress_asan PRIVATE -fsanitize=undefined,address,fuzzer)
        target_link_libraries(fuzz_decompress_asan PRIVATE -fsanitize=undefined,address,fuzzer)
    endif()
    if (HAVE_MSAN)
        add_executable(fuzz_decompress_msan test/fuzz-decompress.cpp)
        target_compile_options(fuzz_decompress_msan PRIVATE -fsanitize=undefined,memory,fuzzer)
        target_link_libraries(fuzz_decompress_msan PRIVATE -fsanitize=undefined,memory,fuzzer)
    endif()
endif()
