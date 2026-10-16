# CLAUDE should ignore this file

CLAUDE.md should be stripped down so its just enough to *maintain* once we match showdown

remove pkmn references, that's a 'trademark' of libpkmn. The repo is called hoenn so that should be the master namespace prolly

review scheduling of libpkmn's testing - we should be able to automatically catch departure from showdown main (or whatever libpkmn thinks is prudent to test vs) automatically using Github Actions

README that informs users/claude what this repo is. Good enough for search (and therefore RL) sim. No rng matching. Fast. Safe. Styled after/derivative of libpkmn. AI slop. You can do perfect info search for IS-MCTS and logs let you just slap shit into transformer in imperfect info case.

Make a static lib so we don't have to rebuild for all of a driver's binaries

Live document for cart/ps differences. Claude should cross shit off when they get patched by showdown.

Check/spec ko roll determinization. It should probably return pair of u8 for min roll that ko's on single hit move. 0 - 15 and 16 means no KO rolls. 

Expand testing for correctness per claudes suggestions

Wtf is going on with calc/summaries/whatever. Still don't really get besides that we use it for clamping.

Maybe eventuall add support for EV (IV?) deduction from damage rolls (we can use ko threshold fn for this probably)

# Not this repo

Make unrecog args an error in argparse

