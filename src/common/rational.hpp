// Exact probabilities for Chance, ported from libpkmn's
// src/lib/common/rational.zig.
//
// A probability is p / q with p, q >= 1. update(p, q) multiplies the numerator
// by p and the denominator by q. Reduction is lazy: the fraction is reduced
// only when an operation would overflow (integer T) or come within 2^10 of the
// mantissa (double), so call reduce() before reading p and q. Being lazy, it
// can overflow where an eagerly reduced fraction wouldn't. To get the eager
// behaviour, reduce() after every operation.
//
// T is uint64_t, unsigned __int128 or double. Chance uses double, so it never
// needs overflow bounds on the fraction. For integer T, operations return
// false on overflow, like libpkmn's error.Overflow. For double they always
// return true: past 2^53 they lose precision instead of failing.
//
// Deviations from libpkmn:
// - libpkmn asserts that p and q are coprime when both are comptime-known.
//   Here that check is the template overload update<P, Q>(). The runtime
//   overload accepts any p and q (an accuracy of 85 / 100, say).
// - add, mul and cmp take their argument by value, so they reduce a copy
//   instead of the caller's rational.
// - Every gcd loop has a fixed bound.

#pragma once

#include <cassert>
#include <cmath>
#include <compare>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace pkmn {

namespace rational {

__extension__ using u128 = unsigned __int128;

template <typename T>
constexpr bool is_integer = std::is_same_v<T, uint64_t> ||
                            std::is_same_v<T, u128>;

template <typename T>
constexpr int bits = std::is_same_v<T, u128> ? 128 : 64;

template <typename T> constexpr int ctz(T x) {
  assert(x != 0);
  if constexpr (std::is_same_v<T, u128>) {
    const auto lo = uint64_t(x);
    return lo ? __builtin_ctzll(lo) : 64 + __builtin_ctzll(uint64_t(x >> 64));
  } else {
    return __builtin_ctzll(x);
  }
}

// Binary gcd (Stein's algorithm), as libpkmn's. Each pass takes at least one
// bit off x or y, so it finishes within 2 * bits passes.
template <typename T> constexpr T gcd(T x, T y) {
  static_assert(is_integer<T>);
  assert(x >= 1 && y >= 1);
  const int xz = ctz(x);
  const int yz = ctz(y);
  const int shift = xz < yz ? xz : yz;
  x >>= xz;
  y >>= yz;
  for (int i = 0; i <= 2 * bits<T>; i++) {
    T diff = y - x;
    if (diff == 0) {
      const T result = y << shift;
      assert(result > 0);
      return result;
    }
    const int zeros = ctz(diff);
    if (x > y) diff = -diff;
    y = x < y ? x : y;
    x = diff >> zeros;
  }
  assert(false && "gcd: pass bound exceeded");
  return 1;
}

// x mod y for whole doubles x, y >= 1, exactly and in constant evaluation
// (std::fmod is neither constexpr nor needed). Binary long division: each
// subtraction has y * 2^k <= r < y * 2^(k+1), so it is exact by Sterbenz's
// lemma, and scaling by 2 is exact. A double spans fewer than 2^11 binades,
// so both loops are bounded by 2100.
constexpr double mod(double x, double y) {
  assert(x >= 1 && y >= 1);
  double m = y;
  int k = 0;
  while (k < 2100 && m * 2 <= x) {
    m *= 2;
    k++;
  }
  assert(k < 2100);
  double r = x;
  for (; k >= 0; k--) {
    if (r >= m) r -= m;
    m /= 2;
  }
  assert(r < y);
  return r;
}

// The gcd of two whole doubles >= 1. Below 2^64 it uses the integer gcd.
// Above that, Euclid with the exact mod above, as libpkmn's float path does
// with @mod. By Lame's theorem that takes at most about 1.44 * 1024 steps for
// any finite double.
constexpr double gcd(double x, double y) {
  assert(x >= 1 && y >= 1);
  constexpr double limit = 18446744073709551616.0; // 2^64
  if (x < limit && y < limit) return double(gcd(uint64_t(x), uint64_t(y)));
  for (int i = 0; i < 1500; i++) {
    if (y == 0) return x;
    const double c = y;
    y = c <= x ? mod(x, c) : x;
    x = c;
  }
  assert(false && "gcd: step bound exceeded");
  return 1;
}

} // namespace rational

template <typename T> struct Rational {
  static_assert(rational::is_integer<T> || std::is_same_v<T, double>);

  // Numerator and denominator, both always >= 1. Not necessarily reduced.
  T p = 1;
  T q = 1;

  // Resets the rational to 1.
  constexpr void reset() {
    p = 1;
    q = 1;
  }

  // Multiplies the numerator by p and the denominator by q. Both must be
  // >= 1. Use the template overload when both are constants, so their
  // coprimality is checked at compile time.
  [[nodiscard]] constexpr bool update(uint64_t np, uint64_t nq) {
    assert(np >= 1 && nq >= 1);
    if constexpr (integer) {
      if (multiply(T(np), T(nq))) return true;
      reduce();
      return multiply(T(np), T(nq));
    } else {
      // Reduce where precision is likely to start going.
      if (p > o || q > o) reduce();
      p *= double(np);
      q *= double(nq);
      assert(whole());
      return true;
    }
  }

  template <uint64_t NP, uint64_t NQ> [[nodiscard]] constexpr bool update() {
    static_assert(NP >= 1 && NQ >= 1);
    static_assert(rational::gcd(NP, NQ) == 1,
                  "unreduced constants overflow prematurely");
    return update(NP, NQ);
  }

  // Adds s, using (a / b) + (c / d) = (ad + bc) / bd.
  [[nodiscard]] constexpr bool add(Rational s) {
    if constexpr (integer) {
      if (q == s.q) {
        T sum;
        if (!__builtin_add_overflow(p, s.p, &sum)) {
          p = sum;
          return true;
        }
        reduce();
        s.reduce();
        if (q == s.q) {
          if (__builtin_add_overflow(p, s.p, &sum)) return false;
          p = sum;
          return true;
        }
        return addition(s);
      }
      if (addition(s)) return true;
      reduce();
      s.reduce();
      return addition(s);
    } else {
      if (q == s.q) {
        if (p > o) reduce();
        if (s.p > o) s.reduce();
      }
      if (q == s.q) {
        p += s.p;
      } else {
        // Always reduce, to lose as little precision as possible.
        reduce();
        s.reduce();
        p = p * s.q + q * s.p;
        q *= s.q;
      }
      assert(whole());
      return true;
    }
  }

  // Multiplies by s.
  [[nodiscard]] constexpr bool mul(Rational s) {
    if constexpr (integer) {
      if (multiply(s.p, s.q)) return true;
      reduce();
      s.reduce();
      return multiply(s.p, s.q);
    } else {
      if (p > o || q > o) reduce();
      if (s.p > o || s.q > o) s.reduce();
      p *= s.p;
      q *= s.q;
      assert(whole());
      return true;
    }
  }

  // Compares with s, using a / b > c / d <=> ad > bc. Empty on overflow.
  [[nodiscard]] constexpr std::optional<std::strong_ordering>
  cmp(Rational s) {
    if constexpr (integer) {
      T ad, bc;
      if (__builtin_mul_overflow(p, s.q, &ad)) {
        reduce();
        s.reduce();
        if (__builtin_mul_overflow(p, s.q, &ad)) return std::nullopt;
      }
      if (__builtin_mul_overflow(q, s.p, &bc)) {
        reduce();
        s.reduce();
        if (__builtin_mul_overflow(q, s.p, &bc)) return std::nullopt;
        // Reducing changed p and s.q, so recompute ad.
        if (__builtin_mul_overflow(p, s.q, &ad)) return std::nullopt;
      }
      return ad <=> bc;
    } else {
      if (p > o || q > o) reduce();
      if (s.p > o || s.q > o) s.reduce();
      const double ad = p * s.q;
      const double bc = q * s.p;
      return ad < bc ? std::strong_ordering::less
           : ad > bc ? std::strong_ordering::greater
                     : std::strong_ordering::equal;
    }
  }

  // Reduces by the greatest common divisor.
  constexpr void reduce() {
    const T d = rational::gcd(p, q);
    if (d == 1) return;
    p /= d;
    q /= d;
    assert(p >= 1 && q >= 1);
    if constexpr (!integer) assert(whole());
  }

  friend constexpr bool operator==(const Rational&, const Rational&) = default;

 private:
  static constexpr bool integer = rational::is_integer<T>;

  // For double: start reducing within 2^10 of the 52-bit mantissa, since a
  // single update multiplies by less than about 2^10 (libpkmn's `o`).
  static constexpr double o = double(uint64_t(1) << (52 - 10));

  constexpr bool whole() const {
    if constexpr (integer) {
      return true;
    } else if (std::is_constant_evaluated()) {
      return true;
    } else {
      return std::trunc(p) == p && std::trunc(q) == q;
    }
  }

  // Integer T only. Commits nothing on overflow.
  constexpr bool multiply(T np, T nq) {
    T rp, rq;
    if (__builtin_mul_overflow(p, np, &rp)) return false;
    if (__builtin_mul_overflow(q, nq, &rq)) return false;
    p = rp;
    q = rq;
    return true;
  }

  // Integer T only: (a / b) + (c / d) = (ad + bc) / bd. Commits nothing on
  // overflow.
  constexpr bool addition(const Rational& s) {
    T bd, ad, bc, sum;
    if (__builtin_mul_overflow(q, s.q, &bd)) return false;
    if (__builtin_mul_overflow(p, s.q, &ad)) return false;
    if (__builtin_mul_overflow(q, s.p, &bc)) return false;
    if (__builtin_add_overflow(ad, bc, &sum)) return false;
    p = sum;
    q = bd;
    return true;
  }
};

static_assert(sizeof(Rational<double>) == 16);
static_assert(std::is_trivially_copyable_v<Rational<double>>);

// libpkmn's tests (rational.zig), for each T.
namespace rational::test {

static_assert(gcd(uint64_t(300'000), uint64_t(2'300'000)) == 100'000);
static_assert(gcd(uint64_t(1), uint64_t(1)) == 1);
static_assert(gcd(uint64_t(12), uint64_t(18)) == 6);
static_assert(gcd(u128(1) << 100, u128(3) << 90) == u128(1) << 90);
static_assert(gcd(48.0, 180.0) == 12.0);
static_assert(gcd(0x1p80 * 3, 0x1p70 * 9) == 0x1p70 * 3);
static_assert(mod(0x1p80 + 0x1p30, 0x1p60) == 0x1p30);

template <typename T> constexpr bool turn(Rational<T>& r) {
  return r.template update<1, 163>()     // Metronome
         && r.template update<33, 256>() // Critical Hit (67 Speed)
         && r.template update<1, 39>()   // Damage roll
         && r.template update<89, 128>() // Thunder accuracy (178/256)
         && r.template update<77, 256>(); // Thunder secondary proc
}

template <typename T> constexpr bool run() {
  using R = Rational<T>;
  R r;

  // Not coprime, so it goes through the runtime overload.
  if (!r.update(128, 256) || !turn(r)) return false;
  r.reduce();
  if (!(r == R{75383, 35550920704})) return false;

  if (!r.update(1, 4)) return false;
  if constexpr (std::is_same_v<T, uint64_t>) {
    if (turn(r)) return false; // overflows
  } else {
    if (!turn(r)) return false;
    r.reduce();
    if (!(r == R{5682596689, T(0x890762) * T(uint64_t(1) << 48)})) return false;
  }

  r.reset();
  if (!r.mul(R{10, 13}) || !r.mul(R{3, 4})) return false;
  if (!(r == R{30, 52})) return false;
  if (!r.add(R{1, 3})) return false;
  r.reduce();
  if (!(r == R{71, 78})) return false;

  // Equal denominators take the shortcut.
  R h{1, 4};
  if (!h.add(R{2, 4})) return false;
  h.reduce();
  if (!(h == R{3, 4})) return false;

  R u{3, 5};
  if (u.cmp(R{2, 5}) != std::strong_ordering::greater) return false;
  if (u.cmp(R{4, 5}) != std::strong_ordering::less) return false;
  if (u.cmp(R{3, 6}) != std::strong_ordering::greater) return false;
  if (u.cmp(R{3, 4}) != std::strong_ordering::less) return false;
  if (u.cmp(R{3, 5}) != std::strong_ordering::equal) return false;

  if (!turn(u)) return false;
  const auto c = u.cmp(R{4567216874, 89124781931235});
  if constexpr (std::is_same_v<T, uint64_t>) {
    if (c) return false; // overflows
  } else {
    if (c != std::strong_ordering::less) return false;
  }
  return true;
}

static_assert(run<uint64_t>());
static_assert(run<u128>());
static_assert(run<double>());

} // namespace rational::test

} // namespace pkmn
