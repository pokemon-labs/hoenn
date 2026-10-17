# ADV OU teams

Pokémon Showdown exports scraped 2026-10-06 from pokepast.es links in
[Smogon's ADV forum](https://www.smogon.com/forums/forums/adv/), one file per
thread. Only six-Pokémon teams that import cleanly are kept, and
duplicates within a file are dropped.

| File | Teams | Thread |
|---|---|---|
| `sample-teams.txt` | 50 | ADV OU Sample Teams (3687813) |
| `teams-through-the-ages.txt` | 14 | ADV Teams Through the Ages (3687203) |
| `johnald-teamdump-2026-03.txt` | 49 | Johnald Teamdump March 2026 (3779110) |
| `ji3-free-teams.txt` | 64 | ji3 Win and Even More Free Teams (3785090) |
| `reflections-and-free-teams.txt` | 10 | Reflections and Free Teams (3780474) |
| `four-bl-challenge.txt` | 14 | Four BLs, Six Months and 44 Teams (3768374) |
| `spl-xi.txt`, `spl-xii.txt`, `spl-xiv.txt` | 10, 7, 9 | SPL XI and XII postseason threads, SPL XIV teams report |
| `teambuilding-competition-8-fire-offense.txt` | 93 | Teambuilding Competition Round 8 (3770678) |

Edited (2026-10-15) so every team
passes Showdown's gen3ou validator (pinned in `tools/criteria.toml`): 13 Pokémon
without an EVs line have `EVs: 1 HP` (no stat changes), a nickname in
parentheses lost them, and 7 teams that break today's rules (Quick Claw,
Soundproof, the Baton Pass clauses) were removed. 320 teams.

Threads are at `https://www.smogon.com/forums/threads/<number>/`.
