if(APPLE)
set(OC_OEM_SHARE_ICNS "${CMAKE_BINARY_DIR}/src/gui/${APPLICATION_ICON_NAME}.icns")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(XCODE_CONFIG "Debug")
else()
    set(XCODE_CONFIG "Release")
endif()

# The bundle identifier and application group need to have compatible values with the client
# to be able to open a Mach port across the extension's sandbox boundary.
# Pass the info through the xcodebuild command line and make sure that the project uses
# those user-defined settings to build the plist.
add_custom_target( mac_overlayplugin ALL
    xcodebuild -project ${CMAKE_SOURCE_DIR}/shell_integration/MacOSX/OwnCloudFinderSync/OwnCloudFinderSync.xcodeproj
        -target FinderSyncExt -configuration "${XCODE_CONFIG}" "SYMROOT=${CMAKE_CURRENT_BINARY_DIR}"
        "OC_OEM_SHARE_ICNS=${OC_OEM_SHARE_ICNS}"
        "OC_APPLICATION_NAME=${APPLICATION_NAME}"
        "OC_APPLICATION_REV_DOMAIN=${APPLICATION_REV_DOMAIN}"
        "OC_SOCKETAPI_TEAM_IDENTIFIER_PREFIX=${SOCKETAPI_TEAM_IDENTIFIER_PREFIX}"
    COMMENT building Mac Overlay icons
    VERBATIM)
add_dependencies(mac_overlayplugin owncloud) # for the ownCloud.icns to be generated
add_custom_command(TARGET mac_overlayplugin POST_BUILD COMMAND ${CMAKE_COMMAND}
    ARGS -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/${XCODE_CONFIG}/FinderSyncExt.appex" "$<TARGET_FILE_DIR:owncloud>/../PlugIns/FinderSyncExt.appex" MAIN_DEPENDENCY "${mac_overlayplugin}")

# Experimental macOS virtual files (File Provider).
#
# Unlike the Finder Sync extension this one cannot be ad-hoc signed: a File
# Provider extension only activates when it is signed with a real Team ID and
# shares an app group with the client. Builds without a signing identity (CI,
# packagers) therefore leave it off, which is why this is opt-in.
option(BUILD_MAC_FILE_PROVIDER "Build the experimental macOS File Provider extension (needs a signing identity)" OFF)
if(BUILD_MAC_FILE_PROVIDER)
    if(NOT SOCKETAPI_TEAM_IDENTIFIER_PREFIX)
        message(FATAL_ERROR "BUILD_MAC_FILE_PROVIDER requires SOCKETAPI_TEAM_IDENTIFIER_PREFIX to be set to '<TEAMID>.'")
    endif()
    # The prefix carries a trailing dot; xcodebuild wants the bare team id.
    string(REGEX REPLACE "\\.$" "" MAC_FILE_PROVIDER_TEAM "${SOCKETAPI_TEAM_IDENTIFIER_PREFIX}")

    add_custom_target( mac_fileproviderplugin ALL
        xcodebuild -project ${CMAKE_SOURCE_DIR}/shell_integration/MacOSX/OwnCloudFileProvider/OwnCloudFileProvider.xcodeproj
            -target FileProviderExt -configuration "${XCODE_CONFIG}" "SYMROOT=${CMAKE_CURRENT_BINARY_DIR}"
            -allowProvisioningUpdates
            "DEVELOPMENT_TEAM=${MAC_FILE_PROVIDER_TEAM}"
            "OC_APPLICATION_NAME=${APPLICATION_NAME}"
            "OC_APPLICATION_REV_DOMAIN=${APPLICATION_REV_DOMAIN}"
            "OC_SOCKETAPI_TEAM_IDENTIFIER_PREFIX=${SOCKETAPI_TEAM_IDENTIFIER_PREFIX}"
        COMMENT building the macOS File Provider extension
        VERBATIM)
    add_dependencies(mac_fileproviderplugin owncloud)
    add_custom_command(TARGET mac_fileproviderplugin POST_BUILD COMMAND ${CMAKE_COMMAND}
        ARGS -E copy_directory "${CMAKE_CURRENT_BINARY_DIR}/${XCODE_CONFIG}/FileProviderExt.appex" "$<TARGET_FILE_DIR:owncloud>/../PlugIns/FileProviderExt.appex" MAIN_DEPENDENCY "${mac_fileproviderplugin}")
endif()

endif()

