# Splitting and packing the battle: dropped (2026-10-19)

The plan was to move what can't change in a legal battle (species, stats,
move IDs, level, ...) out of `Battle` into a `Team` passed to `update`, then
bit-pack the live `Battle`. Both are dropped.

- **MCTS nodes don't store battles.** Children are keyed by `(c1, c2,
  Actions)`, and each iteration copies the root once and steps it down. One
  battle per thread is in use: a copy (about 20 ns, `OPTIMIZATION.md`,
  unverified source) against an `update` (about 500 ns) per step.
- **`update` is bound by instructions, not caches** (`OPTIMIZATION.md`, "The
  profile now"). The battle, the tables and the log buffer fit in L1 even
  with two hyperthreads per core, so a smaller battle doesn't speed it up,
  and bitfields add a shift and mask to every access.
- **Batched policy gradient** keeps N battles per thread; at 800 B they fit
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
