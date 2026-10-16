// Which moves on the given teams the engine handles: the share of move
// slots (and of damaging move slots) whose effect has a written mechanic,
// then the most-used damaging and status moves still missing. A move counts
// as handled if use_move runs its effect (detail::handled); everything
// else only uses PP.
//
// Usage: coverage team files...

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "data/names.hpp"
#include "mechanics.hpp"
#include "showdown.hpp"

using namespace hoenn;

int main(int argc, char** argv) {
  RNG rng{0};
  std::map<uint16_t, int> uses;
  for (int i = 1; i < argc; i++) {
    std::ifstream in(argv[i]);
    std::stringstream ss;
    ss << in.rdbuf();
    for (const auto& team : showdown::parse_teams(ss.str(), rng)) {
      if (!team.value) continue;
      for (const auto& p : team.value->pokemon) {
        for (const Move m : p.moves) {
          if (m != Move::None) uses[uint16_t(m)]++;
        }
      }
    }
  }

  int total = 0, handled = 0, damaging = 0, handled_damaging = 0;
  std::vector<std::pair<int, uint16_t>> missing;
  for (const auto& [m, n] : uses) {
    const MoveData& data = get(Move(m));
    const bool ok = detail::handled(data.effect);
    total += n;
    handled += ok ? n : 0;
    if (data.bp != 0) {
      damaging += n;
      handled_damaging += ok ? n : 0;
    }
    if (!ok) missing.push_back({n, m});
  }
  if (total == 0) {
    std::fprintf(stderr, "coverage: no moves (pass Showdown export files)\n");
    return 2;
  }
  std::printf("move slots %d: handled %d (%.0f%%); damaging %d: handled %d "
              "(%.0f%%)\n",
              total, handled, 100.0 * handled / total, damaging,
              handled_damaging, 100.0 * handled_damaging / damaging);
  std::sort(missing.rbegin(), missing.rend());
  std::printf("most-used missing moves:\n");
  for (size_t i = 0; i < missing.size() && i < 30; i++) {
    const auto [n, m] = missing[i];
    std::printf("%5d  %s\n", n, std::string(names::MOVE_IDS[m]).c_str());
  }
  return 0;
}
