// The player view's square (docs/ENGINE.md, "Protocol"): over random self-play,
// per update and per player p,
//
//   split_filter(p, format(bytes)) == format_view(view(p, bytes))
//
// with exact HP and with HP Percentage Mod, where format_view's names are
// permuted as the view numbers the other side. Also: view(p) is idempotent
// (a second View over its output changes nothing, so the other side is
// numbered in order of appearance), and it leaves p's own HP fields as they
// were.
//
// Usage: check_view [--seed N] [--games N] team files...

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "format.hpp"
#include "helpers.hpp"
#include "log.hpp"
#include "mechanics.hpp"
#include "options.hpp"
#include "showdown.hpp"
#include "view.hpp"

using namespace hoenn;

namespace {

using Teams = std::vector<std::vector<helpers::Pokemon>>;
using Logged = Options<protocol::FixedLog, NullChance, NullCalc>;

// The test's reference, from Showdown's extractChannelMessages
// (showdown/sim/battle.ts:35), not from format.hpp: a |split|pX triplet is
// its secret line for player X and its shared line for anyone else, skipped
// when empty; the regex's `.+` skips any other empty line.
std::vector<std::string> split_filter(Player p, const std::vector<std::string>& lines) {
  const int channel = p == Player::P1 ? 1 : 2;
  std::vector<std::string> out;
  for (size_t i = 0; i < lines.size(); i++) {
    const std::string& line = lines[i];
    if (line.size() == 9 && line.starts_with("|split|p")) {
      if (i + 2 >= lines.size()) {
        out.push_back("<truncated split>");
        break;
      }
      const std::string& kept = line[8] - '0' == channel ? lines[i + 1] : lines[i + 2];
      if (!kept.empty()) out.push_back(kept);
      i += 2;
    } else if (!line.empty()) {
      out.push_back(line);
    }
  }
  return out;
}

// The HP fields (hp, max) of p's own Pokémon in messages that carry HP, in
// order.
std::vector<uint32_t> own_hp(Player p, std::span<const uint8_t> bytes) {
  using protocol::ArgType;
  std::vector<uint32_t> out;
  size_t i = 0;
  while (i < bytes.size()) {
    const ArgType type = ArgType(bytes[i]);
    if (type == ArgType::None) break;
    const uint8_t* m = bytes.data() + i;
    size_t at = 0;
    if (type == ArgType::Damage || type == ArgType::Heal || type == ArgType::SetHP) at = 2;
    if (type == ArgType::Switch || type == ArgType::Drag) at = 6;
    if (at != 0 && ID{m[1]}.player() == p) {
      uint32_t v;
      std::memcpy(&v, m + at, 4);
      out.push_back(v);
    }
    i += 1 + protocol::message_size(type, m + 1);
  }
  return out;
}

int failures = 0;

void report(uint64_t game_seed, uint64_t update, Player p, bool exact, const char* what,
            const std::vector<std::string>& want, const std::vector<std::string>& got) {
  failures++;
  if (failures > 10) return;
  std::printf("game %llu, update %llu, p%d%s: %s\n", (unsigned long long)game_seed,
              (unsigned long long)update, p == Player::P1 ? 1 : 2, exact ? " (exact HP)" : "",
              what);
  const size_t n = std::max(want.size(), got.size());
  for (size_t i = 0; i < n; i++) {
    const std::string& a = i < want.size() ? want[i] : "<none>";
    const std::string& b = i < got.size() ? got[i] : "<none>";
    std::printf("  %s %s\n", a == b ? " " : "!", a.c_str());
    if (a != b) std::printf("  > %s\n", b.c_str());
  }
}

// One player's views over a game: `view` of the log and `again` of its output.
struct Viewer {
  protocol::View view;
  protocol::View again;
};

// The names as `view` numbers the other side: its nth Pokémon to appear is
// party position n.
format::Names viewed_names(const format::Names& names, const protocol::View& view) {
  format::Names out = names;
  const size_t other = view.player == Player::P1 ? 1 : 0;
  for (size_t i = 0; i < 6; i++) {
    out.nickname[other][i] = "<unseen>";
    out.shiny[other][i] = false;
  }
  for (size_t i = 0; i < 6; i++) {
    const uint8_t k = view.numbers[i];
    if (k == 0) continue;
    out.nickname[other][k - 1] = names.nickname[other][i];
    out.shiny[other][k - 1] = names.shiny[other][i];
  }
  return out;
}

// Checks one update's bytes; returns the lines compared.
uint64_t check(const format::Names& names, std::array<std::array<Viewer, 2>, 2>& viewers,
               std::span<const uint8_t> bytes, uint64_t game_seed, uint64_t update) {
  uint64_t lines = 0;
  for (const bool exact : {false, true}) {
    format::Formatter omniscient(names, exact);
    const std::vector<std::string> text = omniscient.format(bytes);
    for (const Player p : {Player::P1, Player::P2}) {
      Viewer& viewer = viewers[exact][size_t(p)];
      const std::vector<std::string> want = split_filter(p, text);
      std::vector<uint8_t> v(bytes.begin(), bytes.end());
      v.resize(viewer.view.apply(v));
      const format::Names permuted = viewed_names(names, viewer.view);
      format::Formatter viewed(permuted, exact, format::Stream::View);
      const std::vector<std::string> got = viewed.format(v);
      lines += want.size();
      if (got != want) report(game_seed, update, p, exact, "the square doesn't commute", want, got);

      std::vector<uint8_t> twice = v;
      twice.resize(viewer.again.apply(twice));
      if (twice != v) report(game_seed, update, p, exact, "view isn't idempotent", got, viewed.format(twice));

      if (own_hp(p, bytes) != own_hp(p, v)) {
        report(game_seed, update, p, exact, "view changed its own HP", want, got);
      }
    }
  }
  return lines;
}

struct Totals {
  uint64_t updates = 0;
  uint64_t lines = 0;
};

void play(const Teams& teams, uint64_t game_seed, Totals& totals) {
  RNG rng{game_seed};
  const uint32_t a = rng.range(uint32_t(teams.size()));
  const uint32_t b = rng.range(uint32_t(teams.size()));
  Battle battle = *helpers::init_battle(rng.next(), teams[a], teams[b]);

  format::Names names;
  for (size_t i = 0; i < 6; i++) {
    for (int s = 0; s < 2; s++) {
      const auto& team = teams[s == 0 ? a : b];
      if (i < team.size()) {
        names.nickname[size_t(s)][i] = std::string(display::SPECIES_NAMES[uint16_t(team[i].species)]);
      }
    }
  }

  std::array<std::array<Viewer, 2>, 2> viewers;
  for (const bool exact : {false, true}) {
    for (const Player p : {Player::P1, Player::P2}) {
      viewers[exact][size_t(p)] = {{p, exact}, {p, exact}};
    }
  }

  std::vector<uint8_t> buffer(protocol::MAX_LOG_SIZE);
  protocol::Writer writer{buffer.data(), buffer.size(), 0};
  Logged o{protocol::fixed_log(writer), {}, {}};
  Result r = update(battle, Choice::pass(), Choice::pass(), o);
  totals.lines += check(names, viewers, std::span(buffer.data(), writer.pos), game_seed, totals.updates++);
  std::array<Choice, MAX_CHOICES> c1{}, c2{};
  while (!r.over()) {
    const uint8_t n1 = choices(battle, Player::P1, r.p1(), c1);
    const uint8_t n2 = choices(battle, Player::P2, r.p2(), c2);
    const Choice x = c1[rng.range(n1)];
    const Choice y = c2[rng.range(n2)];
    writer.reset();
    r = update(battle, x, y, o);
    totals.lines += check(names, viewers, std::span(buffer.data(), writer.pos), game_seed, totals.updates++);
  }
}

Teams load(const std::vector<std::string>& files) {
  Teams teams;
  RNG rng{0};
  for (const std::string& file : files) {
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& team : showdown::parse_teams(ss.str(), rng)) {
      if (team.value) teams.push_back(team.value->pokemon);
    }
  }
  return teams;
}

} // namespace

int main(int argc, char** argv) {
  uint64_t seed = 1;
  uint64_t games = 20;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--seed" && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--games" && i + 1 < argc) {
      games = std::strtoull(argv[++i], nullptr, 10);
    } else {
      files.push_back(arg);
    }
  }
  const Teams teams = load(files);
  if (teams.empty()) {
    std::fprintf(stderr, "check_view: no teams (pass Showdown export files)\n");
    return 2;
  }

  RNG seeds{seed};
  Totals totals;
  for (uint64_t i = 0; i < games; i++) play(teams, seeds.next(), totals);
  std::printf("check_view: %llu games, %llu updates, %llu lines per view, %d failures\n",
              (unsigned long long)games, (unsigned long long)totals.updates,
              (unsigned long long)totals.lines, failures);
  return failures ? 1 : 0;
}
