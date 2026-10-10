# SPDX-License-Identifier: GPL-3.0-or-later
foreach (t log platform input focus font)
    add_executable(test_${t} unit/test_${t}.cpp)
    target_include_directories(test_${t} PRIVATE unit)
    add_test(NAME unit.${t} COMMAND test_${t})
endforeach ()
target_link_libraries(test_log PRIVATE wnk_logging)
target_link_libraries(test_platform PRIVATE wnk_platform)
target_link_libraries(test_input PRIVATE wnk_input)
target_link_libraries(test_focus PRIVATE wnk_focus)
target_link_libraries(test_font PRIVATE wnk_render)
target_compile_definitions(test_font PRIVATE WNK_TEST_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")

# Renderer pixel tests need a GLES2 context (SDL offscreen driver on Mesa); they SKIP without one.
add_executable(test_renderer_gl integration/test_renderer_gl.cpp)
target_include_directories(test_renderer_gl PRIVATE unit)
target_link_libraries(test_renderer_gl PRIVATE wnk_render_gles2 wnk_sdl2)
target_compile_definitions(test_renderer_gl PRIVATE WNK_TEST_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
add_test(NAME integration.renderer_gl COMMAND test_renderer_gl)
set(WNK_LSAN "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan-host.supp")
set_tests_properties(integration.renderer_gl PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN}")

find_package(Python3 3.8 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME tools.packaging
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -v)
endif ()

# PASS_REGULAR_EXPRESSION makes CTest ignore the exit code, so sanitizer reports must fail explicitly.
set(WNK_SANITIZER_FAIL "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

# End-to-end smoke tests of Example A on the host: real SDL events → ActionMapper → FocusManager →
# renderer, driven by the app's scripted-key mode (offscreen video driver, Mesa GLES2).
if (TARGET wnk-hello)
    add_test(NAME e2e.hello.dialog_focus_restore COMMAND wnk-hello)
    set_tests_properties(e2e.hello.dialog_focus_restore PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_HELLO_KEYS=Down,Right,Return,Right,Escape"
            PASS_REGULAR_EXPRESSION "dialog closed, focus restored to 105"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    add_test(NAME e2e.hello.double_back_exits COMMAND wnk-hello)
    set_tests_properties(e2e.hello.double_back_exits PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_HELLO_KEYS=Escape,Escape"
            PASS_REGULAR_EXPRESSION "Back twice at root: quitting"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    add_test(NAME e2e.hello.unmapped_keys_have_no_effect COMMAND wnk-hello)
    set_tests_properties(e2e.hello.unmapped_keys_have_no_effect PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_HELLO_KEYS=A,B,Z,Right"
            PASS_REGULAR_EXPRESSION "focus\\] \\([0-9]+\\) 100 -> 101"
            FAIL_REGULAR_EXPRESSION "dialog opened|quitting|${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
endif ()
