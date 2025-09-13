#include "pch.h"
#include "NES/NesSoundMixer.h"
#include "NES/NesConsole.h"
#include "NES/NesConstants.h"
#include "NES/NesTypes.h"
#include "Shared/Emulator.h"
#include "Shared/SettingTypes.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"
#include "Utilities/Audio/blip_buf.h"

NesSoundMixer::NesSoundMixer(NesConsole* console)
{
	_clockRate = 0;
	_console = console;
	_mixer = console->GetEmulator()->GetSoundMixer();
	_outputBuffer = new int16_t[NesSoundMixer::MaxSamplesPerFrame];
	_blipBufLeft = blip_new(NesSoundMixer::MaxSamplesPerFrame);
	_blipBufRight = blip_new(NesSoundMixer::MaxSamplesPerFrame);
	_sampleRate = 96000;
}

NesSoundMixer::~NesSoundMixer()
{
	delete[] _outputBuffer;
	_outputBuffer = nullptr;

	blip_delete(_blipBufLeft);
	blip_delete(_blipBufRight);
}

void NesSoundMixer::Serialize(Serializer& s)
{
	SV(_clockRate);
	SV(_sampleRate);

	if(!s.IsSaving()) {
		Reset();
		UpdateRates(true);
	}

	SVArray(_currentOutput, MaxChannelCount);
	SV(_previousOutputLeft);
	SV(_previousOutputRight);
}

void NesSoundMixer::Reset()
{
	_sampleCount = 0;

	_previousOutputLeft = 0;
	_previousOutputRight = 0;
	blip_clear(_blipBufLeft);
	blip_clear(_blipBufRight);

	_timestamps.clear();

	for(uint32_t i = 0; i < MaxChannelCount; i++) {
		_volumes[i] = 1.0;
		_panning[i] = 0;
	}
	memset(_channelOutput, 0, sizeof(_channelOutput));
	memset(_currentOutput, 0, sizeof(_currentOutput));

	UpdateRates(true);
}

void NesSoundMixer::PlayAudioBuffer(uint32_t time)
{
	EndFrame(time);

	const char* audioDbg2 = getenv("MESEN_LIBRETRO_AUDIO_DEBUG");
	int dbg2 = audioDbg2 ? atoi(audioDbg2) : 0;
	static uint64_t nesDbgCounter = 0;
	nesDbgCounter++;
	if(dbg2 > 0 && (nesDbgCounter % (uint64_t)dbg2) == 0) {
		// Print timestamp count and a small summary of the current output channels
		size_t tsCount = _timestamps.size();
		double sumAbs = 0.0;
		for(size_t i = 0; i < MaxChannelCount; ++i) sumAbs += fabs(_currentOutput[i]);
		fprintf(stderr, "[mesen] NesSoundMixer debug2: timestamps=%zu sampleCount=%u sumAbsCurrentOutput=%.2f hasPanning=%d\n",
				tsCount, (unsigned)_sampleCount, sumAbs, (int)_hasPanning);
		fprintf(stderr, "[mesen] NesSoundMixer debug2: channelOutputs:");
		for(size_t i = 0; i < std::min<size_t>(8, MaxChannelCount); ++i) fprintf(stderr, " %d", (int)_currentOutput[i]);
		fprintf(stderr, "\n");
	}

	int16_t* out = _outputBuffer + (_sampleCount * 2);
	size_t sampleCount = blip_read_samples(_blipBufLeft, out, NesSoundMixer::MaxSamplesPerFrame, 1);

	if(_hasPanning) {
		blip_read_samples(_blipBufRight, out + 1, NesSoundMixer::MaxSamplesPerFrame, 1);
	} else {
		//Copy left channel to right channel (optimization - when no panning is used)
		for(size_t i = 0; i < sampleCount * 2; i += 2) {
			out[i + 1] = out[i];
		}
	}

	_sampleCount += sampleCount;

	// Optional diagnostic: print a small snapshot of the raw NES mixer output before
	// it is forwarded to the shared SoundMixer. Gate behind MESEN_LIBRETRO_AUDIO_DEBUG
	// to avoid noisy logs. The env var is interpreted as an integer interval.
	const char* audioDbg = getenv("MESEN_LIBRETRO_AUDIO_DEBUG");
	int dbgInterval = audioDbg ? atoi(audioDbg) : 0;
	static uint64_t nesAudioCounter = 0;
	nesAudioCounter++;
	if(dbgInterval > 0 && (nesAudioCounter % (uint64_t)dbgInterval) == 0) {
		size_t frames = sampleCount;
		bool allZero = true;
		int16_t maxAbs = 0;
		// samples are interleaved stereo in out (left,right)
		for(size_t i = 0; i < frames * 2; ++i) {
			int16_t v = out[i];
			if(v != 0) allZero = false;
			int16_t absV = (v == INT16_MIN) ? INT16_MAX : (v < 0 ? -v : v);
			if(absV > maxAbs) maxAbs = absV;
		}
		fprintf(stderr, "[mesen] NesSoundMixer debug: frames=%zu allZero=%d maxAbs=%d\n", frames, (int)allZero, (int)maxAbs);
		// print first up to 6 samples
		size_t printCount = std::min<size_t>(6, frames * 2);
		fprintf(stderr, "[mesen] NesSoundMixer debug: samples:");
		for(size_t i = 0; i < printCount; ++i) fprintf(stderr, " %d", (int)out[i]);
		fprintf(stderr, "\n");
	}

	if(_console->GetVsMainConsole()) {
		//Keep samples in buffer if this is the VS dualsystem sub console - the main console will read them and play them
		return;
	}

	NesConfig& cfg = _console->GetNesConfig();
	if(_console->GetVsSubConsole()) {
		ProcessVsDualSystemAudio();
	}

	switch(cfg.StereoFilter) {
		case StereoFilterType::None: break;
		case StereoFilterType::Delay: _stereoDelay.ApplyFilter(_outputBuffer, _sampleCount, _sampleRate, cfg.StereoDelay); break;
		case StereoFilterType::Panning: _stereoPanning.ApplyFilter(_outputBuffer, _sampleCount, cfg.StereoPanningAngle); break;
		case StereoFilterType::CombFilter: _stereoCombFilter.ApplyFilter(_outputBuffer, _sampleCount, _sampleRate, cfg.StereoCombFilterDelay, cfg.StereoCombFilterStrength); break;
	}

	_mixer->PlayAudioBuffer(_outputBuffer, (uint32_t)_sampleCount, 96000);
	_sampleCount = 0;

	UpdateRates(false);
}

void NesSoundMixer::ProcessVsDualSystemAudio()
{
	NesConfig& cfg = _console->GetNesConfig();

	//If this is a VS dualsystem game
	if(cfg.VsDualAudioOutput == VsDualOutputOption::SubSystemOnly) {
		//Mute the main system's sound
		memset(_outputBuffer, 0, _sampleCount * sizeof(int16_t));
	}

	NesSoundMixer* subMixer = _console->GetVsSubConsole()->GetSoundMixer();
	if(cfg.VsDualAudioOutput != VsDualOutputOption::MainSystemOnly) {
		size_t i;
		for(i = 0; i < _sampleCount && subMixer->_sampleCount; i++) {
			_outputBuffer[i * 2] += subMixer->_outputBuffer[i * 2];
			_outputBuffer[i * 2 + 1] += subMixer->_outputBuffer[i * 2 + 1];
		}

		if(i < subMixer->_sampleCount) {
			size_t samplesToCopy = subMixer->_sampleCount - i;
			memmove(subMixer->_outputBuffer, subMixer->_outputBuffer + i * 2, samplesToCopy * 2 * sizeof(int16_t));
			subMixer->_sampleCount = samplesToCopy;
		}
	} else {
		subMixer->_sampleCount = 0;
	}
}

void NesSoundMixer::SetRegion(ConsoleRegion region)
{
	UpdateRates(true);
}

void NesSoundMixer::UpdateRates(bool forceUpdate)
{
	uint32_t clockRate = NesConstants::GetClockRate(_console->GetRegion());
	if(forceUpdate || _clockRate != clockRate) {
		_clockRate = clockRate;

		blip_set_rates(_blipBufLeft, _clockRate, _sampleRate);
		blip_set_rates(_blipBufRight, _clockRate, _sampleRate);
	}

	NesConfig& cfg = _console->GetNesConfig();
	bool hasPanning = false;
	for(uint32_t i = 0; i < MaxChannelCount; i++) {
		_volumes[i] = cfg.ChannelVolumes[i] / 100.0;
		_panning[i] = (cfg.ChannelPanning[i] + 100) / 100.0;
		if(_panning[i] != 1.0) {
			if(!_hasPanning) {
				blip_clear(_blipBufLeft);
				blip_clear(_blipBufRight);
			}
			hasPanning = true;
		}
	}
	_hasPanning = hasPanning;
}

double NesSoundMixer::GetChannelOutput(AudioChannel channel, bool forRightChannel)
{
	if(forRightChannel) {
		return _currentOutput[(int)channel] * _volumes[(int)channel] * _panning[(int)channel];
	} else {
		return _currentOutput[(int)channel] * _volumes[(int)channel] * (2.0 - _panning[(int)channel]);
	}
}

int16_t NesSoundMixer::GetOutputVolume(bool forRightChannel)
{
	double squareOutput = GetChannelOutput(AudioChannel::Square1, forRightChannel) + GetChannelOutput(AudioChannel::Square2, forRightChannel);
	double tndOutput = GetChannelOutput(AudioChannel::DMC, forRightChannel) + 2.7516713261 * GetChannelOutput(AudioChannel::Triangle, forRightChannel) + 1.8493587125 * GetChannelOutput(AudioChannel::Noise, forRightChannel);

	uint16_t squareVolume = (uint16_t)((95.88*5000.0) / (8128.0 / squareOutput + 100.0));
	uint16_t tndVolume = (uint16_t)((159.79*5000.0) / (22638.0 / tndOutput + 100.0));

	return (int16_t)(squareVolume + tndVolume +
		GetChannelOutput(AudioChannel::FDS, forRightChannel) * 20 +
		GetChannelOutput(AudioChannel::MMC5, forRightChannel) * 43 +
		GetChannelOutput(AudioChannel::Namco163, forRightChannel) * 20 +
		GetChannelOutput(AudioChannel::Sunsoft5B, forRightChannel) * 15 +
		GetChannelOutput(AudioChannel::VRC6, forRightChannel) * 5 +
		GetChannelOutput(AudioChannel::VRC7, forRightChannel));
}
void NesSoundMixer::AddDelta(AudioChannel channel, uint32_t time, int16_t delta)
{
	if(delta != 0) {
		_timestamps.push_back(time);
		_channelOutput[(int)channel][time] += delta;
	}
}

void NesSoundMixer::EndFrame(uint32_t time)
{
	sort(_timestamps.begin(), _timestamps.end());
	_timestamps.erase(std::unique(_timestamps.begin(), _timestamps.end()), _timestamps.end());

	for(size_t i = 0, len = _timestamps.size(); i < len; i++) {
		uint32_t stamp = _timestamps[i];
		for(uint32_t j = 0; j < MaxChannelCount; j++) {
			_currentOutput[j] += _channelOutput[j][stamp];
		}

		int16_t currentOutput = GetOutputVolume(false) * 4;
		blip_add_delta(_blipBufLeft, stamp, (int)(currentOutput - _previousOutputLeft));
		_previousOutputLeft = currentOutput;

		if(_hasPanning) {
			currentOutput = GetOutputVolume(true) * 4;
			blip_add_delta(_blipBufRight, stamp, (int)(currentOutput - _previousOutputRight));
			_previousOutputRight = currentOutput;
		}
	}

	blip_end_frame(_blipBufLeft, time);
	if(_hasPanning) {
		blip_end_frame(_blipBufRight, time);
	}

	//Reset everything
	_timestamps.clear();
	memset(_channelOutput, 0, sizeof(_channelOutput));
}

