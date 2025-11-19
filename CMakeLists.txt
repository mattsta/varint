cmake_minimum_required(VERSION 3.10...3.28)
project(varint C)

# Enable testing - allows "make test" or "ctest" to run all tests
enable_testing()

add_subdirectory(src)
add_subdirectory(examples)

# Custom target to run comprehensive tests with sanitizers
add_custom_target(test-comprehensive
    COMMAND ${CMAKE_SOURCE_DIR}/test_all_comprehensive.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running comprehensive test suite with sanitizers"
    DEPENDS
        # Unit tests
        varintPackedTest varintDimensionTest
        varintDeltaTest varintFORTest varintPFORTest
        varintGroupTest varintDictTest varintAdaptiveTest
        varintFloatTest varintBitmapTest
        # Standalone examples (only those defined in CMake)
        example_tagged example_external example_split example_chained
        example_packed example_bitstream example_dimension
)

# Custom target to check for compiler warnings across all compilers
add_custom_target(check-warnings
    COMMAND ${CMAKE_SOURCE_DIR}/run_all_compilers.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking for compiler warnings with GCC and Clang"
)

# vi:ai et sw=4 ts=4:
