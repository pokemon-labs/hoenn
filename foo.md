# Handoff: making src/mechanics.hpp readable (branch `readable`)

The user asked for `src/mechanics.hpp` to be easy to explain: no brute
forcing or ad hoc machinery, comments shorter than the code they describe,
and decomp citations (util:N, bsc:N, bs1:N, main:N) kept. Work happens on the
branch `readable`, made from `main`. The user said to get on with it without
checking in on every step.

## Done (committed on `readable`)

- `87640ee`: `execute()` is the one frame every move runs through: setup,
  `cancel`, the shared opening (attackstring, ppreduce, Snatch) for every
  script but the hits', `script()`, `moveend`, then Bide's end or the user's
  faint (`m.faints`: Explosion, Memento).
  - `script()` is one `switch` in which each effect appears once, with its
    parameters.
  - `hit()` is an outline over named steps: `fails_early`, `charges`,
    `counter_damage`, `explodes`, `base_hit`, `lands`, `strike`,
    `multi_hit_strikes`, `present_heals`, `on_hit`, `self_effects`,
    `after_hit`, `contact`, `recoil`.
  - `tools/coverage.cpp` was deleted, along with its CMake target and its
    line in `docs/TESTING.md`.
- `d951b48`: `cancel()` has one `stop` lambda; `hurt_itself`, `imprisoned`,
  `add_bide`, `but_it_failed` and `rolls::nth_bit` replace copied code.
  - Quick Claw: `pass_order_matters` replaces running the end-of-turn passes
    on copies in release builds; `orders_differ` stays as a debug assert.
  - This is the only intended behaviour change, and only for games with
    Quick Claw (none of the team files hold it).
- `98f078a`: `MoveState` is in three commented groups; `after_move`, `ran`,
  `contact_done`, `kings_rock_done` and the two ignore flags are gone.
  `delegated` means a called move's moveend ended the action.

## In progress (uncommitted in the working tree)

A comment-only pass, done down to about line 4,200 of 7,518 (it stopped
around the Roar and Baton Pass comments). The code is unchanged: a
comment-and-whitespace-stripped fingerprint matches the one taken before the
pass.

Style for the pass:
- One to three lines per comment.
- Keep the decomp citation, and a Showdown file:line citation where we
  follow Showdown over the decomp, plus the difference in one clause.
- Drop history ("decided 2026-10-07", "Step 5 group N"), "Log:" paragraphs,
  and any restating of the code.
- Comments that say "after the canceler, attackstring and ppreduce" are now
  wrong, because `execute()` does those for every script. Drop that phrase.

## Still to do

1. Finish the comment pass: the move scripts from about line 4,200 down, the
   copy and call moves, `execute`, `use_move`, `actions`, the end of turn,
   `update`. Leftover section headers such as "Step 5 group 8" are history;
   rename them.
2. Group `plain_hit`'s case list into labelled families. This reorders code,
   so check it with the equivalence check, not the fingerprint.
3. Maybe split the file into topic headers, with `mechanics.hpp` as the
   umbrella: rolls, order and selection, switching, moves, hits, scripts,
   end of turn, update.
4. Fix the stale function names in `docs/ENGINE.md` and `CLAUDE.md`, for
   example `handled`, `plain_hit` and `charge()`.
5. Possible Showdown fix, not done: a King's Rock stolen with Thief can
   flinch in the same move, because King's Rock is still checked in moveend
   after the item is handed over. In Showdown it couldn't. Ask before
   changing behaviour.

Then commit, run the debug suite, and report back to the user.

## Checks

These live in the old session's scratchpad, which may be cleaned up:
`/tmp/claude-1000/-home-user-hoenn/9ff47dbe-68d3-450f-b9ff-89dfbba8800e/scratchpad/`
(written `S/` below). A copy is at
`~/.claude/projects/-home-user-hoenn/readable-scratch/`. `run.sh` has the
`/tmp` path hard-coded, so edit its `S=` line if you use the copy. The copy's
`main/` is a plain snapshot of `main`, not a registered worktree.

- **Equivalence check:** `S/same/run.sh 2000`.
  - It builds `S/same/check_same.cpp` against `main`'s headers (a
    `git worktree` at `S/main`) and this tree's.
  - It plays the same seeded games in full and null modes on
    `teams/ou/*.txt` and `tools/teams/*.txt`, and hashes after every update:
    the choices offered, the Result, the log bytes, the battle bytes, and
    Chance.
  - Expect "same" everywhere; it takes about 40 seconds.
  - To rebuild it if the scratchpad is gone: random self-play like
    `tools/bench.cpp`, FNV-hashing those fields, one line per game, then
    `diff` the outputs.
  - If `S/main` is gone, run `git worktree prune` and re-add it.
- **Quick Claw teams:** `S/qc/*.txt` are every team with ` @ Quick Claw` on
  all Pokémon (made with sed). Use them with `check_chance` and the
  equivalence check.
- **Comment fingerprint:** `python3 S/py/code_only.py src/mechanics.hpp |
  cmp - S/code_before.txt`.
- **Comment tool:** `S/py/blocks.py list FILE START END` lists comment
  blocks by line; `S/py/blocks.py apply FILE MAPFILE` rewrites them (the
  format is in its header). Apply one batch, then re-list, because line
  numbers shift.
- **Full suite:** `cmake --build --preset debug -j16 && ctest --preset debug
  -j6`, about 2 minutes. All 9 tests passed at `98f078a`.
