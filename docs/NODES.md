# Chance children in the search driver

Context for the driver, not the engine: how a node keys its chance children
by `(c1, c2, Actions)`, and what a smaller key would buy. Nothing here is
decided or written yet.

## The problem

`Actions` is 167 bytes (`chance.hpp`; 153 when the figures below were
measured, before `HitDraws::lethal`), so the key is 169, and the driver
stores one per child in a per-node `std::map`. It is nearly all zeros: over
2000 random OU games (198k updates) an update's `Actions` had 1.6 nonzero
bytes on average, 6 or fewer in 99.6%, at most 16. And it repeats: those
198k updates had 10,632 distinct values.

## The candidate

- **Intern `Actions`.** One table for the whole tree maps the 167 bytes to a
  `uint32_t` id. It is exact, not a hash key: the table compares the full
  bytes on a hash match, so equal bytes get one id and different bytes
  different ids, and within a node `(c1, c2, id)` separates children exactly
  as `(c1, c2, Actions)` does. A plain 64- or 128-bit hash would be
  collision-unlikely, not collision-free, which CLAUDE.md rules out.
- **Pack the key** as `c1.bits | c2.bits << 8 | uint64_t(id) << 16`, built
  explicitly (no memcpy of a padded struct).
- **A flat child list per node**: a sorted vector of `(uint64_t key,
  uint32_t child)` instead of `std::map`, one allocation per node instead of
  one per child.

Holding it right:

- The table compares full bytes (any standard hash map does).
- Never remove or reuse an id while a tree refers to it; clear the table only
  with the tree (keep both if a subtree is reused).
- One table per tree. Threads sharing a tree need a locked or concurrent
  table.
- Assert the id stays below 2^32.
- Vector elements move when it grows: store children as indices into a node
  pool, and don't hold a reference into a child list across an insert.

## Measurements (2026-10-19)

A throwaway microbenchmark, since deleted. 20,000 nodes sampled from random
`teams/ou` self-play (one in four states); each node drew 3 random choice
pairs and 64 updates among them, from fresh seeds after `randomize_hidden`:
1.28M find-or-insert operations, 444,888 children (22.2 per node), 26,734
distinct `Actions`. Each structure ran a build pass from empty and a lookup
pass over the full tree. "Hot" runs each node's operations together;
"cold" shuffles them across all nodes, like a large tree. Release, one
core, median of 5 rounds. All three returned the same child for every
operation.

ns per operation, and live memory after both passes (`malloc_usable_size`,
intern table included, glibc's ~8 B header per allocation not):

| structure | build hot | lookup hot | build cold | lookup cold | B/child | allocations |
|---|---|---|---|---|---|---|
| `map`, raw 155 B key (now) | 154 | **97** | 521 | 500 | 202 | 444,889 |
| intern + `map` | 157 | 132 | 472 | 516 | 70 | 471,624 |
| intern + flat vector | **140** | 136 | **215** | **236** | **35** | **46,736** |

- Memory: interning takes 202 to 70 B per child; the flat vector halves it
  again and cuts allocations 10x. 5.8x overall.
- Speed: the flat vector is the cold-cache win (about 2x); intern + `map`
  still chases one pointer per tree level and gains nothing.
- Interning costs about 40 ns a step, visible hot: the table was a
  node-based `std::unordered_map` of 26k entries (about 5 MB), so a probe
  usually misses. A flat open-addressing table holding each hash beside its
  id would likely cut this; untested.

Caveats: the benchmark read each `Actions` from a 4 MB pool (often a miss),
where search has it hot from `update`, which flatters none of the three but
probably hurts interning most. A real tree sits between hot and cold (the
top is hot). One workload of random choice pairs, not search, single
threaded. For scale, an `update` is about 2,240 cycles
(`docs/OPTIMIZATION.md`).

## Alternatives set aside

- Sparse `(offset, value)` pairs inline: about 8 pairs cover 99.9%, but the
  worst case (Beat Up) needs an overflow path, i.e. interning anyway.
- Packing `Actions` in the engine: about 50 B at best (3x, against interning's
  20x on the key), touching every roll helper, `Calc` and `check_chance`.
- Fewer children instead: lethal hits now record only `lethal`
  (2026-10-21, `docs/ENGINE.md`), so the counts above are from before it.
