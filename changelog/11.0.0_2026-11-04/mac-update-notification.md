Bugfix: macOS shows when a new version is available

On macOS the client asked Sparkle for updates, but the update feed of
owncloud.online is not a Sparkle appcast, so no update was ever found. Like on
Windows, the client now shows that a new version is available, with the choice
to skip it or to get it; "Get update" opens the download page. To install,
replace the app in Applications with the new one from the disk image; settings,
accounts and the Finder integration stay. The pre-releases 11.0.0-beta1 to beta4
cannot show this notice and have to be updated by hand once.
