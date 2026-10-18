// Compares damage.hpp with Pokémon Showdown 0.11.11, every roll with and
// without a crit, over the cases in damage_cases.inc (written by
// damage_oracle.cjs). Each case builds a battle from two Showdown sets,
// switches both in by hand (copying what switch-in copies), applies the
// case's conditions and runs matchup, damage and apply_roll.
//
// Prints each mismatch and exits nonzero if any.

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>

#include "damage.hpp"
#include "helpers.hpp"
#include "showdown.hpp"

using namespace hoenn;

namespace {

struct Boosted {
  int atk, def, spa, spd;
};

struct Case {
  const char* name;
  const char* attacker;
  const char* defender;
  const char* weather;
  const char* attacker_status;
  const char* defender_status;
  Boosted attacker_boosts;
  Boosted defender_boosts;
  bool reflect;
  bool light_screen;
  int attacker_hp; // 0: full
  const char* attacker_flags;
  const char* defender_flags;
  bool struggle;
  bool crit;
  uint32_t expected[16];
};

const Case CASES[] = {
#include "damage_cases.inc"
};

bool has(std::string_view flags, std::string_view flag) {
  while (!flags.empty()) {
    const size_t comma = flags.find(',');
    if (flags.substr(0, comma) == flag) return true;
    if (comma == std::string_view::npos) break;
    flags.remove_prefix(comma + 1);
  }
  return false;
}

Status status(std::string_view s) {
  if (s == "brn") return Status::brn();
  if (s == "par") return Status::par();
  if (s == "psn") return Status::psn();
  if (s == "slp") return Status::slp(2);
  return Status::none();
}

// What switch-in copies (ENGINE.md decision 4).
void switch_in(Side& side) {
  const Pokemon& p = side.get(1);
  ActivePokemon& a = side.active;
  a = ActivePokemon{};
  a.stats = p.stats;
  a.species = p.species;
  a.types[0] = p.types[0];
  a.types[1] = p.types[1];
  a.ability = p.ability;
  for (int i = 0; i < 4; i++) a.moves[i] = p.moves[i];
}

void set_flags(Volatiles& v, Move& locked, std::string_view flags) {
  v.flash_fire = has(flags, "flashfire");
  v.charged_up = has(flags, "charge");
  v.mud_sport = has(flags, "mudsport");
  v.water_sport = has(flags, "watersport");
  v.foresight = has(flags, "foresight");
  v.underground = has(flags, "dig");
  v.underwater = has(flags, "dive");
  v.minimized = has(flags, "minimize");
  v.on_air = has(flags, "fly") || has(flags, "bounce");
  if (has(flags, "fly")) locked = Move::Fly;
  if (has(flags, "bounce")) locked = Move::Bounce;
}

Weather weather(std::string_view w) {
  if (w == "raindance") return Weather::Rain;
  if (w == "sunnyday") return Weather::Sun;
  if (w == "sandstorm") return Weather::Sandstorm;
  if (w == "hail") return Weather::Hail;
  return Weather::None;
}

// The hit the backbone would build: Hidden Power's power, Weather Ball's
// type and doubled power.
Hit resolve(const Battle& battle, Player player, Move move, bool crit) {
  Hit hit = Hit::of(move);
  hit.crit = crit;
  const Effect effect = get(move).effect;
  if (effect == Effect::HiddenPower) hit.power = battle.side(player).get(1).hidden_power;
  if (effect == Effect::WeatherBall) {
    switch (detail::effective_weather(battle)) {
    case Weather::Rain: hit.type = Type::Water; break;
    case Weather::Sun: hit.type = Type::Fire; break;
    case Weather::Sandstorm: hit.type = Type::Rock; break;
    case Weather::Hail: hit.type = Type::Ice; break;
    case Weather::None: return hit;
    }
    hit.power = uint16_t(hit.power * 2);
  }
  return hit;
}

} // namespace

int main() {
  int failures = 0;
  for (const Case& c : CASES) {
    RNG rng{1};
    const auto a = showdown::parse_set(c.attacker, rng);
    const auto d = showdown::parse_set(c.defender, rng);
    if (!a.value || !d.value) {
      std::printf("%s: can't parse: %s%s\n", c.name, a.error.c_str(), d.error.c_str());
      failures++;
      continue;
    }
    Battle b = *helpers::init_battle(1, std::span(&*a.value, 1), std::span(&*d.value, 1));
    b.turn = 1;
    for (Side& s : b.sides) switch_in(s);
    Side& att = b.sides[0];
    Side& def = b.sides[1];
    b.field.weather = weather(c.weather);
    b.field.weather_turns = b.field.weather == Weather::None ? 0 : 5;
    att.get(1).status = status(c.attacker_status);
    def.get(1).status = status(c.defender_status);
    att.active.boosts.atk = int8_t(c.attacker_boosts.atk);
    att.active.boosts.spa = int8_t(c.attacker_boosts.spa);
    def.active.boosts.def = int8_t(c.defender_boosts.def);
    def.active.boosts.spd = int8_t(c.defender_boosts.spd);
    if (c.reflect) def.conditions.reflect = 5;
    if (c.light_screen) def.conditions.light_screen = 5;
    if (c.attacker_hp != 0) att.get(1).hp = uint16_t(c.attacker_hp);
    set_flags(att.active.volatiles, att.active.locked_move, c.attacker_flags);
    set_flags(def.active.volatiles, def.active.locked_move, c.defender_flags);
    if (!b.valid()) {
      std::printf("%s: invalid battle\n", c.name);
      failures++;
      continue;
    }

    const Move move = c.struggle ? Move::Struggle : att.active.moves[0].id;
    // Showdown's CriticalHit event: Battle Armor and Shell Armor.
    const bool crit = c.crit && can_crit(b, Player::P1);
    const Hit hit = resolve(b, Player::P1, move, crit);
    const uint8_t flags = matchup(b, Player::P1, hit);
    uint32_t got[16] = {};
    if (!(flags & MoveResult::NO_EFFECT)) {
      const uint32_t base = damage(b, Player::P1, hit);
      for (uint8_t i = 0; i < 16; i++) got[i] = apply_roll(base, i);
    }
    if (std::memcmp(got, c.expected, sizeof got) != 0) {
      failures++;
      std::printf("%s (crit %d):\n  got     ", c.name, c.crit);
      for (uint32_t x : got) std::printf(" %u", x);
      std::printf("\n  expected");
      for (uint32_t x : c.expected) std::printf(" %u", x);
      std::printf("\n");
    }
  }
  const size_t n = sizeof CASES / sizeof *CASES;
  std::printf("%zu of %zu cases match Showdown\n", n - size_t(failures), n);
  return failures == 0 ? 0 : 1;
}
