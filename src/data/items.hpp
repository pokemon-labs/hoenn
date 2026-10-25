// Generated once from the decomp (pret/pokeemerald 731ad5b); maintained by
// hand since (docs/ENGINE.md, "Data").

#pragma once

#include <cstdint>

namespace hoenn {

// Items with a hold effect (src/data/items.h) plus the Enigma Berry, in
// cartridge order, then two stand-ins. Mail is any of the 12 Mail items,
// which Trick, Thief and Covet can't move (nor the Enigma Berry). Other is
// any other holdable item without a battle effect (balls, evolution stones
// and so on). Those only matter to Trick, Knock Off, Thief and Covet.
enum class Item : uint8_t {
  None,
  BerryJuice,
  CheriBerry,
  ChestoBerry,
  PechaBerry,
  RawstBerry,
  AspearBerry,
  LeppaBerry,
  OranBerry,
  PersimBerry,
  LumBerry,
  SitrusBerry,
  FigyBerry,
  WikiBerry,
  MagoBerry,
  AguavBerry,
  IapapaBerry,
  LiechiBerry,
  GanlonBerry,
  SalacBerry,
  PetayaBerry,
  ApicotBerry,
  LansatBerry,
  StarfBerry,
  EnigmaBerry,
  BrightPowder,
  WhiteHerb,
  MachoBrace,
  ExpShare,
  QuickClaw,
  SootheBell,
  MentalHerb,
  ChoiceBand,
  KingsRock,
  SilverPowder,
  AmuletCoin,
  CleanseTag,
  SoulDew,
  DeepSeaTooth,
  DeepSeaScale,
  SmokeBall,
  Everstone,
  FocusBand,
  LuckyEgg,
  ScopeLens,
  MetalCoat,
  Leftovers,
  DragonScale,
  LightBall,
  SoftSand,
  HardStone,
  MiracleSeed,
  BlackGlasses,
  BlackBelt,
  Magnet,
  MysticWater,
  SharpBeak,
  PoisonBarb,
  NeverMeltIce,
  SpellTag,
  TwistedSpoon,
  Charcoal,
  DragonFang,
  SilkScarf,
  UpGrade,
  ShellBell,
  SeaIncense,
  LaxIncense,
  LuckyPunch,
  MetalPowder,
  ThickClub,
  Stick,
  Mail,
  Other,
};

inline constexpr uint8_t ITEMS = 74;

// HOLD_EFFECT_* from include/constants/hold_effects.h, in order.
enum class HoldEffect : uint8_t {
  None,
  RestoreHp,
  CurePar,
  CureSlp,
  CurePsn,
  CureBrn,
  CureFrz,
  RestorePp,
  CureConfusion,
  CureStatus,
  ConfuseSpicy,
  ConfuseDry,
  ConfuseSweet,
  ConfuseBitter,
  ConfuseSour,
  AttackUp,
  DefenseUp,
  SpeedUp,
  SpAttackUp,
  SpDefenseUp,
  CriticalUp,
  RandomStatUp,
  EvasionUp,
  RestoreStats,
  MachoBrace,
  ExpShare,
  QuickClaw,
  FriendshipUp,
  CureAttract,
  ChoiceBand,
  Flinch,
  BugPower,
  DoublePrize,
  Repel,
  SoulDew,
  DeepSeaTooth,
  DeepSeaScale,
  CanAlwaysRun,
  PreventEvolve,
  FocusBand,
  LuckyEgg,
  ScopeLens,
  SteelPower,
  Leftovers,
  DragonScale,
  LightBall,
  GroundPower,
  RockPower,
  GrassPower,
  DarkPower,
  FightingPower,
  ElectricPower,
  WaterPower,
  FlyingPower,
  PoisonPower,
  IcePower,
  GhostPower,
  PsychicPower,
  FirePower,
  DragonPower,
  NormalPower,
  UpGrade,
  ShellBell,
  LuckyPunch,
  MetalPowder,
  ThickClub,
  Stick,
};

struct ItemData {
  HoldEffect effect;
  uint8_t param; // holdEffectParam: the percentage, HP or stat it applies to
};

static_assert(sizeof(Item) == 1);

namespace detail {
// Indexed by uint8_t(Item) - 1 (None has no entry).
inline constexpr ItemData ITEM_DATA[ITEMS - 1] = {
    {HoldEffect::RestoreHp, 20}, // BerryJuice
    {HoldEffect::CurePar, 0}, // CheriBerry
    {HoldEffect::CureSlp, 0}, // ChestoBerry
    {HoldEffect::CurePsn, 0}, // PechaBerry
    {HoldEffect::CureBrn, 0}, // RawstBerry
    {HoldEffect::CureFrz, 0}, // AspearBerry
    {HoldEffect::RestorePp, 10}, // LeppaBerry
    {HoldEffect::RestoreHp, 10}, // OranBerry
    {HoldEffect::CureConfusion, 0}, // PersimBerry
    {HoldEffect::CureStatus, 0}, // LumBerry
    {HoldEffect::RestoreHp, 30}, // SitrusBerry
    {HoldEffect::ConfuseSpicy, 8}, // FigyBerry
    {HoldEffect::ConfuseDry, 8}, // WikiBerry
    {HoldEffect::ConfuseSweet, 8}, // MagoBerry
    {HoldEffect::ConfuseBitter, 8}, // AguavBerry
    {HoldEffect::ConfuseSour, 8}, // IapapaBerry
    {HoldEffect::AttackUp, 4}, // LiechiBerry
    {HoldEffect::DefenseUp, 4}, // GanlonBerry
    {HoldEffect::SpeedUp, 4}, // SalacBerry
    {HoldEffect::SpAttackUp, 4}, // PetayaBerry
    {HoldEffect::SpDefenseUp, 4}, // ApicotBerry
    {HoldEffect::CriticalUp, 4}, // LansatBerry
    {HoldEffect::RandomStatUp, 4}, // StarfBerry
    {HoldEffect::None, 0}, // EnigmaBerry
    {HoldEffect::EvasionUp, 10}, // BrightPowder
    {HoldEffect::RestoreStats, 0}, // WhiteHerb
    {HoldEffect::MachoBrace, 0}, // MachoBrace
    {HoldEffect::ExpShare, 0}, // ExpShare
    {HoldEffect::QuickClaw, 20}, // QuickClaw
    {HoldEffect::FriendshipUp, 0}, // SootheBell
    {HoldEffect::CureAttract, 0}, // MentalHerb
    {HoldEffect::ChoiceBand, 0}, // ChoiceBand
    {HoldEffect::Flinch, 10}, // KingsRock
    {HoldEffect::BugPower, 10}, // SilverPowder
    {HoldEffect::DoublePrize, 10}, // AmuletCoin
    {HoldEffect::Repel, 0}, // CleanseTag
    {HoldEffect::SoulDew, 0}, // SoulDew
    {HoldEffect::DeepSeaTooth, 0}, // DeepSeaTooth
    {HoldEffect::DeepSeaScale, 0}, // DeepSeaScale
    {HoldEffect::CanAlwaysRun, 0}, // SmokeBall
    {HoldEffect::PreventEvolve, 0}, // Everstone
    {HoldEffect::FocusBand, 10}, // FocusBand
    {HoldEffect::LuckyEgg, 0}, // LuckyEgg
    {HoldEffect::ScopeLens, 0}, // ScopeLens
    {HoldEffect::SteelPower, 10}, // MetalCoat
    {HoldEffect::Leftovers, 10}, // Leftovers
    {HoldEffect::DragonScale, 10}, // DragonScale
    {HoldEffect::LightBall, 0}, // LightBall
    {HoldEffect::GroundPower, 10}, // SoftSand
    {HoldEffect::RockPower, 10}, // HardStone
    {HoldEffect::GrassPower, 10}, // MiracleSeed
    {HoldEffect::DarkPower, 10}, // BlackGlasses
    {HoldEffect::FightingPower, 10}, // BlackBelt
    {HoldEffect::ElectricPower, 10}, // Magnet
    {HoldEffect::WaterPower, 10}, // MysticWater
    {HoldEffect::FlyingPower, 10}, // SharpBeak
    {HoldEffect::PoisonPower, 10}, // PoisonBarb
    {HoldEffect::IcePower, 10}, // NeverMeltIce
    {HoldEffect::GhostPower, 10}, // SpellTag
    {HoldEffect::PsychicPower, 10}, // TwistedSpoon
    {HoldEffect::FirePower, 10}, // Charcoal
    {HoldEffect::DragonPower, 10}, // DragonFang
    {HoldEffect::NormalPower, 10}, // SilkScarf
    {HoldEffect::UpGrade, 0}, // UpGrade
    {HoldEffect::ShellBell, 8}, // ShellBell
    {HoldEffect::WaterPower, 5}, // SeaIncense
    {HoldEffect::EvasionUp, 5}, // LaxIncense
    {HoldEffect::LuckyPunch, 0}, // LuckyPunch
    {HoldEffect::MetalPowder, 0}, // MetalPowder
    {HoldEffect::ThickClub, 0}, // ThickClub
    {HoldEffect::Stick, 0}, // Stick
    {HoldEffect::None, 0}, // Mail
    {HoldEffect::None, 0}, // Other
};
} // namespace detail

constexpr const ItemData &get(Item item) { return detail::ITEM_DATA[uint8_t(item) - 1]; }

} // namespace hoenn
