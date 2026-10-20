Bugfix: No empty policies key left in the registry

Started with administrator rights, for example right after the setup or on a
system without user account control, the client created an empty key
HKEY_LOCAL_MACHINE\Software\Policies\BW-Tech GmbH\owncloud.online while
looking for administrator policies, and no uninstallation removed it. The
client now reads this key only if it exists.
