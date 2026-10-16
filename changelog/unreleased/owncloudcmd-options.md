Enhancement: owncloudcmd offers the options of the 6.0.3 client again

owncloudcmd understands --trust, --httpproxy (http:// or socks5://),
--exclude, --unsyncedfolders, --uplimit/--downlimit (KB/s), --sync-hidden-files
(hidden files are ignored by default, as in 6.0.3), --max-sync-retries,
--non-interactive, --silent, --logdebug and --version, and accepts the remote
folder as third argument. The password can come from OWNCLOUDCMD_PASSWORD
instead of -p (which is visible in the process list); without both,
owncloudcmd asks for it on the terminal.
