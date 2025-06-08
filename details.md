# Details

### Inotify

Linux uses Inotify to watch files. These events are stored in a buffer where
the #updates stored is limited by `/proc/sys/fs/inotify/max_queued_events`. If
overflowed, all events are dropped and a new `IN_Q_OVERFLOW` record is made.
We must assume *all* files changed in such a scenario and do a complete resync.

Inotify is *not* recursive. If we watch `/dir`, we will receive no notifications
about `/dir/sub/`.

You can avoid using Inotify by monitoring a mounted filesystem instead (`fanotify` API). 

<https://github.blog/engineering/improve-git-monorepo-performance-with-a-file-system-monitor>
