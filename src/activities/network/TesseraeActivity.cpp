#include "TesseraeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#ifndef SIMULATOR
#include <WiFi.h>
#else
// millis() stub for simulator builds
#include <chrono>
static uint32_t millis() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
#endif

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TesseraeClient.h"

namespace {
constexpr const char* TAG = "TESS_ACT";
}

TesseraeActivity::TesseraeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Tesserae", renderer, mappedInput) {}

void TesseraeActivity::onEnter() {
  Activity::onEnter();
  statusMessage_.clear();
  errorMessage_.clear();
  approvalPollMs_ = 0;
  requestUpdate();

#ifndef SIMULATOR
  if (WiFi.status() != WL_CONNECTED) {
    setState(State::WIFI_SELECTION);
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               finish();
                               return;
                             }
                             // WiFi connected — start discover or skip straight to fetch if already paired.
                             if (SETTINGS.tessDeviceToken[0] != '\0') {
                               setState(State::FETCHING_FRAME);
                             } else {
                               setState(State::DISCOVERING);
                             }
                           });
  } else {
    if (SETTINGS.tessDeviceToken[0] != '\0') {
      setState(State::FETCHING_FRAME);
    } else {
      setState(State::DISCOVERING);
    }
  }
#else
  // Simulator: skip WiFi and jump to DISCOVERING to exercise the UI flow.
  if (SETTINGS.tessDeviceToken[0] != '\0') {
    setState(State::FETCHING_FRAME);
  } else {
    setState(State::DISCOVERING);
  }
#endif
}

void TesseraeActivity::onExit() { Activity::onExit(); }

void TesseraeActivity::loop() {
  using MIB = MappedInputManager::Button;

  // Back button or touch-header back always available.
  const bool backPressed = mappedInput.wasPressed(MIB::Back) || TouchHeaderBackButton::wasTapped(mappedInput, renderer);

  switch (state_) {
    case State::WIFI_SELECTION:
      // Handled by WifiSelectionActivity sub-flow; nothing to do here.
      break;

    case State::DISCOVERING:
      // Let the "Registering device..." screen reach the panel before the blocking network call.
      if (!drawn_) break;
      runDiscover();
      break;

    case State::ENTER_URL:
      // Handled by KeyboardEntryActivity sub-flow; nothing to do here.
      break;

    case State::AWAITING_APPROVAL: {
      if (backPressed) {
        exitViaBack();
        return;
      }
      // Retry button (front/OK on button devices).
      if (mappedInput.wasPressed(MIB::Confirm)) {
        setState(State::DISCOVERING);
        return;
      }
      // Auto-poll every APPROVAL_POLL_INTERVAL_MS.
      const uint32_t now = millis();
      if (approvalPollMs_ == 0 || (now - approvalPollMs_) >= APPROVAL_POLL_INTERVAL_MS) {
        approvalPollMs_ = now;
        setState(State::DISCOVERING);
      }
      break;
    }

    case State::FETCHING_FRAME:
      if (!drawn_) break;
      runFetchFrame();
      break;

    case State::DONE:
      // Any press finishes the activity.
      if (backPressed || mappedInput.wasPressed(MIB::Confirm)) {
        finish();
      }
      break;

    case State::ERROR:
      if (backPressed) {
        exitViaBack();
        return;
      }
      // Retry.
      if (mappedInput.wasPressed(MIB::Confirm)) {
        if (SETTINGS.tessDeviceToken[0] != '\0') {
          setState(State::FETCHING_FRAME);
        } else {
          setState(State::DISCOVERING);
        }
      }
      break;
  }
}

void TesseraeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};

  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_TESSERAE), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_TESSERAE));
  }

  drawn_ = true;

  switch (state_) {
    case State::ENTER_URL:
      break;

    case State::WIFI_SELECTION:
      // WifiSelectionActivity is rendered as a separate overlay; nothing here.
      break;

    case State::DISCOVERING:
      renderStatus(tr(STR_TESSERAE_DISCOVERING), tr(STR_TESSERAE_SYNCING), false, false);
      break;

    case State::AWAITING_APPROVAL:
      renderStatus(tr(STR_TESSERAE_AWAITING_APPROVAL), tr(STR_TESSERAE_PAIRING), true, true);
      break;

    case State::FETCHING_FRAME:
      renderStatus(tr(STR_TESSERAE_SYNCING), nullptr, false, false);
      break;

    case State::DONE:
      renderStatus(tr(STR_TESSERAE_SYNCED), tr(STR_TESSERAE_PAIRING_COMPLETE), true, false);
      break;

    case State::ERROR:
      renderStatus(nullptr, errorMessage_.c_str(), true, true);
      break;
  }

  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}

// ── private ───────────────────────────────────────────────────────────────────

void TesseraeActivity::setState(State state) {
  state_ = state;
  drawn_ = false;
  requestUpdate();
}

void TesseraeActivity::runDiscover() {
  if (SETTINGS.tessServerUrl[0] == '\0') {
    promptForUrl();
    return;
  }

  const TesseraeClient::Error err = TesseraeClient::discover();
  LOG_INF(TAG, "discover: %s", TesseraeClient::errorString(err));

  switch (err) {
    case TesseraeClient::OK:
      setState(State::FETCHING_FRAME);
      break;
    case TesseraeClient::AWAITING_APPROVAL:
      approvalPollMs_ = millis();
      setState(State::AWAITING_APPROVAL);
      break;
    case TesseraeClient::NOT_CONFIGURED:
      errorMessage_ = tr(STR_TESSERAE_NOT_CONFIGURED);
      setState(State::ERROR);
      break;
    case TesseraeClient::NETWORK_ERROR:
      errorMessage_ = tr(STR_TESSERAE_ERROR_NETWORK);
      setState(State::ERROR);
      break;
    case TesseraeClient::LOW_MEMORY:
      errorMessage_ = tr(STR_TESSERAE_ERROR_MEMORY);
      setState(State::ERROR);
      break;
    default:
      errorMessage_ = tr(STR_TESSERAE_ERROR_SERVER);
      setState(State::ERROR);
      break;
  }
}

void TesseraeActivity::promptForUrl() {
  setState(State::ENTER_URL);
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TESSERAE_SERVER_URL), "https://",
                                              sizeof(SETTINGS.tessServerUrl) - 1, InputType::Url),
      [this](const ActivityResult& result) {
        const auto* kb = result.isCancelled ? nullptr : std::get_if<KeyboardResult>(&result.data);
        if (kb == nullptr || kb->text.empty() || kb->text == "https://") {
          finish();
          return;
        }
        strlcpy(SETTINGS.tessServerUrl, kb->text.c_str(), sizeof(SETTINGS.tessServerUrl));
        SETTINGS.saveToFile();
        setState(State::DISCOVERING);
      });
}

void TesseraeActivity::runFetchFrame() {
  const TesseraeClient::Error err = TesseraeClient::fetchFrame();
  LOG_INF(TAG, "fetchFrame: %s", TesseraeClient::errorString(err));

  switch (err) {
    case TesseraeClient::OK:
    case TesseraeClient::FRAME_UNCHANGED:
      setState(State::DONE);
      break;
    case TesseraeClient::NO_FRAME:
      errorMessage_ = tr(STR_TESSERAE_NO_FRAME);
      setState(State::ERROR);
      break;
    case TesseraeClient::NOT_PAIRED:
      // Token gone — re-discover.
      setState(State::DISCOVERING);
      break;
    case TesseraeClient::NETWORK_ERROR:
      errorMessage_ = tr(STR_TESSERAE_ERROR_NETWORK);
      setState(State::ERROR);
      break;
    case TesseraeClient::LOW_MEMORY:
      errorMessage_ = tr(STR_TESSERAE_ERROR_MEMORY);
      setState(State::ERROR);
      break;
    default:
      errorMessage_ = tr(STR_TESSERAE_ERROR_SERVER);
      setState(State::ERROR);
      break;
  }
}

void TesseraeActivity::exitViaBack() {
  mappedInput.suppressNextBackRelease();
  finish();
}

void TesseraeActivity::renderStatus(const char* heading, const char* body, bool showBack, bool showRetry) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int headerH = TouchHeaderBackButton::height(metrics, mappedInput);
  const int centerY = metrics.topPadding + headerH + (pageHeight - metrics.topPadding - headerH) / 2;

  if (heading && heading[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - renderer.getLineHeight(UI_10_FONT_ID) / 2, heading, true,
                              EpdFontFamily::BOLD);
  }
  if (body && body[0] != '\0') {
    const Rect textArea{metrics.contentSidePadding, 0, pageWidth - metrics.contentSidePadding * 2, pageHeight};
    UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID,
                                     centerY + renderer.getLineHeight(UI_10_FONT_ID) / 2 + 8, body, 3, false,
                                     EpdFontFamily::REGULAR);
  }

  if (!mappedInput.hasTouchHardware()) {
    // On touch devices Back lives in the header; only button devices need hints.
    const auto labels = mappedInput.mapLabels(
        showBack ? mappedInput.withBackArrow(tr(STR_BACK)) : MappedInputManager::Label{},
        showRetry ? MappedInputManager::Label{tr(STR_TESSERAE_SYNC_NOW)} : MappedInputManager::Label{}, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}
