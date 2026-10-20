# SPDX-License-Identifier: GPL-3.0-or-later
foreach (t log platform input focus font storage async http http_stream segment_reader image ui lifecycle crash capabilities media manifest subtitles subtitle_view starfish secret websocket auth i18n audio lan blob bitstream)
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
target_link_libraries(test_bitstream PRIVATE wnk_media)
target_link_libraries(test_manifest PRIVATE wnk_media)
target_link_libraries(test_subtitle_view PRIVATE wnk_media_ui)
target_link_libraries(test_starfish PRIVATE wnk_media_starfish)
target_link_libraries(test_secret PRIVATE wnk_storage)
target_link_libraries(test_websocket PRIVATE wnk_net)
target_link_libraries(test_auth PRIVATE wnk_auth)
target_link_libraries(test_i18n PRIVATE wnk_i18n)
find_package(Threads REQUIRED)
target_link_libraries(test_lan PRIVATE wnk_net wnk_platform)
target_link_libraries(test_blob PRIVATE wnk_storage)
target_link_libraries(test_audio PRIVATE wnk_audio Threads::Threads)
target_include_directories(test_secret SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
target_include_directories(test_image SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/stb")

# Compile checks: ignoring the result of HttpClient::send / auth::Session::send (false = on_done is
# never called) must not build with -Werror=unused-result; checking it must. The probes are built by
# the tests themselves, one at a time, and are not part of the normal build.
foreach (v checked ignored)
    add_library(probe_send_${v} OBJECT EXCLUDE_FROM_ALL compile/send_result.cpp)
    target_link_libraries(probe_send_${v} PRIVATE wnk_net wnk_auth)
    target_compile_options(probe_send_${v} PRIVATE -Werror=unused-result)
    add_test(NAME build.send_result_${v}
            COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS
                    "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target probe_send_${v})
    set_tests_properties(build.send_result_${v} PROPERTIES RESOURCE_LOCK build_dir)
endforeach ()
target_compile_definitions(probe_send_ignored PRIVATE WNK_PROBE_IGNORE)
set_tests_properties(build.send_result_ignored PROPERTIES WILL_FAIL TRUE)

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
    # Settings built from the form controls: the slider steps, the check box flips, entering the tabs
    # from below lands on the selected tab, the option picker's list, and editing text in a dialog.
    set(WNK_TVUI_SETTINGS "Left,Down,Down,Down,Right")
    wnk_tvui_e2e(settings_slider_and_toggle "${WNK_TVUI_SETTINGS},Right,Right,Up,Return"
            "setting subtitle size = 110.*setting subtitle size = 120.*focus settings.clock.*setting clock = on")
    wnk_tvui_e2e(settings_tabs_and_choice "${WNK_TVUI_SETTINGS},Up,Up,Up,Right,Down,Right,Return,Down,Return"
            "focus settings.tab0.*settings tab 1.*focus settings.quality.*setting quality = 1.*focus settings.quality.option1.*focus settings.quality.option2.*setting quality = 2.*focus settings.quality")
    string(REPEAT "Backspace," 14 WNK_TVUI_ERASE)
    wnk_tvui_e2e(settings_edit_text_in_dialog "${WNK_TVUI_SETTINGS},Down,Return,Return,${WNK_TVUI_ERASE}text@Den,Return,Z"
            "focus settings.device.field.*text input on for settings.device.field.*setting device name = Den.*text input off.*focus settings.device")
    wnk_tvui_e2e(settings_text_view_scrolls "${WNK_TVUI_SETTINGS},Up,Up,Up,Right,Right,Down,Down,Down,Down,Down,Down,Up"
            "focus settings.tab2.*focus settings.terms")
    # App audio through SDL's disk driver (a file instead of a speaker): interface ticks when focus moves,
    # the Widgets chord at the sum of its three tones' level.
    add_test(NAME e2e.tvui.interface_sounds COMMAND wnk-tvui "{\"sounds\":true}")
    set_tests_properties(e2e.tvui.interface_sounds PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;SDL_AUDIODRIVER=disk;SDL_DISKAUDIOFILE=${CMAKE_CURRENT_BINARY_DIR}/e2e-ticks.raw;${WNK_LSAN};WNK_TVUI_KEYS=Right,Right,Left,Z,Z"
            PASS_REGULAR_EXPRESSION "audio\\] \\([0-9]+\\) output disk, 48000 Hz.*interface sounds on.*sounds played: [1-9].*closed; peak level 0\\.1[0-9]"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    add_test(NAME e2e.tvui.widgets_chord COMMAND wnk-tvui)
    set_tests_properties(e2e.tvui.widgets_chord PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;SDL_AUDIODRIVER=disk;SDL_DISKAUDIOFILE=${CMAKE_CURRENT_BINARY_DIR}/e2e-chord.raw;${WNK_LSAN};WNK_TVUI_KEYS=Left,Down,Down,Right,Right,Right,Return,Z,Z,Z,Z,Z,Z,Z"
            PASS_REGULAR_EXPRESSION "focus widgets.sound.*sound: chord.*closed; peak level 0\\.4[0-9]"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    # A game controller (SDL's virtual pad): the D-pad and A/B navigate, a held stick repeats, X goes to
    # the app's on_gamepad, and unplugging is reported.
    wnk_tvui_e2e(gamepad_navigates "pad,Z,pad+dpright,pad-dpright,pad+a,pad-a,Z,pad+b,pad-b,Z,pad+x,pad-x,padx=1,Z,Z,Z,Z,Z,Z,padx=0,pad~,Z"
            "pad 0 connected: wnk virtual pad.*focus rail0.item1.*open details for item 1.*back to home focus=rail0.item1.*gamepad 0: X pressed \\(handled by the app\\).*focus rail0.item2.*focus rail0.item3.*focus rail0.item4.*pad 0 disconnected")
    # Outside the app: the network status at start; on the host the browser and notification requests
    # are refused (no Luna bus) and reported; device discovery answers with whatever the LAN has (0 or more).
    wnk_tvui_e2e(system_requests "Left,Down,Down,Right,Up,Return,Right,Return,Right,Return,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z"
            "network\\] \\([0-9]+\\) (online|offline).*focus widgets.url.*open url: unsupported: not a webOS build.*toast: unsupported: not a webOS build.*find devices: [0-9]+ service types")
    # The app's own GLES2 drawing inside the UI (Widgets): presented every frame while it is visible.
    wnk_tvui_e2e(gl_view_draws "Left,Down,Down,Z,Z" "gl view: first frame drawn by the app")
    # Audio Guidance forced on: focus changes are spoken (logged on the host, where there is no TTS service).
    add_test(NAME e2e.tvui.speech_follows_focus COMMAND wnk-tvui "{\"wnkSpeak\":true}")
    set_tests_properties(e2e.tvui.speech_follows_focus PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_TVUI_KEYS=Left,Down,Down,Down,Right,Up"
            PASS_REGULAR_EXPRESSION "focus is spoken \\(launch parameters\\).*say \"Home, button\".*say \"Settings, button\".*say \"Subtitle size, 100 %\".*say \"Show clock, off\""
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 30)
    # Other image formats on the Widgets page: an SVG at three sizes and a WebP, none failing to load.
    wnk_tvui_e2e(svg_and_webp_load "Left,Down,Down,Z,Z,Z,Z" "section 2.*focus nav.Widgets")
    set_tests_properties(e2e.tvui.svg_and_webp_load PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[image\\].*failed|${WNK_SANITIZER_FAIL}")
    # A still screen is drawn but not presented: under 100 presents (about 7; 13 under ASan) in ~7 s of
    # waiting, not 400.
    string(REPEAT "Z," 60 WNK_TVUI_IDLE)
    wnk_tvui_e2e(still_screen_is_not_presented "${WNK_TVUI_IDLE}Z" "main loop ended after [0-9][0-9]? frames presented, [0-9][0-9][0-9] drawn")
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

# Example C (networked app): its catalogue code on its own, then the app against the synthetic
# /catalogue routes of http_server.py (nothing reaches the real TVmaze API).
if (TARGET wnk_example_shows_catalogue)
    add_executable(test_shows_catalogue unit/test_shows_catalogue.cpp)
    target_include_directories(test_shows_catalogue PRIVATE unit)
    target_link_libraries(test_shows_catalogue PRIVATE wnk_example_shows_catalogue)
    add_test(NAME unit.shows_catalogue COMMAND test_shows_catalogue)
endif ()
if (TARGET wnk-shows AND Python3_FOUND)
    # Z is unmapped on the host keymap: a step that only waits (steps are ~0.1 s apart).
    set(WNK_SHOWS_WAIT "Z,Z,Z,Z,Z,Z,Z,Z,Z,Z")
    set(WNK_SHOWS_DATA "${CMAKE_CURRENT_BINARY_DIR}/e2e-shows")
    # Each run gets its own data directory: a saved copy from another run would change what loads first.
    function(wnk_shows_e2e name mode keys pass)
        add_test(NAME e2e.shows.${name}
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                        "$<TARGET_FILE:wnk-shows>" "{\"api\":\"@HTTP@/catalogue${mode}\"}")
        set_tests_properties(e2e.shows.${name} PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_SHOWS_DATA}/${name};WNK_SHOWS_KEYS=${keys}"
                PASS_REGULAR_EXPRESSION "${pass}"
                FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
                TIMEOUT 60)
    endfunction()
    add_test(NAME e2e.shows.reset_state COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WNK_SHOWS_DATA}")
    set_tests_properties(e2e.shows.reset_state PROPERTIES FIXTURES_SETUP shows_state)

    # Rails fill from page 0; OK opens the details (seasons and cast fetched), Back returns to the card.
    wnk_shows_e2e(details_back_restores_focus "" "${WNK_SHOWS_WAIT},Right,Return,${WNK_SHOWS_WAIT},Escape,Z,Z"
            "catalogue page 0: 250 shows.*open details for show 127.*details 127 loaded in [0-9.]+ s: 3 seasons, 3 cast.*back to home focus=rail0.item1")
    # 503 three times (HttpClient retries with backoff), the error panel takes focus, Try again succeeds.
    wnk_shows_e2e(retry_after_server_error "/flaky" "${WNK_SHOWS_WAIT},${WNK_SHOWS_WAIT},Return,${WNK_SHOWS_WAIT}"
            "HTTP 503 after 3 attempt.*showing error: The catalogue server has a problem \\(HTTP 503\\).*focus home.retry.*catalogue page 0: 250 shows.*focus rail0.item0")
    wnk_shows_e2e(unreadable_reply "/broken" "${WNK_SHOWS_WAIT}"
            "showing error: The catalogue sent a reply the app cannot read")
    # Back while the details are still loading stops the transfer; the late reply never reaches the UI.
    # The server answers after 4 s (http_server.py "slow"): any cancel before that passes, since the
    # timed key script reaches Back later on a slow machine (1.1 s seen on a CI runner).
    wnk_shows_e2e(back_cancels_details "/slow" "${WNK_SHOWS_WAIT},Right,Return,Z,Z,Z,Z,Z,Escape,${WNK_SHOWS_WAIT}"
            "details 127: closed while loading, cancelling the request.*details 127: request cancelled after [0-3]\\.[0-9]+ s")
    # Paging: the last rows of page 0 fetch page 1, the end of page 1 meets the 404 that ends the list.
    string(REPEAT ",Down" 50 WNK_SHOWS_DOWN50)
    wnk_shows_e2e(grid_pages_to_the_end "" "${WNK_SHOWS_WAIT},Left,Down,Right${WNK_SHOWS_DOWN50}${WNK_SHOWS_DOWN50},${WNK_SHOWS_WAIT}"
            "page 1: 50 shows.*page 2: end of the catalogue at 300 shows")
    # Search: OK on the field turns text input on; typed text (SDL_TEXTINPUT, as LG's keyboard sends)
    # filters the grid; Backspace erases instead of going Back; OK submits and moves to the results.
    wnk_shows_e2e(search_filters_grid "" "${WNK_SHOWS_WAIT},Left,Down,Right,Up,Return,text@embex,Z,Backspace,text@r,Z,Return,Z,Z"
            "focus browse.search.*text input on for browse.search.*search \"ember\": 36 of 250 shows.*text input off.*focus grid.item0")
    # The viewer closes the keyboard: editing ends, OK opens it again, Back ends editing on the screen.
    wnk_shows_e2e(search_keyboard_closed "" "${WNK_SHOWS_WAIT},Left,Down,Right,Up,Return,kbclose,Z,Z,Return,Z,Escape,Z,Z"
            "text input on for browse.search.*keyboard closed by the system: editing ends.*text input on for browse.search.*text input off")
    set_tests_properties(e2e.shows.search_keyboard_closed PROPERTIES
            FAIL_REGULAR_EXPRESSION "Back at root|${WNK_SANITIZER_FAIL}")
    # Locale: from LANG on the host, with the app's catalog for it.
    wnk_shows_e2e(locale_from_environment "" "${WNK_SHOWS_WAIT}"
            "locale de-DE \\(formats de-DE\\) from LANG; 1 catalog file\\(s\\), [0-9]+ entries")
    set_tests_properties(e2e.shows.locale_from_environment PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_SHOWS_DATA}/locale_from_environment;WNK_SHOWS_KEYS=${WNK_SHOWS_WAIT};LC_ALL=;LC_MESSAGES=;LANG=de_DE.UTF-8")
    # Japanese from the launch parameters: the bundled font has no kana or kanji, so the shell finds a
    # system face for them (needs a CJK font on the host).
    if (EXISTS "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc" OR
        EXISTS "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf")
        add_test(NAME e2e.shows.japanese_uses_system_font
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                        "$<TARGET_FILE:wnk-shows>" "{\"api\":\"@HTTP@/catalogue\",\"wnkLocale\":\"ja-JP\"}")
        set_tests_properties(e2e.shows.japanese_uses_system_font PROPERTIES
                ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_SHOWS_DATA}/japanese;WNK_SHOWS_KEYS=${WNK_SHOWS_WAIT}"
                PASS_REGULAR_EXPRESSION "locale ja-JP \\(formats ja-JP\\) from launch parameters; 1 catalog file.*system fallback font /usr/share/fonts/"
                FAIL_REGULAR_EXPRESSION "no font has U\\+|${WNK_SANITIZER_FAIL}"
                FIXTURES_REQUIRED shows_state
                TIMEOUT 60)
    endif ()
    foreach (t details_back_restores_focus retry_after_server_error unreadable_reply back_cancels_details grid_pages_to_the_end
            search_filters_grid search_keyboard_closed locale_from_environment)
        set_tests_properties(e2e.shows.${t} PROPERTIES FIXTURES_REQUIRED shows_state)
    endforeach ()
    set_tests_properties(e2e.shows.back_cancels_details PROPERTIES
            FAIL_REGULAR_EXPRESSION "details 127 loaded|${WNK_SANITIZER_FAIL}")

    # Across launches: the first run saves the catalogue and a favourite in the store; the second has
    # no server at all (port 9 refuses) and shows the saved copy and the favourite.
    wnk_shows_e2e(saved_copy_first_run "" "${WNK_SHOWS_WAIT},Right,Return,${WNK_SHOWS_WAIT},Return,${WNK_SHOWS_WAIT}"
            "saved copy: 150 shows.*favourites: 1 saved")
    set_tests_properties(e2e.shows.saved_copy_first_run PROPERTIES
            FIXTURES_REQUIRED shows_state FIXTURES_SETUP shows_saved RESOURCE_LOCK shows_saved
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_SHOWS_DATA}/saved;WNK_SHOWS_KEYS=${WNK_SHOWS_WAIT},Right,Return,${WNK_SHOWS_WAIT},Return,${WNK_SHOWS_WAIT}")
    add_test(NAME e2e.shows.saved_copy_offline COMMAND wnk-shows "{\"api\":\"http://127.0.0.1:9/catalogue\"}")
    set_tests_properties(e2e.shows.saved_copy_offline PROPERTIES
            FIXTURES_REQUIRED "shows_state;shows_saved" RESOURCE_LOCK shows_saved
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};XDG_DATA_HOME=${WNK_SHOWS_DATA}/saved;WNK_SHOWS_KEYS=${WNK_SHOWS_WAIT},${WNK_SHOWS_WAIT}"
            PASS_REGULAR_EXPRESSION "saved copy ok \\(150 shows\\), favourites ok \\(1\\).*showing saved copy of 150 shows.*keeping the saved list: Cannot reach the catalogue"
            FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}"
            TIMEOUT 60)
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
        # ClearKey: licence exchange with the server's /clearkey route, CENC MP4 and DASH decrypted
        # byte for byte against the clear file.
        add_executable(test_clearkey integration/test_clearkey.cpp)
        target_include_directories(test_clearkey PRIVATE unit)
        target_link_libraries(test_clearkey PRIVATE wnk_media_ffmpeg)
        add_test(NAME integration.clearkey
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/integration/http_server.py"
                        "$<TARGET_FILE:test_clearkey>")
    endif ()
endif ()

# End-to-end runs of Example D on the host: bundled FFmpeg demuxing real clips into the controller,
# with the pacing backend (no picture on the host). Steps are 0.1 s apart; Z is unmapped and used to wait.
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
    wnk_player_e2e(background_resumes_playing "{\"autoplay\":0}" "Z,bg,Z,fg,Z,Z"
            "Playing -> Paused.*back in the foreground: resuming.*Paused -> Playing")
    wnk_player_e2e(background_keeps_viewer_pause "{\"autoplay\":0}" "Space,bg,Z,fg,Z,Z"
            "Playing -> Paused.*Resuming -> Running")
    set_tests_properties(e2e.player.background_keeps_viewer_pause PROPERTIES
            FAIL_REGULAR_EXPRESSION "back in the foreground: resuming|Paused -> Playing|${WNK_SANITIZER_FAIL}")
    # Subtitles: the default English track shows; Down switches to French, then off.
    wnk_player_e2e(subtitles_default_and_switch "{\"autoplay\":3}" "Z,Z,Z,Z,Z,Down,Z,Z,Z,Z,Z,Z,Down,Z"
            "subtitles: track 3, Subtitles: English \\(subrip\\).*subtitle shown: Second [0-9].*subtitles: track 4, Subtitles: Fran.*subtitle shown: Sous-titre fran.+ais num.+ro [0-9] / deuxi.+me ligne.*subtitles: track -1, Subtitles off")
    # The track menu (Up) opens on the playing English audio track; Right, OK plays the French one from
    # the position, with the decoder reloaded.
    wnk_player_e2e(tracks_menu_switches_audio "{\"autoplay\":3}" "Z,Z,Z,Z,Up,Right,Return,Z,Z,Z,Z,Z,Z"
            "audio: track 1, Audio: English.*tracks menu open, focus=tracks.audio.1.*tracks: audio 2 chosen.*Playing -> Seeking.*audio track 2 \\(.*\\) at [0-9.]+ s.*(Seeking -> Playing.*audio: track 2, Audio: Fran|audio: track 2, Audio: Fran.*Seeking -> Playing)")
    # The screensaver is allowed by default, blocked while the video plays, allowed again on pause.
    wnk_player_e2e(screensaver_follows_playback "{\"autoplay\":0}" "Z,Z,Space,Z,Z"
            "screensaver\\] \\([0-9]+\\) allowed \\(default\\).*blocked \\(video playing\\).*Playing -> Paused.*allowed \\(video playing\\)")
    # Relaunch with new parameters (what the kit's SDL patch hands over on the TV): the playing clip
    # stops and the requested one opens. A relaunch without a request changes nothing.
    wnk_player_e2e(relaunch_opens_new_request "{\"autoplay\":0}" "Z,Z,Z,relaunch,Z,Z,Z,Z,Z"
            "relaunch: message fields \\[event,parameters,returnValue\\], parameters \\[autoplay\\].*stopping at.*relaunch: opening the new request.*opening file://.*h264-aac-subs.mkv")
    set_tests_properties(e2e.player.relaunch_opens_new_request PROPERTIES ENVIRONMENT
            "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_PLAYER_KEYS=Z,Z,Z,relaunch,Z,Z,Z,Z,Z;WNK_PLAYER_RELAUNCH={\"event\":\"relaunch\",\"parameters\":{\"autoplay\":3},\"returnValue\":true}")
    wnk_player_e2e(relaunch_without_request "{\"autoplay\":0}" "Z,Z,Z,relaunch,Z,Z"
            "relaunch: message fields \\[event,returnValue\\], parameters \\[\\].*relaunch: nothing to open, carrying on")
    set_tests_properties(e2e.player.relaunch_without_request PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=offscreen;${WNK_LSAN};WNK_PLAYER_KEYS=Z,Z,Z,relaunch,Z,Z;WNK_PLAYER_RELAUNCH={\"event\":\"relaunch\",\"returnValue\":true}"
            FAIL_REGULAR_EXPRESSION "relaunch: opening|-> Error|${WNK_SANITIZER_FAIL}")
    wnk_player_e2e(back_stops_playback "{\"autoplay\":0}" "Z,Z,Escape" "stopping at .*-> Stopped")
    wnk_player_e2e(plays_to_the_end "{\"autoplay\":1}"
            "Right,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z,Z"
            "Playing -> Ended")
    wnk_player_e2e(missing_file_reports_error "{\"uri\":\"file:///nonexistent/clip.mkv\"}" "Z"
            "open failed: No such file")
    set_tests_properties(e2e.player.missing_file_reports_error PROPERTIES FAIL_REGULAR_EXPRESSION "${WNK_SANITIZER_FAIL}")
endif ()
