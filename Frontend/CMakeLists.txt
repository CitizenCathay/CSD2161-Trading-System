# CMakeList.txt : CMake project for Frontend, include source and define
# project specific logic here.
#

file(GLOB_RECURSE FRONTEND_SOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

file(GLOB_RECURSE FRONTEND_HEADER_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hh"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hxx"
)

add_executable(Frontend)
target_sources(Frontend PRIVATE ${FRONTEND_SOURCE_FILES} ${FRONTEND_HEADER_FILES})

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${FRONTEND_SOURCE_FILES} ${FRONTEND_HEADER_FILES})

foreach(include_dir IN ITEMS
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
  if (EXISTS "${include_dir}")
    target_include_directories(Frontend PRIVATE "${include_dir}")
  endif()
endforeach()

target_compile_definitions(Frontend PRIVATE FRONTEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if (WIN32)
  target_link_libraries(Frontend PRIVATE ws2_32)
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/public")
  add_custom_command(TARGET Frontend POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/public"
            "$<TARGET_FILE_DIR:Frontend>/public"
  )
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  target_compile_features(Frontend PRIVATE cxx_std_20)
endif()

# TODO: Add tests and install targets if needed.
