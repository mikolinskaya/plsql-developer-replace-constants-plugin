cmake_minimum_required(VERSION 3.20)
project(replace_constants VERSION 1.0.0 LANGUAGES CXX)

add_library(replace_constants SHARED
    src/plugin.cpp
    src/constant_replacer.cpp
    src/replace_constants.def
)

target_compile_features(replace_constants PRIVATE cxx_std_17)
target_include_directories(replace_constants PRIVATE src)
set_target_properties(replace_constants PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ReplaceConstants"
)

if(MSVC)
    target_compile_options(replace_constants PRIVATE /W4 /permissive- /EHsc)
    target_link_options(replace_constants PRIVATE /DYNAMICBASE:NO /NXCOMPAT:NO /FIXED:NO)
    set_property(TARGET replace_constants PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
else()
    target_compile_options(replace_constants PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(replace_constants PRIVATE
        -static
        -Wl,--disable-dynamicbase
        -Wl,--disable-nxcompat
    )
endif()

enable_testing()
add_executable(constant_replacer_tests
    tests/constant_replacer_tests.cpp
    src/constant_replacer.cpp
)
target_compile_features(constant_replacer_tests PRIVATE cxx_std_17)
target_include_directories(constant_replacer_tests PRIVATE src)
if(NOT MSVC)
    target_link_options(constant_replacer_tests PRIVATE -static)
endif()
foreach(test_case IN ITEMS
    qualified_plsql
    qualified_sql
    unqualified_plsql_same_window
    unqualified_sql_same_window
    whole_window_declarations
    dependency_discovery
)
    add_test(NAME constant_replacer_${test_case} COMMAND constant_replacer_tests ${test_case})
endforeach()

add_executable(plugin_integration_tests
    tests/plugin_integration_tests.cpp
    src/plugin.cpp
    src/constant_replacer.cpp
)
target_compile_features(plugin_integration_tests PRIVATE cxx_std_17)
target_include_directories(plugin_integration_tests PRIVATE src)
target_link_libraries(plugin_integration_tests PRIVATE user32)
if(NOT MSVC)
    target_link_options(plugin_integration_tests PRIVATE -static)
endif()
foreach(test_case IN ITEMS
    qualified_sql_other_window
    qualified_plsql_other_window
    package_first_only_other_window
    package_dependency_other_window
    synonym_other_window
    cross_schema_synonym_other_window
    cross_schema_synonym_and_qualified_name
    reported_cross_schema_synonym_example
)
    add_test(NAME plugin_${test_case} COMMAND plugin_integration_tests ${test_case})
endforeach()
