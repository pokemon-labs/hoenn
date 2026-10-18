# RL plan (context, 2026-10-16)

Notes from a design discussion. Nothing here is decided unless it says so.

## Phases

1. Policy-only self-play RL on the imperfect-information game, without
   search. No belief net and no Chance or Calc.
2. Showdown bridge and text tokenizer; ladder the raw policy for a
   baseline rating.
3. Belief net, building a battle from what a player has seen, and
   test-time search (Sokota et al., arXiv 2511.07312, "Ataraxos").

## Observation (per player, two inferences per step)

- **The binary log is not the input.** It carries exact HP for both
  sides; only `format.hpp` builds the `|split|` triplets. And at test time
  only Showdown's text exists.
- **One path through text** (agreed): engine binary -> `format.hpp` ->
  Showdown's split filter for that player -> tokenizer. Training and ladder
  share one tokenizer.
- **Event tokens, not raw bytes.** One token per log message, the sum of
  field embeddings (opcode, actor, species, move, item, ability, HP bucket).
  About 20 per turn.
- **Private information as a snapshot**, not a private log stream: six
  own-team tokens with current exact HP, PP, item and status, built by the
  driver. It must exclude what the owner can't see (own sleep and confusion
  counters). Search needs the snapshot form: in a rollout, the opponent's
  input must come from the sampled `Battle`, and its private history can't
  be made up.
- **Action tokens**, one per legal choice (at most 9), scored by key-query
  against the state.
- **Log mismatches with Showdown:** fuzz the log in training so the policy
  is robust to them (drop a line, swap adjacent lines, add or remove a
  `[from]` tag). Fix the mismatches that change meaning (Fake Out against
  Focus Punch) instead of fuzzing them. Measure how often each one happens.
- Gen 3 OU has no Team Preview (`showdown/config/formats.ts:4698`; not
  checked that `Standard` doesn't add it), so the opponent's unrevealed
  team is hidden.

## History encoding (open; also a hardware decision)

Memory per stream (8 layers, d = 512). A batch needs 2-8k streams.

| encoding | per stream | 8k streams |
|---|---|---|
| event-level causal attention + KV cache (~1,000 tokens) | ~16 MB | ~130 GB: doesn't fit |
| turn-level (encode the turn, causal over at most 100 turn embeddings) | ~1.6 MB | ~13 GB: tight |
| recurrent core over turns | a few KB | trivial |

Recomputing the history without a cache costs about 30x. Lean:
turn-level, or recurrent if concurrency matters more.

## Training

- A value head is still needed: for λ-return advantages, and as the
  rollout leaf value in search.
- Regularized self-play, as Ataraxos: reverse KL to a magnet (uniform
  legal, annealed) and to the data-collection policy, plus PPO ratio
  clipping. Plain self-play can cycle; in their ablations, removing the KL
  term is what "devastates performance".
- Ataraxos also trains only on high-advantage moves (about 2.5x cheaper).

## Hardware (napkin; measure before renting big)

15M parameters, about 35 tokens per decision, 2 decisions per step, 2 PPO
epochs: about 15 GFLOP per env step. At 40 TFLOPS sustained on a 4090,
that's about 2,700 steps/s, about 55 games/s, about 30M games per
GPU-week. The engine uses about 0.1 core; Python batching and the trainer
need 6-8 cores per GPU. **GPU-bound**: rent for the GPU.

- Consumer cards beat data-center cards per dollar at 15M parameters.
  4090 at roughly $0.30-0.50/hr on-demand on vast.ai (2026 sources vary
  widely). An H100 is wasted on a model this small.
- First rent one 4090 for a few hours. Measure steps/s at the real batch
  size and CPU time per step for formatting, filtering and tokenizing.
- Then 4x 4090 on one host (at least 32 cores, at least 64 GB RAM, a
  verified host). Use C++ env processes, each holding thousands of games,
  with one batched round trip per tick; not one Python process per game.
- Interruptible halves the cost, if checkpoints every ~10 minutes and
  auto-resume have been tested by actually killing the instance.
- Belief training is a separate, later job: supervised, on positions from
  the final policy. Ataraxos: 4 H100s for 4 days, against 16 H100s for a
  week of RL.

## Ladder (phase 2)

- A websocket bridge: `|request|` gives the legal choices and own state;
  `/choose`. Check that hoenn's switch SLOT matches Showdown's request
  order.
- A test for the text tokenizer: the engine log and Showdown's text of
  the same game must give identical tokens.
- Bring teams from the training pool.
- Check Showdown's bot rules first.
- Expect humans to be off-distribution, and self-play habits to be
  exploitable.

## Search (phase 3, after Ataraxos)

- **Belief net:** a transformer over the player's view; a decoder
  predicts, per opponent slot, autoregressively: species if unrevealed,
  moves (conditioned on the revealed ones), item, ability, nature and EV
  spread (a discrete set of spreads), and exact HP within the displayed
  percentage. Train by supervised max likelihood (teacher forcing) on
  self-play positions labelled with the true `Battle`. Use dropout for
  off-distribution human play. The team pool sets the prior: mix in
  `gen_teams` teams.
- **Battle construction:** a public-state tracker turns the filtered log
  (engine or Showdown text) into a partial `Battle`; the belief sample fills
  in the hidden fields, and the result must pass `valid()`. Hidden counters
  come from durations worked out from the log, given the sampled ability
  (Early Bird), then `randomize_hidden`. Engine `Durations` assume perfect
  information, so they're not the source. Reject samples that break hard
  constraints (revealed moves, item clause, speed order). Rejected: replaying
  the game with Calc overrides (damage rolls must land on the observed HP
  percentages).
- **Rollouts:** for each legal choice and sample, play about N turns with
  the policy on both sides (log on); Q is the mean of the leaf values. Then
  a tabular magnetic mirror descent step toward the network policy, and
  sample a move from the result. Ataraxos: 1,000 rollouts x 40 ply, about
  1.26 s per move on one H100.
- Chance is used only to test: compare the tracker's durations with
  Chance's `Durations` at every step; rebuild a mid-game battle with the
  true hidden values and check its logs match the real battle's.
- The chance-key collisions, `min_ko_roll`, `Summaries` and engine speed
  don't block this plan (it never keys on `Actions`; the network is the
  bottleneck).

## Engine work this needs

- **A bound for the log buffer.** `MAX_LOG_SIZE` (2048) has never been
  measured against the worst case, and release doesn't check it. Record the
  most bytes one update writes in `soak` and `check_frame`.
- The per-player text view above.
- For search: building a mid-game `Battle` (`helpers.hpp` only builds
  turn 0) and the public-state tracker.

## Unverified

The 40 TFLOPS sustained and the CPU cost of formatting per step; the GPU
TFLOPS figures (from memory); vast.ai prices; Ataraxos details past the
main text (the supplementary sections weren't read).
