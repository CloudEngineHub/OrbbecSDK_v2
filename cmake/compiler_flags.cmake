# Copyright (c) Orbbec Inc. All Rights Reserved.
# Licensed under the MIT License.

if (NOT ("${CMAKE_C_COMPILER_ID}" STREQUAL "${CMAKE_CXX_COMPILER_ID}"))
    message(FATAL_ERROR "C compiler (${CMAKE_C_COMPILER_ID}) does not match C++ compiler (${CMAKE_CXX_COMPILER_ID})")
endif()

if ("${CMAKE_C_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
    set(CLANG_ALL_WARNINGS "-Weverything")
    list(APPEND CLANG_ALL_WARNINGS "-Wno-missing-field-initializers") # Allow c structs without all fields initialized
    list(APPEND CLANG_ALL_WARNINGS "-Wno-reserved-id-macro") # Needed for azure-c-shared-utility which defines new macros that start with "_"
    list(APPEND CLANG_ALL_WARNINGS "-Wno-gnu-zero-variadic-macro-arguments") # Needed too allow variadic macros with zero args
    list(APPEND CLANG_ALL_WARNINGS "-Wno-extra-semi") # Allow for multiple semi-colons in a row
    list(APPEND CLANG_ALL_WARNINGS "-Wno-c++98-compat-pedantic") # Allow commas on the last enum value
    list(APPEND CLANG_ALL_WARNINGS "-Wno-padded") # Do not warn about inserted padding to structs
    list(APPEND CLANG_ALL_WARNINGS "-Wno-switch-enum") # Do not warn about missing case statements in enums
    list(APPEND CLANG_ALL_WARNINGS "-Wno-old-style-cast") # Allow old style c casts
    list(APPEND CLANG_ALL_WARNINGS "-Wno-global-constructors") # Allow global constructors. Needed for gtest
    list(APPEND CLANG_ALL_WARNINGS "-Wno-newline-eof") # Allow no newline at eof. Needed for azure-c-utility
    list(APPEND CLANG_ALL_WARNINGS "-Wno-exit-time-destructors") # Allow exit time destructors. Needed for spdlog
    list(APPEND CLANG_ALL_WARNINGS "-Wno-weak-vtables") # Allow weak vtables. Needed for spdlog
    list(APPEND CLANG_ALL_WARNINGS "-Wno-undef") # Allow undefined macros. Needed for azure-c-shared-utility
    list(APPEND CLANG_ALL_WARNINGS "-Wno-disabled-macro-expansion") # Allow recursive macro expansion
    list(APPEND CLANG_ALL_WARNINGS "-Wno-documentation-unknown-command") # Allow undocumented documentation commands used by doxygen
    list(APPEND CLANG_ALL_WARNINGS "-Wno-covered-switch-default") # Allow default: in switch statements that cover all enum values
    list(APPEND CLANG_ALL_WARNINGS "-Wno-unreachable-code-break") # Allow break even if it is unreachable
    list(APPEND CLANG_ALL_WARNINGS "-Wno-double-promotion") # Allow floats to be promoted to doubles. Needed for isnan() on some systems
    list(APPEND CLANG_ALL_WARNINGS "-Wno-inconsistent-missing-destructor-override") # Allow for android wrapper
    list(APPEND CLANG_ALL_WARNINGS "-Wno-nested-anon-types") # Allow for android wrapper
    list(APPEND CLANG_ALL_WARNINGS "-Wno-missing-braces") # Allow for android wrapper
    list(APPEND CLANG_ALL_WARNINGS "-Wno-braced-scalar-init") # Allow for android wrapper

    if (NOT (${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS "5.0.0"))
        # Added in clang 5
        list(APPEND CLANG_ALL_WARNINGS "-Wno-zero-as-null-pointer-constant") # Allow zero as nullptr
    endif()
    if (NOT (${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS "8.0.0"))
        # Added in clang 8
        list(APPEND CLANG_ALL_WARNINGS "-Wno-extra-semi-stmt") # Allow semi-colons to be used after #define's
        list(APPEND CLANG_ALL_WARNINGS "-Wno-atomic-implicit-seq-cst") # Allow use of __sync_add_and_fetch() atomic
    endif()
    if(NOT "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
        set(CLANG_WARNINGS_AS_ERRORS "-Werror")
    endif()
    add_compile_options(${CLANG_ALL_WARNINGS})
    add_compile_options(${CLANG_WARNINGS_AS_ERRORS})
    add_compile_options(-Wno-documentation) # Disable warnings related to Doxygen comments
    add_compile_options(-Wno-sign-conversion) # Disable warnings related to implicit sign conversions
    add_compile_options(-Wno-implicit-float-conversion) # Disable warnings related to implicit float conversions
    add_compile_options(-Wno-cast-qual) # TODO: should enable
    add_compile_options(-Wno-cast-align) #TODD: should enable
    add_compile_options(-Wno-implicit-int-conversion) # uint16_t -> int conversion
    add_compile_options(-Wno-float-equal) # float comparison
    add_compile_options(-Wno-undefined-func-template)
    if(NOT "${CMAKE_C_COMPILER_ID}" STREQUAL "AppleClang")
        add_compile_options(-Wno-return-std-move-in-c++11)
    endif()
    add_compile_options(-Wno-comma)
    add_compile_options(-Wno-missing-prototypes)
    add_compile_options(-Wno-shadow-field-in-constructor) # TODO: should enable
    add_compile_options(-Wno-shadow-uncaptured-local)
    add_compile_options(-Wno-shadow-field)
    add_compile_options(-Wno-missing-variable-declarations)
    add_compile_options(-Wno-float-conversion)
    add_compile_options(-Wno-implicit-exception-spec-mismatch)
    add_compile_options(-Wno-format-nonliteral)
    add_compile_options(-Wno-implicit-fallthrough)
    add_compile_options(-Wno-unused-private-field)
    add_compile_options(-Wno-unreachable-code-return)
    add_compile_options(-Wno-deprecated)
    add_compile_options(-Wno-error=deprecated)
    add_compile_options(-Wno-shorten-64-to-32) # todo: should enable
    add_compile_options(-Wno-error=shorten-64-to-32)# todo: should enable
    add_compile_options(-Wno-unused-lambda-capture)
    add_compile_options(-Wno-error=unused-lambda-capture)
    add_compile_options(-Wno-unused-macros)
    add_compile_options(-Wno-error=unused-macros)
    add_compile_options(-Wno-unused-const-variable)
    add_compile_options(-Wno-error=unused-const-variable)
    add_compile_options(-Wno-over-aligned)
    add_compile_options(-Wno-error=over-aligned)
    add_compile_options(-Wno-unused-lambda-capture)
    add_compile_options(-Wno-error=unused-lambda-capture)
    add_compile_options(-Wno-constexpr-not-const)
    add_compile_options(-Wno-error=unreachable-code) # Ignore unreachable-code warning: some code is platform-dependent (Windows/Linux)

elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    set(GNU_ALL_WARNINGS "-Wall" "-Wextra")
    list(APPEND GNU_ALL_WARNINGS "-Wno-missing-field-initializers") # Allow c structs without all fields initialized
    set(GNU_WARNINGS_AS_ERRORS "-Werror")
    add_compile_options(${GNU_ALL_WARNINGS})
    add_compile_options(${GNU_WARNINGS_AS_ERRORS})
elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    set(MSVC_ALL_WARNINGS "/W4" "/wd4200") #Note: allow zero length arrays
    set(MSVC_WARNINGS_AS_ERRORS "/WX")
    string(REGEX REPLACE " /W[0-4]" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE " /W[0-4]" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    add_compile_options(${MSVC_ALL_WARNINGS})
    add_compile_options(${MSVC_WARNINGS_AS_ERRORS})
else()
    message(FATAL_ERROR "Unknown C++ compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DOB_DEBUG)
else()
    add_definitions(-DOB_RELEASE)
endif()
