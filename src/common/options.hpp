// Options for update, as libpkmn's src/lib/common/battle.zig: the protocol
// log, chance tracking (Actions, Durations and the probability) and calc
// overrides, passed together as one object.
//
// Each is a template parameter, the C++ equivalent of libpkmn's comptime
// Options. Search runs with all three null, and the null types are empty, so
// they take no space and their calls compile to nothing. The log's types are
// in protocol.hpp. Chance and Calc are generation-specific, so their real and
// null types belong in gen3/chance.hpp and gen3/calc.hpp, with the hook
// interface (not designed yet).

#pragma once

namespace pkmn {

template <typename Log, typename Chance, typename Calc> struct Options {
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

} // namespace pkmn
