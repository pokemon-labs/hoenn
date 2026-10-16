// Compile-time spot checks of the generated tables:
// Built and run by the `check_data` test (CMakeLists.txt).
#include "data/moves.hpp"
#include "data/species.hpp"
#include "data/items.hpp"
using namespace hoenn;
static_assert(get(Move::Tackle).bp == 35);
static_assert(get(Move::HiddenPowerFire).type == Type::Fire);
static_assert(hidden_power(Type::Fire) == Move::HiddenPowerFire);
static_assert(hidden_power(Type::Fighting) == Move::HiddenPowerFighting);
static_assert(hidden_power(Type::Dark) == Move::HiddenPowerDark);
static_assert(get(Species::Skarmory).stats.def == 140);
static_assert(get(Species::Celebi).types[0] == Type::Psychic);
static_assert(get(Species::Treecko).stats.spe == 70);
static_assert(get(Species::DeoxysSpeed).stats.spe == 180);
static_assert(uint16_t(Species::Deoxys) == 386);
static_assert(get(Item::Leftovers).effect == HoldEffect::Leftovers);
static_assert(get(Item::EnigmaBerry).effect == HoldEffect::None);
static_assert(uint8_t(Item::KnockedOff) == ITEMS - 1);
static_assert(get(Item::KnockedOff).effect == HoldEffect::None);
static_assert(get(Move::Curse).type == Type::Mystery);
static_assert(effectiveness(Type::Ground, Type::Flying) == Effectiveness::Immune);
static_assert(get(Move::SleepTalk).extra & Extra::NoSleepTalk);
static_assert(get(Move::SolarBeam).extra & Extra::NoSleepTalk);
static_assert(!(get(Move::Earthquake).extra & Extra::NoSleepTalk));
static_assert(uint16_t(Move::PsychoBoost) == 354);
int main(){}
