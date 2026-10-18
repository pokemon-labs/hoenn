#!/usr/bin/env node
// Writes seeded random teams as Showdown export text, for the random test
// environments (docs/TESTING.md, "Generators").
//
//   node tools/gen_teams.cjs --mode ou-legal|random [--teams N] [--seed N]
//       [--showdown PATH] > teams.txt
//
// Defaults: 2000 teams, seed 1, showdown/. The file starts with a comment
// (mode, seed, Showdown commit), which both team parsers skip, then one
// "=== [FORMAT] Random N ===" block per team (gen3ou, or gen3customgame).
//
// ou-legal: per slot, a species uniform over those gen3ou allows (distinct
// within the team); an ability uniform over its allowed ones; up to 4
// distinct moves uniform over what it can learn and gen3ou allows; an item
// uniform over ITEMS, or none 10% of the time; IVs uniform 0-31; 508 EVs in
// random multiples of 4, at most 252 per stat; a nature uniform; level 100.
// Bans are filtered before drawing, so a species isn't thinned by them; a
// set gen3ou's validateSet still rejects (egg and event combinations, the
// OHKO clause) redraws its moves, then the team goes through validateTeam
// (the Baton Pass clauses) and is redrawn if rejected.
//
// random: no legality at all, only what both sims can name. 1-6 Pokémon,
// each drawn independently: a species (repeats allowed), an ability, 1-4
// distinct moves and an item (or none 10% of the time), each uniform over
// every gen 3 one the engine names; level 1-100; IVs 0-31, EVs 0-255 with
// no total; a nature; happiness 0-255; a gender of M, F or unstated.
'use strict';
const fs = require('fs');
const path = require('path');
const {execFileSync} = require('child_process');

const NO_ITEM = 0.1;
const MOVE_TRIES = 100;
const TEAM_TRIES = 1000;

function parseArgs(argv) {
  const opts = {mode: '', teams: 2000, seed: '1', showdown: path.join(__dirname, '../showdown')};
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i].startsWith('--') ? argv[i].slice(2) : '';
    if (!(key in opts) || i + 1 >= argv.length) usage();
    opts[key] = key === 'teams' ? +argv[++i] : argv[++i];
  }
  if (opts.mode !== 'ou-legal' && opts.mode !== 'random') usage();
  return opts;
}

function usage() {
  console.error('usage: gen_teams.cjs --mode ou-legal|random [--teams N] [--seed N] [--showdown PATH]');
  process.exit(1);
}

// The engine's items with an effect: names.hpp maps every other decomp item
// to Mail or Other, which the log can't name yet. `all` keeps those too.
function engineItems(all = false) {
  const text = fs.readFileSync(path.join(__dirname, '../src/data/names.hpp'), 'utf8');
  const ids = new Set();
  for (const [, id, item] of text.matchAll(/\{"([a-z0-9]+)", Item::(\w+)\}/g)) {
    if (item !== 'None' && (all || !['Mail', 'Other'].includes(item))) ids.add(id);
  }
  if (!ids.size) throw new Error('no items found in names.hpp');
  return ids;
}

// The ids in one of names.hpp's string arrays (SPECIES_IDS, MOVE_IDS...).
function engineIds(table) {
  const text = fs.readFileSync(path.join(__dirname, '../src/data/names.hpp'), 'utf8');
  const start = text.indexOf(`${table}[`);
  const body = text.slice(start, text.indexOf('};', start));
  const ids = new Set([...body.matchAll(/"([a-z0-9]+)",/g)].map(m => m[1]));
  if (!ids.size) throw new Error(`no ${table} found in names.hpp`);
  return ids;
}

// Showdown spells four moves differently from the decomp (showdown.hpp's
// MOVE_ALIASES).
const MOVE_ALIASES = {highjumpkick: 'hijumpkick', feintattack: 'faintattack',
  smellingsalts: 'smellingsalt', visegrip: 'vicegrip'};

function commitOf(dir) {
  try {
    return execFileSync('git', ['-C', dir, 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim();
  } catch {
    return 'unknown';
  }
}

function main() {
  const opts = parseArgs(process.argv.slice(2));
  const sd = path.resolve(opts.showdown);
  const {PRNG} = require(sd);
  const seed64 = BigInt.asUintN(64, BigInt(opts.seed));
  const prng = new PRNG([48n, 32n, 16n, 0n].map(k => Number(seed64 >> k & 0xffffn)));
  // out[0] is a summary for the file's comment.
  const {out, counts} = (opts.mode === 'random' ? random : ouLegal)(opts, require(sd), prng);
  out[0] = [`// gen_teams.cjs --mode ${opts.mode} --teams ${opts.teams} --seed ${seed64}`,
    `// Showdown ${commitOf(sd)}; ${out[0]}`, ''].join('\n');
  process.stdout.write(out.join('\n') + '\n');
  console.error(counts);
}

function random(opts, {Dex, Teams}, prng) {
  const dex = Dex.mod('gen3');
  const gen3 = x => x.exists && !x.isNonstandard && x.gen <= 3;
  const speciesIds = engineIds('SPECIES_IDS');
  const moveIds = engineIds('MOVE_IDS');
  const abilityIds = engineIds('ABILITY_IDS');
  const itemIds = engineItems(true);
  // Castform's formes are battle-only; Hidden Power is written bare and takes
  // its type from the IVs.
  const species = dex.species.all().filter(s => gen3(s) && speciesIds.has(s.id)).map(s => s.name);
  const abilities = dex.abilities.all().filter(a => gen3(a) && abilityIds.has(a.id)).map(a => a.name);
  const moves = dex.moves.all().filter(m => gen3(m) && m.id !== 'struggle' &&
    !m.name.startsWith('Hidden Power ') && moveIds.has(MOVE_ALIASES[m.id] || m.id)).map(m => m.name);
  const items = dex.items.all().filter(i => gen3(i) && itemIds.has(i.id)).map(i => i.name);
  const natures = dex.natures.all().map(n => n.name);
  const stats = ['hp', 'atk', 'def', 'spa', 'spd', 'spe'];

  const drawSet = () => {
    const ivs = {}, evs = {};
    for (const s of stats) { ivs[s] = prng.random(32); evs[s] = prng.random(256); }
    const left = moves.slice();
    const set = {
      name: '', species: prng.sample(species), level: prng.random(1, 101),
      gender: prng.sample(['M', 'F', '']), ability: prng.sample(abilities),
      item: prng.random() < NO_ITEM ? '' : prng.sample(items),
      nature: prng.sample(natures), ivs, evs, moves: [],
    };
    for (let n = prng.random(1, 5); set.moves.length < n;) {
      set.moves.push(left.splice(prng.random(left.length), 1)[0]);
    }
    // Export leaves out happiness 255, and the two parsers default it
    // differently when the set has Frustration (showdown.hpp), so 255 isn't
    // drawn with Frustration.
    const top = set.moves.includes('Frustration') ? 255 : 256;
    set.happiness = prng.random(top);
    return set;
  };

  const out = [`${species.length} species, ${abilities.length} abilities, ` +
    `${moves.length} moves, ${items.length} items`];
  for (let n = 0; n < opts.teams; n++) {
    const team = [];
    for (let k = prng.random(1, 7); team.length < k;) team.push(drawSet());
    out.push(`=== [gen3customgame] Random ${n + 1} ===`, '', Teams.export(team).trim(), '');
  }
  return {out, counts: {teams: opts.teams}};
}

function ouLegal(opts, {Teams, TeamValidator}, prng) {
  const validator = TeamValidator.get('gen3ou');
  const dex = validator.dex;
  const rules = validator.ruleTable;

  const engine = engineItems();
  const items = dex.items.all()
    .filter(i => i.exists && !i.isNonstandard && i.gen <= 3 && engine.has(i.id) &&
      !rules.has(`-item:${i.id}`))
    .map(i => i.name);
  // Hidden Power is written bare and takes its type from the IVs; the typed
  // entries ("Hidden Power Bug") share its id.
  const moves = dex.moves.all().filter(m => m.exists && !m.isNonstandard && m.gen <= 3 &&
    m.id !== 'struggle' && !m.name.startsWith('Hidden Power ') &&
    !rules.has(`-move:${m.id}`));
  const pool = [];
  for (const s of dex.species.all()) {
    if (!s.exists || s.isNonstandard || s.gen > 3 || s.battleOnly || rules.isBannedSpecies(s)) continue;
    const abilities = [...new Set(Object.values(s.abilities))]
      .filter(a => !rules.has(`-ability:${dex.toID(a)}`));
    if (!abilities.length) continue;
    const set = {species: s.name, level: 100};
    const learnable = moves.filter(m => !validator.checkCanLearn(m, s, validator.allSources(s), set))
      .map(m => m.name);
    pool.push({name: s.name, abilities, learnable});
  }

  const stats = ['hp', 'atk', 'def', 'spa', 'spd', 'spe'];
  const natures = dex.natures.all().map(n => n.name);

  const drawSet = species => {
    const ivs = {}, evs = {};
    for (const s of stats) { ivs[s] = prng.random(32); evs[s] = 0; }
    for (let units = 0; units < 127;) {
      const s = prng.sample(stats);
      if (evs[s] < 252) { evs[s] += 4; units++; }
    }
    return {
      name: '', species: species.name, gender: '', level: 100,
      ability: prng.sample(species.abilities),
      item: prng.random() < NO_ITEM ? '' : prng.sample(items),
      nature: prng.sample(natures), ivs, evs, moves: [],
    };
  };
  const drawMoves = (set, species) => {
    const left = species.learnable.slice();
    set.moves = [];
    while (set.moves.length < 4 && left.length) {
      set.moves.push(left.splice(prng.random(left.length), 1)[0]);
    }
    if (set.moves.includes('Frustration')) set.happiness = 0;
    else delete set.happiness;
  };
  const drawSlot = species => {
    for (;;) {
      const set = drawSet(species);
      for (let t = 0; t < MOVE_TRIES; t++) {
        drawMoves(set, species);
        // validateSet edits the set it checks (it adds hpType).
        const checked = structuredClone(set);
        if (!validator.validateSet(checked, {})) return set;
      }
    }
  };

  const out = [`${pool.length} species, ${items.length} items`];
  const counts = {teams: 0, teamRedraws: 0};
  for (let n = 0; n < opts.teams; n++) {
    let team = null;
    for (let t = 0; t < TEAM_TRIES && !team; t++) {
      const species = [];
      const left = pool.slice();
      while (species.length < 6) species.push(left.splice(prng.random(left.length), 1)[0]);
      const sets = species.map(drawSlot);
      if (!validator.validateTeam(structuredClone(sets))) team = sets;
      else counts.teamRedraws++;
    }
    if (!team) throw new Error(`team ${n}: no valid team in ${TEAM_TRIES} tries`);
    out.push(`=== [gen3ou] Random ${n + 1} ===`, '', Teams.export(team).trim(), '');
    counts.teams++;
  }
  return {out, counts};
}

main();
