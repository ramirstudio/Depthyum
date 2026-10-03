# After Effects plug-in (.aex). Windows / MSVC. Configure the top-level project with
#   -DAE_SDK_DIR="C:/path/to/AfterEffectsSDK"
# (the folder that contains Examples/Headers and Examples/Resources).

set(AE_EXAMPLES "${AE_SDK_DIR}/Examples")
if(NOT EXISTS "${AE_EXAMPLES}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE_SDK_DIR does not look like the After Effects SDK: ${AE_EXAMPLES}/Headers/AE_Effect.h not found")
endif()
if(NOT WIN32)
    message(FATAL_ERROR "This CMake script builds the Windows .aex; use Xcode with the SDK templates on macOS")
endif()

set(AE_HEADERS "${AE_EXAMPLES}/Headers" "${AE_EXAMPLES}/Headers/SP" "${AE_EXAMPLES}/Util")
# The PiPL resource is committed pre-generated (DepthyumPiPL.rc), as in Lensyum.
set(PIPL_RC "${CMAKE_CURRENT_SOURCE_DIR}/DepthyumPiPL.rc")

# Without the RC language CMake silently skips the .rc and the .aex ships with no PiPL, which
# After Effects then ignores.
enable_language(RC)

add_library(Depthyum SHARED
    DepthyumAE.cpp
    DepthyumParams.h
    "${PIPL_RC}"
)
target_include_directories(Depthyum PRIVATE ${AE_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(Depthyum PRIVATE MSWindows WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(Depthyum PRIVATE /O2 /fp:fast /EHsc /W3)
target_link_libraries(Depthyum PRIVATE depthyum_core)
set_target_properties(Depthyum PROPERTIES SUFFIX ".aex" OUTPUT_NAME "Depthyum")

# Optional: copy the plug-in into After Effects after each build, e.g.
#   -DDEPTHYUM_INSTALL_DIR="C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/Depthyum"
set(DEPTHYUM_INSTALL_DIR "" CACHE PATH "Folder the .aex is copied to after building")
if(DEPTHYUM_INSTALL_DIR)
    add_custom_command(TARGET Depthyum POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPTHYUM_INSTALL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Depthyum>" "${DEPTHYUM_INSTALL_DIR}")
endif()
