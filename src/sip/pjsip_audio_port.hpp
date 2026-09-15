#pragma once

#include <pjsua2.hpp>

#include <string>

#include "sip/engine.hpp"

namespace aes67sip {

/**
 * Conference bridge port that connects a line's AES67 audio to a PJSIP call.
 *
 * The port is created at the negotiated codec rate (8 kHz for G.711, 16 kHz for
 * G.722) and carries mono 16 bit PCM, so the router does the only sample rate
 * conversion in the path and the conference bridge never has to resample.
 */
class PjsipAudioPort : public pj::AudioMediaPort {
 public:
  PjsipAudioPort(int line_id, SipMediaSource* media);

  /** Creates and registers the port; `rate` is the codec clock rate. */
  void open(const std::string& name, unsigned rate, unsigned ptime_ms);

  /** Unregisters the port (safe to call twice). */
  void shutdown();

  void onFrameRequested(pj::MediaFrame& frame) override;
  void onFrameReceived(pj::MediaFrame& frame) override;

  int line_id() const { return line_id_; }

 private:
  int line_id_;
  SipMediaSource* media_{nullptr};
  unsigned rate_{8000};
  bool opened_{false};
};

}  // namespace aes67sip
