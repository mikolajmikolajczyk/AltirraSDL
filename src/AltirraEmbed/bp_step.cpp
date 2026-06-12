// AltirraCore — breakpoint / step / advanceFrame / frameRefresh plumbing.
//
// All four touch the debugger directly: BPs go through Altirra's
// IATDebugger so the simulator's own Advance() loop respects them
// (vs. our JS-side polling which only sees post-frame state).
//
// frameRefresh is the snapshot/advance/restore trick. It's known-broken
// (Apply leaves the sim in an inconsistent debugger state, so the next
// StepInto bails out and turns into a full-frame run). Embind keeps it
// exposed so a future research outcome can re-wrap it — the JS host has
// already stopped calling it.

#include <stdafx.h>

#ifdef __EMSCRIPTEN__

#include "altirra_core.h"
#include "embed_init.h"

#include <emscripten/console.h>
#include <emscripten/val.h>
#include <vd2/system/error.h>
#include <vd2/system/refcount.h>
#include <at/atcore/serializable.h>

#include "simulator.h"
#include "debugger.h"

extern ATSimulator g_sim;

extern bool ATBridgeNullVideoDisplayConsumeFramePosted(IVDVideoDisplay *display);
extern IATDebugger *ATGetDebugger();

using emscripten::val;

void AltirraCore::setBreakpoints(val addrs) {
    // Register BPs with Altirra's debugger so they're checked inside the
    // simulator's own Advance() loop (vs. our JS-side polling which only
    // sees post-frame state — Advance() runs full frames at a time, so
    // JS-side checks never observe transient PCs).
    IATDebugger* dbg = ATGetDebugger();
    if (!dbg) {
        emscripten_console_error("[altirra-embed] setBreakpoints: no debugger");
        return;
    }
    // Wipe prior BPs we registered.
    for (uint32_t id : mDebugBpIds) (void)dbg->ClearUserBreakpoint(id, false);
    mDebugBpIds.clear();
    const auto len = addrs["length"].as<unsigned>();
    for (unsigned i = 0; i < len; ++i) {
        ATDebuggerBreakpointInfo info;
        info.mTargetIndex = 0;
        info.mAddress     = (uint32_t)(addrs[i].as<int>() & 0xffff);
        info.mLength      = 1;
        info.mbBreakOnPC  = true;
        info.mpCondition  = nullptr;
        try {
            const uint32_t id = dbg->SetBreakpoint(-1, info);
            mDebugBpIds.push_back(id);
        } catch (const MyError& e) {
            emscripten_console_errorf("[altirra-embed] BP set failed @ %04x: %s",
                info.mAddress, e.c_str());
        }
    }
}

int AltirraCore::advanceFrame(val /*trapFnLegacy*/) {
    // Sim may be halted from a prior BP hit / step — resume it so Advance()
    // actually advances. (We do NOT Resume after a trap, see comment at
    // end of this function.)
    if (!g_sim.IsRunning()) g_sim.Resume();
    constexpr int kMaxIters = 4 * 35568;
    int iters = 0;
    while (iters < kMaxIters) {
        const auto r = g_sim.Advance(false);
        ++iters;
        // Debugger BP hits halt the simulator → Stopped. Do NOT Resume
        // here — leave the sim halted so the host's next step() /
        // advanceFrame() can decide what to do. (StepInto bails out if
        // the sim is already running, which would turn every Step into a
        // full-frame run.)
        if (r == ATSimulator::kAdvanceResult_Stopped) break;
        if (ATBridgeNullVideoDisplayConsumeFramePosted(g_pNullDisplay)) break;
    }
    return iters;
}

void AltirraCore::frameRefresh() {
    // Snapshot → advance one frame → capture pixels → Apply. Known-broken:
    // Apply leaves the sim in `mbRunning=true` plus inconsistent debugger
    // linkage, so the next StepInto bails out early and turns into a
    // full-frame run. The JS host stopped calling this in `61414f2`
    // (madside) — Embind exposure stays so a future research outcome can
    // re-wrap it under a more honest name.
    IATDebugger* dbg = ATGetDebugger();
    std::vector<ATDebuggerBreakpointInfo> savedBps;
    if (dbg) {
        for (uint32_t id : mDebugBpIds) {
            ATDebuggerBreakpointInfo info;
            if (dbg->GetBreakpointInfo(id, info)) savedBps.push_back(info);
            dbg->ClearUserBreakpoint(id, false);
        }
        mDebugBpIds.clear();
        dbg->Break();
    }

    vdrefptr<IATSerializable> snap;
    vdrefptr<IATSerializable> snapInfo;
    try { g_sim.CreateSnapshot(~snap, ~snapInfo); }
    catch (...) {
        if (dbg) {
            for (auto& info : savedBps) {
                try { mDebugBpIds.push_back(dbg->SetBreakpoint(-1, info)); }
                catch (...) {}
            }
        }
        return;
    }

    if (!g_sim.IsRunning()) g_sim.Resume();
    constexpr int kMaxIters = 2 * 35568;
    for (int i = 0; i < kMaxIters; ++i) {
        g_sim.Advance(false);
        if (ATBridgeNullVideoDisplayConsumeFramePosted(g_pNullDisplay)) break;
    }
    capturePixels();
    mPixelsCached = true;

    if (snap) {
        try { g_sim.ApplySnapshot(*snap, nullptr); }
        catch (...) {}
    }
    if (g_sim.IsRunning()) g_sim.Pause();

    if (dbg) {
        for (auto& info : savedBps) {
            try { mDebugBpIds.push_back(dbg->SetBreakpoint(-1, info)); }
            catch (...) {}
        }
    }
}

int AltirraCore::step() {
    // Altirra's single-instruction step via the debugger: arms
    // ATCPUStepCondition::CreateSingleStep() on the CPU and Resume()s —
    // the CPU halts itself at the next instruction boundary, sim returns
    // kAdvanceResult_Stopped from its next Advance().
    IATDebugger* dbg = ATGetDebugger();
    if (!dbg) { g_sim.Resume(); return 0; }
    dbg->StepInto(kATDebugSrcMode_Disasm);

    int iters = 0;
    constexpr int kMaxIters = 2 * 35568;
    for (; iters < kMaxIters; ++iters) {
        const auto r = g_sim.Advance(false);
        if (r == ATSimulator::kAdvanceResult_Stopped) break;
    }
    return iters;
}

#endif  // __EMSCRIPTEN__
