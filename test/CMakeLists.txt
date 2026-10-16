include(owncloud_add_test.cmake)

add_subdirectory(testutils)

owncloud_add_test(JHash)

owncloud_add_test(OwncloudPropagator)
owncloud_add_test(OwnSql)
owncloud_add_test(SyncJournalDB)
owncloud_add_test(SyncFileItem)
owncloud_add_test(ConcatUrl)
owncloud_add_test(XmlParse)
owncloud_add_test(OAuthHtmlPage ../src/gui/creds/oauthhtmlpage.cpp)
owncloud_add_test(ChecksumValidator)
owncloud_add_test(ConnectionValidator)
owncloud_add_test(ShareApi)

# TODO: we need keychain access for this test
if (NOT APPLE OR NOT DEFINED ENV{GITHUB_ACTION})
    owncloud_add_test(CredentialManager)
endif()

owncloud_add_test(ExcludedFiles)

owncloud_add_test(Utility)
owncloud_add_test(AppConfig ../src/libsync/owncloudtheme.cpp)
owncloud_add_test(Theme)

# Qt-Kataloge (Standardknöpfe Ja/Nein) aus den Paket-Layouts laden; qtbase_de.ts ist ein Ausschnitt des Qt-Katalogs
owncloud_add_test(Translations)
qt_add_lrelease(TranslationsTest TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/testtranslations/qtbase_de.ts QM_FILES_OUTPUT_VARIABLE test_qtbase_de_qm)
target_compile_definitions(TranslationsTest PRIVATE TEST_QTBASE_DE_QM="${test_qtbase_de_qm}")

owncloud_add_test(SyncEngine)
owncloud_add_test(SyncMove)
add_dependencies(SyncMoveTest test_helper)
owncloud_add_test(SyncDelete)
owncloud_add_test(SyncConflict)
owncloud_add_test(SyncFileStatusTracker)
owncloud_add_test(Download)
owncloud_add_test(UploadReset)
owncloud_add_test(BulkUpload)
owncloud_add_test(ChunkingNG)
owncloud_add_test(Blacklist)
owncloud_add_test(LocalDiscovery)
owncloud_add_test(RemoteDiscovery)
owncloud_add_test(Permissions)
owncloud_add_test(DatabaseError)
owncloud_add_test(LockedFiles)
owncloud_add_test(Adapters
    ../src/gui/networkadapters/determineauthtypeadapter.cpp
    ../src/gui/networkadapters/discoverwebfingerserviceadapter.cpp
    ../src/gui/networkadapters/fetchcapabilitiesadapter.cpp
    ../src/gui/networkadapters/serverdiagnostics.cpp
#    ../src/gui/networkadapters/resolveurladapter.cpp
#    ../src/gui/tlserrordialog.cpp
    ../src/gui/networkadapters/webfingerlookupadapter.cpp
)

if (WITH_AUTO_UPDATER)
    owncloud_add_test(Updater)
endif()

owncloud_add_test(FolderWatcher)

if( UNIX AND NOT APPLE )
    owncloud_add_test(InotifyWatcher)
endif(UNIX AND NOT APPLE)

owncloud_add_test(FileSystem)

owncloud_add_test(FolderMan)

# Bandbreite: Altwert -1 („automatisch“) bleibt, bis der Nutzer die Richtung ändert. Der Aufbau liest das
# Proxy-Passwort aus dem Schlüsselbund, darum wie CredentialManager nicht auf den macOS-Läufern von GitHub.
if (NOT APPLE OR NOT DEFINED ENV{GITHUB_ACTION})
    owncloud_add_test(NetworkSettings)
endif()

owncloud_add_test(OAuth ../src/gui/creds/oauth.cpp ../src/gui/networkadapters/userinfoadapter.cpp ../src/gui/creds/oauthhtmlpage.cpp
    # Anmeldeseite des Assistenten: Browser erneut öffnen, Browser lässt sich nicht öffnen, Assistent geschlossen
    ../src/gui/newaccountwizard/oauthpagecontroller.cpp ../src/gui/networkadapters/fetchcapabilitiesadapter.cpp
    ../src/gui/networkadapters/webfingerlookupadapter.cpp ../src/gui/mainwindow/colormanager.cpp
    # Neuanmeldung (Dialog „Anmeldung erforderlich“): Browser lässt sich nicht öffnen, Server nicht erreichbar
    ../src/gui/creds/requestauthenticationcontroller.cpp ../src/gui/creds/requestauthenticationwidget.cpp
    ../src/gui/accountsgui/accountmodalwidget.cpp)
# accountmodalwidget.ui
set_target_properties(OAuthTest PROPERTIES AUTOUIC ON)

# owncloudcmd mit OAuth2-Token-Datei: Bearer, Erneuern nach 401 und bei Ablauf, Exit-Codes
owncloud_add_test(CmdOAuth ../src/cmd/cmdsession.cpp ../src/cmd/httpoauthcredentials.cpp ../src/cmd/oauthtokenfile.cpp)
target_include_directories(CmdOAuthTest PRIVATE "${CMAKE_SOURCE_DIR}/src/cmd")

configure_file(test_journal.db "${PROJECT_BINARY_DIR}/bin/test_journal.db" COPYONLY)

owncloud_add_test(JobQueue)
owncloud_add_test(SpacesManager)

add_subdirectory(modeltests)

if(WIN32)
    find_file(cldapi_FOUND CldApi.dll)
    if (cldapi_FOUND)
        include("${CMAKE_SOURCE_DIR}/test/owncloud_add_test.cmake")
        owncloud_add_test(WinVfs)
        target_link_libraries(WinVfsTest vfs_plugin_win_static)
        add_dependencies(WinVfsTest vfs_wincfapi)
    else()
        message(WARNING "Failed to detect CldApi.dll: Skipping WinVfsTest")
    endif()
endif()
