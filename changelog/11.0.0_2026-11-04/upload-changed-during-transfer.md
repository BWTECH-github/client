Bugfix: A file that changes while it is uploaded is retried, not blocked

When a file changed while it was being uploaded, the server rejected the
upload because the content no longer matched the checksum sent with it
(400 "The computed checksum does not match the one received from the
client."). The client reported an error and blocked the file for a while,
so the next sync run skipped it and failed as well. owncloudcmd, which
uploads without a minimum file age, ran into this right after a restart.

Such a rejection is now treated like any other change detected during the
sync: the item is reported as "Local file changed during sync. It will be
resumed.", another sync is scheduled and the new content is uploaded, for
single uploads as well as chunked ones.
