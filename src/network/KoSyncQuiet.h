#pragma once

#include <string>

/**
 * KoSync push/pull with no UI, for moments when the radio is already coming up anyway: the dashboard refresh on
 * the way to sleep (push) and the boot after a power-button wake (pull). Both expect Wi-Fi to be connected and
 * the reader to be closed, so there is plenty of heap for TLS.
 */
namespace KoSyncQuiet {

enum class Result {
  PUSHED,   // server was behind: uploaded the local position
  PULLED,   // server was ahead: wrote its position into the book's progress file
  IN_SYNC,  // positions match (or the other side is ahead and this direction does not move it)
  SKIPPED,  // not configured, no open EPUB, or not enough memory
  FAILED,   // network, server or mapping error
};

/** Upload the open book's position when the server is behind it. */
Result pushIfAhead(const std::string& epubPath);

/** Apply the server's position to the open book's progress file when the server is ahead. */
Result pullIfBehind(const std::string& epubPath);

/** Power-button wake: join saved Wi-Fi, pullIfBehind() the open book, drop Wi-Fi. Records failures for the reader. */
void pullOnWake();

/** True once after a wake-time pull failed; the reader shows a toast for it. */
bool consumeWakePullFailure();

}  // namespace KoSyncQuiet
