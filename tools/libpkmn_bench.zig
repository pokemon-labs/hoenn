//! libpkmn's Gen I speed with the same frame as tools/bench.cpp: null log,
//! Chance on (Rational(f64), like ours), Calc off; or, built with
//! libpkmn_bench_log_options.zig, as bench --log: the real log (reset before
//! each update), Chance and Calc off. libpkmn's options are comptime, so each
//! mode is its own build. Builds every battle first (libpkmn's random teams:
//! it has no Showdown team parser), then times from the first battle's first
//! update to the last battle's last update.
//!
//! Build (from the libpkmn checkout, `E`; OPTIONS is
//! tools/libpkmn_bench_options.zig or tools/libpkmn_bench_log_options.zig):
//!   zig build-exe -O ReleaseFast --dep pkmn --dep build_options \
//!     -Mroot=tools/libpkmn_bench.zig --dep build_options \
//!     -Mpkmn=$E/src/lib/pkmn.zig -Mbuild_options=$OPTIONS
//!
//! Usage: libpkmn_bench BATTLES [SEED]

const build_options = @import("build_options");
const pkmn = @import("pkmn");
const std = @import("std");

const log_on = build_options.log orelse false;
const chance_on = build_options.chance orelse false;

const Battle = @TypeOf(pkmn.gen1.helpers.Battle.random(@constCast(&pkmn.PSRNG.init(0)), .{}));

// xorshift64: cheap, and good enough for picking choices.
const Cheap = struct {
    s: u64,
    fn next(self: *Cheap) u64 {
        self.s ^= self.s << 13;
        self.s ^= self.s >> 7;
        self.s ^= self.s << 17;
        return self.s;
    }
    fn range(self: *Cheap, n: u8) u8 {
        return @intCast(((self.next() >> 32) * n) >> 32);
    }
};

pub fn main(init: std.process.Init) !void {
    const allocator = init.arena.allocator();
    const args = try init.minimal.args.toSlice(allocator);
    if (args.len < 2) {
        std.debug.print("usage: libpkmn_bench BATTLES [SEED]\n", .{});
        std.process.exit(2);
    }
    const battles = try std.fmt.parseUnsigned(usize, args[1], 10);
    const seed = if (args.len > 2) try std.fmt.parseUnsigned(u64, args[2], 10) else 1;

    var random = pkmn.PSRNG.init(seed);
    const all = try allocator.alloc(Battle, battles);
    for (all) |*b| b.* = pkmn.gen1.helpers.Battle.random(&random, .{});

    var chance = pkmn.gen1.Chance(pkmn.Rational(f64)){ .probability = .{} };
    var buf: [pkmn.LOGS_SIZE]u8 = undefined;
    var writer: pkmn.protocol.Writer = .{ .buffer = &buf };
    const log = if (log_on) pkmn.protocol.FixedLog{ .writer = &writer } else pkmn.protocol.NULL;
    const options = pkmn.battle.options(
        log,
        if (chance_on) &chance else pkmn.gen1.chance.NULL,
        pkmn.gen1.calc.NULL,
    );

    var rng = Cheap{ .s = if (seed == 0) 1 else seed };
    var choices: [pkmn.CHOICES_SIZE]pkmn.Choice = undefined;
    var updates: u64 = 0;
    var turns: u64 = 0;

    const timer = std.Io.Clock.awake.now(init.io);
    for (all) |*battle| {
        var c1 = pkmn.Choice{};
        var c2 = pkmn.Choice{};
        while (true) {
            if (chance_on) options.chance.reset();
            if (log_on) writer.reset();
            const result = try battle.update(c1, c2, &options);
            updates += 1;
            if (result.type != .None) break;
            var n = battle.choices(.P1, result.p1, &choices);
            c1 = choices[rng.range(n)];
            n = battle.choices(.P2, result.p2, &choices);
            c2 = choices[rng.range(n)];
        }
        turns += battle.turn;
    }
    const ns: u64 = @intCast(timer.untilNow(init.io, .awake).toNanoseconds());

    const seconds = @as(f64, @floatFromInt(ns)) / 1e9;
    std.debug.print(
        "libpkmn_bench{s}: {d} battles, {d} updates in {d:.3} s: {d:.0} updates/s ({d:.1} turns per battle)\n",
        .{
            if (log_on) " (log)" else "",                      battles,
            updates,                                           seconds,
            @as(f64, @floatFromInt(updates)) / seconds,
            @as(f64, @floatFromInt(turns)) / @as(f64, @floatFromInt(battles)),
        },
    );
}
