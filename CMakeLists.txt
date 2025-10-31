cmake_minimum_required(VERSION 3.21)
project(TouchReader VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(LIBRARY_NAME "touch_reader")

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

find_package(Threads REQUIRED)

# Source files
set(TOUCH_READER_SOURCES
    src/cpp/touch_reader.cpp
    src/cpp/ini_parser.cpp
)

# Header files
set(TOUCH_READER_HEADERS
    src/cpp/touch_reader.hpp
    src/cpp/device_helper.hpp
)

# Create shared library
add_library(${LIBRARY_NAME} SHARED ${TOUCH_READER_SOURCES} ${TOUCH_READER_HEADERS})
target_link_libraries(${LIBRARY_NAME} PRIVATE ${CMAKE_THREAD_LIBS_INIT})

set_target_properties(${LIBRARY_NAME} PROPERTIES
    OUTPUT_NAME ${LIBRARY_NAME}
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

# Install targets
install(TARGETS ${LIBRARY_NAME}
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${TOUCH_READER_HEADERS}
    DESTINATION include/touch_reader
)

# Add a test program if desired and if test.cpp exists
option(BUILD_TEST_PROGRAM "Build the test program" ON)
if(BUILD_TEST_PROGRAM)
    set(TEST_SOURCE "${CMAKE_SOURCE_DIR}/src/cpp/test.cpp")
    if(EXISTS "${TEST_SOURCE}")
        add_executable(touch_test "${TEST_SOURCE}")
        target_link_libraries(touch_test PRIVATE touch_reader)
    else()
        message(STATUS "test.cpp not found, skipping test program build")
        set(BUILD_TEST_PROGRAM OFF)
    endif()
endif()

# Option for C# example build
option(BUILD_CSHARP_EXAMPLE "Build the C# example" OFF)
if(BUILD_CSHARP_EXAMPLE)
    # Find dotnet executable
    find_program(DOTNET_EXECUTABLE dotnet)
    if(DOTNET_EXECUTABLE)
        # Create build directory for C# output
        set(CSHARP_BUILD_DIR ${CMAKE_BINARY_DIR}/csharp)
        file(MAKE_DIRECTORY ${CSHARP_BUILD_DIR})

        # Copy the library to the C# build directory
        add_custom_command(
            TARGET ${LIBRARY_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                    $<TARGET_FILE:touch_reader>
                    ${CSHARP_BUILD_DIR}/
            COMMENT "Copying touch_reader library to C# build directory"
        )

        # Add a custom target to build the C# example
        add_custom_target(csharp_example
            COMMAND ${DOTNET_EXECUTABLE} build -p:RunAnalyzers=false -p:UseSharedCompilation=false -c Release -o ${CSHARP_BUILD_DIR} ${CMAKE_SOURCE_DIR}/src/csharp/TouchScreenExample.csproj
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/csharp
            COMMENT "Building C# example with dotnet"
            DEPENDS ${LIBRARY_NAME}
        )
    else()
        message(STATUS "dotnet not found. C# example will not be built automatically.")
    endif()
endif()
