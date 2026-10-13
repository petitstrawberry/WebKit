cmake_minimum_required(VERSION 3.24)
project(ScarletSQLite VERSION 3.50.4 LANGUAGES C)
if (NOT EXISTS "${SCARLET_SQLITE_SOURCE_DIR}/sqlite3.c")
    message(FATAL_ERROR "SCARLET_SQLITE_SOURCE_DIR must contain the pinned SQLite amalgamation")
endif ()
add_library(sqlite3 STATIC "${SCARLET_SQLITE_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${SCARLET_SQLITE_SOURCE_DIR}")
# Native FileLock implements real flock, not POSIX byte-range fcntl locks.
target_compile_definitions(sqlite3 PRIVATE SQLITE_OS_UNIX=1 SQLITE_THREADSAFE=1
    SQLITE_ENABLE_LOCKING_STYLE=1 SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_MAX_MMAP_SIZE=0 SQLITE_TEMP_STORE=3
    HAVE_LOCALTIME_R=1 HAVE_GMTIME_R=1 HAVE_PREAD=1 HAVE_PWRITE=1)
install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES "${SCARLET_SQLITE_SOURCE_DIR}/sqlite3.h" "${SCARLET_SQLITE_SOURCE_DIR}/sqlite3ext.h" DESTINATION include)
