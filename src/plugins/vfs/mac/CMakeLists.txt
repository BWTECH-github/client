# macOS File Provider VFS plugin (experimental).
# Built only on Apple platforms; on other platforms this directory is a no-op
# so it can stay in VIRTUAL_FILE_SYSTEM_PLUGINS unconditionally.
if(APPLE)
    add_vfs_plugin(NAME macfileprovider SRC vfs_mac.cpp fileproviderdomain.mm)
    target_link_libraries(vfs_macfileprovider "-framework FileProvider" "-framework Foundation")
endif()
