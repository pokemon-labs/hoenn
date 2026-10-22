// A player's view of the binary log: the bytes rewritten so that
// format::Formatter's Stream::View prints what Showdown's stream for that
// player shows (extractChannelMessages, showdown/sim/battle.ts:35). That is,
// for the omniscient Formatter's text `text(bytes)`:
//
//   split_filter(p, text(bytes)) == view_text(view(p, bytes))
//
// where view_text's names are permuted as the view renumbers. Tested by
// tools/check_view.cpp (docs/ENGINE.md, "Protocol"). Not part of update, and
// allocation-free: a driver can call it on every update.
//
// What it hides is what the bytes show and Showdown's stream doesn't:
//   - the other side's HP becomes HP Percentage Mod's (max 100), unless
//     `exact_hp` (Showdown's reportExactHP, a debug format);
//   - every status byte loses what the text doesn't show: a sleep becomes
//     Status::slp(1), with no counter and no Rest flag;
//   - the other side's Pressure, which has no public line, is dropped;
//   - the other side's party positions (libpkmn's ident, docs/PROTOCOL.md):
//     its Pokémon are numbered 1-6 in the order they first appear, as a
//     model built from the text would hold them. Our own side keeps the
//     true positions, as our request does.
// Side relabelling (the viewer always p1) is the driver's: Showdown's p2
// stream still says p1 and p2.
//
// The spectators' stream (Showdown's channel 0, every triplet's shared line)
// is one player's view of the other's: Spectator.

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "protocol.hpp"

namespace hoenn::protocol {

namespace detail {

inline void view_status(uint8_t& bits) {
  const hoenn::Status s{bits};
  if (s.asleep()) bits = hoenn::Status::slp(1).bits;
}

// The HP fields at `hp`: hp u16, max HP u16, status, of a Pokémon on `owner`.
inline void view_hp(Player p, Player owner, uint8_t* hp, bool exact_hp) {
  view_status(hp[4]);
  if (owner == p || exact_hp) return;
  uint16_t now, max;
  std::memcpy(&now, hp, 2);
  std::memcpy(&max, hp + 2, 2);
  now = hp_percent(now, max);
  max = 100;
  std::memcpy(hp, &now, 2);
  std::memcpy(hp + 2, &max, 2);
}

// The offsets, from the first field, of a message's idents (NO_ID allowed),
// by the encodings in protocol.hpp; `n` is message_size. Wish's wisher, a
// bare party position, isn't an ident.
inline uint8_t idents(ArgType type, const uint8_t* f, size_t n, std::array<uint8_t, 2>& at) {
  at = {0, 0};
  switch (type) {
  case ArgType::LastStill:
  case ArgType::LastMiss:
  case ArgType::LastNoTarget:
  case ArgType::OHKO:
  case ArgType::Tie:
  case ArgType::ClearAllBoost:
  case ArgType::Nothing:
  case ArgType::Turn:
  case ArgType::Win:
  case ArgType::SideStart:
  case ArgType::FieldActivate: return 0;
  case ArgType::SideEnd:
    at[0] = 2;
    return SideCondition(f[1]) == SideCondition::SpikesRapidSpin ? 1 : 0;
  case ArgType::Weather:
    at[0] = 3;
    return WeatherFrom(f[1]) == WeatherFrom::Ability ? 1 : 0;
  case ArgType::Move:
  case ArgType::Anim: at[1] = 3; return 2;
  case ArgType::Miss:
  case ArgType::CopyBoost:
  case ArgType::Transform: at[1] = 1; return 2;
  case ArgType::Activate: at[1] = uint8_t(n - 1); return activate_of(Activate(f[1])) ? 2 : 1;
  case ArgType::Start: {
    const Start s = Start(f[1]);
    at[1] = 2;
    return s >= Start::Curse && s <= Start::AttractCuteCharm ? 2 : 1;
  }
  case ArgType::Cant: at[1] = uint8_t(n - 1); return Cant(f[1]) == Cant::Damp ? 2 : 1;
  case ArgType::Damage:
    switch (Damage(f[6])) {
    case Damage::LeechSeed: at[1] = 7; return 2;
    case Damage::Ability: at[1] = 8; return 2;
    default: return 1;
    }
  case ArgType::Heal:
    switch (Heal(f[6])) {
    case Heal::Drain: at[1] = 7; return 2;
    case Heal::Item:
    case Heal::Ability: at[1] = 8; return 2;
    default: return 1;
    }
  case ArgType::Status: at[1] = 4; return StatusFrom(f[2]) == StatusFrom::Ability ? 2 : 1;
  case ArgType::Item: {
    const ItemFrom from = ItemFrom(f[2]);
    at[1] = 3;
    return from == ItemFrom::Thief || from == ItemFrom::Covet ? 2 : 1;
  }
  case ArgType::EndItem: {
    const EndItemFrom from = EndItemFrom(f[2]);
    at[1] = 3;
    return from == EndItemFrom::KnockOff || from == EndItemFrom::Thief ? 2 : 1;
  }
  case ArgType::Ability: {
    const AbilityFrom from = AbilityFrom(f[2]);
    at[1] = 4;
    return from == AbilityFrom::Trace || from == AbilityFrom::RolePlay ? 2 : 1;
  }
  case ArgType::NoTarget:
  case ArgType::Fail:
  case ArgType::Immune:
  case ArgType::End:
  case ArgType::SingleTurn:
  case ArgType::SingleMove:
  case ArgType::SetHP:
  case ArgType::Switch:
  case ArgType::Drag:
  case ArgType::Faint:
  case ArgType::CureStatus:
  case ArgType::CureTeam:
  case ArgType::Boost:
  case ArgType::SetBoost:
  case ArgType::ClearNegativeBoost:
  case ArgType::Crit:
  case ArgType::SuperEffective:
  case ArgType::Resisted:
  case ArgType::HitCount:
  case ArgType::Prepare:
  case ArgType::MustRecharge:
  case ArgType::FormeChange: return 1;
  default: assert(false && "not a gen3 message"); return 0;
  }
}

} // namespace detail

// Player `player`'s view of one battle's log, given its updates in order.
// Trivially copyable; a new battle needs a new View.
struct View {
  Player player = Player::P1;
  bool exact_hp = false;
  uint8_t revealed = 0;
  // By the other side's party position: its number, 0 before it appears.
  std::array<uint8_t, 6> numbers{};

  // Rewrites one update's messages in place, up to the terminating None
  // (kept) or the end of `bytes`, and returns the view's length, never more
  // than the input's.
  size_t apply(std::span<uint8_t> bytes) {
    size_t in = 0, out = 0;
    while (in < bytes.size()) {
      const ArgType type = ArgType(bytes[in]);
      const size_t n = type == ArgType::None ? 0 : message_size(type, bytes.data() + in + 1);
      assert(in + 1 + n <= bytes.size());
      uint8_t* m = bytes.data() + in;
      const Player owner = n > 0 ? ID{m[1]}.player() : Player::P1;
      bool keep = true;
      switch (type) {
      case ArgType::Damage:
      case ArgType::Heal:
      case ArgType::SetHP: detail::view_hp(player, owner, m + 2, exact_hp); break;
      case ArgType::Switch:
      case ArgType::Drag: detail::view_hp(player, owner, m + 6, exact_hp); break;
      case ArgType::Status:
      case ArgType::CureStatus: detail::view_status(m[2]); break;
      case ArgType::Ability:
        keep = owner == player || AbilityFrom(m[3]) != AbilityFrom::Pressure;
        break;
      default: break;
      }
      if (keep && n > 0) renumber(type, m + 1, n);
      if (keep) {
        if (out != in) std::memmove(bytes.data() + out, m, 1 + n);
        out += 1 + n;
      }
      in += 1 + n;
      if (type == ArgType::None) break;
    }
    return out;
  }

 private:
  uint8_t number(uint8_t position) {
    assert(position >= 1 && position <= 6);
    uint8_t& k = numbers[position - 1];
    if (k == 0) {
      assert(revealed < 6);
      k = ++revealed;
    }
    return k;
  }

  void renumber(ArgType type, uint8_t* f, size_t n) {
    std::array<uint8_t, 2> at;
    const uint8_t count = detail::idents(type, f, n, at);
    for (uint8_t i = 0; i < count; i++) {
      uint8_t& b = f[at[i]];
      if (b == NO_ID.bits || ID{b}.player() == player) continue;
      b = uint8_t((b & ~7) | number(ID{b}.position()));
    }
    if (type == ArgType::Heal && Heal(f[6]) == Heal::Wish && ID{f[0]}.player() != player) {
      f[7] = number(f[7]);
    }
  }
};

static_assert(std::is_trivially_copyable_v<View>);

// The spectators' view: P2's view of P1's view, which hides both sides. Each
// View touches only the other side, so P1's view of P2's gives the same
// bytes (check_view).
struct Spectator {
  View p1{Player::P1};
  View p2{Player::P2};

  size_t apply(std::span<uint8_t> bytes) { return p2.apply(bytes.first(p1.apply(bytes))); }
};

static_assert(std::is_trivially_copyable_v<Spectator>);

} // namespace hoenn::protocol
