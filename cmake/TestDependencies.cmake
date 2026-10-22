find_package(GTest 1.14 CONFIG QUIET)
if(NOT GTest_FOUND)
    if(NOT CTE_FETCH_TEST_DEPS)
        message(FATAL_ERROR "GoogleTest >= 1.14 required for tests. Install it, set CTE_FETCH_TEST_DEPS=ON, or set CTE_BUILD_TESTS=OFF.")
    endif()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
