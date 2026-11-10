#pragma once

#include "audio/audio_manager.hpp"
#include "audio/headphone.hpp"
#include "audio/microphone.hpp"
#include "module.hpp"

class AudioModule : public Module {
public:
  AudioModule()
      : Module("audio"), microphone_(audio_manager_),
        headphone_(audio_manager_) {
    append(microphone_);
    append(headphone_);
  }

private:
  AudioManager audio_manager_;
  MicrophoneControl microphone_;
  HeadphoneControl headphone_;
};
