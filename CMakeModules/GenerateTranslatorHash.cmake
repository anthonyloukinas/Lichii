# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Hashes what decides the result of a shader translation and writes the hash to OUTPUT as a C++
# source. Runs as a script on every build; OUTPUT is rewritten only when the hash changes.
#   cmake -DSOURCE_ROOT=<repository root> -DOUTPUT=<file> -DINPUTS=<file> [-DCONFIG=<configuration>]
#         -P GenerateTranslatorHash.cmake
# INPUTS is written at configure time and sets
#   TRANSLATOR_EMITTER_PATHS  files and directories of the SPIR-V emitter library the build uses
#   TRANSLATOR_HEADER_PATHS   the SPIR-V headers it is built against (may be empty)
#   TRANSLATOR_BUILD_CONFIG   target, compiler and compile flags
# The hash is empty, which keeps the translation cache off disk, when the emitter library is unknown.

if (NOT SOURCE_ROOT OR NOT OUTPUT OR NOT INPUTS)
    message(FATAL_ERROR "GenerateTranslatorHash: SOURCE_ROOT, OUTPUT and INPUTS are required")
endif()
include("${INPUTS}")

set(translator_dir "src/shader_recompiler")
# Hashed but not followed: the environment a translation asks its questions through, the cache
# itself, and the code that runs a translation (what it does to a program before the SPIR-V is
# emitted and which stages it admits are not in any key).
set(translator_callers
    src/shader_recompiler/CMakeLists.txt
    src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
    src/video_core/renderer_vulkan/vk_pipeline_cache.h
    src/video_core/shader_environment.cpp
    src/video_core/shader_environment.h
    src/video_core/shader_translation_cache.cpp
    src/video_core/shader_translation_cache.h)
# Followed but not hashed: logging, asserts and the settings machinery cannot change what a
# translation produces. Every setting the translator reads is in the cache's configuration digest.
set(translator_neutral "^src/common/(logging\\.|log_classes\\.inc$|assert\\.|settings)")

file(GLOB_RECURSE translator_files RELATIVE "${SOURCE_ROOT}" LIST_DIRECTORIES false
    "${SOURCE_ROOT}/${translator_dir}/*.cpp"
    "${SOURCE_ROOT}/${translator_dir}/*.h"
    "${SOURCE_ROOT}/${translator_dir}/*.inc")

# The project headers the translator includes, directly or not, with the source next to each.
set(visited ${translator_files})
set(pending ${translator_files})
while (pending)
    list(POP_FRONT pending current)
    file(STRINGS "${SOURCE_ROOT}/${current}" include_lines
        REGEX "^[ \t]*#[ \t]*include[ \t]*\"[^\"]+\"")
    cmake_path(GET current PARENT_PATH current_dir)
    foreach (include_line IN LISTS include_lines)
        string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$" "\\1" include_name
            "${include_line}")
        set(included "")
        foreach (candidate IN ITEMS "src/${include_name}" "${current_dir}/${include_name}")
            cmake_path(NORMAL_PATH candidate)
            if (EXISTS "${SOURCE_ROOT}/${candidate}" AND NOT IS_DIRECTORY "${SOURCE_ROOT}/${candidate}")
                set(included "${candidate}")
                break()
            endif()
        endforeach()
        if (NOT included OR included IN_LIST visited)
            continue()
        endif()
        list(APPEND visited "${included}")
        list(APPEND pending "${included}")
        if (NOT included MATCHES "^${translator_dir}/" AND included MATCHES "\\.h$")
            string(REGEX REPLACE "\\.h$" ".cpp" implementation "${included}")
            if (EXISTS "${SOURCE_ROOT}/${implementation}" AND NOT implementation IN_LIST visited)
                list(APPEND visited "${implementation}")
            endif()
        endif()
    endforeach()
endwhile()

list(APPEND visited ${translator_callers})
list(REMOVE_DUPLICATES visited)
list(SORT visited)
set(manifest "")
foreach (translator_file IN LISTS visited)
    if (translator_file MATCHES "${translator_neutral}")
        continue()
    endif()
    file(SHA256 "${SOURCE_ROOT}/${translator_file}" file_hash)
    string(APPEND manifest "${translator_file} ${file_hash}\n")
endforeach()

# The libraries are named by position and relative path, so the hash does not depend on where a
# checkout or a package cache lives.
function(append_library_files label paths)
    set(index 0)
    set(found FALSE)
    foreach (library_path IN LISTS paths)
        if (IS_DIRECTORY "${library_path}")
            file(GLOB_RECURSE library_files RELATIVE "${library_path}" LIST_DIRECTORIES false
                "${library_path}/*")
            list(SORT library_files)
            foreach (library_file IN LISTS library_files)
                if (library_file MATCHES "(^|/)(\\.|tests/)")
                    continue()
                endif()
                file(SHA256 "${library_path}/${library_file}" file_hash)
                string(APPEND manifest "${label} ${index} ${library_file} ${file_hash}\n")
                set(found TRUE)
            endforeach()
        elseif (EXISTS "${library_path}")
            file(SHA256 "${library_path}" file_hash)
            cmake_path(GET library_path FILENAME library_file)
            string(APPEND manifest "${label} ${index} ${library_file} ${file_hash}\n")
            set(found TRUE)
        endif()
        math(EXPR index "${index} + 1")
    endforeach()
    set(manifest "${manifest}" PARENT_SCOPE)
    set(${label}_found ${found} PARENT_SCOPE)
endfunction()

append_library_files(emitter "${TRANSLATOR_EMITTER_PATHS}")
append_library_files(headers "${TRANSLATOR_HEADER_PATHS}")
string(APPEND manifest "build ${TRANSLATOR_BUILD_CONFIG}\n")
string(APPEND manifest "configuration ${CONFIG}\n")

if (emitter_found)
    string(SHA256 translator_hash "${manifest}")
else()
    message(WARNING "GenerateTranslatorHash: the SPIR-V emitter library was not found, "
        "the shader translation cache will not be kept on disk")
    set(translator_hash "")
endif()

set(content "// Generated by CMakeModules/GenerateTranslatorHash.cmake. Do not edit.

namespace VideoCommon::TranslationCache {

extern const char g_translator_source_hash[];
const char g_translator_source_hash[] = \"${translator_hash}\";

} // namespace VideoCommon::TranslationCache
")

function(write_if_changed path text)
    set(previous "")
    if (EXISTS "${path}")
        file(READ "${path}" previous)
    endif()
    if (NOT previous STREQUAL text)
        file(WRITE "${path}" "${text}")
    endif()
endfunction()

write_if_changed("${OUTPUT}" "${content}")
# What went into the hash, to see why two builds do not share a cache.
write_if_changed("${OUTPUT}.manifest.txt" "${manifest}")
