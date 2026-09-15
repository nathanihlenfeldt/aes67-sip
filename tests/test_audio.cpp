#include <cmath>
#include <thread>
#include <vector>

#include "audio/backend.hpp"
#include "audio/null_backend.hpp"
#include "audio/resampler.hpp"
#include "audio/ring.hpp"
#include "audio/router.hpp"
#include "test_framework.hpp"
#include "util.hpp"

using namespace aes67sip;

namespace {

double rms(const std::vector<float>& buffer) {
  double sum = 0.0;
  for (const float value : buffer) {
    sum += static_cast<double>(value) * value;
  }
  return std::sqrt(sum / static_cast<double>(buffer.size()));
}

std::vector<float> tone(double hz, unsigned rate, size_t frames) {
  std::vector<float> buffer(frames);
  for (size_t i = 0; i < frames; ++i) {
    buffer[i] = static_cast<float>(0.5 * std::sin(2.0 * M_PI * hz * i / rate));
  }
  return buffer;
}

}  // namespace

TEST_CASE(ring_buffer_is_fifo_and_drops_old_data) {
  SpscRing ring(8);
  const float input[] = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(ring.write(input, 6), 6U);
  CHECK_EQ(ring.available(), 6U);

  float output[8] = {0};
  CHECK_EQ(ring.read(output, 4), 4U);
  CHECK_NEAR(output[0], 1.0, 1e-9);
  CHECK_NEAR(output[3], 4.0, 1e-9);
  CHECK_EQ(ring.available(), 2U);

  CHECK_EQ(ring.write(input, 6), 6U);  // the earlier read freed six slots
  CHECK_EQ(ring.available(), 8U);

  // wraparound must preserve order: 5, 6 were kept, then 1..6 were appended
  CHECK_EQ(ring.read(output, 3), 3U);
  CHECK_NEAR(output[0], 5.0, 1e-9);
  CHECK_NEAR(output[1], 6.0, 1e-9);
  CHECK_NEAR(output[2], 1.0, 1e-9);

  ring.discard(ring.available());
  CHECK_EQ(ring.available(), 0U);
  CHECK_EQ(ring.read(output, 4), 0U);
}

TEST_CASE(resampler_handles_integer_ratios) {
  Resampler down(48000, 8000, 1);
  CHECK(down.is_integer_ratio());
  Resampler up(8000, 48000, 1);
  CHECK(up.is_integer_ratio());

  const auto input = tone(1000.0, 48000, 4800);  // 100 ms of 1 kHz
  std::vector<float> downsampled(down.max_output_frames(input.size()));
  const size_t down_frames =
      down.process(input.data(), input.size(), downsampled.data());
  CHECK(down_frames >= 780 && down_frames <= 820);
  downsampled.resize(down_frames);

  const double input_level = rms(input);
  CHECK_NEAR(rms(downsampled), input_level, 0.05);

  std::vector<float> upsampled(up.max_output_frames(down_frames));
  const size_t up_frames =
      up.process(downsampled.data(), down_frames, upsampled.data());
  CHECK(up_frames >= down_frames * 5 && up_frames <= down_frames * 7);
  upsampled.resize(up_frames);
  CHECK_NEAR(rms(upsampled), input_level, 0.05);
}

TEST_CASE(resampler_passthrough_when_rates_match) {
  Resampler resampler(8000, 8000, 2);
  const std::vector<float> input(160, 0.25F);
  std::vector<float> output(resampler.max_output_frames(input.size()));
  CHECK_EQ(resampler.process(input.data(), 80, output.data()), 80U);
  CHECK_NEAR(output[0], 0.25, 1e-9);
  CHECK_NEAR(output[159], 0.25, 1e-9);
}

TEST_CASE(null_backend_paces_and_generates_tone) {
  NullAudioBackend backend(1000.0);
  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = 2;
  format.period_frames = 480;  // 10 ms

  std::string error;
  CHECK(backend.open(format, &error));
  CHECK_EQ(backend.kind(), std::string("null"));

  std::vector<float> block(format.frames_to_samples(480));
  const double started = monotonic_seconds();
  CHECK(backend.read(block.data(), 480, &error));
  CHECK(monotonic_seconds() - started >= 0.008);  // never faster than real time
  CHECK(rms(block) > 0.1);

  CHECK(backend.write(block.data(), 480, &error));
  CHECK(backend.last_written_dbfs() > -20.0);
  backend.close();
}

TEST_CASE(router_moves_audio_both_ways) {
  auto backend = std::make_unique<NullAudioBackend>(1000.0);
  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = 4;
  format.period_frames = 48;

  AudioRouter router(backend.get(), format);
  AudioRouter::LineParams params;
  params.channels = {0, 1};
  params.call_active = true;
  router.add_line(0, params, 8000);

  std::string error;
  CHECK(router.start(&error));
  CHECK(router.running());

  // the audio thread turns the 1 kHz AES67 tone into 8 kHz frames
  std::vector<float> pulled(160, 0.0F);
  size_t total = 0;
  for (int attempt = 0; attempt < 50 && total < 320; ++attempt) {
    total += router.pull_to_sip(0, pulled.data(), 160);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(total > 0);
  CHECK(rms(pulled) > 0.02);

  // audio pushed from the SIP side must reach the AES67 output
  const std::vector<float> remote(160, 0.5F);
  for (int i = 0; i < 10; ++i) {
    router.push_from_sip(0, remote.data(), remote.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const AudioRouter::LineMeters meters = router.meters(0);
  CHECK(meters.to_aes67_dbfs > -20.0);
  CHECK(meters.to_sip_dbfs > -20.0);
  CHECK(meters.rx_frames > 0);
  CHECK(router.ptt_active(0));  // AES67 input energy above the threshold
  CHECK(router.capture_channel_dbfs().size() == 4U);

  // muting must silence the AES67 output
  router.set_line_mute(0, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  CHECK(router.meters(0).to_aes67_dbfs < -40.0);

  router.set_line_mute(0, false);
  router.set_line_call_active(0, false);
  router.stop();
  CHECK(!router.running());
}

TEST_CASE(router_test_tone_reaches_the_aes67_output) {
  auto backend = std::make_unique<NullAudioBackend>(0.0);
  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = 2;
  format.period_frames = 48;

  AudioRouter router(backend.get(), format);
  AudioRouter::LineParams params;
  params.channels = {0, 1};
  router.add_line(0, params, 8000);
  std::string error;
  CHECK(router.start(&error));

  router.start_test_tone(0, 1000.0, 0.5);
  CHECK(router.test_tone_running(0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(router.meters(0).to_aes67_dbfs > -20.0);
  CHECK(router.playback_channel_dbfs()[0] > -20.0);
  router.stop();
}

