message(STATUS "Building junction filesystem")

find_library(DML_LIB   # 查找 libdml (通常在 /usr/local/lib 或 /usr/lib64)
    NAMES dml 
    PATHS /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64
    NO_DEFAULT_PATH    # 以此优先查找指定路径，若失败去掉此行可查系统默认
)
if (NOT DML_LIB)       # 如果上面找不到，尝试在默认系统路径找
    find_library(DML_LIB NAMES dml)
endif()

find_path(DML_INCLUDE_DIR   # 查找 dml.h 头文件路径
    NAMES dml/dml.h 
    PATHS /usr/local/include /usr/include
)

find_library(UUID_LIB NAMES uuid)  # 查找 libuuid (DML 硬件路径通常依赖 UUID)

if (DML_LIB AND DML_INCLUDE_DIR)   # 检查是否找到
    message(STATUS "Found Intel DML Library: ${DML_LIB}")
    message(STATUS "Found Intel DML Header: ${DML_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "Could NOT find Intel DML library. Please install it to /usr/local")
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

  shaofs/disk.cc
  shaofs/group.cc
  shaofs/blockCache.cc
  shaofs/file.cc
  shaofs/dentry.cc
  shaofs/inode.cc
  shaofs/inodeCache.cc
  shaofs/dentryCache.cc
  shaofs/extent.cc
  shaofs/dsa.cc
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
