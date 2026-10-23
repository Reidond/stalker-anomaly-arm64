# libarchive (BSD-2-Clause) and liblzma from xz (0BSD / public domain): the mod importer reads .zip, .7z and .rar
# (RAR 4 and 5) with them. Built from the pinned submodules external/libarchive and external/xz; zlib, bzip2 and iconv come
# from the SDK (macOS and iOS both have them).
#
# libarchive's own CMake probes ~400 headers and functions with try_compile, which is slow and can't see a liblzma that is
# built in the same tree, so this file compiles the sources the importer needs with a fixed config.h
# (cmake/deps/libarchive/config.h, the same for macOS and iOS). When the submodule is updated, compare the source list
# below with libarchive/CMakeLists.txt and config.h with build/cmake/config.h.in.

set(libarchive_dir "${ANOMALY_EXTERNAL_DIR}/libarchive")
set(xz_dir "${ANOMALY_EXTERNAL_DIR}/xz")
foreach(dep libarchive xz)
    if(NOT EXISTS "${ANOMALY_EXTERNAL_DIR}/${dep}/.git")
        message(FATAL_ERROR "external/${dep} is missing; run: git submodule update --init --depth 1 external/${dep}")
    endif()
endforeach()

# --- liblzma (7z needs LZMA, LZMA2 and the BCJ filters) -------------------------------------------------------------------
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${xz_dir}" "${CMAKE_BINARY_DIR}/external/xz" EXCLUDE_FROM_ALL SYSTEM)
target_compile_options(liblzma PRIVATE -w)

# --- libarchive ------------------------------------------------------------------------------------------------------------
# No disk reader/writer (the importer writes files itself, with its own path checks), no external-program filters (fork is not
# available on iOS), no tar/cpio/iso/xar/...: only the zip, 7-zip and RAR readers, plus the zip and 7-zip writers the tests use
# to make their fixtures.
set(libarchive_src "${libarchive_dir}/libarchive")
set(libarchive_sources
    archive_acl.c
    archive_blake2s_ref.c
    archive_blake2sp_ref.c
    archive_check_magic.c
    archive_crc32.c
    archive_cryptor.c
    archive_digest.c
    archive_entry.c
    archive_entry_copy_stat.c
    archive_entry_link_resolver.c
    archive_entry_sparse.c
    archive_entry_stat.c
    archive_entry_strmode.c
    archive_entry_xattr.c
    archive_hmac.c
    archive_options.c
    archive_pack_dev.c
    archive_parse_date.c
    archive_ppmd7.c
    archive_ppmd8.c
    archive_random.c
    archive_rb.c
    archive_read.c
    archive_read_add_passphrase.c
    archive_read_open_filename.c
    archive_read_set_format.c
    archive_read_set_options.c
    archive_read_support_filter_none.c
    archive_read_support_format_7zip.c
    archive_read_support_format_rar.c
    archive_read_support_format_rar5.c
    archive_read_support_format_zip.c
    archive_string.c
    archive_string_sprintf.c
    archive_time.c
    archive_util.c
    archive_version_details.c
    archive_virtual.c
    archive_write.c
    archive_write_add_filter_none.c
    archive_write_open_fd.c
    archive_write_open_filename.c
    archive_write_set_format_7zip.c
    archive_write_set_format_zip.c
    archive_write_set_options.c
    archive_write_set_passphrase.c
    xxhash.c
)
list(TRANSFORM libarchive_sources PREPEND "${libarchive_src}/")
list(APPEND libarchive_sources "${CMAKE_CURRENT_LIST_DIR}/libarchive/write_support.c")

add_library(anomaly_libarchive STATIC ${libarchive_sources})
target_include_directories(anomaly_libarchive
    PUBLIC "${libarchive_src}"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libarchive" "${xz_dir}/src/liblzma/api")
target_compile_definitions(anomaly_libarchive PUBLIC LIBARCHIVE_STATIC PRIVATE HAVE_CONFIG_H)
target_compile_options(anomaly_libarchive PRIVATE -w)
target_link_libraries(anomaly_libarchive PUBLIC liblzma z bz2)
add_library(libarchive::archive ALIAS anomaly_libarchive)
