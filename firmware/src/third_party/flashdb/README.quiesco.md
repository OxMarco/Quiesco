# FlashDB

This directory vendors FlashDB 2.2.0 from
<https://github.com/armink/FlashDB/releases/tag/2.2.0> under Apache-2.0.

Only the core KVDB/TSDB and FAL sources required by this firmware are kept.
`fdb_tsdb.c` carries a small Quiesco patch: an erased TSDB is initialized one
sector at a time instead of erasing the entire 7.9 MB partition at boot. This
keeps startup bounded, repairs invalid sectors as the ring reaches them, and
does not discard the readable sectors merely because another header is bad. A
ring that has wrapped can also be left with every sector FULL, when power is
lost after the current sector is closed and before the next is erased; init
then finishes that step instead of failing every append. Two sectors that
both read as in use still format the whole log, as upstream does; that takes
minutes, so `format_all_cb` calls a weak hook the firmware uses to reload its
watchdog, which otherwise reset the unit mid-format on every boot.

Init also handles two more power-loss states. A sector marked in use whose
first record never committed has no end time; upstream then took the last
time from an uninitialised stack struct, which on sector 0 (a new unit, or
each time the ring wraps) restarted sequences or failed every mount. Such a
sector now continues from the FULL sector before it. And when erased or
damaged sectors sit in a ring that has wrapped (an erase cut short), the
oldest sector is the FULL one that started first rather than the lowest
address, which skipped most of the history on download.

`fdb_kvdb.c` also checks reads before interpreting headers or values. Startup
formats only confirmed all-0xFF sectors; a nonblank invalid header fails mount
instead of erasing user configuration, unless another sector holds KVs. That
is what a GC erase cut short by power loss leaves (GC erases a sector only
after moving its KVs out), and failing mount there kept the unit from
starting on every boot. `FlashDbPort` latches read failures for
an operation and refuses subsequent writes/erases, even through legacy
FlashDB recovery paths. ConfigStore discards caches after a failed operation,
and App retries startup rather than overwriting unreadable settings.

`fdb_def.h` and `flashdb.h` add a resumable forward iterator (marked
`Quiesco:`) so the log can be read one batch per application step.
