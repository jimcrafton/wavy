add_executable(wavy_tests
    test_audioEngine.cpp
    test_audioEvents.cpp
    test_audioProgram.cpp
    test_audioStereo.cpp
    test_audioSteps.cpp
    test_clipPlayer.cpp
    test_version.cpp
)

if(TARGET vst3::hosting)
  # wavyChaos.vst3: a deliberately misbehaving plugin (sleeps, crashes, NaN output, exceptions, failing setup) for fault-handling tests.
  add_library(wavyChaos MODULE
    chaosPlugin/chaosPlugin.cpp
    ${vst3Root}/public.sdk/source/main/dllmain.cpp
    ${vst3Root}/public.sdk/source/main/pluginfactory.cpp
    ${vst3Root}/public.sdk/source/main/moduleinit.cpp
    ${vst3Root}/public.sdk/source/common/pluginview.cpp
    ${vst3Root}/public.sdk/source/vst/vstaudioeffect.cpp
    ${vst3Root}/public.sdk/source/vst/vstcomponent.cpp
    ${vst3Root}/public.sdk/source/vst/vstcomponentbase.cpp
    ${vst3Root}/public.sdk/source/vst/vstbus.cpp
    ${vst3Root}/public.sdk/source/vst/vsteditcontroller.cpp
    ${vst3Root}/public.sdk/source/vst/vstparameters.cpp)
  set_target_properties(wavyChaos PROPERTIES PREFIX "" SUFFIX ".vst3" CXX_STANDARD 17)
  target_link_libraries(wavyChaos PRIVATE vst3::hosting)
  if(MSVC)
    target_compile_options(wavyChaos PRIVATE /W0 /utf-8 /EHsc)
  endif()

  target_sources(wavy_tests PRIVATE test_vstPlugin.cpp test_vstChaos.cpp test_pluginHost.cpp)
  target_compile_definitions(wavy_tests PRIVATE WAVY_CHAOS_PATH="$<TARGET_FILE:wavyChaos>")
  add_dependencies(wavy_tests wavyChaos)
endif()

target_link_libraries(wavy_tests PRIVATE wavy::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(wavy_tests)
