# storage: where the web server's files live when they can be changed
# without new firmware.
#
#   SpiNorFlash        a SPI NOR flash chip (W25Q, MX25L, IS25LP, SST26...)
#                      over itransport's iBlockTransport: JEDEC ID, size,
#                      3- or 4-byte addressing, unprotecting, read,
#                      program, 4 KB erase. Always built (inet_spinor).
#   LittleFsNor        LittleFS on it, with static buffers (LFS_NO_MALLOC);
#   HttpLittleFsFiles  its files served (HttpFileSource) and uploaded
#                      (HttpFileStore). With LITTLEFS_DIR (inet_littlefs).
#   HttpFatFsFiles     the same on FatFs, for an SD card. The application
#                      builds FatFs (CubeMX's FATFS middleware, its
#                      ffconf.h, its diskio for the card); this is just
#                      the one source file to add to it. With FATFS_DIR,
#                      the host test builds FatFs itself, with
#                      test/fatfs/ffconf.h and a RAM disk.
#
# Added by iNetTransport/CMakeLists.txt.
#   LITTLEFS_DIR: git clone --branch v2.9.3 https://github.com/littlefs-project/littlefs
#   FATFS_DIR:    git clone https://github.com/STMicroelectronics/stm32_mw_fatfs
#                 (R0.15, as CubeMX ships it; or elm-chan's ff15)
add_library(inet_spinor STATIC
    inc/SpiNorFlash.h
    src/SpiNorFlash.cpp
)
target_include_directories(inet_spinor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(inet_spinor PUBLIC sensor_transport inet_core)
target_compile_features(inet_spinor PUBLIC cxx_std_17)
set_target_properties(inet_spinor PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

set(LITTLEFS_DIR "" CACHE PATH "A LittleFS source tree (2.9 or later), for LittleFsNor")
set(FATFS_DIR "" CACHE PATH "A FatFs R0.15 source tree, for HttpFatFs_test")

if(NOT LITTLEFS_DIR STREQUAL "")
    enable_language(C)
    add_library(littlefs_inet STATIC ${LITTLEFS_DIR}/lfs.c ${LITTLEFS_DIR}/lfs_util.c)
    target_include_directories(littlefs_inet PUBLIC ${LITTLEFS_DIR})
    target_compile_definitions(littlefs_inet PUBLIC LFS_NO_MALLOC)
    set_target_properties(littlefs_inet PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

    add_library(inet_littlefs STATIC
        inc/LittleFsNor.h
        inc/HttpLittleFsFiles.h
        src/LittleFsNor.cpp
        src/HttpLittleFsFiles.cpp
    )
    target_link_libraries(inet_littlefs PUBLIC inet_spinor littlefs_inet inet_http)
    set_target_properties(inet_littlefs PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

    if(SENSOR_FW_BUILD_TESTS)
        find_package(Threads REQUIRED)
        add_executable(SpiNorFlash_test test/SpiNorFlash_test.cpp)
        target_include_directories(SpiNorFlash_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
        target_link_libraries(SpiNorFlash_test PRIVATE inet_littlefs Threads::Threads)
        add_test(NAME SpiNorFlash_test COMMAND SpiNorFlash_test)
    endif()
endif()

if(SENSOR_FW_BUILD_TESTS AND NOT FATFS_DIR STREQUAL "")
    enable_language(C)
    find_package(Threads REQUIRED)
    set(FATFS_SRC ${FATFS_DIR}/source)
    if(NOT EXISTS ${FATFS_SRC}/ff.c)
        set(FATFS_SRC ${FATFS_DIR})
    endif()
    add_library(fatfs_host STATIC ${FATFS_SRC}/ff.c ${FATFS_SRC}/ffunicode.c test/fatfs/RamDisk.cpp)
    target_include_directories(fatfs_host PUBLIC ${FATFS_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/test/fatfs)

    add_executable(HttpFatFs_test test/HttpFatFs_test.cpp src/HttpFatFsFiles.cpp)
    target_include_directories(HttpFatFs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
    target_link_libraries(HttpFatFs_test PRIVATE fatfs_host inet_core inet_http Threads::Threads)
    add_test(NAME HttpFatFs_test COMMAND HttpFatFs_test)
endif()
