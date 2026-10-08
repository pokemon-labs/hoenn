#!/usr/bin/env node
// Plays seeded gen3 OU games in Pokémon Showdown with random legal choices
// and writes them as JSONL traces, for the realizability test (CLAUDE.md,
// "Testing"). Showdown picks every choice, so nothing here needs the engine;
// check_showdown replays the traces against it.
//
//   node tools/showdown_traces.cjs [--games N] [--seed N]
//       [--teams FILE] [--showdown PATH] > traces.jsonl
//
// Defaults: 100 games, seed 1, teams/adv/all.txt, showdown/ (0.11.11; build
// it first with `node build` there). Each team goes through Showdown's
// gen3ou validator, as in real play: it sets the IVs a Hidden Power type
// needs. Teams it rejects are skipped (9 of 327 on 0.11.11, Quick Claw among
// them, which Showdown's gen3 doesn't log), and so are teams whose
// nicknames Showdown's importer reads as species.
//
// Each game is a "game" line, then one "step" line per update:
//
//   {"type":"game","game":G,"seed":[4 x u16],"choice_seed":[4 x u16],
//    "p1":{"team":I,"text":"...","sets":[...]},"p2":{...}}
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
// `team` is the team's index in FILE and `text` its export block, which
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
    seed: 1,
    teams: path.join(__dirname, '../../teams/adv/all.txt'),
    showdown: path.join(__dirname, '../showdown'),
  };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i].replace(/^--/, '');
    if (!(key in opts) || i + 1 >= argv.length) {
      console.error('usage: showdown_traces.cjs [--games N] [--seed N] [--teams FILE] [--showdown PATH]');
      process.exit(1);
    }
    opts[key] = typeof opts[key] === 'number' ? +argv[++i] : argv[++i];
  }
  return opts;
}

function loadTeams(file, Dex, Teams, TeamValidator) {
  const species = Dex.mod('gen3').species;
  const validator = TeamValidator.get('gen3ou');
  const blocks = fs.readFileSync(file, 'utf8').split(/^=== .*===$/m)
    .map(s => s.trim()).filter(Boolean);
  const teams = [];
  blocks.forEach((text, index) => {
    const team = Teams.import(text);
    if (!team || team.length !== 6 || !team.every(p => species.get(p.species).exists)) return;
    if (validator.validateTeam(team)) return;
    teams.push({index, text, packed: Teams.pack(team)});
  });
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
  const teams = loadTeams(opts.teams, Dex, Teams, TeamValidator);
  console.error({teams: teams.length});
  const master = new PRNG([0, 0, opts.seed >>> 16 & 0xffff, opts.seed & 0xffff]);
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
    const battle = new Battle({formatid: 'gen3ou', seed});
    battle.setPlayer('p1', {name: 'p1', team: t1.packed});
    battle.setPlayer('p2', {name: 'p2', team: t2.packed});
    out.push(JSON.stringify({
      type: 'game', game: g, seed, choice_seed: choiceSeed,
      p1: {team: t1.index, text: t1.text, sets: sets(battle.p1)},
      p2: {team: t2.index, text: t2.text, sets: sets(battle.p2)},
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
