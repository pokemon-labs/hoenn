#!/usr/bin/env node
// Surveys the protocol messages Pokémon Showdown writes in gen3 singles, for
// the opcode inventory in src/README.md, "Protocol". Two modes:
//
//   node tools/protocol_survey.cjs static [path/to/pokemon-showdown]
//   node tools/protocol_survey.cjs games FORMAT N [--showdown PATH] [TEAM_FILE...]
//
// `static` lists every add(...) call (and addMove, addSplit, hint,
// attrLastMove) in the handlers of the gen3 moves, abilities, items and
// conditions, the clause rules, the merged gen3 scripts, and the sim core
// files, grouped by message type. It sees literal messages only: the
// `[from]` sources that damage(), heal() and boost() fill in at runtime
// don't show, and neither do the later-gen and doubles branches' guards, so
// some lines are unreachable in gen3 singles.
//
// `games` plays N random games of FORMAT (gen3ou over the teams of every
// TEAM_FILE, pooled, by default every teams/ou/*.txt; or
// gen3randombattle with generated teams) with random legal choices, and
// prints each distinct message shape with its count. Shapes replace
// Pokémon, HP, numbers, moves and most item names with placeholders and keep
// the effect names. Random play reaches what's common, not everything.
//
// The default path is `showdown/` (0.11.11), the copy the realizability test
// pins; build it first (`node build` there). The sim core files are read
// from the package's dist/sim/; another package layout (@pkmn/sim) needs
// that path changed.
'use strict';
const fs = require('fs');
const path = require('path');

const [mode, ...rest] = process.argv.slice(2);
const DEFAULT_PS = path.join(__dirname, '../showdown');

function scanStatic(PS) {
  const {Dex} = require(PS);
  const dex = Dex.mod('gen3');
  const out = new Map(); // message type -> Map(source -> Set(arguments))
  const note = (type, src, args) => {
    if (!out.has(type)) out.set(type, new Map());
    const m = out.get(type);
    if (!m.has(src)) m.set(src, new Set());
    m.get(src).add(args.replace(/\s+/g, ' ').slice(0, 160));
  };
  const scanText = (text, src) => {
    for (const line of text.split('\n')) {
      const re = /\.(add|addMove|addSplit|hint|attrLastMove)\(([^\n]*)/g;
      let m;
      while ((m = re.exec(line))) {
        const [, fn, args] = m;
        if (fn === 'add' || fn === 'addMove') {
          const t = /^\s*(['"`])([^'"`]*)\1/.exec(args);
          note(t ? t[2] : '<dynamic>', src, args);
        } else {
          note(`<${fn}>`, src, args);
        }
      }
    }
  };
  const NESTED = ['condition', 'secondary', 'secondaries', 'self'];
  const scanEffect = (obj, src, seen = new Set()) => {
    if (!obj || typeof obj !== 'object' || seen.has(obj)) return;
    seen.add(obj);
    for (const k in obj) {
      const v = obj[k];
      if (typeof v === 'function') scanText(v.toString(), `${src}.${k}`);
      else if (v && typeof v === 'object' && (NESTED.includes(k) || !isNaN(k))) {
        scanEffect(v, `${src}.${k}`, seen);
      }
    }
  };
  const gen3 = e => e.exists && !e.isNonstandard && e.gen > 0 && e.gen <= 3;
  for (const m of dex.moves.all()) if (gen3(m)) scanEffect(m, `move:${m.id}`);
  for (const a of dex.abilities.all()) if (gen3(a)) scanEffect(a, `ability:${a.id}`);
  for (const i of dex.items.all()) if (gen3(i)) scanEffect(i, `item:${i.id}`);
  for (const id of Object.keys(dex.data.Conditions)) {
    scanEffect(dex.conditions.get(id), `cond:${id}`);
  }
  for (const id of ['sleepclausemod', 'freezeclausemod', 'endlessbattleclause',
    'hppercentagemod', 'speciesclause', 'evasionmovesclause', 'ohkoclause']) {
    scanEffect(dex.formats.get(id), `rule:${id}`);
  }
  const scripts = dex.data.Scripts;
  for (const sec of ['battle', 'pokemon', 'actions', 'side', 'field', 'queue']) {
    for (const k in scripts[sec] || {}) {
      if (typeof scripts[sec][k] === 'function') {
        scanText(scripts[sec][k].toString(), `gen3scripts.${sec}.${k}`);
      }
    }
  }
  for (const f of ['battle', 'battle-actions', 'pokemon', 'side', 'field', 'battle-queue']) {
    const lines = fs.readFileSync(path.join(PS, 'dist/sim', `${f}.js`), 'utf8').split('\n');
    lines.forEach((l, i) => scanText(l, `sim/${f}:${i + 1}`));
  }
  for (const t of [...out.keys()].sort()) {
    console.log(`== ${t} (${out.get(t).size})`);
    for (const [src, args] of out.get(t)) console.log(`  ${src} :: ${[...args].join(' || ')}`);
  }
}

// Messages kept as their type alone (libpkmn's FILTER and the bookkeeping).
const BARE = new Set(['', 't:', 'gametype', 'player', 'teamsize', 'gen', 'tier', 'rule',
  'start', 'upkeep', 'j', 'debug', 'split', 'turn', 'win', 'tie', 'teampreview', 'poke',
  'clearpoke', 'rated', 'message', '-message', '-hint', 'bigerror', 'inactive']);
// Items named in shapes; others become ITEM.
const NAMED_ITEM = /Berry|Herb|Leftovers|Focus Band/;

function shape(line) {
  const p = line.split('|').slice(1);
  const t = p[0];
  if (BARE.has(t)) return t;
  const out = [t];
  for (let i = 1; i < p.length; i++) {
    let a = p[i];
    if (/^p[12][a-z]?: /.test(a)) a = 'P';
    else if (/^p[12]$/.test(a)) a = 'S';
    else if (/^\d+\/\d+( \w+)?$/.test(a) || /^\d+ fnt$/.test(a)) a = 'HP';
    else if (/^-?\d+$/.test(a)) a = 'N';
    else if (['switch', 'drag', 'replace'].includes(t) && i === 2) a = 'DETAILS';
    else if (t === 'move' && i === 2) a = 'MOVE';
    else if (t === 'cant' && i === 3 && !a.startsWith('[')) a = 'MOVE';
    else if (t === '-activate' && i === 3 && /Mimic|Spite|Leppa/.test(p[2])) a = 'MOVE';
    else if (t === '-start' && i === 3 && p[2] === 'Disable') a = 'MOVE';
    else if (t === '-start' && i === 3 && p[2] === 'typechange') a = 'TYPE';
    else if ((t === '-enditem' || t === '-item') && i === 2 && !NAMED_ITEM.test(a)) a = 'ITEM';
    else if (a.startsWith('[of] ')) a = '[of] P';
    else if (a.startsWith('[wisher] ')) a = '[wisher] X';
    out.push(a);
  }
  return out.join('|');
}

function playGames(format, games, PS, files) {
  const {Battle, Dex, PRNG, Teams} = require(PS);
  let teams = null;
  if (format === 'gen3ou') {
    if (!files.length) {
      const dir = path.join(__dirname, '../teams/ou');
      files = fs.readdirSync(dir).filter(f => f.endsWith('.txt'))
        .sort().map(f => path.join(dir, f));
    }
    const species = Dex.mod('gen3').species;
    teams = files.flatMap(file => fs.readFileSync(file, 'utf8').split(/^=== .*===$/m))
      .map(s => s.trim()).filter(Boolean).map(s => Teams.import(s))
      // Skips teams whose nicknames Showdown's importer reads as species.
      .filter(t => t && t.length === 6 && t.every(p => species.get(p.species).exists));
  }
  const prng = new PRNG([1, 2, 3, 4]);
  const alive = (req, i) => i > 0 && !req.side.pokemon[i].condition.endsWith(' fnt');
  const options = req => {
    const opts = [];
    if (req.forceSwitch) {
      req.side.pokemon.forEach((_, i) => { if (alive(req, i)) opts.push(`switch ${i + 1}`); });
      if (!opts.length) opts.push('pass');
    } else if (req.active) {
      const a = req.active[0];
      a.moves.forEach((m, i) => { if (!m.disabled) opts.push(`move ${i + 1}`); });
      if (!a.trapped) {
        req.side.pokemon.forEach((_, i) => { if (alive(req, i)) opts.push(`switch ${i + 1}`); });
      }
      if (!opts.length) opts.push('move 1');
    }
    return opts;
  };
  const seen = new Map();
  let turns = 0;
  for (let g = 0; g < games; g++) {
    const seed = [0, 0, 0, 0].map(() => prng.random(65536));
    const b = new Battle({formatid: format, seed});
    for (const id of ['p1', 'p2']) {
      b.setPlayer(id, teams ? {name: id, team: Teams.pack(prng.sample(teams))} : {name: id});
    }
    for (let k = 0; k < 2000 && !b.ended; k++) {
      for (const side of b.sides) {
        const req = side.activeRequest;
        if (!req || req.wait || side.isChoiceDone()) continue;
        const opts = options(req);
        // Random choices can be invalid (a disabled move Showdown still lists).
        for (let tries = 0; tries < 20 && !side.choose(prng.sample(opts)); tries++);
      }
      if (b.allChoicesDone()) b.commitChoices();
    }
    turns += b.turn;
    for (const l of b.log) {
      const s = shape(l);
      seen.set(s, (seen.get(s) || 0) + 1);
    }
  }
  console.error({format, games, turns, teams: teams ? teams.length : 'generated'});
  for (const [s, n] of [...seen].sort()) console.log(`${String(n).padStart(8)}  ${s}`);
}

if (mode === 'static') {
  scanStatic(path.resolve(rest[0] || DEFAULT_PS));
} else if (mode === 'games' && rest.length >= 2) {
  const [format, n, ...args] = rest;
  let ps = DEFAULT_PS;
  const files = [];
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--showdown' && i + 1 < args.length) ps = args[++i];
    else files.push(args[i]);
  }
  playGames(format, +n, path.resolve(ps), files);
} else {
  console.error('usage: protocol_survey.cjs static [ps] | games FORMAT N [--showdown ps] [TEAM_FILE...]');
  process.exit(1);
}
