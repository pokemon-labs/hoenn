# Packing the battle

`Volatiles` is partly packed (2026-10-23): 68 -> 36 B, `Battle` 800 -> 736
B. The rest of `Battle` keeps natural alignment, and the `Team` split stays
dropped (below).

## The `Volatiles` layout

A bitfield costs by kind of access:

- **A 1-bit `bool`** reads as one `test byte, mask` and writes as one `or`
  or `and`: as cheap as a whole `bool`. Every flag is one bit, the hot ones
  included (`substitute`, `flinched`, `imprisoned_others`, `torment`).
- **A multi-bit counter** reads with a shift and mask, and each decrement is
  a read-modify-write.
- **A 9-bit `Move`** is masked at every compare.

So the hottest moves and counters are whole bytes, and the rest are packed.
A field of type T can't cross a `sizeof(T)` boundary, so each packed `Move`
shares its 16 bits with 7 bits of cold counters; the order is the packing's.

Whole bytes, by share of `Volatiles` accesses (gcov line counts on `bench`,
both configurations; a line naming two fields counts for both, and counts
aren't cycles):

| Field | Share | |
|---|---|---|
| `kind` | 16.5% | nearly every multi-turn check |
| `choice_move` | 7.2% | `choices()`, every turn |
| `encored_move` | 4.0% | `choices()` |
| `taunt` | 4.0% | `choices()`, a residual |
| `substitute_hp` | 3.4% | hits on a substitute |
| `last_move` | 3.4% | every move |
| `disabled_move` | 3.3% | `choices()` |
| `encore_timer` | 3.2% | a residual |
| `wrapped` | 2.1% | a residual, `trapped()` |
| `lock` | 2.1% | with `kind` |
| `move_actions` | 1.9% | every move |
| `disable_timer` | 1.4% | a residual |
| `fury_cutter` | 1.4% | a residual |

`bide_damage` stays `u32` (ENGINE.md, decision 15). Packed: `locked_move`
(0.1%), `wrapped_move`, `last_used_move` and `last_taken_move` (under 1%),
and the counters `perish_song`, `stall`, `protect_uses`, `confusion`,
`lock_on`, `lock_on_target`, `first_turn`, `last_used_type`, `yawn`,
`last_taken_from`, `mimicked`, `status_counter`, `encored_slot` and
`stockpile`. The cut at about 1.4% is by eye: `stall`, `protect_uses`,
`perish_song` and `last_taken_from` are about as hot as the last two whole
bytes. perf was unavailable (`perf_event_paranoid` 4), hence gcov.

## Measured

Four layouts, `bench` in the release flags (-O3), 30,000 battles on
`teams/ou`, 10 rounds round-robin pinned to one core, medians; every build
made the same 3,003,827 updates:

| Layout | `Battle` | `bench --log` | `bench` |
|---|---|---|---|
| A: natural alignment (`5237533`) | 800 B | 1.968M/s | 2.230M/s |
| B: everything packed | 720 B | −1.4% (slower in 9/10) | +1.8% (faster in 9/10) |
| **C: mixed (this)** | 736 B | +0.4% (faster in 8/10) | +1.1% (faster in 9/10) |
| D: only the flags packed | 760 B | −0.8% (slower in 7/10) | +0.7% (faster in 7/10) |

Every difference is within about ±2%, where code placement matters as much
as the masks: D packs less than C and is slower. C is the only layout not
slower than A in either configuration, and the best in `bench --log`, RL's.
An earlier, noisier run had B −2.2% and −2.9%; this one supersedes it.
`bench` copies no battles, so it can't show what size buys.

## Not done: splitting out a `Team`

The plan was to move what can't change in a legal battle (species, stats,
move IDs, level, ...) out of `Battle` into a `Team` passed to `update`.
Dropped (2026-10-19):

- **MCTS nodes don't store battles.** Children are keyed by `(c1, c2,
  Actions)`, and each iteration copies the root once and steps it down. One
  battle per thread is in use: a copy (about 20 ns, `OPTIMIZATION.md`,
  unverified source) against an `update` (about 500 ns) per step.
- **`update` is bound by instructions, not caches** (`OPTIMIZATION.md`, "The
  profile now"). The battle, the tables and the log buffer fit in L1 even
  with two hyperthreads per core, so a smaller battle doesn't speed it up.
- **Batched policy gradient** keeps N battles per thread; at 736 B they fit
  in L2 until hundreds per thread.
- **The split** would change every signature and add a second base pointer
  for no gain. If copying only the live bytes is ever wanted, order
  `Battle` so the static data is one block at the end instead.

Where search memory goes instead is the map key: `Actions` is 167 B,
fixed-size, with a field for every possible draw and mostly zero in a given
turn. A lossless compact form (bit widths, or a presence mask plus values)
would cut it several-fold without hashing, if tree memory becomes a limit.

Revisit on a measurement: data-cache misses in `bench`, or per-thread
throughput at 1, 8 and 16 threads.
