#pragma once
// Hyprutils weak references may refer to unique-owned objects. Never lock()
// them into shared ownership. Borrow only on the compositor event-loop thread;
// no callback may destroy the referent while the predicate accesses it.
template<class WeakFrame>
bool pendingCaptureFrame(const WeakFrame& weak) {
    if (weak.expired()) return false;
    const auto* frame = weak.get();
    return frame && !frame->done() && !frame->m_copyInFlight;
}
