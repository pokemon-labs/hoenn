#!/usr/bin/env node
// Writes tools/damage_cases.inc: expected damage for every roll, with and
// without a crit, from Pokémon Showdown 0.11.10's gen3 getDamage. The
// fixture is committed; tools/check_damage.cpp compares damage.hpp with it.
//
//   node tools/damage_oracle.cjs [path/to/pokemon-showdown] > tools/damage_cases.inc
//
// The default path is ADV-Simulator's pinned copy. Each case's attacker uses
// its first move (or Struggle). Sets are level 100; Hidden Power sets give
// their IVs explicitly, since Showdown's import doesn't derive them.
'use strict';
const path = require('path');
const PS = process.argv[2] ||
  path.join(__dirname, '../../ADV-Simulator/tools/showdown-oracle/node_modules/pokemon-showdown');
const {Battle, Teams} = require(PS);

const set = (species, item, ability, nature, evs, move, ivs) =>
  `${species} @ ${item}\nAbility: ${ability}\nEVs: ${evs}\n${nature} Nature\n` +
  (ivs ? `IVs: ${ivs}\n` : '') + `- ${move}`;
const PHYS = '252 Atk / 4 SpD / 252 Spe', SPEC = '252 SpA / 4 SpD / 252 Spe';
const WALL = '252 HP / 252 Def / 4 SpD', SWALL = '252 HP / 4 Def / 252 SpD';

const S = {
  ttar: set('Tyranitar', 'Choice Band', 'Sand Stream', 'Adamant', PHYS, 'Rock Slide'),
  ttarW: set('Tyranitar', 'Leftovers', 'Sand Stream', 'Careful', SWALL, 'Rock Slide'),
  skarm: set('Skarmory', 'Leftovers', 'Keen Eye', 'Impish', SWALL, 'Drill Peck'),
  bliss: set('Blissey', 'Leftovers', 'Natural Cure', 'Bold', WALL, 'Seismic Toss'),
  meta: set('Metagross', 'Choice Band', 'Clear Body', 'Adamant', PHYS, 'Meteor Mash'),
  metaX: set('Metagross', 'Leftovers', 'Clear Body', 'Adamant', PHYS, 'Explosion'),
  zapdos: set('Zapdos', 'Leftovers', 'Pressure', 'Modest', SPEC, 'Thunderbolt'),
  zapdosHP: set('Zapdos', 'Leftovers', 'Pressure', 'Modest', SPEC, 'Hidden Power [Electric]', '30 SpA'),
  starmie: set('Starmie', 'Leftovers', 'Natural Cure', 'Timid', SPEC, 'Surf'),
  starmieSI: set('Starmie', 'Sea Incense', 'Natural Cure', 'Timid', SPEC, 'Surf'),
  suicune: set('Suicune', 'Mystic Water', 'Pressure', 'Modest', SPEC, 'Surf'),
  swampert: set('Swampert', 'Leftovers', 'Torrent', 'Adamant', PHYS, 'Earthquake'),
  swampertS: set('Swampert', 'Leftovers', 'Torrent', 'Modest', SPEC, 'Surf'),
  hera: set('Heracross', 'Choice Band', 'Guts', 'Jolly', PHYS, 'Megahorn'),
  heraL: set('Heracross', 'Leftovers', 'Guts', 'Jolly', PHYS, 'Megahorn'),
  celebi: set('Celebi', 'Leftovers', 'Natural Cure', 'Modest', SPEC, 'Hidden Power [Fire]', '30 Atk / 30 SpA / 30 Spe'),
  snorlax: set('Snorlax', 'Leftovers', 'Thick Fat', 'Impish', WALL, 'Body Slam'),
  snorlaxA: set('Snorlax', 'Leftovers', 'Immunity', 'Adamant', PHYS, 'Body Slam'),
  snorlaxSS: set('Snorlax', 'Silk Scarf', 'Immunity', 'Adamant', PHYS, 'Double-Edge'),
  snorlaxF: set('Snorlax', 'Leftovers', 'Immunity', 'Adamant', PHYS, 'Facade'),
  charizard: set('Charizard', 'Leftovers', 'Blaze', 'Modest', SPEC, 'Flamethrower'),
  charizardC: set('Charizard', 'Charcoal', 'Blaze', 'Modest', SPEC, 'Fire Blast'),
  mence: set('Salamence', 'Leftovers', 'Intimidate', 'Naive', PHYS, 'Earthquake'),
  jirachi: set('Jirachi', 'Leftovers', 'Serene Grace', 'Modest', SPEC, 'Ice Punch'),
  lapras: set('Lapras', 'Leftovers', 'Water Absorb', 'Modest', SPEC, 'Ice Beam'),
  houndoom: set('Houndoom', 'Leftovers', 'Flash Fire', 'Timid', SPEC, 'Flamethrower'),
  dugtrio: set('Dugtrio', 'Choice Band', 'Arena Trap', 'Jolly', PHYS, 'Earthquake'),
  magneton: set('Magneton', 'Leftovers', 'Magnet Pull', 'Modest', SPEC, 'Thunderbolt'),
  gengar: set('Gengar', 'Leftovers', 'Levitate', 'Timid', SPEC, 'Thunderbolt'),
  medicham: set('Medicham', 'Choice Band', 'Pure Power', 'Jolly', PHYS, 'Brick Break'),
  azumarill: set('Azumarill', 'Leftovers', 'Huge Power', 'Adamant', PHYS, 'Double-Edge'),
  corsola: set('Corsola', 'Leftovers', 'Hustle', 'Adamant', PHYS, 'Rock Slide'),
  marowak: set('Marowak', 'Thick Club', 'Rock Head', 'Adamant', PHYS, 'Earthquake'),
  pikachu: set('Pikachu', 'Light Ball', 'Static', 'Timid', SPEC, 'Thunderbolt'),
  clamperl: set('Clamperl', 'Deep Sea Tooth', 'Shell Armor', 'Modest', SPEC, 'Surf'),
  clamperlD: set('Clamperl', 'Deep Sea Scale', 'Shell Armor', 'Calm', SWALL, 'Surf'),
  ditto: set('Ditto', 'Metal Powder', 'Limber', 'Impish', WALL, 'Transform'),
  latias: set('Latias', 'Soul Dew', 'Levitate', 'Timid', SPEC, 'Psychic'),
  latios: set('Latios', 'Soul Dew', 'Levitate', 'Calm', SWALL, 'Psychic'),
  milotic: set('Milotic', 'Leftovers', 'Marvel Scale', 'Bold', WALL, 'Surf'),
  plusle: set('Plusle', 'Leftovers', 'Plus', 'Modest', SPEC, 'Thunderbolt'),
  minun: set('Minun', 'Leftovers', 'Minus', 'Calm', SWALL, 'Thunderbolt'),
  swellow: set('Swellow', 'Leftovers', 'Guts', 'Jolly', PHYS, 'Facade'),
  venusaur: set('Venusaur', 'Leftovers', 'Overgrow', 'Modest', SPEC, 'Solar Beam'),
  // Not Castform: Forecast would change its type, which isn't damage.hpp's.
  weatherball: set('Jirachi', 'Leftovers', 'Serene Grace', 'Modest', SPEC, 'Weather Ball'),
  golduck: set('Golduck', 'Leftovers', 'Cloud Nine', 'Calm', SWALL, 'Surf'),
  shedinja: set('Shedinja', 'Lum Berry', 'Wonder Guard', 'Adamant', PHYS, 'Shadow Ball'),
  pidgeot: set('Pidgeot', 'Leftovers', 'Keen Eye', 'Jolly', PHYS, 'Gust'),
  tauros: set('Tauros', 'Leftovers', 'Intimidate', 'Jolly', PHYS, 'Stomp'),
  kingdra: set('Kingdra', 'Leftovers', 'Swift Swim', 'Calm', SWALL, 'Surf'),
  ludicolo: set('Ludicolo', 'Leftovers', 'Swift Swim', 'Calm', SWALL, 'Surf'),
};

// [name, attacker, defender, mods]. mods: weather (Showdown id), aStatus,
// dStatus, aBoosts, dBoosts, reflect, lightScreen, aHp (fraction of max),
// aFlags and dFlags (volatiles: flashfire charge mudsport watersport;
// foresight dig dive fly bounce minimize), struggle.
const CASES = [
  ['cb_rockslide_vs_skarm', 'ttar', 'skarm', {}],
  ['cb_rockslide_plus1_vs_skarm', 'ttar', 'skarm', {aBoosts: {atk: 1}}],
  ['cb_meteormash_vs_bliss', 'meta', 'bliss', {}],
  ['cb_meteormash_plus1_vs_bliss', 'meta', 'bliss', {aBoosts: {atk: 1}}],
  ['tbolt_vs_skarm', 'zapdos', 'skarm', {}],
  ['tbolt_vs_skarm_ls', 'zapdos', 'skarm', {lightScreen: true}],
  ['surf_rain_vs_ttar', 'starmie', 'ttarW', {weather: 'raindance'}],
  ['surf_sun_vs_ttar', 'starmie', 'ttarW', {weather: 'sunnyday'}],
  ['eq_burned_vs_meta', 'swampert', 'meta', {aStatus: 'brn'}],
  ['megahorn_cb_guts_vs_celebi', 'hera', 'celebi', {aStatus: 'par'}],
  ['megahorn_guts_vs_celebi', 'heraL', 'celebi', {aStatus: 'par'}],
  ['hpfire_vs_skarm', 'celebi', 'skarm', {}],
  ['explosion_vs_snorlax', 'metaX', 'snorlax', {}],
  ['explosion_vs_snorlax_def_m1', 'metaX', 'snorlax', {dBoosts: {def: -1}}],
  ['flamethrower_thickfat', 'charizard', 'snorlax', {}],
  ['flamethrower_thickfat_sun', 'charizard', 'snorlax', {weather: 'sunnyday'}],
  ['icebeam_thickfat', 'lapras', 'snorlax', {}],
  ['flamethrower_blaze', 'charizard', 'skarm', {aHp: 0.3}],
  ['flamethrower_blaze_thickfat', 'charizard', 'snorlax', {aHp: 0.3}],
  ['flamethrower_flashfire', 'houndoom', 'meta', {aFlags: 'flashfire'}],
  ['flamethrower_flashfire_ls', 'houndoom', 'meta', {aFlags: 'flashfire', lightScreen: true}],
  ['eq_plus2_vs_ttar', 'mence', 'ttarW', {aBoosts: {atk: 2}}],
  ['eq_minus2_vs_ttar', 'mence', 'ttarW', {aBoosts: {atk: -2}}],
  ['bodyslam_curse_vs_skarm', 'snorlaxA', 'skarm', {aBoosts: {atk: 2}, dBoosts: {def: 2}}],
  ['bodyslam_reflect_vs_skarm', 'snorlaxA', 'skarm', {reflect: true}],
  ['icebeam_vs_mence', 'lapras', 'mence', {}],
  ['icepunch_vs_mence', 'jirachi', 'mence', {}],
  ['eq_levitate', 'dugtrio', 'gengar', {}],
  ['tbolt_immune', 'magneton', 'swampert', {}],
  ['surf_torrent_vs_ttar', 'swampertS', 'ttarW', {aHp: 0.2}],
  ['surf_vs_ttar_spd_m2', 'swampertS', 'ttarW', {dBoosts: {spd: -2}}],
  ['cb_purepower_vs_snorlax', 'medicham', 'snorlax', {}],
  ['hugepower_vs_skarm', 'azumarill', 'skarm', {}],
  ['hustle_vs_mence', 'corsola', 'mence', {}],
  ['thickclub_vs_meta', 'marowak', 'meta', {}],
  ['lightball_vs_skarm', 'pikachu', 'skarm', {}],
  ['deepseatooth_vs_ttar', 'clamperl', 'ttarW', {}],
  ['deepseascale_vs_tbolt', 'zapdos', 'clamperlD', {}],
  ['metalpowder_vs_rockslide', 'ttar', 'ditto', {}],
  ['souldew_attacker', 'latias', 'snorlax', {}],
  ['souldew_defender', 'lapras', 'latios', {}],
  ['marvelscale_par', 'snorlaxA', 'milotic', {dStatus: 'par'}],
  ['seaincense_vs_ttar', 'starmieSI', 'ttarW', {}],
  ['mysticwater_vs_ttar', 'suicune', 'ttarW', {}],
  ['charcoal_vs_meta', 'charizardC', 'meta', {}],
  ['silkscarf_vs_bliss', 'snorlaxSS', 'bliss', {}],
  ['plus_vs_minus', 'plusle', 'minun', {}],
  ['charge_tbolt_vs_skarm', 'zapdos', 'skarm', {aFlags: 'charge'}],
  ['charge_hpelectric_vs_skarm', 'zapdosHP', 'skarm', {aFlags: 'charge'}],
  ['facade_guts_burned', 'swellow', 'snorlax', {aStatus: 'brn'}],
  ['facade_burned', 'snorlaxF', 'skarm', {aStatus: 'brn'}],
  ['facade_asleep', 'snorlaxF', 'skarm', {aStatus: 'slp'}],
  ['solarbeam_rain', 'venusaur', 'swampert', {weather: 'raindance'}],
  ['solarbeam_sun', 'venusaur', 'swampert', {weather: 'sunnyday'}],
  ['weatherball_rain', 'weatherball', 'ttarW', {weather: 'raindance'}],
  ['weatherball_sand', 'weatherball', 'zapdos', {weather: 'sandstorm'}],
  ['weatherball_hail', 'weatherball', 'mence', {weather: 'hail'}],
  ['weatherball_none', 'weatherball', 'ttarW', {}],
  ['mudsport_tbolt', 'zapdos', 'skarm', {dFlags: 'mudsport'}],
  ['watersport_flamethrower', 'charizard', 'skarm', {aFlags: 'watersport'}],
  ['cloudnine_rain_surf', 'starmie', 'golduck', {weather: 'raindance'}],
  ['foresight_bodyslam_vs_gengar', 'snorlaxA', 'gengar', {dFlags: 'foresight'}],
  ['bodyslam_vs_gengar', 'snorlaxA', 'gengar', {}],
  ['wonderguard_rockslide', 'ttar', 'shedinja', {}],
  ['wonderguard_eq', 'swampert', 'shedinja', {}],
  ['struggle_vs_gengar', 'snorlaxA', 'gengar', {struggle: true}],
  ['struggle_vs_skarm', 'snorlaxA', 'skarm', {struggle: true}],
  ['gust_vs_fly', 'pidgeot', 'mence', {dFlags: 'fly'}],
  ['gust_vs_bounce', 'pidgeot', 'mence', {dFlags: 'bounce'}],
  ['eq_vs_dig', 'swampert', 'ttarW', {dFlags: 'dig'}],
  ['surf_vs_dive', 'starmie', 'ttarW', {dFlags: 'dive'}],
  ['stomp_vs_minimize', 'tauros', 'bliss', {dFlags: 'minimize'}],
  ['flamethrower_quarter', 'charizard', 'kingdra', {}],
  ['icebeam_super_resisted', 'lapras', 'ludicolo', {}],
];

function run(a, d, mods, crit) {
  const battle = new Battle({formatid: 'gen3customgame', seed: [1, 2, 3, 4]});
  battle.setPlayer('p1', {team: Teams.pack(Teams.import(S[a]))});
  battle.setPlayer('p2', {team: Teams.pack(Teams.import(S[d]))});
  const src = battle.p1.active[0], tgt = battle.p2.active[0];
  // Undo switch-in effects (Intimidate, weather abilities), then set the case.
  battle.field.clearWeather();
  src.clearBoosts(); tgt.clearBoosts();
  if (mods.weather) battle.field.setWeather(mods.weather, src);
  if (mods.aStatus) src.setStatus(mods.aStatus, null, null, true);
  if (mods.dStatus) tgt.setStatus(mods.dStatus, null, null, true);
  if (mods.aBoosts) src.setBoost(mods.aBoosts);
  if (mods.dBoosts) tgt.setBoost(mods.dBoosts);
  if (mods.reflect) battle.p2.addSideCondition('reflect', src);
  if (mods.lightScreen) battle.p2.addSideCondition('lightscreen', src);
  if (mods.aHp) src.hp = Math.floor(src.maxhp * mods.aHp);
  for (const f of (mods.aFlags || '').split(',').filter(Boolean)) src.addVolatile(f);
  for (const f of (mods.dFlags || '').split(',').filter(Boolean)) tgt.addVolatile(f);
  const out = [];
  for (let i = 0; i < 16; i++) {
    const move = battle.dex.getActiveMove(mods.struggle ? 'struggle' : src.moveSlots[0].id);
    // getDamage doesn't run ModifyMove (Hidden Power's and Weather Ball's
    // type, category and power) or TryHit (Wonder Guard).
    if (move.onModifyMove) move.onModifyMove.call(battle, move, src, tgt);
    move.willCrit = crit;
    const ability = tgt.getAbility();
    if (ability.onTryHit && ability.onTryHit.call(battle, tgt, src, move) === null) {
      out.push(0);
      continue;
    }
    const real = battle.random.bind(battle);
    battle.random = (m, n) => (m === 16 && n === undefined ? 15 - i : real(m, n));
    const dmg = battle.actions.getDamage(src, tgt, move, true);
    battle.random = real;
    out.push(typeof dmg === 'number' ? dmg : 0);
  }
  return {hp: mods.aHp ? src.hp : 0, out};
}

const q = (s) => `R"(${s})"`;
const boosts = (o = {}) => `{${['atk', 'def', 'spa', 'spd'].map(k => o[k] || 0).join(', ')}}`;
const lines = [
  '// Generated by tools/damage_oracle.cjs from Pokémon Showdown 0.11.10. Do not edit.',
];
for (const [name, a, d, mods] of CASES) {
  for (const crit of [false, true]) {
    const {hp, out} = run(a, d, mods, crit);
    lines.push(`{"${name}", ${q(S[a])}, ${q(S[d])}, "${mods.weather || ''}", ` +
      `"${mods.aStatus || ''}", "${mods.dStatus || ''}", ${boosts(mods.aBoosts)}, ` +
      `${boosts(mods.dBoosts)}, ${!!mods.reflect}, ${!!mods.lightScreen}, ${hp}, ` +
      `"${mods.aFlags || ''}", "${mods.dFlags || ''}", ${!!mods.struggle}, ${crit}, ` +
      `{${out.join(', ')}}},`);
  }
}
process.stdout.write(lines.join('\n') + '\n');
