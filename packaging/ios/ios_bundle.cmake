# iOS app bundle settings for LuigisMansioniOS (docs/IOS.md). Included from CMakeLists.txt.
set(LM_IOS_BUNDLE_ID "io.github.lmrecompiled.ios" CACHE STRING "CFBundleIdentifier of the iOS app")
set(LM_IOS_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS version")

function(lm_ios_bundle tgt)
    set_target_properties(${tgt} PROPERTIES
        OUTPUT_NAME LuigisMansion
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/ios/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER ${LM_IOS_BUNDLE_ID}
        MACOSX_BUNDLE_BUNDLE_NAME "Luigi's Mansion"
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${LM_IOS_DEPLOYMENT_TARGET}
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
    target_compile_definitions(${tgt} PRIVATE LM_IOS=1)
    # Release code for the generated game functions even in RelWithDebInfo; keep symbols for crash
    # reports (the session log's stacks name functions through dladdr).
    target_link_options(${tgt} PRIVATE -Wl,-export_dynamic)
    # Resources the app reads next to its executable (flat iOS bundle): app icons. pipelines.bin is
    # copied by lm_runtime_target.
    file(GLOB _icons ${CMAKE_SOURCE_DIR}/packaging/ios/AppIcon*.png)
    foreach (_icon ${_icons})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_icon} $<TARGET_FILE_DIR:${tgt}>)
    endforeach ()
endfunction()
