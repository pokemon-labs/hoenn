// The battle's PRNG: SplitMix64 on a 64-bit seed stored inside the battle.
//
// We don't reproduce the cartridge's LCG stream, only its distributions, so
// the generator is chosen for speed: one add and three xor-shift-multiplies
// per draw, a single u64 of state, and any seed (including 0) is fine.
//
// The draw helpers mirror the shapes the decomp uses, so a call site reads
// like the decomp line it cites and Chance can record the probability:
//
//   decomp                              here                 probability
//   Random() % n                        range(n)             1 / n each
//   (Random() & 3) + 2                  range(4) + 2         1 / 4 each
//   Random() % 100 < acc                chance(acc, 100)     acc / 100
//   !(Random() % n)                     chance(1, n)         1 / n
//   rate >= Random()  (Random() a u16)  chance(rate + 1, 65536)
//
// range() maps a 64-bit draw onto [0, n) with a multiply and shift (Lemire)
// and skips the rejection step, so it has no loop. Its bias is below
// n / 2^64, far beneath anything a battle can observe, so Chance treats
// every outcome as exactly 1 / n. The decomp's own `Random() % n` on a u16
// is slightly biased for n that don't divide 65536; we don't copy that bias.

#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace pkmn {

struct RNG {
  uint64_t seed;

  // SplitMix64 (Steele, Lea and Flood, 2014).
  constexpr uint64_t next() {
    uint64_t z = (seed += 0x9E3779B97F4A7C15);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    return z ^ (z >> 31);
  }

  // A uniform value in [0, n). n must be at least 1.
  constexpr uint32_t range(uint32_t n) {
    assert(n > 0);
    __extension__ using u128 = unsigned __int128;
    return uint32_t(u128(next()) * n >> 64);
  }

  // True with probability num / den. num may be 0 (never) or >= den
  // (always); den must be at least 1.
  constexpr bool chance(uint32_t num, uint32_t den) {
    assert(den > 0);
    return range(den) < num;
  }
};

static_assert(sizeof(RNG) == 8);
static_assert(std::is_trivial_v<RNG> && std::is_standard_layout_v<RNG>);

// The reference SplitMix64 output for seed 0.
static_assert(RNG{0}.next() == 0xE220A8397B1DCDAF);
static_assert([] {
  RNG rng{0x1234};
  for (int i = 0; i < 1000; i++) {
    if (rng.range(1) != 0) return false;
    if (rng.range(6) >= 6) return false;
    if (rng.chance(0, 100) || !rng.chance(100, 100)) return false;
  }
  return true;
}());

} // namespace pkmn
