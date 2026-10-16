#!/usr/bin/env node
// Plays seeded gen3 OU games in Pokémon Showdown with random legal choices
// and writes them as JSONL traces, for the realizability test (TESTING.md,
// "check_showdown"). Showdown picks every choice, so nothing here needs the engine;
// check_showdown replays the traces against it.
//
//   node tools/showdown_traces.cjs [--games N] [--seed N] [--format gen3ou|custom]
//       [--showdown PATH] [TEAM_FILE...] > traces.jsonl
//
// The teams in every TEAM_FILE are pooled, duplicates included. Defaults:
// 100 games, seed 1, gen3ou, every teams/ou/*.txt, showdown/ (0.11.11;
// build it first with `node build` there). In gen3ou each team goes through
// Showdown's validator, as in real play: it sets the IVs a Hidden Power type
// needs. Teams it rejects are skipped (9 of 327 on 0.11.11, Quick Claw among
// them, which Showdown's gen3 doesn't log), and so are teams whose
// nicknames Showdown's importer reads as species. `--format custom` (the truly
// random environment) plays CUSTOM and takes teams of 1-6 as given.
//
// Each game is a "game" line, then one "step" line per update:
//
//   {"type":"game","game":G,"seed":[4 x u16],"choice_seed":[4 x u16],
//    "p1":{"file":"...","team":I,"text":"...","sets":[...]},"p2":{...}}
//   {"type":"step","turn":T,                the turn after the step
//    "choices":{"p1":[...],"p2":[...]},   legal choices before the step
//    "forced":{"p1":B,"p2":B},             the move is locked (or Struggle)
//    "chosen":{"p1":"...","p2":"..."},
//    "log":[...],                           the raw lines the step wrote,
//                                           |split| triplets included,
//                                           |t:| timestamps dropped
//    "state":{"p1":[[species,hp,maxhp,status,[pp...],item],...],"p2":[...],
//             "p1active":{boosts,volatiles,subhp,stall,moves},"p2active":...,
//             "p1side":{spikes,reflect,lightscreen,safeguard,mist},"p2side":...,
//             "weather":ID},
//    "result":null | "p1" | "p2" | "tie"}
//
// `file` is the team's file as given, `team` its index there and `text`
// its export block, which
// both sides parse. `sets` is what Showdown made of each set, in team order:
// {"species","gender","ability","name","shiny","stats":[hp,atk,def,spa,spd,spe]},
// so the engine can take Showdown's random genders, check its own stats and
// give the log formatter each Pokémon's nickname. Choices are Showdown's strings: "move N", "switch N"
// (N is the position in Showdown's current order, which is our SLOT) or
// "pass". The legal set is every candidate side.choose() accepts, which
// checks the true state (a disabled move, a real trap). The first step is
// the battle start: both sides pass, and its log is what setPlayer wrote.
// `state` lists each side's Pokémon in Showdown's current order after the
// step (status '' when healthy, 'fnt' at 0 HP: Showdown's own status drops
// 'fnt' once a fainted Pokémon leaves the field) with its moves' PP and its
// item ('' for none), and
// each active Pokémon's boosts, volatiles and move slots, for diagnosis.
'use strict';
const fs = require('fs');
const path = require('path');

function parseArgs(argv) {
  const opts = {
    games: 100,
    seed: '1',
    format: 'gen3ou',
    showdown: path.join(__dirname, '../showdown'),
    files: [],
  };
  for (let i = 0; i < argv.length; i++) {
    if (!argv[i].startsWith('--')) {
      opts.files.push(argv[i]);
      continue;
    }
    const key = argv[i].slice(2);
    if (!(key in opts) || key === 'files' || i + 1 >= argv.length) {
      console.error('usage: showdown_traces.cjs [--games N] [--seed N] [--format gen3ou|custom] [--showdown PATH] [TEAM_FILE...]');
      process.exit(1);
    }
    // The seed stays a string: a u64 doesn't fit a double.
    opts[key] = key === 'games' ? +argv[++i] : argv[++i];
  }
  if (opts.format !== 'gen3ou' && opts.format !== 'custom') {
    console.error(`showdown_traces.cjs: unknown format ${opts.format}`);
    process.exit(1);
  }
  if (!opts.files.length) {
    const dir = path.join(__dirname, '../teams/ou');
    opts.files = fs.readdirSync(dir).filter(f => f.endsWith('.txt'))
      .sort().map(f => path.join(dir, f));
  }
  return opts;
}

// gen3customgame without Smogon's clauses, which the engine always applies
// (src/gen3/README.md, "Turn frame").
const CUSTOM = 'gen3customgame@@@Sleep Clause Mod,Freeze Clause Mod';

function loadTeams(files, format, Dex, Teams, TeamValidator) {
  const species = Dex.mod('gen3').species;
  const validator = format === 'gen3ou' ? TeamValidator.get(format) : null;
  const teams = [];
  for (const file of files) {
    // Text before the first header (gen_teams.cjs's comment) isn't a team.
    const blocks = fs.readFileSync(file, 'utf8').split(/^=== .*===$/m).slice(1)
      .map(s => s.trim()).filter(Boolean);
    blocks.forEach((text, index) => {
      const team = Teams.import(text);
      if (!team || !team.length || team.length > 6 ||
        !team.every(p => species.get(p.species).exists)) return;
      if (validator && validator.validateTeam(team)) return;
      teams.push({file, index, text, packed: Teams.pack(team)});
    });
  }
  return teams;
}

// The candidate choices for one side's request, before checking them.
function candidates(side) {
  const req = side.activeRequest;
  if (!req || req.wait) return {list: ['pass'], forced: false};
  const n = side.pokemon.length;
  const switches = [];
  for (let i = 2; i <= n; i++) switches.push(`switch ${i}`);
  if (req.forceSwitch) return {list: switches, forced: false};
  const moves = req.active[0].moves;
  // A locked move (charging, a rampage, recharge, Bide) or Struggle is the
  // request's only move, and only a locked move has no PP field.
  const forced = moves.length === 1 && (moves[0].pp === undefined || moves[0].id === 'struggle');
  return {list: moves.map((_, i) => `move ${i + 1}`).concat(switches), forced};
}

function legal(side) {
  const {list, forced} = candidates(side);
  if (list[0] === 'pass') return {list, forced};
  const ok = [];
  for (const c of list) {
    side.clearChoice();
    if (side.choose(c)) ok.push(c);
  }
  side.clearChoice();
  return {list: ok, forced};
}

function sets(side) {
  return side.pokemon.map(p => ({
    species: p.species.id, gender: p.gender, ability: p.baseAbility,
    name: p.name, shiny: !!p.set.shiny,
    stats: [p.maxhp, p.storedStats.atk, p.storedStats.def, p.storedStats.spa,
      p.storedStats.spd, p.storedStats.spe],
  }));
}

function state(battle) {
  const out = {};
  for (const side of battle.sides) {
    out[side.id] = side.pokemon.map(p => [p.species.id, p.hp, p.maxhp, p.hp ? p.status || '' : 'fnt',
      p.baseMoveSlots.map(m => m.pp), p.item]);
    const sc = side.sideConditions;
    out[side.id + 'side'] = {
      spikes: sc.spikes ? sc.spikes.layers : 0, reflect: !!sc.reflect,
      lightscreen: !!sc.lightscreen, safeguard: !!sc.safeguard, mist: !!sc.mist,
    };
    // A future attack on this side: its turns left and its stored damage,
    // which the log shows only when it lands.
    const fm = side.slotConditions[0] && side.slotConditions[0].futuremove;
    out[side.id + 'side'].future = fm ? [fm.duration, fm.moveData.damage] : [0, 0];
    const a = side.active[0];
    out[side.id + 'active'] = a ? {
      boosts: a.boosts, volatiles: Object.keys(a.volatiles),
      subhp: a.volatiles.substitute ? a.volatiles.substitute.hp : 0,
      stall: a.volatiles.stall ? a.volatiles.stall.duration : 0,
      moves: a.moveSlots.map(m => [m.id, m.pp, !!m.disabled]),
    } : null;
  }
  out.weather = battle.field.weather;
  return out;
}

function result(battle) {
  if (!battle.ended) return null;
  return battle.winner === 'p1' ? 'p1' : battle.winner === 'p2' ? 'p2' : 'tie';
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const {Battle, Dex, PRNG, Teams, TeamValidator} = require(path.resolve(opts.showdown));
  const teams = loadTeams(opts.files, opts.format, Dex, Teams, TeamValidator);
  console.error({teams: teams.length});
  // A 64-bit seed, one u16 per PRNG seed word (below 2^32, the old [0, 0, hi, lo]).
  const seed64 = BigInt.asUintN(64, BigInt(opts.seed));
  const master = new PRNG([48n, 32n, 16n, 0n].map(k => Number(seed64 >> k & 0xffffn)));
  const newSeed = () => [0, 0, 0, 0].map(() => master.random(65536));
  const out = [];
  const flush = () => { if (out.length) process.stdout.write(out.join('\n') + '\n'); out.length = 0; };
  const counts = {games: 0, steps: 0, p1: 0, p2: 0, tie: 0, unfinished: 0};

  for (let g = 0; g < opts.games; g++) {
    const seed = newSeed();
    const choiceSeed = newSeed();
    const prng = new PRNG(choiceSeed);
    const t1 = prng.sample(teams);
    const t2 = prng.sample(teams);
    const battle = new Battle({formatid: opts.format === 'custom' ? CUSTOM : 'gen3ou', seed});
    battle.setPlayer('p1', {name: 'p1', team: t1.packed});
    battle.setPlayer('p2', {name: 'p2', team: t2.packed});
    out.push(JSON.stringify({
      type: 'game', game: g, seed, choice_seed: choiceSeed,
      p1: {file: t1.file, team: t1.index, text: t1.text, sets: sets(battle.p1)},
      p2: {file: t2.file, team: t2.index, text: t2.text, sets: sets(battle.p2)},
    }));
    let logged = 0;
    const step = (choices, forced, chosen) => {
      out.push(JSON.stringify({
        type: 'step', turn: battle.turn, choices, forced, chosen,
        log: battle.log.slice(logged).filter(l => !l.startsWith('|t:|')), state: state(battle), result: result(battle),
      }));
      logged = battle.log.length;
      counts.steps++;
    };
    step({p1: ['pass'], p2: ['pass']}, {p1: false, p2: false}, {p1: 'pass', p2: 'pass'});

    // Each step is one update; Showdown ties at turn 1000 as the engine does,
    // so the bound only guards against a stuck battle.
    for (let k = 0; k < 4000 && !battle.ended; k++) {
      const choices = {}, forced = {}, chosen = {};
      for (const side of battle.sides) {
        const l = legal(side);
        if (!l.list.length) throw new Error(`game ${g}: no legal choice for ${side.id}`);
        choices[side.id] = l.list;
        forced[side.id] = l.forced;
        chosen[side.id] = prng.sample(l.list);
      }
      for (const side of battle.sides) {
        if (chosen[side.id] === 'pass') continue;
        if (!side.choose(chosen[side.id])) {
          throw new Error(`game ${g}: ${side.id} rejected ${chosen[side.id]}: ${side.choice.error}`);
        }
      }
      if (!battle.allChoicesDone()) throw new Error(`game ${g}: choices not done`);
      battle.commitChoices();
      step(choices, forced, chosen);
    }
    counts.games++;
    counts[result(battle) || 'unfinished']++;
    flush();
  }
  console.error(counts);
}

main();
