// The binary protocol log, as libpkmn's src/lib/common/protocol.zig.
//
// Only the plumbing is here for now: the fixed-buffer Writer, the null
// writer, and the Log that wraps one of them. The message methods (move,
// switch, damage and so on) and their encodings come last, after the
// mechanics (see CLAUDE.md, "The log comes last").
//
// The driver owns the buffer. A Writer only points into it, so it lives in
// Options, never in Battle (which holds no pointers). Search runs with the
// null log, whose writes are empty inline functions and compile to nothing.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pkmn::protocol {

// Writes protocol bytes into a fixed buffer owned by the caller, as libpkmn's
// protocol.Writer. Each write is all or nothing: it returns false, writing
// nothing, if the bytes don't fit (libpkmn's error.NoSpaceLeft). Integers
// are written in native byte order, as libpkmn's are.
struct Writer {
  uint8_t* buffer = nullptr;
  size_t len = 0;
  size_t pos = 0;

  [[nodiscard]] bool write_byte(uint8_t byte) {
    if (pos >= len) return false;
    buffer[pos++] = byte;
    return true;
  }

  [[nodiscard]] bool write_all(const uint8_t* bytes, size_t n) {
    if (n > len - pos) return false;
    if (n) std::memcpy(buffer + pos, bytes, n);
    pos += n;
    return true;
  }

  template <typename T> [[nodiscard]] bool write_int(T v) {
    static_assert(std::is_integral_v<T>);
    static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big);
    if (sizeof(T) > len - pos) return false;
    std::memcpy(buffer + pos, &v, sizeof(T));
    pos += sizeof(T);
    return true;
  }

  void reset() { pos = 0; }
};

// Ignores everything, as libpkmn's NullWriter.
struct NullWriter {
  [[nodiscard]] constexpr bool write_byte(uint8_t) const { return true; }
  [[nodiscard]] constexpr bool write_all(const uint8_t*, size_t) const {
    return true;
  }
  template <typename T> [[nodiscard]] constexpr bool write_int(T) const {
    return true;
  }
};

// Logs protocol messages to its writer, as libpkmn's Log(WriterT). W is
// Writer* (FixedLog) or NullWriter (null_log).
template <typename W> struct Log {
  static_assert(std::is_same_v<W, Writer*> || std::is_same_v<W, NullWriter>);

  [[no_unique_address]] W writer;

  // True if this log writes anything. Message methods check it first, so the
  // null log does no work building their arguments.
  static constexpr bool enabled = !std::is_same_v<W, NullWriter>;
};

using FixedLog = Log<Writer*>;
using NullLog = Log<NullWriter>;

// libpkmn's protocol.NULL. (NULL itself is a C macro.)
inline constexpr NullLog null_log{};

static_assert(std::is_empty_v<NullLog>);
static_assert(std::is_trivially_copyable_v<FixedLog>);

} // namespace pkmn::protocol
