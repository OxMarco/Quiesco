# FlashDB

This directory vendors FlashDB 2.2.0 from
<https://github.com/armink/FlashDB/releases/tag/2.2.0> under Apache-2.0.

Only the core KVDB/TSDB and FAL sources required by this firmware are kept.
`fdb_tsdb.c` carries a small Quiesco patch: an erased TSDB is initialized one
sector at a time instead of erasing the entire 7.9 MB partition at boot. This
keeps startup bounded, repairs invalid sectors as the ring reaches them, and
does not discard the readable sectors merely because another header is bad.

`fdb_kvdb.c` also checks reads before interpreting headers or values. Startup
formats only confirmed all-0xFF sectors; a nonblank invalid header fails mount
instead of erasing user configuration. `FlashDbPort` latches read failures for
an operation and refuses subsequent writes/erases, even through legacy
FlashDB recovery paths. ConfigStore discards caches after a failed operation,
and App retries startup rather than overwriting unreadable settings.
