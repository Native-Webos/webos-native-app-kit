# SPDX-License-Identifier: GPL-3.0-or-later
foreach (t log platform)
    add_executable(test_${t} unit/test_${t}.cpp)
    target_include_directories(test_${t} PRIVATE unit)
    add_test(NAME unit.${t} COMMAND test_${t})
endforeach ()
target_link_libraries(test_log PRIVATE wnk_logging)
target_link_libraries(test_platform PRIVATE wnk_platform)

find_package(Python3 3.8 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME tools.packaging
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -v)
endif ()
