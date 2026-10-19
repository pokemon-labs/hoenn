// Options for update, as libpkmn's src/lib/common/battle.zig: the protocol
// log, chance tracking (Actions, Durations and the probability) and calc
// overrides, passed together as one object.
//
// Each is a template parameter, the C++ equivalent of libpkmn's comptime
// Options. Search runs with all three null, and the null types are empty, so
// they take no space and their calls compile to nothing. The log's types are
// in protocol.hpp. Chance and Calc are generation-specific, so their real and
// null types belong in chance.hpp and calc.hpp. The roll helpers
// in effects.hpp use them directly behind `if constexpr
// (T::enabled)`; the null types are only `enabled = false` (CLAUDE.md,
// "Chance", Hooks).

#pragma once

namespace hoenn {

template <typename Log, typename Chance, typename Calc> struct Options {
  using LogType = Log;
  using ChanceType = Chance;
  using CalcType = Calc;

  [[no_unique_address]] Log log;
  [[no_unique_address]] Chance chance;
  [[no_unique_address]] Calc calc;
};

// libpkmn's battle.options(log, chance, calc). Aggregate deduction (C++20)
// also works: Options{log, chance, calc}.
template <typename Log, typename Chance, typename Calc>
constexpr Options<Log, Chance, Calc> options(Log log, Chance chance,
                                             Calc calc) {
  return {log, chance, calc};
}

} // namespace hoenn
