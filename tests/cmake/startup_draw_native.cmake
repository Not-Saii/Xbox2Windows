# A VS/PS pair compiled in-process by the C++ compiler (X2W_NATIVE_SHADERS) on its first draw through the
# stand-in, then loaded from the cache by a second process (#2978).
if(TARGET x2w_shader_compiler AND TARGET startup_stream_consumer AND EXISTS "${INTAKE_PYTHON}"
        AND EXISTS "${DXC_BIN_DIR}/dxcompiler.dll" AND EXISTS "${DXC_BIN_DIR}/dxil.dll")
    foreach(adapter IN ITEMS warp hardware)
        add_test(NAME startup_draw_native_${adapter} COMMAND "${INTAKE_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_draw_native_check.py"
            "$<TARGET_FILE:startup_stream_consumer>" "${DXC_BIN_DIR}" ${adapter})
        set_tests_properties(startup_draw_native_${adapter} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endforeach()
endif()
