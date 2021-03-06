cmake_minimum_required (VERSION 3.5.1)
project (libcineform C CXX)

# Build settings
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
add_definitions(-D_ALLOCATOR=1)
option(BUILD_STATIC_LIBS "Build static library" ON)
option(BUILD_SHARED_LIBS "Build shared library" ON)

if (WIN32)
    set(COMPILER_FLAGS "")

    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        set(COMPILER_FLAGS_W_OMP "/openmp")
    else ()
        set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--allow-multiple-definition")
    endif ()
    set(ADDITIONAL_LIBS "")
endif (WIN32)

if (UNIX)
    set(COMPILER_FLAGS -fPIC -O3)
    set(COMPILER_FLAGS_W_OMP -fopenmp -O3)
    set(INTERNAL_LIBS "-lpthread -lgomp")
    set(ADDITIONAL_LIBS "-lm -luuid")
endif (UNIX)

if (APPLE)
    set(COMPILER_FLAGS -fvisibility=hidden -O3)
    set(COMPILER_FLAGS_W_OMP -O3)
    set(INTERNAL_LIBS "-lpthread")
    set(ADDITIONAL_LIBS "-lm")
endif (APPLE)

# Source files
include_directories("Common" "Tables" "Codec" "ConvertLib")
file(GLOB PUBLIC_HEADERS "Common/CFHD*.h")
file(GLOB CODEC_SOURCES "Codec/*.c" "Codec/*.h" "Codec/*.cpp" "Common/Settings.cpp")
file(GLOB ENCODER_SOURCES "EncoderSDK/*.cpp" "Common/*.h" )
file(GLOB DECODER_SOURCES "DecoderSDK/*.cpp" "Common/*.h" "ConvertLib/*.cpp" "ConvertLib/*.h")

# Build library (static and shared rules)
if (BUILD_STATIC_LIBS)
    add_library(CineFormStatic STATIC ${CODEC_SOURCES} ${ENCODER_SOURCES} ${DECODER_SOURCES})
    target_compile_options(CineFormStatic PUBLIC ${COMPILER_FLAGS})
    set_target_properties(CineFormStatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (UNIX)
        set_target_properties(CineFormStatic PROPERTIES OUTPUT_NAME cineform)
    endif (UNIX)
    target_link_libraries(CineFormStatic)
endif (BUILD_STATIC_LIBS)

if (BUILD_SHARED_LIBS)
    add_library(CineFormShared SHARED ${CODEC_SOURCES} ${ENCODER_SOURCES} ${DECODER_SOURCES})
    set_target_properties(CineFormShared PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_target_properties(CineFormShared PROPERTIES OUTPUT_NAME cineform)
    target_compile_options(CineFormShared PUBLIC ${COMPILER_FLAGS})
    target_compile_definitions(CineFormShared PUBLIC -DDYNAMICLIB=1)
    target_link_libraries(CineFormShared)
endif (BUILD_SHARED_LIBS)

# pkg-config integration
set(PROJECT_VERSION "0.2")
set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name")
set(EXEC_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX} CACHE PATH "Installation prefix for executables and object code libraries" FORCE)
set(BIN_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/bin CACHE PATH "Installation prefix for user executables" FORCE)
set(LIB_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/lib${LIB_SUFFIX} CACHE PATH "Installation prefix for object code libraries" FORCE)
set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/include/libcineform CACHE PATH "Installation prefix for header files" FORCE)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libcineform.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/libcineform.pc)

# System wide installation
if (UNIX)
    if (BUILD_STATIC_LIBS)
        install(TARGETS CineFormStatic DESTINATION lib/)
    endif (BUILD_STATIC_LIBS)
    if (BUILD_SHARED_LIBS)
        install(TARGETS CineFormShared DESTINATION lib/)
    endif (BUILD_SHARED_LIBS)

    install(FILES ${PUBLIC_HEADERS} DESTINATION include/libcineform/)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libcineform.pc DESTINATION lib/pkgconfig/)
endif (UNIX)
