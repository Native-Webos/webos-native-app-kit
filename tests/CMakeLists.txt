# SPDX-License-Identifier: GPL-3.0-or-later
foreach (t log platform input focus font storage async http image ui)
    add_executable(test_${t} unit/test_${t}.cpp)
    target_include_directories(test_${t} PRIVATE unit)
    add_test(NAME unit.${t} COMMAND test_${t})
endforeach ()
target_link_libraries(test_log PRIVATE wnk_logging)
target_link_libraries(test_platform PRIVATE wnk_platform)
target_link_libraries(test_input PRIVATE wnk_input)
target_link_libraries(test_focus PRIVATE wnk_focus)
target_link_libraries(test_font PRIVATE wnk_render)
target_link_libraries(test_storage PRIVATE wnk_storage)
target_link_libraries(test_async PRIVATE wnk_async)
target_link_libraries(test_http PRIVATE wnk_net)
target_link_libraries(test_image PRIVATE wnk_image)
target_link_libraries(test_ui PRIVATE wnk_ui)
target_include_directories(test_image SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")

# curl transport against a local HTTP/HTTPS server started by http_server.py (needs python3 + openssl).
add_executable(test_http_curl integration/test_http_curl.cpp)
target_include_directories(test_http_curl PRIVATE unit)
target_link_libraries(test_http_curl PRIVATE wnk_net)
target_include_directories(test_storage SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
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
    add_test(NAME integration.http_curl
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                    "$<TARGET_FILE:test_http_curl>")
    add_test(NAME tools.packaging
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tools" -v)
endif ()

# PASS_REGULAR_EXPRESSION makes CTest ignore the exit code, so sanitizer reports must fail explicitly.
set(WNK_SANITIZER_FAIL "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:")

# End-to-end smoke tests of Example A on the host: real SDL events → ActionMapper → FocusManager →
# renderer, driven by the app's scripted-key mode (offscreen video driver, Mesa GLES2).
if (TARGET wnk-hello)
    # Each run gets an empty data directory so saved state (focus.tile, launch.count) from a previous
    # run or from the developer's own profile cannot change the starting focus.
    set(WNK_E2E_DATA "${CMAKE_CURRENT_BINARY_DIR}/e2e-data")
    add_test(NAME e2e.hello.reset_state COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WNK_E2E_DATA}")
    set_tests_properties(e2e.hello.reset_state PROPERTIES FIXTURES_SETUP hello_state)
    add_test(NAME e2e.hello.dialog_focus_restore COMMAND wnk-hello)
    set_tests_properties(e2e.hello.dialog_focus_restore PROPERTIES
            FIXTURES_REQUIRED hello_state RESOURCE_LOCK hello_state
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_E2E_DATA}/dialog_focus_restore;WNK_HELLO_KEYS=Down,Right,Return,Right,Escape"
            PASS_REGULAR_EXPRESSION "dialog closed, focus restored to 105"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    add_test(NAME e2e.hello.double_back_exits COMMAND wnk-hello)
    set_tests_properties(e2e.hello.double_back_exits PROPERTIES
            FIXTURES_REQUIRED hello_state RESOURCE_LOCK hello_state
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_E2E_DATA}/double_back_exits;WNK_HELLO_KEYS=Escape,Escape"
            PASS_REGULAR_EXPRESSION "Back twice at root: quitting"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    add_test(NAME e2e.hello.unmapped_keys_have_no_effect COMMAND wnk-hello)
    set_tests_properties(e2e.hello.unmapped_keys_have_no_effect PROPERTIES
            FIXTURES_REQUIRED hello_state RESOURCE_LOCK hello_state
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_E2E_DATA}/unmapped_keys_have_no_effect;WNK_HELLO_KEYS=A,B,Z,Right"
            PASS_REGULAR_EXPRESSION "focus\\] \\([0-9]+\\) 100 -> 101"
            FAIL_REGULAR_EXPRESSION "dialog opened|quitting|${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
endif ()

# End-to-end tests of Example B (UI toolkit): navigation stack, virtualised grid, modal dialog and
# Back handling, through real SDL key events on the offscreen driver.
if (TARGET wnk-tvui)
    function(wnk_tvui_e2e name keys pass)
        add_test(NAME e2e.tvui.${name} COMMAND wnk-tvui)
        set_tests_properties(e2e.tvui.${name} PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_TVUI_KEYS=${keys}"
                PASS_REGULAR_EXPRESSION "${pass}"
                FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
                TIMEOUT 30)
    endfunction()
    wnk_tvui_e2e(details_back_restores_focus "Right,Right,Return,Escape" "back to home focus=rail0.item2")
    wnk_tvui_e2e(browse_grid_scrolls "Left,Down,Right,Down,Down,Down,Down,Down,Down,Down,Down,Down,Down"
            "focus grid.item40")
    wnk_tvui_e2e(dialog_focus_restore "Left,Down,Down,Right,Return,Right,Escape" "dialog closed focus=widgets.dialog")
    wnk_tvui_e2e(double_back_exits "Escape,Escape" "Back twice at root: quitting")
endif ()
