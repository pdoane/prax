# Audio: outputs, the mixer, and the backends that the build turns on.
pr_library(audio INCLUDE include
    src/audio.cpp
    src/audio_mixer.cpp
)
target_link_libraries(pr_audio PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_AUDIO_MEMORY)
    target_sources(pr_audio PRIVATE backends/memory/src/audio_memory.cpp)
    pr_headers(pr_audio PUBLIC backends/memory/include)
    target_compile_definitions(pr_audio PRIVATE PR_AUDIO_HAS_MEMORY)
endif()

if (PR_AUDIO_APPLE)
    target_sources(pr_audio PRIVATE backends/apple/src/audio_apple.cpp)
    pr_headers(pr_audio PUBLIC backends/apple/include)
    target_compile_definitions(pr_audio PRIVATE PR_AUDIO_HAS_APPLE)
    target_link_libraries(pr_audio PUBLIC "-framework AudioToolbox" "-framework CoreAudio")
endif()

if (PR_AUDIO_WINDOWS)
    target_sources(pr_audio PRIVATE backends/windows/src/audio_windows.cpp)
    pr_headers(pr_audio PUBLIC backends/windows/include)
    target_compile_definitions(pr_audio PRIVATE PR_AUDIO_HAS_WINDOWS)
    target_link_libraries(pr_audio PUBLIC avrt ole32)
endif()

if (PR_DEVELOP)
    pr_module_tests(audio
        tests/test_audio.cpp
        tests/test_audio_c.c
        tests/test_audio_mixer.cpp
    )
    if (PR_AUDIO_APPLE)
        target_sources(audio_tests PRIVATE backends/apple/tests/test_audio_apple.cpp)
    endif()
endif()
