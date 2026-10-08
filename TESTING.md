# Testing plan

How to catch bugs that live between the app and the unit, like the
2026-10 sync bug (`cd6b8b8`): right after boot the unit reported newest
sequence 0, the app read that as a replaced log, downloaded everything again
and showed "3,500 of 1 readings". Every piece passed its own tests. The bug
was in how the pieces met.

## Why it slipped through

1. **No test looked at the unit right after boot.** Firmware host tests
   always ran a measurement before connecting, so the status value sent
   before the log was mounted was never checked.
2. **Two meanings for one value.** `PROTOCOL.md` says newest 0 means an empty
   log; the firmware also sent 0 for "not mounted yet". Tests on each side
   used their own meaning.
3. **The app's sync code had no tests.** Jest covers codecs and
   `LogSession`, but not `runSync`/`downloadBatch` in `app/src/ble/link.ts`.
   The batching, retries, cursor handling and progress maths ran only on a
   phone.
4. **Speed was never measured.** A 12 s wait before every batch and retry
   looked like slow radio. `ble-test.py download` stopped early after a lost
   packet, so even the bench numbers were wrong.
5. **No CI for the app or firmware.** Only the landing page runs in CI.

## What to add, in order

### 1. CI for what already exists (small)

A workflow on every push and pull request to `master`:

- `app/`: `npm ci`, `npx tsc --noEmit`, `npx expo lint`, `npx jest`
- `firmware/`: `make check`, `make test` (host build, no hardware)
- `firmware/scripts/ble-test.py --self-test` (codec and crypto vectors, no
  hardware)

### 2. App sync tests against a simulated unit (medium, most value)

A `FakeUnit` in `app/src/ble/__tests__/` that stands in for `./transport`
(`subscribe`, `read`, `write`, `negotiatePayload`). It holds a log of
sequences and speaks §7: START/ABORT on `0009`, DATA/FRAGMENT/END on `000A`,
status on `0004` with the newest sequence and bit 1. It has switches to make
it misbehave:

| Switch | Models |
| --- | --- |
| `newestBeforeMount: 0` | the boot bug: status says 0 until the first measurement |
| `dropPacket(n)` | a notification lost in transit (counter skip) |
| `stallAfter(n)` | the stream stops without END |
| `disconnectAfter(n)` | the link drops mid-batch |
| `wrapped(oldest)` | a ring that overwrote its oldest records |
| `replaced()` | new flash: sequences restart below the phone's cursor |
| `payload(20)` | the fragment path at the minimum MTU |

Drive the real `syncLog()` and check what must always hold, in every
scenario:

- **No re-download:** records sent = records the phone lacked, plus the
  ones resent after a loss. A sync right after a reboot sends only the new
  records.
- **Cursor:** after a full sync it equals newest + 1. After a partial one,
  no record before it is missing from the database.
- **Progress:** in every `link` state the sync passes through, `received ≤
  expected` whenever `expected` is set, and the bar's fraction stays in
  [0, 1].
- **Effort:** STARTs ≤ batches + losses + 1. A loss costs one retry, not a
  whole new download.
- **Ending:** the sync ends as `done` or `failed` and never spins. A
  `failed` sync keeps what it saved.

Each row in the switch table is one test, plus a test that combines them
(boot bug + one loss + a wrapped log).

### 3. Firmware: check status at every point in the life cycle (small)

In `firmware/tests/host/test_runtime.cpp`, read every published
characteristic at each of these points and check that it matches the
flash, not a default:

- just after settings load and BLE start, before the first measurement
  (covered for status by "status reports the log's newest record before the
  first measurement")
- after an interrupted erase, on reboot
- during and after a sync, an erase, an FRC soak, a redraw

General rule: a characteristic the phone can read must never show a
placeholder that looks like real data. If the value isn't known yet, the
protocol needs a way to say "unknown", or the firmware must load it before
BLE starts.

### 4. Protocol contract: one source of truth per field (small, ongoing)

For every field in `PROTOCOL.md` that has a special value (0, 0xFFFF, a flag
bit), state what it means in each device state, and test that meaning on
both sides:

- app: a decode test, plus how the code behaves on that value (as
  `logWasReplaced(1202, 0)` is now)
- firmware: a host test that produces the value only in the state the
  protocol names
- `ble-test.py --self-test`: the same rule in the Mac harness

Review checklist for protocol changes: "Can this field be read before its
source is ready? What does the phone do with that value?"

### 5. Run the firmware against the app's sync code (larger, later)

The sleep-parity tests (`tests/host/parity/`) already check the firmware
against the app's own code. Do the same for sync: build the host firmware
(`App` with `FakeBle` and `FlashSim`) as a small binary that talks over
stdin/stdout, and let the jest `FakeUnit` forward to it instead of
simulating. Then the real firmware and the real app sync code run together
in CI, through boot, a log with filler records, reboot and erase. That would
have caught this bug without anyone thinking of the case in advance.

### 6. On-air checks with numbers (small, bench)

Add to `ble-test.py run` (WARN beyond the limit, FAIL far beyond):

| Check | Limit | 2026-10-08 result |
| --- | --- | --- |
| first packet after START, no measurement due | ≤ 2 s | 0.5 s |
| streaming rate, MTU 247 | ≥ 70 records/s | 90 |
| lost packets per 1,000 records | ≤ 5 | 0.1–2 |
| sync after a reboot sends only new records | exactly | yes |
| newest in status before the first measurement after boot | = flash | yes |
| whole log batched like the app: no gaps, ends with END | exactly | yes |

The reboot check needs USB (`arduino-cli upload` or the console resets the
unit) and follows the bench notes: no display stress, no battery tests.

### 7. Signals from real use (small)

The app's debug report should include, for the last few syncs: STARTs,
retries, records received vs the estimate, seconds to first packet, and
whether the cursor was reset. A sync that resets the cursor with newest 0,
or that receives far more than its estimate, should log a trace line. Then
the next bug like this shows up in a bug report, not after a 4-minute
wait.

## Manual pass before a release

On a phone, with the bench unit:

1. Fresh install, add the unit, first sync: count matches the log, the bar
   finishes at the end.
2. Unplug and replug the unit, open the app at once: the sync fetches only
   the readings since the last one.
3. Walk out of range mid-sync, come back: the sync picks up where it
   stopped.
4. Sync with "erase after sync" on, then sync again: nothing duplicated or
   lost.
5. Change the screen or offsets during a sync: the sync isn't cut off.
