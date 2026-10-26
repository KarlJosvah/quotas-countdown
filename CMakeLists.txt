cmake_minimum_required(VERSION 3.15)
project(CountdownWidget C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optional: compile your own SQLite amalgamation (third_party/sqlite3.c + sqlite3.h)
option(USE_BUNDLED_SQLITE "Compile third_party/sqlite3.c instead of using the system SQLite" OFF)

add_executable(CountdownWidget WIN32 main.cpp)

if(USE_BUNDLED_SQLITE)
  target_sources(CountdownWidget PRIVATE third_party/sqlite3.c)
  target_include_directories(CountdownWidget PRIVATE third_party)
  target_compile_definitions(CountdownWidget PRIVATE USE_BUNDLED_SQLITE)
endif()

if(MSVC)
  # Pragmas in main.cpp take care of libs + common-controls manifest, and
  # winsqlite3.lib comes from the Windows SDK.
  target_compile_options(CountdownWidget PRIVATE /utf-8 /W3)
else()
  # MinGW / GCC: #pragma comment(lib) is ignored, so link everything explicitly.
  enable_language(RC)
  target_sources(CountdownWidget PRIVATE app.rc)          # Common-Controls v6 manifest
  target_link_options(CountdownWidget PRIVATE -municode -static -static-libgcc -static-libstdc++)
  target_link_libraries(CountdownWidget PRIVATE comctl32 shell32 gdi32 user32 ole32)

  if(NOT USE_BUNDLED_SQLITE)
    find_library(SQLITE3_LIB NAMES sqlite3)
    if(SQLITE3_LIB)
      target_link_libraries(CountdownWidget PRIVATE ${SQLITE3_LIB})
    else()
      # No libsqlite3 installed: link straight against the DLL that ships with Windows 10.
      # (Only the header sqlite3.h is needed at compile time.)
      target_link_libraries(CountdownWidget PRIVATE "$ENV{SystemRoot}/System32/winsqlite3.dll")
    endif()
  endif()
endif()
