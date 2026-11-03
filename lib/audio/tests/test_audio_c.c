#include "pr/audio.h"
#include "pr/audio_mixer.h"

int prTestAudioCApi(void);

// The public headers compiled as C.
int prTestAudioCApi(void) {
    int result = prAudioOutputBackendFromName(PR_SV("default")) != PrAudioOutputBackend_Default;
    result |= prAudioOutputBackendName(PrAudioOutputBackend_Memory).size != 6;
    PrAudioClock clock = {0};
    result |= clock.valid;
    prAudioOutputDestroy(NULL);
    prAudioMixerDestroy(NULL);
    prAudioSourceDestroy(NULL);
    return result;
}
