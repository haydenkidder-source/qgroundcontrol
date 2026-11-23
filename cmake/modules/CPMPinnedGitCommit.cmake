if(DEFINED QGC_PINNED_GIT_DOWNLOAD)
    if(NOT EXISTS "${QGC_PINNED_GIT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${QGC_PINNED_GIT_EXECUTABLE}" init
            WORKING_DIRECTORY "${QGC_PINNED_GIT_SOURCE_DIR}"
            RESULT_VARIABLE _result
        )
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "QGC: git init failed in ${QGC_PINNED_GIT_SOURCE_DIR}")
        endif()
    endif()
    execute_process(
        COMMAND "${QGC_PINNED_GIT_EXECUTABLE}" -c gc.auto=0 fetch --depth 1 "${QGC_PINNED_GIT_URL}"
                "${QGC_PINNED_GIT_COMMIT}"
        WORKING_DIRECTORY "${QGC_PINNED_GIT_SOURCE_DIR}"
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "QGC: git fetch failed for ${QGC_PINNED_GIT_COMMIT} from ${QGC_PINNED_GIT_URL}")
    endif()
    execute_process(
        COMMAND "${QGC_PINNED_GIT_EXECUTABLE}" checkout --detach FETCH_HEAD
        WORKING_DIRECTORY "${QGC_PINNED_GIT_SOURCE_DIR}"
        RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "QGC: git checkout failed for ${QGC_PINNED_GIT_COMMIT}")
    endif()
    return()
endif()

include_guard(GLOBAL)

# Downloads a Git repository at a reviewed commit and exposes its source directory.
function(qgc_cpm_add_pinned_git_commit)
    # cmake-lint: disable=C0103
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "NAME;GITHUB_REPOSITORY;COMMIT" "")
    if(ARG_KEYWORDS_MISSING_VALUES
       OR ARG_UNPARSED_ARGUMENTS
       OR NOT ARG_NAME
       OR NOT ARG_GITHUB_REPOSITORY
    )
        message(FATAL_ERROR "QGC: qgc_cpm_add_pinned_git_commit requires NAME, GITHUB_REPOSITORY and COMMIT")
    endif()
    string(LENGTH "${ARG_COMMIT}" _commit_length)
    if(NOT _commit_length EQUAL 40 OR NOT ARG_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "QGC: COMMIT must be exactly 40 lowercase hexadecimal characters")
    endif()

    find_package(Git REQUIRED)
    if(CPM_SOURCE_CACHE)
        string(SHA256 _cache_key "${ARG_NAME};${ARG_GITHUB_REPOSITORY};${ARG_COMMIT}")
        set(FETCHCONTENT_BASE_DIR "${CPM_SOURCE_CACHE}/qgc-pinned/${_cache_key}")
        file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
        file(
            LOCK "${FETCHCONTENT_BASE_DIR}/qgc.lock"
            GUARD FUNCTION
            TIMEOUT 300
        )
    endif()
    CPMAddPackage(
        NAME "${ARG_NAME}"
        DOWNLOAD_ONLY TRUE
        DOWNLOAD_COMMAND
            "${CMAKE_COMMAND}"
            "-DQGC_PINNED_GIT_DOWNLOAD=ON"
            "-DQGC_PINNED_GIT_SOURCE_DIR=<SOURCE_DIR>"
            "-DQGC_PINNED_GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DQGC_PINNED_GIT_URL=https://github.com/${ARG_GITHUB_REPOSITORY}.git"
            "-DQGC_PINNED_GIT_COMMIT=${ARG_COMMIT}"
            -P
            "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    )
    if(NOT EXISTS "${${ARG_NAME}_SOURCE_DIR}/.git")
        message(FATAL_ERROR "QGC: ${ARG_NAME} source directory is not a Git checkout: ${${ARG_NAME}_SOURCE_DIR}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${${ARG_NAME}_SOURCE_DIR}" rev-parse HEAD
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _head
        ERROR_VARIABLE _error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _result EQUAL 0 OR NOT _head STREQUAL ARG_COMMIT)
        message(FATAL_ERROR "QGC: ${ARG_NAME} HEAD '${_head}' does not match pinned commit '${ARG_COMMIT}': ${_error}")
    endif()
    set(${ARG_NAME}_SOURCE_DIR
        "${${ARG_NAME}_SOURCE_DIR}"
        PARENT_SCOPE
    )
endfunction()
