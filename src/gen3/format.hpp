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
      // blanked.
      assert(last_move_ < out.size());
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
      assert(last_move_ < out.size());
      out[last_move_] += "|[miss]";
      return;
    case ArgType::Miss: {
      const ID source{r.byte()};
      out.push_back("|-miss|" + ident(source) + "|" + ident(ID{r.byte()}));
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
      case F::Unboost:
        // Showdown names the holder as both the target and the source.
        s += "|unboost|[from] ability: " +
             std::string(display::ABILITY_NAMES[r.byte()]) + "|[of] " + ident(id);
        break;
      }
      out.push_back(s);
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
      case H::Ingrain: after = "|[from] move: Ingrain"; break;
      }
      split(out, id.player(), "|-heal|" + ident(id) + "|", hp, max, status, after);
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
