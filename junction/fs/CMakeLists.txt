message(STATUS "Building junction filesystem")

option(SHAOFS_CRASH_CONSISTENCY "Enable ShaOFS metadata journal" ON)

find_library(DML_LIB   # 查找 libdml 静态库，避免运行时依赖 libdml.so
    NAMES libdml.a
    PATHS /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64
    NO_DEFAULT_PATH
)

find_path(DML_INCLUDE_DIR   # 查找 dml.h 头文件路径
    NAMES dml/dml.h 
    PATHS /usr/local/include /usr/include
)

find_library(UUID_LIB NAMES uuid)  # 查找 libuuid (DML 硬件路径通常依赖 UUID)

if (DML_LIB AND DML_INCLUDE_DIR)   # 检查是否找到
    message(STATUS "Found Intel DML Library: ${DML_LIB}")
    message(STATUS "Found Intel DML Header: ${DML_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "Could NOT find Intel DML static library. Please install libdml.a to /usr/local/lib")
endif()

add_library(fs
  advisory_lock.cc
  core.cc
  dev.cc
  eventfd.cc
  file.cc
  linuxfs/dir.cc
  linuxfs/linuxfile.cc
  linuxfs/linuxfs.cc
  memfs/memfs.cc
  memfs/dir.cc
  pipe.cc
  poll.cc
  procfs/procfs.cc
  stdiofile.cc

  shaofs/fs.cc
  shaofs/group.cc
  shaofs/blockCache.cc
  shaofs/file.cc
  shaofs/inode.cc
  shaofs/inodeCache.cc
  shaofs/dentryCache.cc
  shaofs/extent.cc
  shaofs/dsa.cc
  shaofs/journal.cc
  shaofs/syscall.cc
  shaofs/namei.cc
  shaofs/dir.cc
)

target_compile_definitions(fs PRIVATE
  CRASH_CONSISTENCY=$<IF:$<BOOL:${SHAOFS_CRASH_CONSISTENCY}>,1,0>
)

target_include_directories(fs PRIVATE ${DML_INCLUDE_DIR})   # 添加头文件包含路径

target_link_libraries(fs
  base
  bindings
  kernel

  ${DML_LIB}   # 链接 libdml
  ${UUID_LIB}  # 链接 libuuid (防止 undefined reference)
)

# Tests
add_executable(memfs_test
  memfs/memfs_test.cc
)
target_link_libraries(memfs_test
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,gtest_main>"
)

if (WRITEABLE_LINUX_FS)
else()
add_test(
  NAME memfs_test_junction
  COMMAND sh -c "sudo -E $<TARGET_FILE:junction_run> ${caladan_test_config_path} $EXTRA_JUNCTION_FLAGS -- $<TARGET_FILE:memfs_test>"
)
endif()
