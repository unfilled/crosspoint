#include "ReaderSession.h"

#include <algorithm>

namespace {
constexpr uint32_t MAX_DWELL_SECONDS = 1800;
}  // namespace

uint16_t ReaderSession::clampProgress(const int progressBp) {
  return static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
}

void ReaderSession::noteTurn(const bool forward, const bool succeeded) { pendingForward = forward && succeeded; }

void ReaderSession::credit(const uint32_t monotonicMs) {
  const uint32_t elapsedSeconds = (monotonicMs - anchorMs) / 1000;
  if (duration == 0) {
    startEpoch = anchorEpoch;
    startProgress = anchorProgress;
    endProgress = anchorProgress;
  }
  duration += std::min(elapsedSeconds, MAX_DWELL_SECONDS);
}

void ReaderSession::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch, const int progressBp) {
  const uint16_t progress = clampProgress(progressBp);
  if (pendingForward && hasAnchor && anchorEpoch > 0) {
    credit(monotonicMs);
    endProgress = std::max(endProgress, progress);
  }

  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  anchorProgress = progress;
  hasAnchor = true;
  pendingForward = false;
}

void ReaderSession::finalize(const uint32_t monotonicMs) {
  // The still-open page has no following turn; credit its dwell at flush time
  // or it is lost. Re-anchoring keeps a repeated flush from double-counting.
  if (hasAnchor && anchorEpoch > 0) {
    credit(monotonicMs);
  }
  anchorMs = monotonicMs;
}

void ReaderSession::reset() {
  anchorMs = 0;
  anchorEpoch = 0;
  startEpoch = 0;
  duration = 0;
  anchorProgress = 0;
  startProgress = 0;
  endProgress = 0;
  hasAnchor = false;
  pendingForward = false;
}
