// The binary protocol log, as libpkmn's src/lib/common/protocol.zig: the
// opcodes (ArgType), the fixed-buffer Writer, the null writer, and the Log
// that wraps one of them. The gen3 messages and their reasons are in
// gen3/protocol.hpp, the text formatter in gen3/format.hpp; the encoding
// decisions are in src/gen3/README.md, "Protocol".
//
// The driver owns the buffer. A Writer only points into it, so it lives in
// Options, never in Battle (which holds no pointers). Search runs with the
// null log, whose writes are empty inline functions and compile to nothing.

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "data.hpp"

namespace pkmn::protocol {

// libpkmn's ArgType, values unchanged (protocol.zig), then three of ours
// after its last (decided 2026-10-08): Showdown's -nothing, -notarget and
// -anim, which libpkmn doesn't log.
enum class ArgType : uint8_t {
  None,

  // Special
  LastStill,
  LastMiss,

  // Gen 1
  Move,
  Switch,
  Cant,
  Faint,
  Turn,
  Win,
  Tie,
  Damage,
  Heal,
  Status,
  CureStatus,
  Boost,
  ClearAllBoost,
  Fail,
  Miss,
  HitCount,
  Prepare,
  MustRecharge,
  Activate,
  FieldActivate,
  Start,
  End,
  OHKO,
  Crit,
  SuperEffective,
  Resisted,
  Immune,
  Transform,

  // Gen 2
  Drag,
  Item,
  EndItem,
  CureTeam,
  SetHP,
  SetBoost,
  CopyBoost,
  SideStart,
  SideEnd,
  SingleMove,
  SingleTurn,
  Weather,

  // Gen 3
  Block,
  Ability,
  EndAbility,
  ClearNegativeBoost,
  FormeChange,
  LastNoTarget,

  // Gen 4
  SwapBoost,
  FieldStart,
  FieldEnd,
  DetailsChange,

  // Gen 5
  ClearPoke,
  Poke,
  TeamPreview,
  Center,
  Swap,
  Combine,
  Waiting,
  Replace,
  ClearBoost,

  // Gen 6
  Mega,
  Primal,
  InvertBoost,

  // Gen 7
  ZBroken,
  ZPower,
  Burst,
  ClearPositiveBoost,

  // Gen 8
  CanDynamax,
  SwapSideConditions,
  UpdatePoke,

  // Gen 9
  Terastallize,

  // Ours
  Nothing,
  NoTarget,
  Anim,
};

static_assert(uint8_t(ArgType::LastNoTarget) == 48);
static_assert(uint8_t(ArgType::Terastallize) == 72);
static_assert(uint8_t(ArgType::Anim) == 75);

// Writes protocol bytes into a fixed buffer owned by the caller, as libpkmn's
// protocol.Writer. The buffer is sized for the most one update can write
// (protocol::MAX_LOG_SIZE in gen3/protocol.hpp), so a write always fits: debug asserts it
// and release doesn't check (decided 2026-10-08; "don't hold it wrong").
// Integers are written in native byte order, as libpkmn's are.
struct Writer {
  uint8_t* buffer = nullptr;
  size_t len = 0;
  size_t pos = 0;

  void write_byte(uint8_t byte) {
    assert(pos < len && "log buffer full");
    buffer[pos++] = byte;
  }

  void write_all(const uint8_t* bytes, size_t n) {
    assert(n <= len - pos && "log buffer full");
    if (n) std::memcpy(buffer + pos, bytes, n);
    pos += n;
  }

  template <typename T> void write_int(T v) {
    static_assert(std::is_integral_v<T>);
    assert(sizeof(T) <= len - pos && "log buffer full");
    std::memcpy(buffer + pos, &v, sizeof(T));
    pos += sizeof(T);
  }

  void reset() { pos = 0; }
};

// Ignores everything, as libpkmn's NullWriter.
struct NullWriter {
  constexpr void write_byte(uint8_t) const {}
  constexpr void write_all(const uint8_t*, size_t) const {}
  template <typename T> constexpr void write_int(T) const {}
};

// The Pokémon that reached 0 HP and whose Faint message is still to come.
// Showdown queues a faint when the HP reaches 0 and writes the queue at
// fixed points (faintMessages: after each action, residual handler and
// multi-hit loop), so lines can come between a Pokémon's 0 HP and its Faint
// (a recoil after a KO). At most the two active Pokémon wait. `written`
// has bit ID::bits set for each Faint written this update, so a Pokémon
// queued again (the engine's faint() after an early queue) isn't written
// twice. `bond` has the bit set for each queued Pokémon whose Destiny Bond
// takes the foe, which the gen3 log writes after its Faint.
struct FaintQueue {
  ID ids[2];
  uint8_t n;
  uint16_t written;
  uint16_t bond;
  // A Grudge's -activate, written after `grudge`'s Faint: its source and
  // the move whose PP it took (0 for none).
  ID grudge;
  ID grudge_source;
  uint16_t grudge_move;
};

struct NoFaintQueue {};

// Logs protocol messages to its writer, as libpkmn's Log(WriterT). W is
// Writer* (FixedLog) or NullWriter (null_log). The messages are free
// functions in gen3/protocol.hpp.
template <typename W> struct Log {
  static_assert(std::is_same_v<W, Writer*> || std::is_same_v<W, NullWriter>);

  // True if this log writes anything. Message functions check it first, so
  // the null log does no work building their arguments.
  static constexpr bool enabled = !std::is_same_v<W, NullWriter>;

  [[no_unique_address]] W writer;
  [[no_unique_address]] std::conditional_t<enabled, FaintQueue, NoFaintQueue> faints;

  void write_byte(uint8_t b) {
    if constexpr (enabled) writer->write_byte(b);
  }
  void write_all(const uint8_t* bytes, size_t n) {
    if constexpr (enabled) writer->write_all(bytes, n);
  }
  template <typename T> void write_int(T v) {
    if constexpr (enabled) writer->write_int(v);
  }
  void write(ArgType type) { write_byte(uint8_t(type)); }
};

using FixedLog = Log<Writer*>;
using NullLog = Log<NullWriter>;

// A log writing into `writer`.
inline FixedLog fixed_log(Writer& writer) { return FixedLog{&writer, {}}; }

// libpkmn's protocol.NULL. (NULL itself is a C macro.)
inline constexpr NullLog null_log{};

static_assert(std::is_empty_v<NullLog>);
static_assert(std::is_trivially_copyable_v<FixedLog>);

} // namespace pkmn::protocol
