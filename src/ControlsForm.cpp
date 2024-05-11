// Copyright (c) 2022 Graphcore Ltd. All rights reserved.

#include "ControlsForm.hpp"
#include "custom_widgets/rotator.hpp"
#include <nanogui/graph.h>

#include <PacketSerialisation.h>
#include <cereal/types/string.hpp>

#include <boost/log/trivial.hpp>

#include <iomanip>
#include <fstream>

std::uint32_t convertSampleValue(float value) {
  // Maximum of 16 otherwise latency will be too high:
  std::uint32_t sampleCount = value * 16;
  // Must be at least 1:
  return std::max(sampleCount, 1u);
}

ControlsForm::ControlsForm(nanogui::Screen* screen,
                           PacketMuxer& sender,
                           PacketDemuxer& receiver,
                           VideoPreviewWindow* videoPreview)
    : nanogui::FormHelper(screen),
      hdrHeader(packets::HdrHeader{0,0,0}),
      saveButton(nullptr),
      preview(videoPreview)
{
  window = add_window(nanogui::Vector2i(10, 10), "Control");

  // Scene controls
  add_group("Scene Parameters");
  auto* rotationWheel = new Rotator(window);
  rotationWheel->set_callback([&](float value) {
    float angle = value/(2.f*M_PI) * 360.f;
    serialise(sender, "env_rotation", angle);
  });
  add_widget("Env NIF Rotation", rotationWheel);

  // Camera controls
  add_group("Camera Parameters");
  fovSlider = new nanogui::Slider(window);
  fovSlider->set_fixed_width(250);
  fovSlider->set_callback([&](float value) {
    serialise(sender, "fov", value * 360.f);
  });
  fovSlider->set_value(90.f / 360.f);
  fovSlider->callback()(fovSlider->value());
  add_widget("Field of View", fovSlider);

  // Subscribe to FOV updates from the server (on start-up the server can decide the initial value):
  subs["fov"] = receiver.subscribe("fov", [this](const ComPacket::ConstSharedPacket& packet) {
    float fovRadians = 0.f;
    deserialise(packet, fovRadians);
    BOOST_LOG_TRIVIAL(trace) << "Received FOV update: " << fovRadians;
    fovSlider->set_value(fovRadians / (2.f * M_PI));
  });

  // Render controls:
  add_group("Variable Parameters");
  auto* exposureSlider = new nanogui::Slider(window);
  exposureSlider->set_fixed_width(250);
  exposureSlider->set_callback([&](float value) {
    value = 4.f * (value - 0.5f);
    serialise(sender, "exposure", value);
  });
  exposureSlider->set_value(.5f);
  exposureSlider->callback()(exposureSlider->value());
  add_widget("Exposure", exposureSlider);

  auto* gammaSlider = new nanogui::Slider(window);
  gammaSlider->set_fixed_width(250);
  gammaSlider->set_callback([&](float value) {
    value = 4.f * value;
    serialise(sender, "gamma", value);
  });
  gammaSlider->set_value(2.2f / 4.f);
  gammaSlider->callback()(gammaSlider->value());
  add_widget("Gamma", gammaSlider);


  auto* XSlider = new nanogui::Slider(window);
  XSlider->set_fixed_width(250);
  XSlider->set_callback([&](float value) {
    serialise(sender, "X", value * 1280.f);
  });
  XSlider->set_value(640.f / 1280.f);
  XSlider->callback()(XSlider->value());
  add_widget("X", XSlider);

  auto* YSlider = new nanogui::Slider(window);
  YSlider->set_fixed_width(250);
  YSlider->set_callback([&](float value) {
    serialise(sender, "Y", value * 720.f);
  });
  YSlider->set_value(360.f / 720.f);
  YSlider->callback()(YSlider->value());
  add_widget("Y", YSlider);

  auto* lambda1Slider = new nanogui::Slider(window);
  lambda1Slider->set_fixed_width(250);
  lambda1Slider->set_callback([&](float value) {
    serialise(sender, "lambda1", value * 100.f);
  });
  lambda1Slider->set_value(500.f / 10.f);
  lambda1Slider->callback()(lambda1Slider->value());
  add_widget("Lambda1", lambda1Slider);

  auto* lambda2Slider = new nanogui::Slider(window);
  lambda2Slider->set_fixed_width(250);
  lambda2Slider->set_callback([&](float value) {
    serialise(sender, "lambda2", value * 100.f);
  });
  lambda2Slider->set_value(500.f / 10.f);
  lambda2Slider->callback()(lambda2Slider->value());
  add_widget("Lambda2", lambda2Slider);
  
  // Info/stats
  add_group("Info/Stats");

  auto hist = new nanogui::Graph(window);
  hist->set_caption("Splats per tile");
  add_widget("Workload Balance", hist);

  subs["tile_histogram"] = receiver.subscribe("tile_histogram", [hist](const ComPacket::ConstSharedPacket& packet) {
    std::vector<std::uint32_t> data;
    deserialise(packet, data);
    std::vector<float> dataf;
    dataf.reserve(data.size());

    std::uint32_t max = 0.f;
    for (const auto& v : data) {
      if (v > max) { max = v; }
    }

    const float scale = 1.f / max;
    for (const auto& v : data) {
      dataf.push_back(v * scale);
    }
    std::stringstream ss;
    ss << "max tile: " << max;
    hist->set_header(ss.str());
    hist->set_values(dataf);
  });

  bitRateText = new nanogui::TextBox(window, "-");
  bitRateText->set_editable(false);
  bitRateText->set_units("Mbps");
  bitRateText->set_alignment(nanogui::TextBox::Alignment::Right);
  add_widget("Video rate:", bitRateText);

  frameRateText = new nanogui::TextBox(window, "-");
  frameRateText->set_editable(false);
  frameRateText->set_units("Frames/sec");
  frameRateText->set_alignment(nanogui::TextBox::Alignment::Right);
  add_widget("Frame rate:", frameRateText);

  // Status/stop button:
  add_group("Render Status");

  deviceChooser = new nanogui::ComboBox(window, {"cpu", "ipu"});
  deviceChooser->set_enabled(true);
  deviceChooser->set_side(nanogui::Popup::Side::Left);
  deviceChooser->set_tooltip("Pass a JSON file using '--nif-paths' option to enable selection.");
  deviceChooser->set_callback([&](int index) {
    auto deviceString = deviceChooser->items()[index];
    BOOST_LOG_TRIVIAL(debug) << "Sending new device: " << deviceString;
    serialise(sender, "device", deviceString);
  });
  deviceChooser->set_font_size(16);
  add_widget("Choose render device: ", deviceChooser);

  add_button("Stop", [screen, &sender]() {
    serialise(sender, "stop", true);
    screen->set_visible(false);
  })->set_tooltip("Stop the remote application.");
}

void ControlsForm::set_position(const nanogui::Vector2i& pos) {
  window->set_position(pos);
}

void ControlsForm::savePfm(const std::string& fileName) {
  if (!hdrBuffer.empty()) {
    // Do not want to save a partially received image:
    std::lock_guard<std::mutex> lock(hdrBufferMutex);

    // Last packet so write a PFM file:
    std::ofstream f(fileName, std::ios::binary);
    f << "PF\n";
    f << std::to_string(hdrHeader.width) << " ";
    f << std::to_string(hdrHeader.height) << "\n";
    f << "-1.0\n";
    for (auto r = hdrHeader.height - 1; r >= 0; --r) {
      auto rowStart = reinterpret_cast<char*>(hdrBuffer.data() + (r * hdrHeader.width * 3));
      f.write(rowStart, hdrHeader.width * 3 * sizeof(float));
    }
  }
}
