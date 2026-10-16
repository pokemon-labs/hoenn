// Turns the binary log of gen3/protocol.hpp into Showdown's text protocol,
// line for line, as libpkmn's src/pkg/protocol.ts. Not part of update: it
// allocates, and search never calls it.
//
// The output is Showdown's raw omniscient log (decided 2026-10-08): a
// message that shows HP is the |split|pN triplet of sim/battle.ts addSplit,
// the marker, the line with exact HP, then the line HP Percentage Mod shows
// everyone else. Each player's view comes from Showdown's own
// extractChannelMessages, not from here. The formatter writes none of the
// lines Showdown's FILTER drops (README "Protocol"), nor the blank `|`
// lines between actions.

#pragma once

#include <array>
#include <cctype>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "data/display.hpp"
#include "protocol.hpp"

namespace pkmn::gen3::format {

namespace protocol = pkmn::protocol;
using protocol::ArgType;

// What the log needs and Battle doesn't hold: the players' names and each
// Pokémon's nickname and shininess, by party position (ID::position() - 1).
// helpers::Pokemon has them.
struct Names {
  std::array<std::string, 2> player = {"p1", "p2"};
  std::array<std::array<std::string, 6>, 2> nickname;
  std::array<std::array<bool, 6>, 2> shiny{};
};

namespace detail {

inline const char* side_id(Player p) { return p == Player::P1 ? "p1" : "p2"; }

inline std::string status_name(uint8_t bits) {
  const Status s{bits};
  if (s.asleep()) return "slp";
  if (s.badly_poisoned()) return "tox";
  if (s.poisoned()) return "psn";
  if (s.burned()) return "brn";
  if (s.frozen()) return "frz";
  if (s.paralyzed()) return "par";
  return "";
}

// Pokemon.getHealth (sim/pokemon.ts:2060): exact, or HP Percentage Mod's
// ceiling with 100 kept for full HP.
inline std::string health(uint16_t hp, uint16_t max, uint8_t status, bool exact) {
  if (hp == 0) return "0 fnt";
  std::string s;
  if (exact) {
    s = std::to_string(hp) + "/" + std::to_string(max);
  } else {
    uint32_t pct = (100u * hp + max - 1) / max;
    if (pct == 100 && hp < max) pct = 99;
    s = std::to_string(pct) + "/100";
  }
  const std::string st = status_name(status);
  if (!st.empty()) s += " " + st;
  return s;
}

// Reads the message bytes in order. Reads past the end assert.
struct Reader {
  std::span<const uint8_t> bytes;
  size_t pos = 0;

  bool done() const { return pos >= bytes.size(); }
  uint8_t byte() {
    assert(pos < bytes.size());
    return bytes[pos++];
  }
  uint16_t u16() {
    assert(pos + 2 <= bytes.size());
    uint16_t v;
    std::memcpy(&v, bytes.data() + pos, 2);
    pos += 2;
    return v;
  }
};

} // namespace detail

// Formats one update's messages, up to the terminating None (or the end of
// `bytes`), appending lines to `out`.
class Formatter {
 public:
  explicit Formatter(const Names& names) : names_(names) {}

  void format(std::span<const uint8_t> bytes, std::vector<std::string>& out) {
    detail::Reader r{bytes};
    last_move_ = SIZE_MAX;
    while (!r.done()) {
      const ArgType type = ArgType(r.byte());
      if (type == ArgType::None) return;
      line(type, r, out);
    }
  }

  std::vector<std::string> format(std::span<const uint8_t> bytes) {
    std::vector<std::string> out;
    format(bytes, out);
    return out;
  }

 private:
  const Names& names_;
  size_t last_move_ = SIZE_MAX;

  std::string ident(ID id) const {
    assert(id.position() >= 1 && id.position() <= 6);
    return std::string(detail::side_id(id.player())) + "a: " +
           names_.nickname[uint8_t(id.player())][id.position() - 1];
  }

  // Pokemon.toString for a Pokémon that isn't active: no position letter.
  std::string bench_ident(ID id) const {
    assert(id.position() >= 1 && id.position() <= 6);
    return std::string(detail::side_id(id.player())) + ": " +
           names_.nickname[uint8_t(id.player())][id.position() - 1];
  }

  std::string nickname(Player p, uint8_t position) const {
    assert(position >= 1 && position <= 6);
    return names_.nickname[uint8_t(p)][position - 1];
  }

  // Showdown's toID: lowercase letters and digits.
  static std::string to_id(const std::string& name) {
    std::string id;
    for (const char c : name) {
      if (c >= 'A' && c <= 'Z') id += char(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id += c;
    }
    return id;
  }

  static std::string move_name(Move m) { return std::string(display::MOVE_NAMES[uint16_t(m)]); }
  // Showdown's type names; the decomp's Mystery is ???.
  static std::string type_name(uint8_t t) {
    static constexpr const char* NAMES[TYPES] = {
        "Normal", "Fighting", "Flying",   "Poison",  "Ground", "Rock",
        "Bug",    "Ghost",    "Steel",    "???",     "Fire",   "Water",
        "Grass",  "Electric", "Psychic",  "Ice",     "Dragon", "Dark"};
    assert(t < TYPES);
    return NAMES[t];
  }
  static std::string ability_name(Ability a) {
    return std::string(display::ABILITY_NAMES[uint8_t(a)]);
  }

  // A line with HP: the split triplet. `before` is the line up to the HP
  // and `after` what follows it.
  static void split(std::vector<std::string>& out, Player p, const std::string& before,
                    uint16_t hp, uint16_t max, uint8_t status,
                    const std::string& after) {
    out.push_back(std::string("|split|") + detail::side_id(p));
    out.push_back(before + detail::health(hp, max, status, true) + after);
    out.push_back(before + detail::health(hp, max, status, false) + after);
  }

  void line(ArgType type, detail::Reader& r, std::vector<std::string>& out) {
    switch (type) {
    case ArgType::Move: {
      const ID source{r.byte()};
      const Move m = Move(r.u16());
      const ID target{r.byte()};
      const auto from = protocol::MoveFrom(r.byte());
      std::string s = "|move|" + ident(source) + "|" + move_name(m) + "|" +
                      (target == protocol::NO_ID ? "" : ident(target));
      if (from == protocol::MoveFrom::From) {
        s += "|[from] " + move_name(Move(r.u16()));
      } else if (from == protocol::MoveFrom::LockedMove) {
        s += "|[from] lockedmove";
      }
      last_move_ = out.size();
      out.push_back(s);
      return;
    }
    case ArgType::LastStill: {
      // Battle.attrLastMove('[still]') (sim/battle.ts:3121): the target is
      // blanked, or an -anim line is removed (and later attributes go
      // nowhere).
      if (last_move_ == SIZE_MAX) return;
      assert(last_move_ < out.size());
      if (out[last_move_].starts_with("|-anim|")) {
        out.erase(out.begin() + std::ptrdiff_t(last_move_));
        last_move_ = SIZE_MAX;
        return;
      }
      std::string& s = out[last_move_];
      // The target is the fifth '|'-separated part: after the fourth '|'.
      size_t bar = 0;
      for (int i = 0; i < 3; i++) bar = s.find('|', bar + 1);
      const size_t next = s.find('|', bar + 1);
      s.erase(bar + 1, next == std::string::npos ? std::string::npos : next - bar - 1);
      s += "|[still]";
      return;
    }
    case ArgType::LastMiss:
      if (last_move_ == SIZE_MAX) return;
      assert(last_move_ < out.size());
      out[last_move_] += "|[miss]";
      return;
    case ArgType::Anim: {
      const ID source{r.byte()};
      const Move m = Move(r.u16());
      last_move_ = out.size();
      out.push_back("|-anim|" + ident(source) + "|" + move_name(m) + "|" + ident(ID{r.byte()}));
      return;
    }
    case ArgType::Status: {
      const ID id{r.byte()};
      std::string s = "|-status|" + ident(id) + "|" + detail::status_name(r.byte());
      using F = protocol::StatusFrom;
      switch (F(r.byte())) {
      case F::None: break;
      case F::Move: s += "|[from] move: " + move_name(Move(r.u16())); break;
      case F::Ability: {
        const Ability a = Ability(r.byte());
        s += "|[from] ability: " + ability_name(a) + "|[of] " + ident(ID{r.byte()});
        break;
      }
      }
      out.push_back(s);
      return;
    }
    case ArgType::CureStatus: {
      const ID id{r.byte()};
      const std::string status = detail::status_name(r.byte());
      const auto cure = protocol::Cure(r.byte());
      const bool bench = r.byte() != 0;
      std::string s = "|-curestatus|" + (bench ? bench_ident(id) : ident(id)) + "|" + status;
      using C = protocol::Cure;
      switch (cure) {
      case C::Msg: s += "|[msg]"; break;
      case C::Silent: s += "|[silent]"; break;
      case C::NaturalCure: s += "|[from] ability: Natural Cure|[silent]"; break;
      case C::Move: s += "|[from] move: " + move_name(Move(r.u16())); break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::CureTeam:
      out.push_back("|-cureteam|" + ident(ID{r.byte()}) + "|[from] move: Aromatherapy");
      return;
    case ArgType::Boost: {
      // Showdown's BoostIDs, by the stage index.
      static constexpr const char* STATS[7] = {"atk", "def", "spe", "spa",
                                               "spd", "accuracy", "evasion"};
      const ID id{r.byte()};
      const uint8_t stat = r.byte();
      const uint8_t n = r.byte();
      assert((stat & 7) < 7);
      std::string s = std::string(stat & protocol::BOOST_DOWN ? "|-unboost|" : "|-boost|") +
                      ident(id) + "|" + STATS[stat & 7] + "|" + std::to_string(n);
      if (stat & protocol::BOOST_ITEM) {
        s += "|[from] item: " + std::string(display::ITEM_NAMES[r.byte()]);
      }
      out.push_back(s);
      return;
    }
    case ArgType::SetBoost:
      out.push_back("|-setboost|" + ident(ID{r.byte()}) + "|atk|6|[from] move: Belly Drum");
      return;
    case ArgType::CopyBoost: {
      const ID source{r.byte()};
      out.push_back("|-copyboost|" + ident(source) + "|" + ident(ID{r.byte()}) +
                    "|[from] move: Psych Up");
      return;
    }
    case ArgType::ClearAllBoost:
      out.push_back("|-clearallboost");
      return;
    case ArgType::ClearNegativeBoost:
      out.push_back("|-clearnegativeboost|" + ident(ID{r.byte()}) + "|[silent]");
      return;
    case ArgType::Crit:
      out.push_back("|-crit|" + ident(ID{r.byte()}));
      return;
    case ArgType::SuperEffective:
      out.push_back("|-supereffective|" + ident(ID{r.byte()}));
      return;
    case ArgType::Resisted:
      out.push_back("|-resisted|" + ident(ID{r.byte()}));
      return;
    case ArgType::OHKO:
      out.push_back("|-ohko");
      return;
    case ArgType::Nothing:
      out.push_back("|-nothing");
      return;
    case ArgType::Transform: {
      const ID source{r.byte()};
      out.push_back("|-transform|" + ident(source) + "|" + ident(ID{r.byte()}));
      return;
    }
    case ArgType::HitCount: {
      const ID id{r.byte()};
      out.push_back("|-hitcount|" + ident(id) + "|" + std::to_string(r.byte()));
      return;
    }
    case ArgType::Prepare: {
      const ID id{r.byte()};
      out.push_back("|-prepare|" + ident(id) + "|" + move_name(Move(r.u16())));
      return;
    }
    case ArgType::MustRecharge:
      out.push_back("|-mustrecharge|" + ident(ID{r.byte()}));
      return;
    case ArgType::Miss: {
      const ID source{r.byte()};
      const std::string user =
          source.bits & protocol::BENCH ? bench_ident(source) : ident(source);
      out.push_back("|-miss|" + user + "|" + ident(ID{r.byte()}));
      return;
    }
    case ArgType::Fail: {
      const ID id{r.byte()};
      std::string s = "|-fail|" + ident(id);
      using F = protocol::Fail;
      switch (F(r.byte())) {
      case F::None: break;
      case F::Heal: s += "|heal"; break;
      case F::Sleep: s += "|slp"; break;
      case F::Poison: s += "|psn"; break;
      case F::Burn: s += "|brn"; break;
      case F::Freeze: s += "|frz"; break;
      case F::Paralysis: s += "|par"; break;
      case F::Toxic: s += "|tox"; break;
      case F::Substitute: s += "|move: Substitute"; break;
      case F::SubstituteWeak: s += "|move: Substitute|[weak]"; break;
      case F::Uproar: s += "|slp|[from] Uproar"; break;
      case F::UproarMsg: s += "|slp|[from] Uproar|[msg]"; break;
      case F::Unboost: {
        // Showdown names the holder as both the target and the source.
        // Hyper Cutter and Keen Eye guard one stat, which they name.
        const Ability a = Ability(r.byte());
        s += "|unboost";
        if (a == Ability::HyperCutter) s += "|atk";
        if (a == Ability::KeenEye) s += "|accuracy";
        s += "|[from] ability: " + ability_name(a) + "|[of] " + ident(id);
        break;
      }
      case F::Ability:
        s += "|[from] ability: " + ability_name(Ability(r.byte())) + "|[of] " + ident(id);
        break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::Immune: {
      std::string s = "|-immune|" + ident(ID{r.byte()});
      using I = protocol::Immune;
      switch (I(r.byte())) {
      case I::None: break;
      case I::Ability: s += "|[from] ability: " + ability_name(Ability(r.byte())); break;
      case I::Confusion:
        s += "|confusion|[from] ability: " + ability_name(Ability(r.byte()));
        break;
      case I::OHKO: s += "|[ohko]"; break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::Activate: {
      std::string s = "|-activate|" + ident(ID{r.byte()}) + "|";
      using A = protocol::Activate;
      const A effect = A(r.byte());
      switch (effect) {
      case A::Protect: s += "Protect"; break;
      case A::Safeguard: s += "move: Safeguard"; break;
      case A::Mist: s += "move: Mist"; break;
      case A::SuctionCups: s += "ability: Suction Cups"; break;
      case A::Ingrain: s += "move: Ingrain"; break;
      case A::HealBell: s += "move: Heal Bell"; break;
      case A::SubstituteDamage: s += "Substitute|[damage]"; break;
      case A::Confusion: s += "confusion"; break;
      case A::Trapped: s += "trapped"; break;
      case A::FocusBand: s += "item: Focus Band"; break;
      case A::DestinyBond: s += "move: Destiny Bond"; break;
      case A::Endure: s += "move: Endure"; break;
      case A::Pursuit: s += "move: Pursuit"; break;
      case A::Bide: s += "move: Bide"; break;
      case A::Ability: s += "ability: " + ability_name(Ability(r.byte())); break;
      case A::Mimic: s += "move: Mimic|" + move_name(Move(r.u16())); break;
      case A::Grudge: s += "move: Grudge|" + move_name(Move(r.u16())); break;
      case A::Leppa: s += "item: Leppa Berry|" + move_name(Move(r.u16())) + "|[consumed]"; break;
      case A::Attract: s += "move: Attract"; break;
      case A::LockOn: s += "move: Lock-On"; break;
      case A::MindReader: s += "move: Mind Reader"; break;
      case A::Snatch: s += "move: Snatch"; break;
      case A::Trick: s += "move: Trick"; break;
      case A::SkillSwap: s += "Skill Swap||"; break;
      case A::Bound: s += "move: " + move_name(Move(r.u16())); break;
      case A::Magnitude: s += "move: Magnitude|" + std::to_string(r.byte()); break;
      case A::Spite: {
        std::string id;
        for (const char c : move_name(Move(r.u16()))) {
          if (std::isalnum(static_cast<unsigned char>(c))) id += char(std::tolower(static_cast<unsigned char>(c)));
        }
        s += "move: Spite|" + id + "|" + std::to_string(r.byte());
        break;
      }
      }
      if (protocol::activate_of(effect)) s += "|[of] " + ident(ID{r.byte()});
      out.push_back(s);
      return;
    }
    case ArgType::Start: {
      std::string s = "|-start|" + ident(ID{r.byte()}) + "|";
      using S = protocol::Start;
      const S effect = S(r.byte());
      switch (effect) {
      case S::Confusion: s += "confusion"; break;
      case S::ConfusionFatigue: s += "confusion|[fatigue]"; break;
      case S::Substitute: s += "Substitute"; break;
      case S::LeechSeed: s += "move: Leech Seed"; break;
      case S::Nightmare: s += "Nightmare"; break;
      case S::Encore: s += "Encore"; break;
      case S::Taunt: s += "move: Taunt"; break;
      case S::Torment: s += "Torment"; break;
      case S::Foresight: s += "Foresight"; break;
      case S::Imprison: s += "move: Imprison"; break;
      case S::Ingrain: s += "move: Ingrain"; break;
      case S::MudSport: s += "Mud Sport"; break;
      case S::WaterSport: s += "move: Water Sport"; break;
      case S::Charge: s += "Charge"; break;
      case S::Uproar: s += "Uproar"; break;
      case S::UproarUpkeep: s += "Uproar|[upkeep]"; break;
      case S::Bide: s += "move: Bide"; break;
      case S::FlashFire: s += "ability: Flash Fire"; break;
      case S::FocusEnergy: s += "move: Focus Energy"; break;
      case S::FocusEnergySilent: s += "move: Focus Energy|[silent]"; break;
      case S::FutureSight: s += "Future Sight"; break;
      case S::DoomDesire: s += "Doom Desire"; break;
      case S::PerishSilent: s += "perish3|[silent]"; break;
      case S::Attract: s += "Attract"; break;
      case S::Curse: s += "Curse"; break;
      case S::Yawn: s += "move: Yawn"; break;
      case S::AttractCuteCharm: s += "Attract|[from] ability: Cute Charm"; break;
      case S::Disable: s += "Disable|" + move_name(Move(r.u16())); break;
      case S::TypeChange: s += "typechange|" + type_name(r.byte()); break;
      case S::ColorChange:
        s += "typechange|" + type_name(r.byte()) + "|[from] ability: Color Change";
        break;
      case S::Stockpile: s += "stockpile" + std::to_string(r.byte()); break;
      case S::Perish: s += "perish" + std::to_string(r.byte()); break;
      }
      if (effect >= S::Curse && effect <= S::AttractCuteCharm) {
        s += "|[of] " + ident(ID{r.byte()});
      }
      out.push_back(s);
      return;
    }
    case ArgType::FormeChange: {
      const ID id{r.byte()};
      const uint8_t t = r.byte();
      const char* forme = t == uint8_t(gen3::Type::Fire)    ? "Castform-Sunny"
                          : t == uint8_t(gen3::Type::Water) ? "Castform-Rainy"
                          : t == uint8_t(gen3::Type::Ice)   ? "Castform-Snowy"
                                                            : "Castform";
      out.push_back("|-formechange|" + ident(id) + "|" + forme +
                    "|[msg]|[from] ability: Forecast");
      return;
    }
    case ArgType::End: {
      const ID id{r.byte()};
      std::string s = "|-end|" + ident(id) + "|";
      using E = protocol::End;
      switch (E(r.byte())) {
      case E::Confusion: s += "confusion"; break;
      case E::Substitute: s += "Substitute"; break;
      case E::LeechSeedRapidSpin:
        s += "Leech Seed|[from] move: Rapid Spin|[of] " + ident(id);
        break;
      case E::Nightmare: s += "Nightmare|[silent]"; break;
      case E::Encore: s += "Encore"; break;
      case E::Disable: s += "Disable"; break;
      case E::Taunt: s += "move: Taunt|[silent]"; break;
      case E::Torment: s += "Torment"; break;
      case E::Yawn: s += "move: Yawn|[silent]"; break;
      case E::Attract: s += "Attract|[silent]"; break;
      case E::AttractOblivious: s += "move: Attract|[from] ability: Oblivious"; break;
      case E::AttractMentalHerb: s += "move: Attract|[from] item: Mental Herb"; break;
      case E::Uproar: s += "Uproar"; break;
      case E::Bide: s += "move: Bide"; break;
      case E::BideSilent: s += "move: Bide|[silent]"; break;
      case E::Stockpile: s += "Stockpile"; break;
      case E::Charge: s += "Charge|[silent]"; break;
      case E::FlashFire: s += "ability: Flash Fire|[silent]"; break;
      case E::Bound: s += move_name(Move(r.u16())) + "|[partiallytrapped]"; break;
      case E::BoundSilent:
        s += move_name(Move(r.u16())) + "|[partiallytrapped]|[silent]";
        break;
      case E::FutureMove: s += "move: " + move_name(Move(r.u16())); break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::SingleTurn: {
      std::string s = "|-singleturn|" + ident(ID{r.byte()}) + "|";
      using T = protocol::SingleTurn;
      switch (T(r.byte())) {
      case T::Protect: s += "Protect"; break;
      case T::Endure: s += "move: Endure"; break;
      case T::FocusPunch: s += "move: Focus Punch"; break;
      case T::Snatch: s += "Snatch"; break;
      case T::MagicCoat: s += "move: Magic Coat"; break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::SingleMove: {
      std::string s = "|-singlemove|" + ident(ID{r.byte()}) + "|";
      using M = protocol::SingleMove;
      switch (M(r.byte())) {
      case M::DestinyBond: s += "Destiny Bond"; break;
      case M::Grudge: s += "Grudge"; break;
      case M::Rage: s += "Rage"; break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::FieldActivate:
      out.push_back(protocol::FieldActivate(r.byte()) == protocol::FieldActivate::PerishSong
                        ? "|-fieldactivate|move: Perish Song"
                        : "|-fieldactivate|move: Pay Day");
      return;
    case ArgType::SideStart:
    case ArgType::SideEnd: {
      const Player p = Player(r.byte() & 1);
      std::string s = std::string(type == ArgType::SideStart ? "|-sidestart|" : "|-sideend|") +
                      detail::side_id(p) + ": " + names_.player[uint8_t(p)] + "|";
      using C = protocol::SideCondition;
      switch (C(r.byte())) {
      case C::Reflect: s += "Reflect"; break;
      case C::LightScreen: s += "move: Light Screen"; break;
      case C::Mist: s += "Mist"; break;
      case C::Safeguard: s += "Safeguard"; break;
      case C::Spikes: s += "Spikes"; break;
      case C::SpikesRapidSpin:
        s += "Spikes|[from] move: Rapid Spin|[of] " + ident(ID{r.byte()});
        break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::Weather: {
      static constexpr const char* NAMES[] = {"none", "RainDance", "SunnyDay", "Sandstorm",
                                              "Hail"};
      const uint8_t w = r.byte();
      assert(w < 5);
      std::string s = std::string("|-weather|") + NAMES[w];
      using F = protocol::WeatherFrom;
      switch (F(r.byte())) {
      case F::None: break;
      case F::Upkeep: s += "|[upkeep]"; break;
      case F::Ability: {
        const Ability a = Ability(r.byte());
        s += "|[from] ability: " + ability_name(a) + "|[of] " + ident(ID{r.byte()});
        break;
      }
      }
      out.push_back(s);
      return;
    }
    case ArgType::SetHP: {
      const ID id{r.byte()};
      const uint16_t hp = r.u16(), max = r.u16();
      const uint8_t status = r.byte();
      const bool silent = r.byte() != 0;
      split(out, id.player(), "|-sethp|" + ident(id) + "|", hp, max, status,
            std::string("|[from] move: Pain Split") + (silent ? "|[silent]" : ""));
      return;
    }
    case ArgType::Switch:
    case ArgType::Drag: {
      const ID id{r.byte()};
      const Species species = Species(r.u16());
      const Gender gender = Gender(r.byte());
      const uint8_t level = r.byte();
      const uint16_t hp = r.u16(), max = r.u16();
      const uint8_t status = r.byte();
      const auto from = protocol::SwitchFrom(r.byte());
      std::string details(display::SPECIES_NAMES[uint16_t(species)]);
      if (level != 100) details += ", L" + std::to_string(level);
      if (gender != Gender::None) details += gender == Gender::Male ? ", M" : ", F";
      if (names_.shiny[uint8_t(id.player())][id.position() - 1]) details += ", shiny";
      const std::string before = std::string("|") +
                                 (type == ArgType::Switch ? "switch" : "drag") + "|" +
                                 ident(id) + "|" + details + "|";
      split(out, id.player(), before, hp, max, status,
            from == protocol::SwitchFrom::BatonPass ? "|[from] Baton Pass" : "");
      return;
    }
    case ArgType::Cant: {
      const ID id{r.byte()};
      const auto reason = protocol::Cant(r.byte());
      std::string s = "|cant|" + ident(id) + "|";
      using C = protocol::Cant;
      switch (reason) {
      case C::Sleep: s += "slp"; break;
      case C::Freeze: s += "frz"; break;
      case C::Paralysis: s += "par"; break;
      case C::Flinch: s += "flinch"; break;
      case C::Recharge: s += "recharge"; break;
      case C::Attract: s += "Attract"; break;
      case C::Truant: s += "ability: Truant"; break;
      case C::FocusPunch: s += "Focus Punch|Focus Punch"; break;
      case C::Taunt: s += "move: Taunt"; break;
      case C::Disable: s += "Disable"; break;
      case C::Imprison: s += "move: Imprison"; break;
      case C::PP:
      case C::SleepTalkPP: s += "nopp"; break;
      }
      if (reason == C::SleepTalkPP) {
        s += "|" + to_id(move_name(Move(r.u16())));
      } else if (protocol::names_move(reason)) {
        s += "|" + move_name(Move(r.u16()));
      }
      out.push_back(s);
      return;
    }
    case ArgType::Faint:
      out.push_back("|faint|" + ident(ID{r.byte()}));
      return;
    case ArgType::Turn:
      out.push_back("|turn|" + std::to_string(r.u16()));
      return;
    case ArgType::Win:
      out.push_back("|win|" + names_.player[r.byte() & 1]);
      return;
    case ArgType::Tie:
      out.push_back("|tie");
      return;
    case ArgType::Damage: {
      const ID id{r.byte()};
      const uint16_t hp = r.u16(), max = r.u16();
      const uint8_t status = r.byte();
      std::string after;
      using D = protocol::Damage;
      switch (D(r.byte())) {
      case D::None: break;
      case D::Poison: after = "|[from] psn"; break;
      case D::Burn: after = "|[from] brn"; break;
      case D::Confusion: after = "|[from] confusion"; break;
      case D::LeechSeed: after = "|[from] Leech Seed|[of] " + ident(ID{r.byte()}); break;
      case D::Recoil: after = "|[from] Recoil"; break;
      case D::Spikes: after = "|[from] Spikes"; break;
      case D::Sandstorm: after = "|[from] Sandstorm"; break;
      case D::Hail: after = "|[from] Hail"; break;
      case D::Curse: after = "|[from] Curse"; break;
      case D::Nightmare: after = "|[from] Nightmare"; break;
      case D::Bound:
        after = "|[from] move: " + move_name(Move(r.u16())) + "|[partiallytrapped]";
        break;
      case D::Ability: {
        const Ability a = Ability(r.byte());
        after = "|[from] ability: " + std::string(display::ABILITY_NAMES[uint8_t(a)]) +
                "|[of] " + ident(ID{r.byte()});
        break;
      }
      }
      split(out, id.player(), "|-damage|" + ident(id) + "|", hp, max, status, after);
      return;
    }
    case ArgType::Heal: {
      const ID id{r.byte()};
      const uint16_t hp = r.u16(), max = r.u16();
      const uint8_t status = r.byte();
      std::string after;
      using H = protocol::Heal;
      switch (H(r.byte())) {
      case H::None: break;
      case H::Silent: after = "|[silent]"; break;
      case H::Drain: after = "|[from] drain|[of] " + ident(ID{r.byte()}); break;
      case H::Item:
        after = "|[from] item: " + std::string(display::ITEM_NAMES[r.byte()]);
        break;
      case H::Ability: {
        const Ability a = Ability(r.byte());
        const ID of{r.byte()};
        after = "|[from] ability: " + std::string(display::ABILITY_NAMES[uint8_t(a)]);
        if (of != protocol::NO_ID) after += "|[of] " + ident(of);
        break;
      }
      case H::Wish:
        after = "|[from] move: Wish|[wisher] " + nickname(id.player(), r.byte());
        break;
      case H::Ingrain: after = "|[from] Ingrain"; break;
      }
      split(out, id.player(), "|-heal|" + ident(id) + "|", hp, max, status, after);
      return;
    }
    case ArgType::Item: {
      const ID id{r.byte()};
      std::string s = "|-item|" + ident(id) + "|" + std::string(display::ITEM_NAMES[r.byte()]);
      using F = protocol::ItemFrom;
      switch (F(r.byte())) {
      case F::Thief: s += "|[from] move: Thief|[of] " + ident(ID{r.byte()}); break;
      case F::Covet: s += "|[from] move: Covet|[of] " + ident(ID{r.byte()}); break;
      case F::Trick: s += "|[from] move: Trick"; break;
      case F::Recycle: s += "|[from] move: Recycle"; break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::EndItem: {
      const ID id{r.byte()};
      std::string s = "|-enditem|" + ident(id) + "|" + std::string(display::ITEM_NAMES[r.byte()]);
      using F = protocol::EndItemFrom;
      switch (F(r.byte())) {
      case F::None: break;
      case F::Eat: s += "|[eat]"; break;
      case F::KnockOff: s += "|[from] move: Knock Off|[of] " + ident(ID{r.byte()}); break;
      case F::Thief: s += "|[silent]|[from] move: Thief|[of] " + ident(ID{r.byte()}); break;
      case F::Trick: s += "|[silent]|[from] move: Trick"; break;
      }
      out.push_back(s);
      return;
    }
    case ArgType::Ability: {
      const ID id{r.byte()};
      std::string s = "|-ability|" + ident(id) + "|" + ability_name(Ability(r.byte()));
      using F = protocol::AbilityFrom;
      const F from = F(r.byte());
      switch (from) {
      case F::Boost: out.push_back(s + "|boost"); return;
      case F::Pressure:
        // addSplit with no public line (data/mods/gen3/abilities.ts:148).
        out.push_back(std::string("|split|") + detail::side_id(id.player()));
        out.push_back(s + "|[silent]");
        out.push_back("");
        return;
      case F::Trace:
      case F::RolePlay: {
        s += "|" + ability_name(Ability(r.byte()));
        s += from == F::Trace ? "|[from] ability: Trace" : "|[from] move: Role Play";
        s += "|[of] " + ident(ID{r.byte()});
        out.push_back(s);
        return;
      }
      }
      return;
    }
    default:
      assert(false && "unknown protocol message");
      out.push_back("|?|" + std::to_string(uint8_t(type)));
      r.pos = r.bytes.size();
      return;
    }
  }
};

} // namespace pkmn::gen3::format
