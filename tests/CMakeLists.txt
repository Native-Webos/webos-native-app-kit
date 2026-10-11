# SPDX-License-Identifier: GPL-3.0-or-later
foreach (t log platform input focus font storage async http http_stream segment_reader image ui lifecycle crash capabilities media manifest subtitles subtitle_view starfish secret websocket)
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
target_link_libraries(test_http_stream PRIVATE wnk_net)
target_link_libraries(test_segment_reader PRIVATE wnk_net)
target_link_libraries(test_image PRIVATE wnk_image)
target_link_libraries(test_ui PRIVATE wnk_ui)
target_link_libraries(test_lifecycle PRIVATE wnk_lifecycle wnk_diagnostics)
target_link_libraries(test_crash PRIVATE wnk_diagnostics)
target_link_libraries(test_capabilities PRIVATE wnk_diagnostics)
target_link_libraries(test_media PRIVATE wnk_media wnk_media_ndl)
target_link_libraries(test_subtitles PRIVATE wnk_media)
target_link_libraries(test_manifest PRIVATE wnk_media)
target_link_libraries(test_subtitle_view PRIVATE wnk_media_ui)
target_link_libraries(test_starfish PRIVATE wnk_media_starfish)
target_link_libraries(test_secret PRIVATE wnk_storage)
target_link_libraries(test_websocket PRIVATE wnk_net)
target_include_directories(test_secret SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_include_directories(test_image SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")

# curl transport against a local HTTP/HTTPS server started by http_server.py (needs python3 + openssl).
add_executable(test_websocket_curl integration/test_websocket_curl.cpp)
target_include_directories(test_websocket_curl PRIVATE unit)
target_link_libraries(test_websocket_curl PRIVATE wnk_net)
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
    add_test(NAME integration.websocket_curl
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                    "$<TARGET_FILE:test_websocket_curl>")
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
if (TARGET wnk-tvui)
    # Pointer: clicking the second card of the first rail, and the wheel over the browse grid (4 columns,
    # two lines down puts item 8 under a stationary pointer). Positions in logical 1920x1080 units.
    wnk_tvui_e2e(pointer_click_opens_details "move@904:230,click@904:230" "open details for item 1")
    wnk_tvui_e2e(pointer_wheel_scrolls_grid "Left,Down,move@556:292,wheel@-2" "focus grid.item8")
endif ()
if (TARGET wnk-tvui)
    # Lifecycle: input while backgrounded is ignored, and drawing resumes after the foreground events.
    add_test(NAME e2e.tvui.background_foreground COMMAND wnk-tvui)
    set_tests_properties(e2e.tvui.background_foreground PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_TVUI_KEYS=bg,Right,fg,Right"
            PASS_REGULAR_EXPRESSION "Resuming -> Running \\(FramePresented\\).*focus rail0.item1"
            FAIL_REGULAR_EXPRESSION "focus rail0.item2|${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    # Every step of the shutdown sequence runs and is timed.
    wnk_tvui_e2e(shutdown_sequence "Escape,Escape" "SDL_Quit: [0-9.]+ ms.*shutdown complete in")
endif ()
if (TARGET wnk-tvui)
    # Crash record end to end: the first run crashes on purpose (launch params), the second reports it.
    set(WNK_CRASH_RUNTIME "${CMAKE_CURRENT_BINARY_DIR}/crash-runtime")
    add_test(NAME e2e.tvui.crash_reset COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WNK_CRASH_RUNTIME}")
    add_test(NAME e2e.tvui.crash_mkdir COMMAND "${CMAKE_COMMAND}" -E make_directory "${WNK_CRASH_RUNTIME}")
    set_tests_properties(e2e.tvui.crash_reset PROPERTIES FIXTURES_SETUP crash_dir)
    set_tests_properties(e2e.tvui.crash_mkdir PROPERTIES FIXTURES_SETUP crash_dir DEPENDS e2e.tvui.crash_reset)
    # The process must die by SIGSEGV (shell status 139) and leave a record behind.
    add_test(NAME e2e.tvui.crash_writes_record
            COMMAND sh -c "\"$1\" '{\"wnkCrashTest\":\"segv\"}'; code=$?; echo \"exit status $code\"; \
                           test $code -eq 139 && test -s \"$2/org.wnk.examples.tvui/crash.log\""
                    sh "$<TARGET_FILE:wnk-tvui>" "${WNK_CRASH_RUNTIME}")
    set_tests_properties(e2e.tvui.crash_writes_record PROPERTIES
            FIXTURES_REQUIRED crash_dir FIXTURES_SETUP crashed RESOURCE_LOCK crash_dir
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;XDG_RUNTIME_DIR=${WNK_CRASH_RUNTIME};ASAN_OPTIONS=handle_segv=0"
            TIMEOUT 30)
    add_test(NAME e2e.tvui.crash_reported_next_launch COMMAND wnk-tvui)
    set_tests_properties(e2e.tvui.crash_reported_next_launch PROPERTIES
            FIXTURES_REQUIRED "crash_dir;crashed" RESOURCE_LOCK crash_dir
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_RUNTIME_DIR=${WNK_CRASH_RUNTIME};WNK_TVUI_KEYS=Escape,Escape"
            PASS_REGULAR_EXPRESSION "previous run crashed: signal=11 \\(SIGSEGV\\) code=[0-9]+ addr=0x[0-9a-f]+.*state=Running"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
endif ()
if (TARGET wnk-tvui)
    # The Blue key (F4 on the host keymap) toggles the metrics overlay, and the app keeps running.
    wnk_tvui_e2e(metrics_overlay_toggle "F4,Right,F4" "metrics overlay on.*focus rail0.item1.*metrics overlay off")
endif ()

# FfmpegSource on real clips generated by the system ffmpeg CLI (SKIPs without it).
if (TARGET wnk_media_ffmpeg)
    add_executable(test_ffmpeg_source integration/test_ffmpeg_source.cpp)
    target_include_directories(test_ffmpeg_source PRIVATE unit)
    target_link_libraries(test_ffmpeg_source PRIVATE wnk_media_ffmpeg)
    add_test(NAME integration.ffmpeg_source COMMAND test_ffmpeg_source)
    if (Python3_FOUND)
        # The same suite with a local HTTP server: clips streamed through HttpStream + custom AVIO.
        add_test(NAME integration.ffmpeg_source_http
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                        "$<TARGET_FILE:test_ffmpeg_source>")
        # DASH and HLS written by the ffmpeg CLI, served by the same local server.
        add_executable(test_adaptive_source integration/test_adaptive_source.cpp)
        target_include_directories(test_adaptive_source PRIVATE unit)
        target_link_libraries(test_adaptive_source PRIVATE wnk_media_ffmpeg)
        add_test(NAME integration.adaptive_source
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                        "$<TARGET_FILE:test_adaptive_source>")
    endif ()
endif ()

# End-to-end runs of Example D on the host: bundled FFmpeg demuxing real clips into the controller,
# with the pacing backend (no picture on the host). Steps are 0.1 s apart; Up is a no-op used to wait.
if (TARGET wnk-player)
    function(wnk_player_e2e name params keys pass)
        add_test(NAME e2e.player.${name} COMMAND wnk-player "${params}")
        set_tests_properties(e2e.player.${name} PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_PLAYER_KEYS=${keys}"
                PASS_REGULAR_EXPRESSION "${pass}"
                FAIL_REGULAR_EXPRESSION "-> Error|${WNK_SANITIZER_FAIL}"
                TIMEOUT 60)
    endfunction()
    wnk_player_e2e(pause_seek_resume "{\"autoplay\":0}" "Space,Right,Space"
            "Playing -> Paused.*Paused -> Seeking.*Seeking -> Paused.*Paused -> Playing")
    # Backgrounding releases the decoder; playback resumes on return only if it was playing.
    wnk_player_e2e(background_resumes_playing "{\"autoplay\":0}" "Up,bg,Up,fg,Up,Up"
            "Playing -> Paused.*back in the foreground: resuming.*Paused -> Playing")
    wnk_player_e2e(background_keeps_viewer_pause "{\"autoplay\":0}" "Space,bg,Up,fg,Up,Up"
            "Playing -> Paused.*Resuming -> Running")
    set_tests_properties(e2e.player.background_keeps_viewer_pause PROPERTIES
            FAIL_REGULAR_EXPRESSION "back in the foreground: resuming|Paused -> Playing|${WNK_SANITIZER_FAIL}")
    # Subtitles: the default English track shows; Down switches to French, then off.
    wnk_player_e2e(subtitles_default_and_switch "{\"autoplay\":3}" "Up,Up,Up,Up,Up,Down,Up,Up,Up,Up,Up,Up,Down,Up"
            "subtitles: track 2, Subtitles: English \\(subrip\\).*subtitle shown: Second [0-9].*subtitles: track 3, Subtitles: Fran.*subtitle shown: Sous-titre fran.+ais num.+ro [0-9] / deuxi.+me ligne.*subtitles: track -1, Subtitles off")
    wnk_player_e2e(back_stops_playback "{\"autoplay\":0}" "Up,Up,Escape" "stopping at .*-> Stopped")
    wnk_player_e2e(plays_to_the_end "{\"autoplay\":1}"
            "Right,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up,Up"
            "Playing -> Ended")
    wnk_player_e2e(missing_file_reports_error "{\"uri\":\"file:///nonexistent/clip.mkv\"}" "Up"
            "open failed: No such file")
    set_tests_properties(e2e.player.missing_file_reports_error PROPERTIES FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}")
endif ()
