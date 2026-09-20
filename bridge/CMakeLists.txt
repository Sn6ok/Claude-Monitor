cmake_minimum_required(VERSION 3.20)
project(claude_monitor_bridge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Windows Bridge збирається лише під Windows")
endif()

# ── Прапорці компілятора ──────────────────────────────────────────────────────
#
# Мета — маленький швидкий бінарник без зайвого рантайму.
# Статичне лінкування CRT прибирає залежність від Visual C++ Redistributable:
# користувач не має нічого доставляти окремо.

if(MSVC)
    add_compile_options(
        /W4             # високий рівень попереджень
        /permissive-    # суворіша відповідність стандарту
        /utf-8          # вихідні файли та літерали в UTF-8
        /Zc:__cplusplus
        /EHsc
        /GR-            # RTTI не потрібен — зайвий розмір
    )

    add_compile_options(
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Oi>
        $<$<CONFIG:Release>:/Gy>   # окремі функції для згортання лінкером
        $<$<CONFIG:Release>:/GL>   # оптимізація на рівні всієї програми
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Debug>:/Zi>
    )

    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
        # Захисні механізми Windows. Вимикати їх заради кількох кілобайтів
        # було б поганим обміном (Частина 3 §19 Master Prompt).
        /DYNAMICBASE
        /NXCOMPAT
        /HIGHENTROPYVA
    )

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# ── Спільна бібліотека логіки ────────────────────────────────────────────────
#
# Винесена окремо, щоб той самий код тестувався модульними тестами
# без запуску реального Bridge.

add_library(bridge_core STATIC
    src/common.cpp
    src/json.cpp
    src/redact.cpp
    src/transcript.cpp
    src/sessions.cpp
    src/state.cpp
    src/queue.cpp
    src/crypto.cpp
    src/config.cpp
    src/logging.cpp
    src/transport.cpp
    src/pairing.cpp
    src/watcher.cpp
    src/lifecycle.cpp
    src/hookpipe.cpp
    src/wire.cpp
)

target_include_directories(bridge_core PUBLIC include)

target_link_libraries(bridge_core PUBLIC
    winhttp      # WebSocket over TLS без зовнішніх бібліотек
    bcrypt       # CNG: ECDSA, ECDH, HKDF, AES-GCM, генерація випадкових чисел
    crypt32      # DPAPI для захисту ключів
    shell32      # шляхи відомих каталогів
    ole32
    advapi32
    psapi        # облік власного споживання пам'яті
)

# ── Виконуваний файл ─────────────────────────────────────────────────────────

add_executable(claude-monitor-bridge WIN32 src/main.cpp)
target_link_libraries(claude-monitor-bridge PRIVATE bridge_core)

# WIN32 робить застосунок безвіконним, але Bridge має працювати і як
# консольна утиліта (pairing, діагностика). Тому підсистема — консоль,
# а вікно приховується вручну лише у фоновому режимі.
set_target_properties(claude-monitor-bridge PROPERTIES
    WIN32_EXECUTABLE OFF
    OUTPUT_NAME "claude-monitor-bridge"
)

# ── Тести ────────────────────────────────────────────────────────────────────

enable_testing()

add_executable(bridge_tests
    tests/test_main.cpp
    tests/test_json.cpp
    tests/test_redact.cpp
    tests/test_transcript.cpp
    tests/test_state.cpp
    tests/test_queue.cpp
    tests/test_crypto.cpp
    tests/test_phones.cpp
)
target_link_libraries(bridge_tests PRIVATE bridge_core)

add_test(NAME bridge_unit_tests COMMAND bridge_tests)
