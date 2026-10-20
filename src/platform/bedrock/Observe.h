#pragma once

// Stage S0 of the overlay rewrite: observation probes (observe.log). Version 2 (r59).
// Compiled into probe builds, or into observe-only builds (--probe=n --observe=y), of the 26.50 line.
// Every call below is a no-op elsewhere.

#ifndef OPTIPISTON_PROBE
#define OPTIPISTON_PROBE 1
#endif

#if (OPTIPISTON_PROBE || defined(OPTIPISTON_OBSERVE_ONLY)) && defined(OPTIPISTON_MC) && OPTIPISTON_MC >= 2651
#define OPTIPISTON_OBSERVE 1
#define OPTIPISTON_OBSERVE_DO(...) __VA_ARGS__
#else
#define OPTIPISTON_OBSERVE 0
#define OPTIPISTON_OBSERVE_DO(...)
#endif

#if OPTIPISTON_OBSERVE
#include <cstdint>
#include <string>

class MovingBlockActor;
#endif

namespace optipiston::platform::observe {

#if OPTIPISTON_OBSERVE
// Opens observe.log and installs the observation hooks. Failure is not fatal.
void install();
void remove();

// Frame boundaries on the render thread.
void frameBegin();
void frameEnd();
// First call per frame marks the point where block actors start drawing.
void renderBlockEntities(bool alphaLayer);
// getDrawPos of a MovingBlockActor; proxy is true for the mod's own proxy actors.
void drawPos(::MovingBlockActor const& actor, float alpha, bool proxy);
// One block handed to a tessellator on a build thread: before the call, and after it with the outcome.
void tessellate(int x, int y, int z, void const* block);
void tessellated(int x, int y, int z, void const* block, bool produced, bool suppressed);
// Build lifetime of one geometry.
void     buildBegin(void const* geometry, int x, int y, int z, unsigned char version);
void     buildEnd();
uint64_t commitBegin();
void     buildCommit(void const* geometry, uint64_t commitBeginQpc);
// Ends the current segment (summary lines) and starts a new one: /optipiston mark <label>.
void mark(std::string const& label);
#endif

} // namespace optipiston::platform::observe
