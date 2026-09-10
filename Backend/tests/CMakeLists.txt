# Tests and benchmarks for the Backend.
#
# These live outside Backend/src on purpose: Backend/CMakeLists.txt globs src/*.cpp
# recursively into the Backend executable, so a second main() placed there would
# collide with Main.cpp.
#
# Each target compiles the production sources it exercises directly, because Backend
# is an executable and cannot be linked against.

find_package(unofficial-sqlite3 CONFIG REQUIRED)

set(BACKEND_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Production sources under test: the matching engine and its persistence layer.
set(ENGINE_SOURCES
  "${BACKEND_SRC}/services/trade_service.cpp"
  "${BACKEND_SRC}/database/db.cpp"
)

# The raw-socket HTTP layer.
set(SERVER_SOURCES
  "${BACKEND_SRC}/server/http_server.cpp"
  "${BACKEND_SRC}/server/router.cpp"
)

function(add_backend_target name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${BACKEND_SRC}")
  target_link_libraries(${name} PRIVATE unofficial::sqlite3::sqlite3)
  if (WIN32)
    target_link_libraries(${name} PRIVATE ws2_32 bcrypt crypt32)
  endif()
  if (CMAKE_VERSION VERSION_GREATER 3.12)
    target_compile_features(${name} PRIVATE cxx_std_20)
  endif()
endfunction()

# ---- correctness tests -------------------------------------------------------
# Semantic coverage of the matching engine: price priority, time priority, the
# execution price rule, partial fills, balance checks, cancellation, conservation.
add_backend_target(TestMatching "${CMAKE_CURRENT_SOURCE_DIR}/test_matching.cpp" ${ENGINE_SOURCES})

add_test(NAME matching_engine COMMAND TestMatching)

# ---- benchmarks --------------------------------------------------------------
# Not registered with CTest: these take minutes and measure throughput, so they are
# run deliberately rather than as part of a test sweep.
add_backend_target(BenchMatching "${CMAKE_CURRENT_SOURCE_DIR}/bench_matching.cpp" ${ENGINE_SOURCES})
add_backend_target(BenchHttp     "${CMAKE_CURRENT_SOURCE_DIR}/bench_http.cpp"     ${ENGINE_SOURCES} ${SERVER_SOURCES})
