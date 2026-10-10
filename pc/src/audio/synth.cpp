#include "spu2.h"
#include "synth.h"

#include <algorithm>
#include <cmath>

namespace pc {
namespace audio {

namespace {

// libsd register numbers this file writes.
const int kRegVolumeLeft = 0x00;
const int kRegVolumeRight = 0x01;
const int kRegPitch = 0x02;
const int kRegAdsr1 = 0x03;
const int kRegAdsr2 = 0x04;
const int kRegKeyOn = 0x15;
const int kRegKeyOff = 0x16;
const int kRegStart = 0x20;

/** A pitch bend range of zero in a bank means the General MIDI default. */
const int kDefaultBendRange = 2;

uint16_t VoiceEntry(int reg, int index) {
    return (uint16_t) ((reg << 8) | ((index % Spu2::kVoices) << 1) | (index / Spu2::kVoices));
}

uint16_t CoreEntry(int reg, int index) {
    return (uint16_t) ((reg << 8) | (index / Spu2::kVoices));
}

uint32_t VoiceBit(int index) {
    return 1u << (index % Spu2::kVoices);
}

} // namespace

Synth::Synth() : voices(), clock(0), mono(false) {
    for (int p = 0; p < kPorts; p++) {
        ports[p].volume = 256;
        ports[p].priority = 0;
        ports[p].effect_volume = 127;
        ports[p].effect_pan = 64;
        ports[p].effect_pitch = 8192;

        for (Channel &channel : ports[p].channels) {
            ResetChannel(channel);
        }
    }
}

void Synth::ResetChannel(Channel &channel) {
    channel.program = 0;
    channel.bank = 0;
    channel.volume = 100;
    channel.expression = 127;
    channel.pan = 64;
    channel.bend = 8192;
    channel.sustain = false;
}

void Synth::SetBank(int port, int bank_no, const uint8_t *hd, uint32_t size, uint32_t spu_address) {
    if (bank_no >= 0 && bank_no < kBanksPerPort) {
        ports[port & 15].banks[bank_no].Load(hd, size, spu_address);
    }
}

void Synth::ResetPort(int port) {
    port &= 15;

    for (int i = 0; i < kVoices; i++) {
        if (voices[i].used && voices[i].port == port && !voices[i].released) {
            Release(i);
        }
    }

    for (Channel &channel : ports[port].channels) {
        ResetChannel(channel);
    }
}

void Synth::SetPortVolume(int port, int volume) {
    ports[port & 15].volume = std::max(volume, 0);

    for (int i = 0; i < kVoices; i++) {
        if (voices[i].used && voices[i].port == (port & 15)) {
            Apply(i, false, true);
        }
    }
}

void Synth::SetPortPriority(int port, int priority) {
    ports[port & 15].priority = priority;
}

void Synth::SetMono(bool value) {
    mono = value;

    for (int i = 0; i < kVoices; i++) {
        if (voices[i].used) {
            Apply(i, false, true);
        }
    }
}

void Synth::Apply(int index, bool pitch, bool volume) {
    Voice         &voice = voices[index];
    const Port    &port = ports[voice.port];
    const Channel &channel = port.channels[voice.channel];
    Spu2          &spu = GetSpu();
    bool           effect = voice.effect_id >= 0;

    if (volume) {
        double level = voice.velocity / 127.0 * voice.zone.volume / 127.0 * channel.volume / 127.0 *
                       channel.expression / 127.0 * port.volume / 256.0;

        if (effect) {
            level *= voice.volume / 127.0;
        }

        int    pan = mono ? 0 : std::min(std::max((effect ? voice.pan : channel.pan) - 64 + voice.zone.pan, -64), 63);
        double left = level * std::min(1.0, (64 - pan) / 64.0);
        double right = level * std::min(1.0, (64 + pan) / 64.0);

        spu.SetParam(VoiceEntry(kRegVolumeLeft, index), (uint16_t) (std::min(left, 1.0) * 0x3FFF));
        spu.SetParam(VoiceEntry(kRegVolumeRight, index), (uint16_t) (std::min(right, 1.0) * 0x3FFF));
    }

    if (pitch) {
        int    bend = (effect ? voice.bend : channel.bend) - 8192;
        int    range = bend > 0 ? voice.zone.bend_range_high : voice.zone.bend_range_low;
        double semitones = voice.note - voice.zone.base_note + voice.zone.transpose + voice.zone.detune / 128.0 +
                           bend / 8192.0 * (range != 0 ? range : kDefaultBendRange);
        double value = 4096.0 * voice.zone.sample_rate / kSampleRate * std::pow(2.0, semitones / 12.0);

        spu.SetParam(VoiceEntry(kRegPitch, index), (uint16_t) std::min(value + 0.5, 16383.0));
    }
}

int Synth::Allocate(int port) {
    int best = -1;

    for (int i = 0; i < kVoices; i++) {
        if (!voices[i].used) {
            return i;
        }

        // Take a released voice before a held one, a lower-priority port's first, then the oldest.
        const Voice &candidate = voices[i];

        if (best < 0) {
            best = i;
            continue;
        }

        const Voice &chosen = voices[best];
        int          candidate_priority = ports[candidate.port].priority;
        int          chosen_priority = ports[chosen.port].priority;

        if (candidate.released != chosen.released) {
            if (candidate.released) {
                best = i;
            }
        } else if (candidate_priority != chosen_priority) {
            if (candidate_priority < chosen_priority) {
                best = i;
            }
        } else if (candidate.age < chosen.age) {
            best = i;
        }
    }

    if (best >= 0 && !voices[best].released && ports[voices[best].port].priority > ports[port].priority) {
        return -1;
    }

    return best;
}

void Synth::NoteOn(int port, int channel, int note, int velocity, int effect_id) {
    Port    &p = ports[port];
    Bank    &bank = p.banks[p.channels[channel].bank % kBanksPerPort].Loaded() ? p.banks[p.channels[channel].bank % kBanksPerPort] : p.banks[0];
    BankZone zones[kZonesPerNote];
    int      count = bank.Find(p.channels[channel].program, note, velocity, zones, kZonesPerNote);
    Spu2    &spu = GetSpu();

    for (int z = 0; z < count; z++) {
        int index = Allocate(port);

        if (index < 0) {
            return;
        }

        Voice &voice = voices[index];
        voice.used = true;
        voice.released = false;
        voice.sustained = false;
        voice.port = port;
        voice.channel = channel;
        voice.note = note;
        voice.effect_id = effect_id;
        voice.velocity = velocity;
        voice.volume = p.effect_volume;
        voice.pan = p.effect_pan;
        voice.bend = p.effect_pitch;
        voice.age = clock++;
        voice.zone = zones[z];

        spu.SetSwitch(CoreEntry(kRegKeyOff, index), VoiceBit(index));
        spu.SetAddr(VoiceEntry(kRegStart, index), voice.zone.spu_address);
        spu.SetParam(VoiceEntry(kRegAdsr1, index), voice.zone.adsr1);
        spu.SetParam(VoiceEntry(kRegAdsr2, index), voice.zone.adsr2);
        Apply(index, true, true);
        spu.SetSwitch(CoreEntry(kRegKeyOn, index), VoiceBit(index));
    }
}

void Synth::Release(int index) {
    GetSpu().SetSwitch(CoreEntry(kRegKeyOff, index), VoiceBit(index));
    voices[index].released = true;
    voices[index].sustained = false;
}

void Synth::NoteOff(int port, int channel, int note, int effect_id) {
    bool sustain = effect_id < 0 && ports[port].channels[channel].sustain;

    for (int i = 0; i < kVoices; i++) {
        Voice &voice = voices[i];

        if (!voice.used || voice.released || voice.port != port || voice.channel != channel || voice.note != note ||
            voice.effect_id != effect_id) {
            continue;
        }

        if (sustain) {
            voice.sustained = true;
        } else {
            Release(i);
        }
    }
}

void Synth::Message(int port, const uint8_t *message) {
    port &= 15;
    int      channel = message[0] & 0x0F;
    Channel &state = ports[port].channels[channel];

    switch (message[0] & 0xF0) {
        case 0x80:
            NoteOff(port, channel, message[1], -1);
            break;
        case 0x90:
            if (message[2] == 0) {
                NoteOff(port, channel, message[1], -1);
            } else {
                NoteOn(port, channel, message[1], message[2], -1);
            }

            break;
        case 0xB0: {
            bool volume = false;

            switch (message[1]) {
                case 0:
                    state.bank = message[2];
                    break;
                case 7:
                    state.volume = message[2];
                    volume = true;
                    break;
                case 10:
                    state.pan = message[2];
                    volume = true;
                    break;
                case 11:
                    state.expression = message[2];
                    volume = true;
                    break;
                case 64:
                    state.sustain = message[2] >= 64;

                    if (!state.sustain) {
                        for (int i = 0; i < kVoices; i++) {
                            if (voices[i].used && voices[i].sustained && voices[i].port == port && voices[i].channel == channel) {
                                Release(i);
                            }
                        }
                    }

                    break;
                case 120:
                case 123:
                    for (int i = 0; i < kVoices; i++) {
                        if (voices[i].used && !voices[i].released && voices[i].port == port && voices[i].channel == channel) {
                            Release(i);
                        }
                    }

                    break;
                case 121: {
                    int program = state.program;
                    int bank = state.bank;
                    ResetChannel(state);
                    state.program = program;
                    state.bank = bank;
                    volume = true;
                    break;
                }
                default:
                    break;
            }

            if (volume) {
                for (int i = 0; i < kVoices; i++) {
                    if (voices[i].used && voices[i].port == port && voices[i].channel == channel) {
                        Apply(i, false, true);
                    }
                }
            }

            break;
        }
        case 0xC0:
            state.program = message[1];
            break;
        case 0xE0:
            state.bend = message[1] | (message[2] << 7);

            for (int i = 0; i < kVoices; i++) {
                if (voices[i].used && voices[i].port == port && voices[i].channel == channel && voices[i].effect_id < 0) {
                    Apply(i, true, false);
                }
            }

            break;
        default:
            break;
    }
}

void Synth::EffectMessage(int port, const uint8_t *message) {
    port &= 15;
    Port &p = ports[port];

    if (message[0] == 0xF9) {
        switch (message[1]) {
            case 0:
                p.effect_volume = message[3];
                break;
            case 1:
                p.effect_pan = message[3];
                break;
            case 2:
                p.effect_pitch = message[3] | (message[4] << 7);
                break;
            default:
                break;
        }

        return;
    }

    if (message[0] != 0xFD) {
        return;
    }

    int key = message[3];
    int id = message[4];

    if (message[1] == 0x10) {
        NoteOff(port, 0, key, id);

        if (message[5] != 0) {
            NoteOn(port, 0, key, message[5], id);
        }

        return;
    }

    for (int i = 0; i < kVoices; i++) {
        Voice &voice = voices[i];

        if (!voice.used || voice.released || voice.port != port || voice.note != key || voice.effect_id != id) {
            continue;
        }

        switch (message[1]) {
            case 0:
                voice.volume = message[5];
                Apply(i, false, true);
                break;
            case 1:
                voice.pan = message[5];
                Apply(i, false, true);
                break;
            case 2:
                voice.bend = message[5] | (message[6] << 7);
                Apply(i, true, false);
                break;
            default:
                break;
        }
    }
}

void Synth::Update() {
    Spu2 &spu = GetSpu();

    for (int i = 0; i < kVoices; i++) {
        if (voices[i].used && !spu.VoiceActive(i / Spu2::kVoices, i % Spu2::kVoices)) {
            voices[i].used = false;
        }
    }
}

} // namespace audio
} // namespace pc
