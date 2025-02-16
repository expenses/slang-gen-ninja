cmake_minimum_required(VERSION 3.10)
project(slang-gen-ninja)

# Find Slang
find_package(slang REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable warnings for all targets
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # GCC and Clang
    add_compile_options(-Wall -Wextra -pedantic -Werror)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC
    #add_compile_options(/W4 /WX)
endif()

# Add executable
add_executable(slang-gen-ninja src/main.cpp)
target_precompile_headers(slang-gen-ninja PRIVATE src/pch.h)
# Link Slang
target_link_libraries(slang-gen-ninja slang::slang)
