# CMakeList.txt : CMake project for PostgreSQLTest, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (PostgreSQLTest "PostgreSQLTest.cpp" "PostgreSQLTest.h" "DatabaseManager.hpp" "ConnectionPool.h" "ConnectionPool.cpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET PostgreSQLTest PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.

# Link and include libpq
target_include_directories(PostgreSQLTest PRIVATE "${LIBPQ_INCLUDE_DIR}")
target_link_directories(PostgreSQLTest PRIVATE "${LIBPQ_LIB_DIR}")
target_link_libraries(PostgreSQLTest PRIVATE libpq)
