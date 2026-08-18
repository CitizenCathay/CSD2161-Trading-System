# CMakeList.txt : CMake project for Backend, include source and define
# project specific logic here.
#

file(GLOB_RECURSE BACKEND_SOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

file(GLOB_RECURSE BACKEND_HEADER_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
)

add_executable(Backend)
target_sources(Backend PRIVATE ${BACKEND_SOURCE_FILES} ${BACKEND_HEADER_FILES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${BACKEND_SOURCE_FILES} ${BACKEND_HEADER_FILES})

foreach(include_dir IN ITEMS
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
  if (EXISTS "${include_dir}")
    target_include_directories(Backend PRIVATE "${include_dir}")
  endif()
endforeach()

if (WIN32)
  target_link_libraries(Backend PRIVATE ws2_32 bcrypt crypt32)
endif()

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(Backend PRIVATE unofficial::sqlite3::sqlite3)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  target_compile_features(Backend PRIVATE cxx_std_20)
endif()

# TODO: Add tests and install targets if needed.
