Bugfix: No empty policies key left in the registry

Started with administrator rights, for example right after the setup or on a
system without user account control, the client created an empty key
HKEY_LOCAL_MACHINE\Software\Policies\BW-Tech GmbH\owncloud.online while
looking for administrator policies, and no uninstallation removed it. The
client now reads this key only if it exists. Uninstalling removes such an
empty key left behind by an earlier version (7.4 up to 11.0.0-beta2), and
then HKEY_LOCAL_MACHINE\Software\Policies\BW-Tech GmbH if it is empty as
well, also when uninstalling silently. A key with values or subkeys, such
as policies set by an administrator, is kept.
