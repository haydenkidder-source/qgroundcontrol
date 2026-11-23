cmake_minimum_required(VERSION 3.25)

find_package(Git REQUIRED)
set(_work "${CMAKE_CURRENT_BINARY_DIR}/pinned git test")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}/repo" "${_work}/project")

# Runs a command and fails the test if it does not succeed.
function(run_checked)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
    )
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Command failed: ${ARGN}\n${_out}${_err}")
    endif()
endfunction()

# Runs a command and checks that it fails with the expected diagnostic.
function(run_expect_failure expected)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
    )
    string(REGEX REPLACE "[ \t\r\n]+" " " _output "${_out}${_err}")
    if(_result EQUAL 0 OR NOT _output MATCHES "${expected}")
        message(FATAL_ERROR "Expected failure containing '${expected}': ${ARGN}\n${_out}${_err}")
    endif()
    message(STATUS "Expected failure: ${_out}${_err}")
endfunction()

run_checked("${GIT_EXECUTABLE}" -C "${_work}/repo" init -q)
file(WRITE "${_work}/repo/data" "first")
run_checked("${GIT_EXECUTABLE}" -C "${_work}/repo" add data)
run_checked(
    "${GIT_EXECUTABLE}"
    -C
    "${_work}/repo"
    -c
    user.name=test
    -c
    user.email=test
    -c
    commit.gpgsign=false
    commit
    -qm
    first
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_work}/repo" rev-parse HEAD
    OUTPUT_VARIABLE _first
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
file(WRITE "${_work}/repo/data" "second")
run_checked("${GIT_EXECUTABLE}" -C "${_work}/repo" add data)
run_checked(
    "${GIT_EXECUTABLE}"
    -C
    "${_work}/repo"
    -c
    user.name=test
    -c
    user.email=test
    -c
    commit.gpgsign=false
    commit
    -qm
    second
)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_work}/repo" rev-parse HEAD
    OUTPUT_VARIABLE _second
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(WIN32)
    set(_file_url "file:///${_work}/repo")
else()
    set(_file_url "file://${_work}/repo")
endif()
file(WRITE "${_work}/gitconfig" "[url \"${_file_url}\"]\n    insteadOf = https://github.com/test/repo.git\n")
set(ENV{GIT_CONFIG_GLOBAL} "${_work}/gitconfig")
set(ENV{GIT_CONFIG_NOSYSTEM} 1)
file(
    WRITE "${_work}/project/CMakeLists.txt"
    [=[
cmake_minimum_required(VERSION 3.25)
project(PinnedTest NONE)
include("${MODULE_DIR}/CPM.cmake")
include("${MODULE_DIR}/CPMPinnedGitCommit.cmake")
qgc_cpm_add_pinned_git_commit(NAME Example GITHUB_REPOSITORY test/repo COMMIT "${COMMIT}")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${Example_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE result OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT head STREQUAL COMMIT)
    message(FATAL_ERROR "Expected HEAD ${COMMIT}, got ${head}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/source.txt" "${Example_SOURCE_DIR}")
]=]
)

string(REPEAT "a" 39 _short_commit)
string(REPEAT "a" 41 _long_commit)
string(REPEAT "a" 39 _hex_prefix)
string(REPEAT "A" 40 _uppercase_commit)
foreach(_invalid_commit IN ITEMS "${_short_commit}" "${_long_commit}" "${_hex_prefix}g" "${_uppercase_commit}" "")
    run_expect_failure(
        "COMMIT must be exactly 40 lowercase hexadecimal characters"
        "${CMAKE_COMMAND}"
        -S
        "${_work}/project"
        -B
        "${_work}/build-invalid"
        "-DMODULE_DIR=${QGC_MODULE_DIR}/modules"
        "-DCPM_SOURCE_CACHE=${_work}/cache"
        "-DCOMMIT=${_invalid_commit}"
    )
endforeach()

foreach(_case IN ITEMS first second reuse)
    if(_case STREQUAL "second")
        set(_commit "${_second}")
    else()
        set(_commit "${_first}")
    endif()
    run_checked(
        "${CMAKE_COMMAND}"
        -S
        "${_work}/project"
        -B
        "${_work}/build-${_case}"
        "-DMODULE_DIR=${QGC_MODULE_DIR}/modules"
        "-DCPM_SOURCE_CACHE=${_work}/cache"
        "-DCOMMIT=${_commit}"
    )
    if(_case STREQUAL "first")
        file(READ "${_work}/build-first/source.txt" _first_source)
        run_checked(
            "${GIT_EXECUTABLE}"
            -C
            "${_first_source}"
            -c
            user.name=test
            -c
            user.email=test
            -c
            commit.gpgsign=false
            commit
            --allow-empty
            -qm
            tamper
        )
        run_expect_failure(
            "does not match pinned commit"
            "${CMAKE_COMMAND}"
            -S
            "${_work}/project"
            -B
            "${_work}/build-tampered"
            "-DMODULE_DIR=${QGC_MODULE_DIR}/modules"
            "-DCPM_SOURCE_CACHE=${_work}/cache"
            "-DCOMMIT=${_first}"
        )
        run_checked("${GIT_EXECUTABLE}" -C "${_first_source}" reset --hard "${_first}")
        run_checked(
            "${CMAKE_COMMAND}"
            -S
            "${_work}/project"
            -B
            "${_work}/build-restored"
            "-DMODULE_DIR=${QGC_MODULE_DIR}/modules"
            "-DCPM_SOURCE_CACHE=${_work}/cache"
            "-DCOMMIT=${_first}"
        )
    endif()
endforeach()
file(READ "${_work}/build-first/source.txt" _first_source)
file(READ "${_work}/build-second/source.txt" _second_source)
if(_first_source STREQUAL _second_source)
    message(FATAL_ERROR "Different commits share source directory: ${_first_source}")
endif()
file(READ "${_work}/build-reuse/source.txt" _reuse_source)
if(NOT _first_source STREQUAL _reuse_source)
    message(FATAL_ERROR "Same commit did not reuse source directory: ${_first_source} vs ${_reuse_source}")
endif()
file(REMOVE_RECURSE "${_work}")
