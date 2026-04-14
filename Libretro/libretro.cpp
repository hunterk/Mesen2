#include <string>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <vector>
#include <iterator>
#include "LibretroKeyManager.h"
#include "LibretroMessageManager.h"
#include "libretro.h"
#include "../Core/Shared/Audio/SoundMixer.h"


#include "../Core/NES/NesConsole.h"
#include "../Core/Shared/Video/VideoDecoder.h"
#include "../Core/Shared/Video/BaseVideoFilter.h"
#include "../Core/Shared/Video/VideoRenderer.h"
#include "../Core/Shared/ColorUtilities.h"
#include "../Core/NES/NesMemoryManager.h"
#include "../Core/NES/BaseMapper.h"
#include "../Core/Shared/EmuSettings.h"
#include "../Core/Shared/CheatManager.h"
#include "../Core/NES/HdPacks/HdData.h"
#include "../Core/Shared/SaveStateManager.h"
#include "../Core/Debugger/DebugTypes.h"
#include "../Core/NES/GameDatabase.h"
#include "../Core/NES/NesSoundMixer.h"
#include "../Core/Shared/Interfaces/IAudioDevice.h"
#include "../Utilities/FolderUtilities.h"
#include "../Utilities/HexUtilities.h"
#include "../Utilities/VirtualFile.h"

#define DEVICE_AUTO               RETRO_DEVICE_JOYPAD
#define DEVICE_GAMEPAD            RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)
#define DEVICE_POWERPAD           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)
#define DEVICE_FAMILYTRAINER      RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2)
#define DEVICE_PARTYTAP           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 3)
#define DEVICE_PACHINKO           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 4)
#define DEVICE_EXCITINGBOXING     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 5)
#define DEVICE_KONAMIHYPERSHOT    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 6)
#define DEVICE_SNESGAMEPAD        RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 7)
#define DEVICE_VBGAMEPAD          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 8)
#define DEVICE_ZAPPER             RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 0)
#define DEVICE_OEKAKIDS           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 1)
#define DEVICE_BANDAIHYPERSHOT    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_POINTER, 2)
#define DEVICE_ARKANOID           RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0)
#define DEVICE_HORITRACK          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 1)
#define DEVICE_SNESMOUSE          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 2)
#define DEVICE_ASCIITURBOFILE     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 0)
#define DEVICE_BATTLEBOX          RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 1)
#define DEVICE_FOURPLAYERADAPTER  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_NONE, 2)

static retro_log_printf_t logCallback = nullptr;
retro_environment_t env_cb = nullptr;
static unsigned _inputDevices[5] = { DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO, DEVICE_AUTO };
static bool _hdPacksEnabled = false;
static string _mesenVersion = "";
static int32_t _saveStateSize = -1;
static bool _shiftButtonsClockwise = false;
static int32_t _audioSampleRate = 48000;

//Include game database as a byte array (representing the MesenDB.txt file)
#include "MesenDB.inc"

static std::unique_ptr<Emulator> _emu;
// shared_ptr to the concrete NesConsole (may be null until emulator initialized)
static std::shared_ptr<NesConsole> _console;
static std::shared_ptr<LibretroKeyManager> _keyManager;
static std::unique_ptr<LibretroMessageManager> _message_manager;
static retro_audio_sample_batch_t _audioSampleBatch = nullptr;
static retro_video_refresh_t _videoRefresh = nullptr;
// Saved input callbacks (set by frontend before core may be ready)
static retro_input_state_t _savedGetInputState = nullptr;
static retro_input_poll_t _savedPollInput = nullptr;
// Store user palette locally until renderer API is wired
static std::vector<uint32_t> _userRgbPalette;
// keep selected filter string until we map to EmuSettings
static std::string _selectedNtscFilter;
// local stubs for settings not yet mapped
static bool _videoFilterRaw = false;
static int _ppuNmiBefore = 0;
static int _ppuNmiAfter = 0;
static uint8_t _overscanLeft = 0, _overscanRight = 0, _overscanTop = 0, _overscanBottom = 0;
// store other high-level choices locally until mapped to EmuSettings
static std::string _selectedAspectRatio;
static std::string _selectedRegion;
static std::string _selectedRamState;
// new local stub for screen rotation
static int _screenRotation = 0;
// Geometry tracking
static bool _geometryDirty = false;
static VideoConfig _lastVideoConfig = {};
static NesConfig _lastNesConfig = {};
static uint32_t _lastReportedWidth = 256;
static uint32_t _lastReportedHeight = 240;

// Small audio device implementation that forwards audio from the core's
// SoundMixer to libretro's audio callback (_audioSampleBatch).
class LibretroAudioDevice : public IAudioDevice {
public:
	LibretroAudioDevice() {
		fprintf(stderr, "[libretro] LibretroAudioDevice created\n");
	}
	~LibretroAudioDevice() override {
		fprintf(stderr, "[libretro] LibretroAudioDevice destroyed\n");
	}

	void PlayBuffer(int16_t *soundBuffer, uint32_t bufferSize, uint32_t sampleRate, bool isStereo) override {
		// bufferSize is number of frames. _audioSampleBatch expects interleaved int16_t samples and frame count.
		if(!_audioSampleBatch) {
			fprintf(stderr, "[libretro] PlayBuffer: _audioSampleBatch is null!\n");
			return;
		}

		static int audioCallCount = 0;
		if(++audioCallCount % 100 == 0) {
			fprintf(stderr, "[libretro] PlayBuffer called (call #%d): bufferSize=%u, sampleRate=%u, stereo=%d\n", 
				audioCallCount, bufferSize, sampleRate, isStereo);
		}

		// Choose output buffer pointer (interleaved samples)
		const int16_t* outPtr = nullptr;
		if(isStereo) {
			outPtr = soundBuffer;
		} else {
			// Convert mono -> stereo by duplicating samples into a temporary buffer
			_monoBuffer.resize(bufferSize * 2);
			for(uint32_t i = 0; i < bufferSize; ++i) {
				int16_t s = soundBuffer[i];
				_monoBuffer[i * 2 + 0] = s;
				_monoBuffer[i * 2 + 1] = s;
			}
			outPtr = _monoBuffer.data();
		}

		// Forward frames (bufferSize is frame count)
		_audioSampleBatch((const int16_t*)outPtr, (size_t)bufferSize);
	}

	void Stop() override {}
	void Pause() override {}
	void ProcessEndOfFrame() override {}

	string GetAvailableDevices() override { return string(); }
	void SetAudioDevice(string deviceName) override { (void)deviceName; }
	AudioStatistics GetStatistics() override { return AudioStatistics(); }

private:
	// reused buffer for mono->stereo conversion
	std::vector<int16_t> _monoBuffer;
};

static std::unique_ptr<LibretroAudioDevice> _audioDevice;

static constexpr const char* MesenNtscFilter = "mesen_ntsc_filter";
static constexpr const char* MesenPalette = "mesen_palette";
static constexpr const char* MesenSpriteLimit = "mesen_sprite_limit";
static constexpr const char* MesenEnablePalBorders = "mesen_enable_pal_borders";
static constexpr const char* MesenSpritesEnabled = "mesen_sprites_enabled";
static constexpr const char* MesenBackgroundEnabled = "mesen_background_enabled";
static constexpr const char* MesenAllowInvalidInput = "mesen_allow_invalid_input";
static constexpr const char* MesenDisableGameGenieBusConflicts = "mesen_disable_game_genie_bus_conflicts";
static constexpr const char* MesenRandomizeMapperPowerOnState = "mesen_randomize_mapper_power_on_state";
static constexpr const char* MesenRandomizeCpuPpuAlignment = "mesen_randomize_cpu_ppu_alignment";
static constexpr const char* MesenOverclock = "mesen_overclock";
static constexpr const char* MesenOverclockType = "mesen_overclock_type";
static constexpr const char* MesenOverscanLeft = "mesen_overscan_left";
static constexpr const char* MesenOverscanRight = "mesen_overscan_right";
static constexpr const char* MesenOverscanTop = "mesen_overscan_up";
static constexpr const char* MesenOverscanBottom = "mesen_overscan_down";
static constexpr const char* MesenAspectRatio = "mesen_aspect_ratio";
static constexpr const char* MesenRegion = "mesen_region";
static constexpr const char* MesenRamState = "mesen_ramstate";
static constexpr const char* MesenControllerTurboSpeed = "mesen_controllerturbospeed";
static constexpr const char* MesenFdsAutoSelectDisk = "mesen_fdsautoinsertdisk";
static constexpr const char* MesenFdsFastForwardLoad = "mesen_fdsfastforwardload";
static constexpr const char* MesenHdPacks = "mesen_hdpacks";
static constexpr const char* MesenScreenRotation = "mesen_screenrotation";
static constexpr const char* MesenFakeStereo = "mesen_fake_stereo";
static constexpr const char* MesenMuteTriangleUltrasonic = "mesen_mute_triangle_ultrasonic";
static constexpr const char* MesenReduceDmcPopping = "mesen_reduce_dmc_popping";
static constexpr const char* MesenSwapDutyCycle = "mesen_swap_duty_cycle";
static constexpr const char* MesenDisableNoiseModeFlag = "mesen_disable_noise_mode_flag";
static constexpr const char* MesenShiftButtonsClockwise = "mesen_shift_buttons_clockwise";
static constexpr const char* MesenAudioSampleRate = "mesen_audio_sample_rate";

uint32_t defaultPalette[0x40] { 0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00, 0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00, 0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22, 0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000, 0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5, 0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000 };
uint32_t unsaturatedPalette[0x40] { 0xFF6B6B6B, 0xFF001E87, 0xFF1F0B96, 0xFF3B0C87, 0xFF590D61, 0xFF5E0528, 0xFF551100, 0xFF461B00, 0xFF303200, 0xFF0A4800, 0xFF004E00, 0xFF004619, 0xFF003A58, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB2B2B2, 0xFF1A53D1, 0xFF4835EE, 0xFF7123EC, 0xFF9A1EB7, 0xFFA51E62, 0xFFA52D19, 0xFF874B00, 0xFF676900, 0xFF298400, 0xFF038B00, 0xFF008240, 0xFF007891, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF63ADFD, 0xFF908AFE, 0xFFB977FC, 0xFFE771FE, 0xFFF76FC9, 0xFFF5836A, 0xFFDD9C29, 0xFFBDB807, 0xFF84D107, 0xFF5BDC3B, 0xFF48D77D, 0xFF48CCCE, 0xFF555555, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC4E3FE, 0xFFD7D5FE, 0xFFE6CDFE, 0xFFF9CAFE, 0xFFFEC9F0, 0xFFFED1C7, 0xFFF7DCAC, 0xFFE8E89C, 0xFFD1F29D, 0xFFBFF4B1, 0xFFB7F5CD, 0xFFB7F0EE, 0xFFBEBEBE, 0xFF000000, 0xFF000000 };
uint32_t yuvPalette[0x40] { 0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0700, 0xFF561D00, 0xFF333500, 0xFF0C4800, 0xFF005200, 0xFF004C18, 0xFF003E5B, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00, 0xFF6B6D00, 0xFF388700, 0xFF0D9300, 0xFF008C47, 0xFF007AA0, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF26AFF, 0xFFFF6ECC, 0xFFFF8170, 0xFFEA9E22, 0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFAC2FF, 0xFFFFC4EA, 0xFFFFCCC5, 0xFFF7D8A5, 0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000 };
uint32_t nestopiaRgbPalette[0x40] { 0xFF6D6D6D, 0xFF002492, 0xFF0000DB, 0xFF6D49DB, 0xFF92006D, 0xFFB6006D, 0xFFB62400, 0xFF924900, 0xFF6D4900, 0xFF244900, 0xFF006D24, 0xFF009200, 0xFF004949, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB6B6B6, 0xFF006DDB, 0xFF0049FF, 0xFF9200FF, 0xFFB600FF, 0xFFFF0092, 0xFFFF0000, 0xFFDB6D00, 0xFF926D00, 0xFF249200, 0xFF009200, 0xFF00B66D, 0xFF009292, 0xFF242424, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF6DB6FF, 0xFF9292FF, 0xFFDB6DFF, 0xFFFF00FF, 0xFFFF6DFF, 0xFFFF9200, 0xFFFFB600, 0xFFDBDB00, 0xFF6DDB00, 0xFF00FF00, 0xFF49FFDB, 0xFF00FFFF, 0xFF494949, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFB6DBFF, 0xFFDBB6FF, 0xFFFFB6FF, 0xFFFF92FF, 0xFFFFB6B6, 0xFFFFDB92, 0xFFFFFF49, 0xFFFFFF6D, 0xFFB6FF49, 0xFF92FF6D, 0xFF49FFDB, 0xFF92DBFF, 0xFF929292, 0xFF000000, 0xFF000000 };
uint32_t compositeDirectPalette[0x40] { 0xFF656565, 0xFF00127D, 0xFF18008E, 0xFF360082, 0xFF56005D, 0xFF5A0018, 0xFF4F0500, 0xFF381900, 0xFF1D3100, 0xFF003D00, 0xFF004100, 0xFF003B17, 0xFF002E55, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFAFAFAF, 0xFF194EC8, 0xFF472FE3, 0xFF6B1FD7, 0xFF931BAE, 0xFF9E1A5E, 0xFF993200, 0xFF7B4B00, 0xFF5B6700, 0xFF267A00, 0xFF008200, 0xFF007A3E, 0xFF006E8A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF64A9FF, 0xFF8E89FF, 0xFFB676FF, 0xFFE06FFF, 0xFFEF6CC4, 0xFFF0806A, 0xFFD8982C, 0xFFB9B40A, 0xFF83CB0C, 0xFF5BD63F, 0xFF4AD17E, 0xFF4DC7CB, 0xFF4C4C4C, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFC7E5FF, 0xFFD9D9FF, 0xFFE9D1FF, 0xFFF9CEFF, 0xFFFFCCF1, 0xFFFFD4CB, 0xFFF8DFB1, 0xFFEDEAA4, 0xFFD6F4A4, 0xFFC5F8B8, 0xFFBEF6D3, 0xFFBFF1F1, 0xFFB9B9B9, 0xFF000000, 0xFF000000 };
uint32_t nesClassicPalette[0x40] { 0xFF60615F, 0xFF000083, 0xFF1D0195, 0xFF340875, 0xFF51055E, 0xFF56000F, 0xFF4C0700, 0xFF372308, 0xFF203A0B, 0xFF0F4B0E, 0xFF194C16, 0xFF02421E, 0xFF023154, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFA9AAA8, 0xFF104BBF, 0xFF4712D8, 0xFF6300CA, 0xFF8800A9, 0xFF930B46, 0xFF8A2D04, 0xFF6F5206, 0xFF5C7114, 0xFF1B8D12, 0xFF199509, 0xFF178448, 0xFF206B8E, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFBFBFB, 0xFF6699F8, 0xFF8974F9, 0xFFAB58F8, 0xFFD557EF, 0xFFDE5FA9, 0xFFDC7F59, 0xFFC7A224, 0xFFA7BE03, 0xFF75D703, 0xFF60E34F, 0xFF3CD68D, 0xFF56C9CC, 0xFF414240, 0xFF000000, 0xFF000000, 0xFFFBFBFB, 0xFFBED4FA, 0xFFC9C7F9, 0xFFD7BEFA, 0xFFE8B8F9, 0xFFF5BAE5, 0xFFF3CAC2, 0xFFDFCDA7, 0xFFD9E09C, 0xFFC9EB9E, 0xFFC0EDB8, 0xFFB5F4C7, 0xFFB9EAE9, 0xFFABABAB, 0xFF000000, 0xFF000000 };
uint32_t originalHardwarePalette[0x40] { 0xFF6A6D6A, 0xFF00127D, 0xFF1E008A, 0xFF3B007D, 0xFF56005D, 0xFF5A0018, 0xFF4F0D00, 0xFF381E00, 0xFF203100, 0xFF003D00, 0xFF004000, 0xFF003B1E, 0xFF002E55, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB9BCB9, 0xFF194EC8, 0xFF472FE3, 0xFF751FD7, 0xFF931EAD, 0xFF9E245E, 0xFF963800, 0xFF7B5000, 0xFF5B6700, 0xFF267A00, 0xFF007F00, 0xFF007842, 0xFF006E8A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF69AEFF, 0xFF9798FF, 0xFFB687FF, 0xFFE278FF, 0xFFF279C7, 0xFFF58F6F, 0xFFDDA932, 0xFFBCB70D, 0xFF88D015, 0xFF60DB49, 0xFF4FD687, 0xFF50CACE, 0xFF515451, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFCCEAFF, 0xFFDEE2FF, 0xFFEEDAFF, 0xFFFAD7FD, 0xFFFDD7F6, 0xFFFDDCD0, 0xFFFAE8B6, 0xFFF2F1A9, 0xFFDBFBA9, 0xFFCAFFBD, 0xFFC3FBD8, 0xFFC4F6F6, 0xFFBEC1BE, 0xFF000000, 0xFF000000 };
uint32_t pvmStylePalette[0x40] { 0xFF696964, 0xFF001774, 0xFF28007D, 0xFF3E006D, 0xFF560057, 0xFF5E0013, 0xFF531A00, 0xFF3B2400, 0xFF2A3000, 0xFF143A00, 0xFF003F00, 0xFF003B1E, 0xFF003050, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB9B9B4, 0xFF1453B9, 0xFF4D2CDA, 0xFF7A1EC8, 0xFF98189C, 0xFF9D2344, 0xFFA03E00, 0xFF8D5500, 0xFF656D00, 0xFF2C7900, 0xFF008100, 0xFF007D42, 0xFF00788A, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF69A8FF, 0xFF9A96FF, 0xFFC28AFA, 0xFFEA7DFA, 0xFFF387B4, 0xFFF1986C, 0xFFE6B327, 0xFFD7C805, 0xFF90DF07, 0xFF64E53C, 0xFF45E27D, 0xFF48D5D9, 0xFF4B4B46, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFD2EAFF, 0xFFE2E2FF, 0xFFF2D8FF, 0xFFF8D2FF, 0xFFF8D9EA, 0xFFFADEB9, 0xFFF9E89B, 0xFFF3F28C, 0xFFD3FA91, 0xFFB8FCA8, 0xFFAEFACA, 0xFFCAF3F3, 0xFFBEBEB9, 0xFF000000, 0xFF000000 };
uint32_t sonyCxa2025AsPalette[0x40] { 0xFF585858, 0xFF00238C, 0xFF00139B, 0xFF2D0585, 0xFF5D0052, 0xFF7A0017, 0xFF7A0800, 0xFF5F1800, 0xFF352A00, 0xFF093900, 0xFF003F00, 0xFF003C22, 0xFF00325D, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFA1A1A1, 0xFF0053EE, 0xFF153CFE, 0xFF6028E4, 0xFFA91D98, 0xFFD41E41, 0xFFD22C00, 0xFFAA4400, 0xFF6C5E00, 0xFF2D7300, 0xFF007D06, 0xFF007852, 0xFF0069A9, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF1FA5FE, 0xFF5E89FE, 0xFFB572FE, 0xFFFE65F6, 0xFFFE6790, 0xFFFE773C, 0xFFFE9308, 0xFFC4B200, 0xFF79CA10, 0xFF3AD54A, 0xFF11D1A4, 0xFF06BFFE, 0xFF424242, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFA0D9FE, 0xFFBDCCFE, 0xFFE1C2FE, 0xFFFEBCFB, 0xFFFEBDD0, 0xFFFEC5A9, 0xFFFED18E, 0xFFE9DE86, 0xFFC7E992, 0xFFA8EEB0, 0xFF95ECD9, 0xFF91E4FE, 0xFFACACAC, 0xFF000000, 0xFF000000 };
uint32_t wavebeamPalette[0x40] { 0xFF6B6B6B, 0xFF001B88, 0xFF21009A, 0xFF40008C, 0xFF600067, 0xFF64001E, 0xFF590800, 0xFF481600, 0xFF283600, 0xFF004500, 0xFF004908, 0xFF00421D, 0xFF003659, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFB4B4B4, 0xFF1555D3, 0xFF4337EF, 0xFF7425DF, 0xFF9C19B9, 0xFFAC0F64, 0xFFAA2C00, 0xFF8A4B00, 0xFF666B00, 0xFF218300, 0xFF008A00, 0xFF008144, 0xFF007691, 0xFF000000, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFF63B2FF, 0xFF7C9CFF, 0xFFC07DFE, 0xFFE977FF, 0xFFF572CD, 0xFFF4886B, 0xFFDDA029, 0xFFBDBD0A, 0xFF89D20E, 0xFF5CDE3E, 0xFF4BD886, 0xFF4DCFD2, 0xFF525252, 0xFF000000, 0xFF000000, 0xFFFFFFFF, 0xFFBCDFFF, 0xFFD2D2FF, 0xFFE1C8FF, 0xFFEFC7FF, 0xFFFFC3E1, 0xFFFFCAC6, 0xFFF2DAAD, 0xFFEBE3A0, 0xFFD2EDA2, 0xFFBCF4B4, 0xFFB5F1CE, 0xFFB6ECF1, 0xFFBFBFBF, 0xFF000000, 0xFF000000 };

extern "C" {
	void logMessage(retro_log_level level, const char* message)
	{
		if(logCallback) {
			logCallback(level, message);
		}
	}

	RETRO_API unsigned retro_api_version()
	{
		return RETRO_API_VERSION;
	}

	RETRO_API void retro_init()
	{
		struct retro_log_callback log;
		if(env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
			logCallback = log.log;
		else
			logCallback = nullptr;

		// Create the emulator and initialize its subsystems
		_emu.reset(new Emulator());
		_emu->Initialize(); // sets up settings, video/audio subsystems, etc.

		// Provide the global KeyManager with the emulator settings so calls like
		// KeyManager::SetForceFeedback can safely read configuration values.
		KeyManager::SetSettings(_emu->GetSettings());

		// Grab the IConsole instance and dynamic_cast to NesConsole when needed
		auto consoleIf = _emu->GetConsole(); // shared_ptr<IConsole>
		_console = std::dynamic_pointer_cast<NesConsole>(consoleIf);

	// Key manager accepts the IConsole shared_ptr returned by the emulator
	// and also needs a pointer to the Emulator for mouse positioning.
	_keyManager = std::make_shared<LibretroKeyManager>(consoleIf, _emu.get());

	// Forward any previously saved input callbacks into the key manager and register it
	if(_savedGetInputState) _keyManager->SetGetInputState(_savedGetInputState);
	if(_savedPollInput) _keyManager->SetPollInput(_savedPollInput);
	// KeyManager now expects a shared_ptr<IKeyManager> for thread-safe registration
	KeyManager::RegisterKeyManager(std::shared_ptr<IKeyManager>(_keyManager));
		_message_manager.reset(new LibretroMessageManager(logCallback, env_cb));

		std::stringstream databaseData;
		databaseData.write((const char*)MesenDatabase, sizeof(MesenDatabase));
		GameDatabase::LoadGameDb(databaseData);

		// Map sample rate into the new EmuSettings API
		AudioConfig ac = _emu->GetSettings()->GetAudioConfig();
		ac.SampleRate = _audioSampleRate;
		_emu->GetSettings()->SetAudioConfig(ac);

		// Create libretro audio device so we can register it later once a ROM is loaded
		_audioDevice.reset(new LibretroAudioDevice());

		// NOTE: many NES-specific flags moved into NesConfig inside EmuSettings.
		// Example: to set FDS auto-load you'd edit NesConfig and call SetNesConfig.
		// _emu->GetSettings()->GetNesConfig().FdsAutoLoadDisk = true; // then SetNesConfig(...)

		if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
			_keyManager->SetSupportsInputBitmasks(true);
	}

	RETRO_API void retro_deinit()
	{
		if(_keyManager) _keyManager->SetSupportsInputBitmasks(false);
	// Unregister the key manager from the global KeyManager to avoid a
	// dangling pointer during emulator shutdown. KeyManager::RegisterKeyManager
	// accepts an empty shared_ptr to clear the registered backend.
	KeyManager::RegisterKeyManager(std::shared_ptr<IKeyManager>());
		_keyManager.reset();
		_message_manager.reset();

		// Properly shut down the emulator instance
		if(_emu) {
			// Unregister libretro audio device before releasing the emulator
			if(_audioDevice) {
				_emu->GetSoundMixer()->RegisterAudioDevice(nullptr);
				_audioDevice.reset();
			}

			// Make sure KeyManager is unregistered before calling Release()
			// (do nothing here because we already unregistered above)
			_emu->Release();
			_emu.reset();
		}
		_console.reset();
		// Now that the emulator instance has been released, clear the settings
		// pointer stored in the global KeyManager to avoid dangling references.
		KeyManager::SetSettings(nullptr);
	}

	RETRO_API void retro_set_environment(retro_environment_t env)
	{
		env_cb = env;

		// Core Options v2: Categories
		static const retro_core_option_v2_category option_cats[] = {
			{ "system", "System", "System settings (region, RAM, overclock)" },
			{ "video", "Video", "Video settings (palette, filters, overscan)" },
			{ "audio", "Audio", "Audio settings (filters, channels, sample rate)" },
			{ "input", "Input", "Input controller settings" },
			{ "enhancements", "Enhancements", "Enhancement options (HD packs, sprite limit)" },
			{ NULL, NULL, NULL }
		};

		// Core Options v2: Definitions
		static const retro_core_option_v2_definition option_defs[] = {
			// System category
			{ MesenRegion, "System - Region", "Region", "Select NES region", NULL, "system",
				{{ "Auto", "Auto" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }, { "Dendy", "Dendy" }, { NULL, NULL }},
				"Auto" },
			{ MesenRamState, "System - RAM Power-On State", "RAM Power-On State", "Default power-on state for RAM", NULL, "system",
				{{ "All 0s (Default)", "All 0s" }, { "All 1s", "All 1s" }, { "Random Values", "Random" }, { NULL, NULL }},
				"All 0s (Default)" },
			{ MesenOverclock, "System - Overclock", "Overclock", "Overclock the NES CPU", NULL, "system",
				{{ "None", "None" }, { "Low", "Low" }, { "Medium", "Medium" }, { "High", "High" }, { "Very High", "Very High" }, { NULL, NULL }},
				"None" },
			{ MesenOverclockType, "System - Overclock Type", "Overclock Type", "When to apply overclock", NULL, "system",
				{{ "Before NMI (Recommended)", "Before NMI" }, { "After NMI", "After NMI" }, { NULL, NULL }},
				"Before NMI (Recommended)" },
			{ MesenFdsAutoSelectDisk, "System - FDS Auto Insert Disk", "FDS Auto Insert", "Automatically insert disks on FDS games", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenFdsFastForwardLoad, "System - FDS Fast Forward On Load", "FDS Fast Forward", "Fast forward while FDS is loading", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenAllowInvalidInput, "System - Allow Invalid Input", "Allow Invalid Input", "Allow invalid input combinations", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenRandomizeMapperPowerOnState, "System - Randomize Mapper Power-On State", "Randomize Mapper Power-On", "Randomize mapper power-on state (for testing)", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenRandomizeCpuPpuAlignment, "System - Randomize CPU/PPU Alignment", "Randomize CPU/PPU Alignment", "Randomize CPU/PPU alignment (for testing)", NULL, "system",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Video category
			{ MesenPalette, "Video - Palette", "Palette", "Select color palette", NULL, "video",
				{{ "Default", "Default" }, { "Composite Direct (by FirebrandX)", "Composite Direct" }, { "Nes Classic", "NES Classic" }, { "Nestopia (RGB)", "Nestopia RGB" }, { "Original Hardware (by FirebrandX)", "Original Hardware" }, { "PVM Style (by FirebrandX)", "PVM Style" }, { "Sony CXA2025AS", "Sony CXA2025AS" }, { "Unsaturated v6 (by FirebrandX)", "Unsaturated v6" }, { "YUV v3 (by FirebrandX)", "YUV v3" }, { "Wavebeam (by nakedarthur)", "Wavebeam" }, { "Custom", "Custom" }, { "Raw", "Raw" }, { NULL, NULL }},
				"Default" },
			{ MesenNtscFilter, "Video - NTSC Filter", "NTSC Filter", "NTSC video filter", NULL, "video",
				{{ "Disabled", "Disabled" }, { "Composite (Blargg)", "Composite (Blargg)" }, { "S-Video (Blargg)", "S-Video (Blargg)" }, { "RGB (Blargg)", "RGB (Blargg)" }, { "Monochrome (Blargg)", "Monochrome (Blargg)" }, { "Bisqwit 2x", "Bisqwit 2x" }, { "Bisqwit 4x", "Bisqwit 4x" }, { "Bisqwit 8x", "Bisqwit 8x" }, { NULL, NULL }},
				"Disabled" },
			{ MesenOverscanLeft, "Video - Overscan Left", "Overscan Left", "Left overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanRight, "Video - Overscan Right", "Overscan Right", "Right overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanTop, "Video - Overscan Top", "Overscan Top", "Top overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenOverscanBottom, "Video - Overscan Bottom", "Overscan Bottom", "Bottom overscan", NULL, "video",
				{{ "None", "None" }, { "4px", "4px" }, { "8px", "8px" }, { "12px", "12px" }, { "16px", "16px" }, { NULL, NULL }},
				"None" },
			{ MesenAspectRatio, "Video - Aspect Ratio", "Aspect Ratio", "Display aspect ratio", NULL, "video",
				{{ "Auto", "Auto" }, { "No Stretching", "No Stretching" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }, { "4:3", "4:3" }, { "4:3 (Preserved)", "4:3 (Preserved)" }, { "16:9", "16:9" }, { "16:9 (Preserved)", "16:9 (Preserved)" }, { NULL, NULL }},
				"Auto" },
			{ MesenScreenRotation, "Video - Screen Rotation", "Screen Rotation", "Rotate screen display", NULL, "video",
				{{ "None", "None" }, { "90 degrees", "90 degrees" }, { "180 degrees", "180 degrees" }, { "270 degrees", "270 degrees" }, { NULL, NULL }},
				"None" },
			{ MesenEnablePalBorders, "Video - Enable PAL Borders", "PAL Borders", "Show borders in PAL mode", NULL, "video",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Audio category
			{ MesenFakeStereo, "Audio - Fake Stereo", "Fake Stereo", "Enable fake stereo effect", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenMuteTriangleUltrasonic, "Audio - Reduce Triangle Popping", "Reduce Triangle Popping", "Mute Triangle channel ultrasonic frequencies", NULL, "audio",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenReduceDmcPopping, "Audio - Reduce DMC Popping", "Reduce DMC Popping", "Reduce popping on DMC channel", NULL, "audio",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenSwapDutyCycle, "Audio - Swap Duty Cycles", "Swap Duty Cycles", "Swap Square channel duty cycles", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenDisableNoiseModeFlag, "Audio - Disable Noise Mode Flag", "Disable Noise Mode", "Disable Noise channel mode flag", NULL, "audio",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },
			{ MesenAudioSampleRate, "Audio - Sample Rate", "Sample Rate", "Audio output sample rate", NULL, "audio",
				{{ "48000", "48000 Hz" }, { "96000", "96000 Hz" }, { "11025", "11025 Hz" }, { "22050", "22050 Hz" }, { "44100", "44100 Hz" }, { NULL, NULL }},
				"48000" },

			// Input category
			{ MesenControllerTurboSpeed, "Input - Controller Turbo Speed", "Turbo Speed", "Turbo button speed", NULL, "input",
				{{ "Fast", "Fast" }, { "Very Fast", "Very Fast" }, { "Disabled", "Disabled" }, { "Slow", "Slow" }, { "Normal", "Normal" }, { NULL, NULL }},
				"Normal" },
			{ MesenShiftButtonsClockwise, "Input - Shift Buttons Clockwise", "Shift Buttons", "Shift A/B/X/Y buttons clockwise", NULL, "input",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Enhancements category (additional options)
			{ MesenSpritesEnabled, "Enhancements - Sprites Enabled", "Sprites Enabled", "Enable sprite rendering", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenBackgroundEnabled, "Enhancements - Background Enabled", "Background Enabled", "Enable background rendering", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenDisableGameGenieBusConflicts, "Enhancements - Disable Game Genie Bus Conflicts", "Game Genie Bus Conflicts", "Disable Game Genie bus conflicts", NULL, "enhancements",
				{{ "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL }},
				"disabled" },

			// Enhancements category
			{ MesenHdPacks, "Enhancements - HD Packs", "HD Packs", "Enable HD graphics packs", NULL, "enhancements",
				{{ "enabled", "On" }, { "disabled", "Off" }, { NULL, NULL }},
				"enabled" },
			{ MesenSpriteLimit, "Enhancements - Sprite Limit", "Sprite Limit", "8-sprite scanline limit", NULL, "enhancements",
				{{ "normal", "Normal" }, { "adaptive", "Adaptive" }, { "off", "Off" }, { NULL, NULL }},
				"normal" },

			{ NULL, NULL, NULL, NULL, NULL, NULL, {{ NULL, NULL }}, NULL }
		};

		static retro_core_options_v2 core_opt_info = { 
			(retro_core_option_v2_category*)option_cats,
			(retro_core_option_v2_definition*)option_defs
		};

		env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &core_opt_info);

		static constexpr struct retro_controller_description pads1[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ "Zapper", DEVICE_ZAPPER },
			{ "Power Pad", DEVICE_POWERPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "SNES Controller", DEVICE_SNESGAMEPAD },
			{ "SNES Mouse", DEVICE_SNESMOUSE },
			{ "Virtual Boy Controller" ,DEVICE_VBGAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads2[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ "Zapper", DEVICE_ZAPPER },
			{ "Power Pad", DEVICE_POWERPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "SNES Controller", DEVICE_SNESGAMEPAD },
			{ "SNES Mouse", DEVICE_SNESMOUSE },
			{ "Virtual Boy Controller", DEVICE_VBGAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads3[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ NULL, 0 },
		};

		static constexpr struct retro_controller_description pads4[] = {
			{ "Auto", DEVICE_AUTO },
			{ "Standard Controller", DEVICE_GAMEPAD },
			{ NULL, 0 },
		};
		
		static constexpr struct retro_controller_description pads5[] = {
			{ "Auto",     RETRO_DEVICE_JOYPAD },
			{ "Arkanoid", DEVICE_ARKANOID },
			{ "Ascii Turbo File", DEVICE_ASCIITURBOFILE },
			{ "Bandai Hypershot", DEVICE_BANDAIHYPERSHOT },
			{ "Battle Box", DEVICE_BATTLEBOX },
			{ "Exciting Boxing", DEVICE_EXCITINGBOXING },
			{ "Family Trainer", DEVICE_FAMILYTRAINER },
			{ "Four Player Adapter", DEVICE_FOURPLAYERADAPTER },
			{ "Hori Track", DEVICE_HORITRACK },
			{ "Konami Hypershot", DEVICE_KONAMIHYPERSHOT },
			{ "Pachinko", DEVICE_PACHINKO },
			{ "Partytap", DEVICE_PARTYTAP },
			{ "Oeka Kids Tablet", DEVICE_OEKAKIDS },			
			{ NULL, 0 },
		};
		
		static constexpr struct retro_controller_info ports[] = {
			{ pads1, 7 },
			{ pads2, 7 },
			{ pads3, 2 },
			{ pads4, 2 },
			{ pads5, 13 },
			{ 0 },
		};

		static const struct retro_system_content_info_override content_overrides[] = {
			{
				"nes|fds|unf|unif", /* extensions */
				false,              /* need_fullpath */
				false               /* persistent_data */
			},
			{ NULL, false, false }
		};

		env_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
		env_cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*)content_overrides);
	}

	RETRO_API void retro_set_video_refresh(retro_video_refresh_t sendFrame)
	{
		// VideoRenderer no longer exposes SetVideoCallback — keep the libretro callback here.
		_videoRefresh = sendFrame;
	}

	RETRO_API void retro_set_audio_sample(retro_audio_sample_t sendAudioSample)
	{
	}

	RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t audioSampleBatch)
	{
		// Mixer API changed; keep the libretro callback for later use by audio path
		_audioSampleBatch = audioSampleBatch;
	}

	RETRO_API void retro_set_input_poll(retro_input_poll_t pollInput)
	{	
		// store the callback for later use and forward to key manager if it's active
		_savedPollInput = pollInput;
		if(_keyManager) _keyManager->SetPollInput(pollInput);

		// Probe input callbacks immediately from the thread that set them to
		// help debug frontend timing/order issues. This is noisy by default so
		// gate it behind an environment variable (MESEN_LIBRETRO_VERBOSE_INPUT).
		if((pollInput || _savedGetInputState) && getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
			fprintf(stderr, "[libretro] Diagnostic (setter): probing input callbacks after SetPollInput\n");
			extern void libretro_probe_inputs(const char*);
			libretro_probe_inputs("setter_poll");
		}
	}

	RETRO_API void retro_set_input_state(retro_input_state_t getInputState)
	{
		_savedGetInputState = getInputState;
		if(_keyManager) _keyManager->SetGetInputState(getInputState);

		if((getInputState || _savedPollInput) && getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
			fprintf(stderr, "[libretro] Diagnostic (setter): probing input callbacks after SetGetInputState\n");
			extern void libretro_probe_inputs(const char*);
			libretro_probe_inputs("setter_getstate");
		}
	}

// Shared diagnostic probe called from multiple places to exercise the saved
// input callbacks and print their results. The argument is a short tag
// describing the caller (for log clarity).
void libretro_probe_inputs(const char* tag)
{
	if(!env_cb) return;
	fprintf(stderr, "[libretro] Diagnostic(%s): probe start\n", tag);
	if(_savedPollInput) {
		try { _savedPollInput(); fprintf(stderr, "[libretro] Diagnostic(%s): poll() ok\n", tag); } catch(...) { fprintf(stderr, "[libretro] Diagnostic(%s): poll() threw\n", tag); }
	}
	// Query bitmask capability but avoid calling the frontend get_state callback here.
	// Some libretro frontends crash if get_state is invoked outside their expected
	// input polling context (we observed a segmentation fault). To be safe, only
	// log availability and defer actual get_state calls to the normal per-frame
	// RefreshState path which runs on the emulator/main thread.
	bool bitmasks = false;
	if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL)) bitmasks = true;
	fprintf(stderr, "[libretro] Diagnostic(%s): supports bitmasks=%d\n", tag, (int)bitmasks);
	fprintf(stderr, "[libretro] Diagnostic(%s): skipping direct get_state() calls to avoid host crashes\n", tag);
	fprintf(stderr, "[libretro] Diagnostic(%s): probe end\n", tag);
}

	RETRO_API void retro_reset()
	{
		// Reset signature changed — call parameterless Reset if available
		_console->Reset();
	}

	bool readVariable(const char* key, retro_variable &var)
	{
		var.key = key;
		var.value = nullptr;
		if(env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value != nullptr)
			return true;
		return false;
	}

	uint8_t readOverscanValue(const char* key)
	{
		retro_variable var = {};
		if(readVariable(key, var)) {
			string value = string(var.value);
			if(value == "4px") {
				return 4;
			} else if(value == "8px") {
				return 8;
			} else if(value == "12px") {
				return 12;
			} else if(value == "16px") {
				return 16;
			}
		}
		return 0;
	}

	void set_flag(const char* flagName, uint64_t flagValue)
	{
		struct retro_variable var = {};
		if(readVariable(flagName, var)) {
			string value = string(var.value);
			if(value == "disabled") {
				_emu->GetSettings()->ClearFlag((EmulationFlags)flagValue);
			} else {
				_emu->GetSettings()->SetFlag((EmulationFlags)flagValue);
			}
		}
	}

	void load_custom_palette()
	{
		//Setup default palette in case we can't load the custom one
		// Ensure video config is accessed so types are instantiated (no-op)
		_emu->GetSettings()->GetVideoConfig();
		// Store default palette locally; renderer wiring to use this will be added later.
		_userRgbPalette.assign(std::begin(defaultPalette), std::end(defaultPalette));

		//Try to load the custom palette from the MesenPalette.pal file
		string palettePath = FolderUtilities::CombinePath(FolderUtilities::GetHomeFolder(), "MesenPalette.pal");
		uint8_t fileData[512 * 3] = {};
		std::ifstream palette(palettePath, std::ios::binary);
		if(palette) {
			palette.seekg(0, std::ios::end);
			std::streamoff fileSize = palette.tellg();
			palette.seekg(0, std::ios::beg);
			if((fileSize == 64 * 3) || (fileSize == 512 * 3)) {
				palette.read((char*)fileData, fileSize);
				uint32_t customPalette[512];
				int paletteCount = (int)(fileSize / 3);
				for(int i = 0; i < paletteCount; i++) {
					customPalette[i] = 0xFF000000 | fileData[i * 3 + 2] | (fileData[i * 3 + 1] << 8) | (fileData[i * 3] << 16);
				}
				_userRgbPalette.assign(customPalette, customPalette + paletteCount);
			}
		}
	}

	void update_settings()
	{
		struct retro_variable var = { };
		NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
		VideoConfig videoCfg = _emu->GetSettings()->GetVideoConfig();
		AudioConfig audioCfg = _emu->GetSettings()->GetAudioConfig();

		// ===== SYSTEM SETTINGS =====
		
		// Region
		if(readVariable(MesenRegion, var)) {
			string value = string(var.value);
			if(value == "NTSC") {
				nesCfg.Region = ConsoleRegion::Ntsc;
			} else if(value == "PAL") {
				nesCfg.Region = ConsoleRegion::Pal;
			} else if(value == "Dendy") {
				nesCfg.Region = ConsoleRegion::Dendy;
			} else {
				nesCfg.Region = ConsoleRegion::Auto;
			}
		}

		// RAM Power-On State
		if(readVariable(MesenRamState, var)) {
			string value = string(var.value);
			if(value == "All 1s") {
				nesCfg.RamPowerOnState = RamState::AllOnes;
			} else if(value == "Random Values") {
				nesCfg.RamPowerOnState = RamState::Random;
			} else {
				nesCfg.RamPowerOnState = RamState::AllZeros;
			}
		}

		// Overclock
		int lineCountBefore = 0;
		int lineCountAfter = 0;
		bool beforeNmi = true;
		if(readVariable(MesenOverclockType, var)) {
			string value = string(var.value);
			beforeNmi = (value != "After NMI");
		}

		if(readVariable(MesenOverclock, var)) {
			string value = string(var.value);
			int lineCount = 0;
			if(value == "Low") {
				lineCount = 100;
			} else if(value == "Medium") {
				lineCount = 250;
			} else if(value == "High") {
				lineCount = 500;
			} else if(value == "Very High") {
				lineCount = 1000;
			}
			if(beforeNmi) {
				lineCountBefore = lineCount;
			} else {
				lineCountAfter = lineCount;
			}
		}
		nesCfg.PpuExtraScanlinesBeforeNmi = lineCountBefore;
		nesCfg.PpuExtraScanlinesAfterNmi = lineCountAfter;

		// FDS options
		if(readVariable(MesenFdsAutoSelectDisk, var)) {
			nesCfg.FdsAutoInsertDisk = (string(var.value) == "enabled");
		}
		if(readVariable(MesenFdsFastForwardLoad, var)) {
			nesCfg.FdsFastForwardOnLoad = (string(var.value) == "enabled");
		}

		// ===== VIDEO SETTINGS =====
		
		// Palette
		if(readVariable(MesenPalette, var)) {
			string value = string(var.value);
			if(value == "Default") {
				_userRgbPalette.assign(std::begin(defaultPalette), std::end(defaultPalette));
			} else if(value == "Composite Direct (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(compositeDirectPalette), std::end(compositeDirectPalette));
			} else if(value == "Nes Classic") {
				_userRgbPalette.assign(std::begin(nesClassicPalette), std::end(nesClassicPalette));
			} else if(value == "Nestopia (RGB)") {
				_userRgbPalette.assign(std::begin(nestopiaRgbPalette), std::end(nestopiaRgbPalette));
			} else if(value == "Original Hardware (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(originalHardwarePalette), std::end(originalHardwarePalette));
			} else if(value == "PVM Style (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(pvmStylePalette), std::end(pvmStylePalette));
			} else if(value == "Sony CXA2025AS") {
				_userRgbPalette.assign(std::begin(sonyCxa2025AsPalette), std::end(sonyCxa2025AsPalette));
			} else if(value == "Unsaturated v6 (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(unsaturatedPalette), std::end(unsaturatedPalette));
			} else if(value == "YUV v3 (by FirebrandX)") {
				_userRgbPalette.assign(std::begin(yuvPalette), std::end(yuvPalette));
			} else if(value == "Wavebeam (by nakedarthur)") {
				_userRgbPalette.assign(std::begin(wavebeamPalette), std::end(wavebeamPalette));
			} else if(value == "Custom") {
				load_custom_palette();
			} else if(value == "Raw") {
				_videoFilterRaw = true;
			}
		}

		// Copy palette into NesConfig (skip if raw mode is enabled)
		if(!_videoFilterRaw) {
			if(_userRgbPalette.size() >= 512) {
				for(size_t i = 0; i < 512; ++i) nesCfg.UserPalette[i] = (i < _userRgbPalette.size()) ? _userRgbPalette[i] : 0xFF000000;
				nesCfg.IsFullColorPalette = true;
			} else {
				for(size_t i = 0; i < 64; ++i) nesCfg.UserPalette[i] = (i < _userRgbPalette.size()) ? _userRgbPalette[i] : 0xFF000000;
				nesCfg.IsFullColorPalette = false;
			}
		}

		// NTSC Filter - map string selection to VideoFilterType enum and apply settings
		if(readVariable(MesenNtscFilter, var)) {
			string filterValue = string(var.value);
			if(filterValue == "Disabled") {
				videoCfg.VideoFilter = VideoFilterType::None;
			} else if(filterValue == "Composite (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Composite;
			} else if(filterValue == "S-Video (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Svideo;
			} else if(filterValue == "RGB (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Rgb;
			} else if(filterValue == "Monochrome (Blargg)") {
				videoCfg.VideoFilter = VideoFilterType::NtscBlargg;
				videoCfg.NtscBlarggPreset_Value = NtscBlarggPreset::Monochrome;
			} else if(filterValue == "Bisqwit 2x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_2x;
			} else if(filterValue == "Bisqwit 4x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_4x;
			} else if(filterValue == "Bisqwit 8x") {
				videoCfg.VideoFilter = VideoFilterType::NtscBisqwit;
				videoCfg.NtscScale = NtscBisqwitFilterScale::_8x;
			}
		}

		// Overscan
		nesCfg.NtscOverscan.Left = readOverscanValue(MesenOverscanLeft);
		nesCfg.NtscOverscan.Right = readOverscanValue(MesenOverscanRight);
		nesCfg.NtscOverscan.Top = readOverscanValue(MesenOverscanTop);
		nesCfg.NtscOverscan.Bottom = readOverscanValue(MesenOverscanBottom);
		// Use same overscan for PAL
		nesCfg.PalOverscan = nesCfg.NtscOverscan;

		// Aspect Ratio
		if(readVariable(MesenAspectRatio, var)) {
			_selectedAspectRatio = std::string(var.value ? var.value : "");
		}

		// Screen Rotation
		if(readVariable(MesenScreenRotation, var)) {
			string value = string(var.value);
			if(value == "90 degrees") {
				videoCfg.ScreenRotation = 90;
			} else if(value == "180 degrees") {
				videoCfg.ScreenRotation = 180;
			} else if(value == "270 degrees") {
				videoCfg.ScreenRotation = 270;
			} else {
				videoCfg.ScreenRotation = 0;
			}
		}

		// PAL Borders
		if(readVariable(MesenEnablePalBorders, var)) {
			nesCfg.EnablePalBorders = (string(var.value) == "enabled");
		}

		// ===== AUDIO SETTINGS =====
		
		// Fake Stereo
		if(readVariable(MesenFakeStereo, var)) {
			nesCfg.StereoFilter = (string(var.value) == "enabled") ? StereoFilterType::Delay : StereoFilterType::None;
		}

		// Reduce Triangle Popping
		if(readVariable(MesenMuteTriangleUltrasonic, var)) {
			nesCfg.SilenceTriangleHighFreq = (string(var.value) == "enabled");
		}

		// Reduce DMC Popping
		if(readVariable(MesenReduceDmcPopping, var)) {
			nesCfg.ReduceDmcPopping = (string(var.value) == "enabled");
		}

		// Swap Duty Cycles
		if(readVariable(MesenSwapDutyCycle, var)) {
			nesCfg.SwapDutyCycles = (string(var.value) == "enabled");
		}

		// Disable Noise Mode Flag
		if(readVariable(MesenDisableNoiseModeFlag, var)) {
			nesCfg.DisableNoiseModeFlag = (string(var.value) == "enabled");
		}

		// Audio Sample Rate
		if(readVariable(MesenAudioSampleRate, var)) {
			int old_value = audioCfg.SampleRate;
			audioCfg.SampleRate = atoi(var.value);
			audioCfg.SampleRate = (audioCfg.SampleRate > 96000) ? 96000 : audioCfg.SampleRate;

			if(old_value != audioCfg.SampleRate) {
				// If core is running, notify frontend of geometry change
				if(_saveStateSize != -1) {
					struct retro_system_av_info system_av_info;
					retro_get_system_av_info(&system_av_info);
					env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &system_av_info);
				}
			}
		}

		// ===== INPUT SETTINGS =====
		
		// Controller Turbo Speed
		int turboSpeed = 1; // default to Normal
		bool turboEnabled = true;
		if(readVariable(MesenControllerTurboSpeed, var)) {
			string value = string(var.value);
			if(value == "Slow") {
				turboSpeed = 0;
			} else if(value == "Normal") {
				turboSpeed = 1;
			} else if(value == "Fast") {
				turboSpeed = 2;
			} else if(value == "Very Fast") {
				turboSpeed = 3;
			} else if(value == "Disabled") {
				turboEnabled = false;
			}
		}

		// Shift Buttons Clockwise
		_shiftButtonsClockwise = false;
		if(readVariable(MesenShiftButtonsClockwise, var)) {
			_shiftButtonsClockwise = (string(var.value) == "enabled");
		}

		auto getKeyCode = [](int port, int retroKey) {
			return (port << 8) | (retroKey + 1);
		};

		auto getKeyBindings = [=](int port) {
			KeyMappingSet keyMappings;
			keyMappings.TurboSpeed = turboSpeed;
			// Default NES-style mapping with optional clockwise shift
			keyMappings.Mapping1.A = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A);
			keyMappings.Mapping1.B = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_B);
			if(turboEnabled) {
				keyMappings.Mapping1.TurboA = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_X);
				keyMappings.Mapping1.TurboB = getKeyCode(port, _shiftButtonsClockwise ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y);
			}
			keyMappings.Mapping1.Start = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_START);
			keyMappings.Mapping1.Select = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_SELECT);
			keyMappings.Mapping1.Up = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_UP);
			keyMappings.Mapping1.Down = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_DOWN);
			keyMappings.Mapping1.Left = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_LEFT);
			keyMappings.Mapping1.Right = getKeyCode(port, RETRO_DEVICE_ID_JOYPAD_RIGHT);
			return keyMappings;
		};

		nesCfg.Port1.Keys = getKeyBindings(0);
		nesCfg.Port2.Keys = getKeyBindings(1);
		nesCfg.Port1SubPorts[0].Keys = getKeyBindings(0);
		nesCfg.Port1SubPorts[1].Keys = getKeyBindings(1);
		nesCfg.Port1SubPorts[2].Keys = getKeyBindings(2);
		nesCfg.Port1SubPorts[3].Keys = getKeyBindings(3);
		nesCfg.ExpPort.Keys = getKeyBindings(4);

		// ===== ENHANCEMENTS =====
		
		// HD Packs
		_hdPacksEnabled = true;
		if(readVariable(MesenHdPacks, var)) {
			_hdPacksEnabled = (string(var.value) != "disabled");
		}
		nesCfg.EnableHdPacks = _hdPacksEnabled;

		// Sprite Limit (3-choice: normal, adaptive, off)
		nesCfg.RemoveSpriteLimit = false;
		nesCfg.AdaptiveSpriteLimit = false;
		if(readVariable(MesenSpriteLimit, var)) {
			string value = string(var.value);
			if(value == "adaptive") {
				nesCfg.AdaptiveSpriteLimit = true;
			} else if(value == "off") {
				nesCfg.RemoveSpriteLimit = true;
			}
		}

		// Sprites Enabled
		if(readVariable(MesenSpritesEnabled, var)) {
			nesCfg.SpritesEnabled = (string(var.value) == "enabled");
		}

		// Background Enabled
		if(readVariable(MesenBackgroundEnabled, var)) {
			nesCfg.BackgroundEnabled = (string(var.value) == "enabled");
		}

		// Disable Game Genie Bus Conflicts
		if(readVariable(MesenDisableGameGenieBusConflicts, var)) {
			nesCfg.DisableGameGenieBusConflicts = (string(var.value) == "enabled");
		}

		// ===== SYSTEM OPTIONS =====

		// Allow Invalid Input
		if(readVariable(MesenAllowInvalidInput, var)) {
			nesCfg.AllowInvalidInput = (string(var.value) == "enabled");
		}

		// Randomize Mapper Power-On State
		if(readVariable(MesenRandomizeMapperPowerOnState, var)) {
			nesCfg.RandomizeMapperPowerOnState = (string(var.value) == "enabled");
		}

		// Randomize CPU/PPU Alignment
		if(readVariable(MesenRandomizeCpuPpuAlignment, var)) {
			nesCfg.RandomizeCpuPpuAlignment = (string(var.value) == "enabled");
		}

		// ===== APPLY ALL SETTINGS =====
		
		_emu->GetSettings()->SetNesConfig(nesCfg);
		_emu->GetSettings()->SetVideoConfig(videoCfg);
		_emu->GetSettings()->SetAudioConfig(audioCfg);

		// Check if geometry-related settings changed
		bool videoFilterChanged = (_lastVideoConfig.VideoFilter != videoCfg.VideoFilter) ||
			(_lastVideoConfig.NtscScale != videoCfg.NtscScale) ||
			(_lastVideoConfig.ScreenRotation != videoCfg.ScreenRotation);
		bool overscanChanged = (nesCfg.NtscOverscan.Left != _lastNesConfig.NtscOverscan.Left) ||
			(nesCfg.NtscOverscan.Right != _lastNesConfig.NtscOverscan.Right) ||
			(nesCfg.NtscOverscan.Top != _lastNesConfig.NtscOverscan.Top) ||
			(nesCfg.NtscOverscan.Bottom != _lastNesConfig.NtscOverscan.Bottom) ||
			(nesCfg.PalOverscan.Left != _lastNesConfig.PalOverscan.Left) ||
			(nesCfg.PalOverscan.Right != _lastNesConfig.PalOverscan.Right) ||
			(nesCfg.PalOverscan.Top != _lastNesConfig.PalOverscan.Top) ||
			(nesCfg.PalOverscan.Bottom != _lastNesConfig.PalOverscan.Bottom);

		if(videoFilterChanged || overscanChanged) {
			_geometryDirty = true;
			if(_emu && _emu->GetVideoDecoder()) {
				_emu->GetVideoDecoder()->ForceFilterUpdate();
			}
		}

		// Save current settings for next comparison
		_lastVideoConfig = videoCfg;
		_lastNesConfig = nesCfg;
	}

	RETRO_API void retro_run()
	{
		// ForceMaxSpeed flag API changed / unavailable here — skip fast-forward behavior for now.
		if(false) {
#if 0
			//Skip frames to speed up emulation while still outputting at 50/60 fps (needed for FDS fast forward while loading)
			_console->GetVideoRenderer()->SetSkipMode(true);
			_console->GetSoundMixer()->SetSkipMode(true);
			for(int i = 0; i < 9; i++) {
				//Attempt to speed up to 1000% speed
				_console->RunSingleFrame();
			}
			_console->GetVideoRenderer()->SetSkipMode(false);
			_console->GetSoundMixer()->SetSkipMode(false);
#endif
		}

		bool updated = false;
		if(env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
			update_settings();
			// Settings API moved; read flag from Emulator settings if available, otherwise skip.
			bool hdPacksEnabled = false;
			if(hdPacksEnabled != _hdPacksEnabled) {
				// Defer HD pack update to emulator API (call removed here) — just track the change
				_hdPacksEnabled = hdPacksEnabled;
			}
		}

		// Frame stepping: run a single emulated frame and submit video/audio to libretro callbacks.
		if(_emu && _emu->GetConsole()) {
			// Run one frame
			auto consoleIf = _emu->GetConsole();
			consoleIf->RunFrame();
			// Allow emulator to run pre-frame hooks (UI, stats, etc.)
			_emu->OnBeforeSendFrame();

			// Get the PPU frame (core provides a 16-bit PPU buffer containing
			// palette indices/intensity bits). Use the emulator's video filter
			// to convert that buffer to a 32-bit ARGB frame so color/palette
			// handling (including emphasis, user palettes, and filters) is
			// consistent with the rest of the emulator.
			PpuFrameInfo frame = _emu->GetPpuFrame();
			if(_videoRefresh && frame.FrameBuffer && frame.FrameBufferSize >= (frame.Width * frame.Height * sizeof(uint16_t))) {
				// Create a temporary video filter (same pattern used by LuaApi)
				std::unique_ptr<BaseVideoFilter> filter(_emu->GetVideoFilter());
				FrameInfo baseSize = { frame.Width, frame.Height };
				filter->SetBaseFrameInfo(baseSize);
				FrameInfo outInfo = filter->SendFrame((uint16_t*)frame.FrameBuffer, _emu->GetFrameCount(), _emu->GetFrameCount() & 0x01, nullptr, false);

				uint32_t* src = filter->GetOutputBuffer();
				// Debugging helpers -- commented out to reduce noise. Uncomment if needed.
				/*
				if(src) {
					fprintf(stderr, "[libretro] video filter outInfo %u x %u, first pixels: %08x %08x %08x %08x\n",
						(unsigned)outInfo.Width, (unsigned)outInfo.Height,
						(unsigned)src[0], (unsigned)src[1], (unsigned)src[2], (unsigned)src[3]);
				} else {
					fprintf(stderr, "[libretro] video filter produced null output buffer\n");
				}
				*/
				size_t pixels = (size_t)outInfo.Width * (size_t)outInfo.Height;

				static std::vector<uint32_t> rotBuffer;

				if(_screenRotation == 0) {
					_videoRefresh(src, outInfo.Width, outInfo.Height, outInfo.Width * 4);
				} else {
					int outW = (_screenRotation == 180) ? outInfo.Width : outInfo.Height;
					int outH = (_screenRotation == 180) ? outInfo.Height : outInfo.Width;
					rotBuffer.resize((size_t)outW * (size_t)outH);

					if(_screenRotation == 180) {
						for(size_t i = 0; i < pixels; ++i) rotBuffer[pixels - 1 - i] = src[i];
						_videoRefresh(rotBuffer.data(), outInfo.Width, outInfo.Height, outInfo.Width * 4);
					} else if(_screenRotation == 90) {
						for(int y = 0; y < outInfo.Height; ++y) {
							for(int x = 0; x < outInfo.Width; ++x) {
								size_t srcIdx = (size_t)y * outInfo.Width + x;
								int dstX = outInfo.Height - 1 - y;
								int dstY = x;
								size_t dstIdx = (size_t)dstY * outW + (size_t)dstX;
								rotBuffer[dstIdx] = src[srcIdx];
							}
						}
						_videoRefresh(rotBuffer.data(), outW, outH, outW * 4);
					} else if(_screenRotation == 270) {
						for(int y = 0; y < outInfo.Height; ++y) {
							for(int x = 0; x < outInfo.Width; ++x) {
								size_t srcIdx = (size_t)y * outInfo.Width + x;
								int dstX = y;
								int dstY = outInfo.Width - 1 - x;
								size_t dstIdx = (size_t)dstY * outW + (size_t)dstX;
								rotBuffer[dstIdx] = src[srcIdx];
							}
						}
						_videoRefresh(rotBuffer.data(), outW, outH, outW * 4);
					} else {
						// Unknown rotation: fallback to non-rotated output
						_videoRefresh(src, outInfo.Width, outInfo.Height, outInfo.Width * 4);
					}
				}
			}
		}

		if(updated) {
			// Only update geometry if something changed or if flag is set
			if(_geometryDirty) {
				retro_system_av_info avInfo = {};
				retro_get_system_av_info(&avInfo);
				uint32_t newWidth = avInfo.geometry.base_width;
				uint32_t newHeight = avInfo.geometry.base_height;
				
				// Only call SET_GEOMETRY if dimensions actually changed
				if(newWidth != _lastReportedWidth || newHeight != _lastReportedHeight) {
					_lastReportedWidth = newWidth;
					_lastReportedHeight = newHeight;
					env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avInfo);
					fprintf(stderr, "[libretro] Geometry updated: %ux%u\n", newWidth, newHeight);
				}
				_geometryDirty = false;
			}
		}

		// Audio upload handled by emulator/sound subsystem; no-op here for now.
	}

	RETRO_API size_t retro_serialize_size()
	{
		return _saveStateSize;
	}

	RETRO_API bool retro_serialize(void *data, size_t size)
	{
		if(!_emu) return false;
		try {
			std::stringstream ss;
			_emu->Serialize(ss, true, 1);
			std::string out = ss.str();
			if(out.size() > size) return false;
			memcpy(data, out.data(), out.size());
			return true;
		} catch(...) {
			return false;
		}
	}

	RETRO_API bool retro_unserialize(const void *data, size_t size)
	{
		if(!_emu) return false;
		try {
			std::string in((const char*)data, size);
			std::stringstream ss(in);
			auto res = _emu->Deserialize(ss, SaveStateManager::FileFormatVersion, true);
			return (res == DeserializeResult::Success);
		} catch(...) {
			return false;
		}
	}

	RETRO_API void retro_cheat_reset()
	{
		if(_emu) {
			// Cheat manager API moved; no-op for now.
		}
	}

	RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *codeStr)
	{
		static const string validGgLetters = "APZLGITYEOXUKSVN";
		static const string validParLetters = "0123456789ABCDEF";
		int chl = 0;

		string code = codeStr;
		std::transform(code.begin(), code.end(), code.begin(), ::toupper);

		if(code[4] == ':') {
			for(;;) {
				string address = code.substr((0 + chl), 4);
				string value = code.substr((5 + chl), 2);
				// Cheat API moved — ignore for now
				if(code[(7 + chl)] != '+') {
					return;
				}
				chl = (chl + 8);
			}
		}

		else if(code[4] == '?' && code[7] == ':') {
			for(;;) {
				string address = code.substr((0 + chl), 4);
				string comparison = code.substr((5 + chl), 2);
				string value = code.substr((8 + chl), 2);
            // Cheat API moved; ignore custom cheat add here for now.
				if(code[(10 + chl)] != '+') {
					return;
				}
				chl = (chl + 11);
			}
		}

		else {
			//This is either a GG or PAR code
			bool isValidGgCode = true;
			bool isValidParCode = true;

			for(size_t i = 0; i < 6; i++) {
				if(validGgLetters.find(code[i]) == string::npos) {
					isValidGgCode = false;
				}
			}
			for(size_t i = 0; i < 8; i++) {
				if(validParLetters.find(code[i]) == string::npos) {
					isValidParCode = false;
				}
			}

			if(isValidGgCode && code[6] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 6);
				// Cheat API moved; ignore Game Genie add here for now.
					if(code[(6 + chl)] != '+') {
						return;
					}
					chl = (chl + 7);
				}
			}
			else if(isValidGgCode && code[8] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 8);
				// Cheat API moved; ignore Game Genie add here for now.
					if(code[(8 + chl)] != '+') {
						return;
					}
					chl = (chl + 9);
				}
			}
			else if(isValidGgCode) {
				// Cheat API moved; ignore Game Genie add here for now.
			}

			else if(isValidParCode && code[8] == '+') {
				for(;;) {
					string code1 = code.substr((0 + chl), 8);
				// Cheat API moved; ignore Pro Action Rocky code add here for now.
					if(code[(8 + chl)] != '+') {
						return;
					}
					chl = (chl + 9);
				}
			}
			else if(isValidParCode) {
				// Cheat API moved; ignore Pro Action Rocky code add here for now.
			}

		}

	}

	void update_input_descriptors()
	{
		vector<retro_input_descriptor> desc;

		auto addDesc = [&desc](unsigned port, unsigned button, const char* name) {
			retro_input_descriptor d = { port, RETRO_DEVICE_JOYPAD, 0, button, name };
			desc.push_back(d);
		};

    auto setupPlayerButtons = [&addDesc](int port) {
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "X");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Y");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "L");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "R");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
        addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
    };
/*			if(device == DEVICE_AUTO) {
				if(port <= 3) {
					switch(_console->GetSettings()->GetControllerType(port)) {
						case ControllerType::StandardController: device = DEVICE_GAMEPAD; break;
						case ControllerType::PowerPad: device = DEVICE_POWERPAD; break;
						case ControllerType::SnesController: device = DEVICE_SNESGAMEPAD; break;
						case ControllerType::SnesMouse: device = DEVICE_SNESMOUSE; break;
						case ControllerType::Zapper: device = DEVICE_ZAPPER; break;
						case ControllerType::ArkanoidController: device = DEVICE_ARKANOID; break;
						case ControllerType::VbController: device = DEVICE_VBGAMEPAD; break;
						default: return;
					}
				} else if(port == 4) {
					switch(_console->GetSettings()->GetExpansionDevice()) {
						case ExpansionPortDevice::ArkanoidController: device = DEVICE_ARKANOID; break;
						case ExpansionPortDevice::BandaiHyperShot: device = DEVICE_BANDAIHYPERSHOT; break;
						case ExpansionPortDevice::ExcitingBoxing: device = DEVICE_EXCITINGBOXING; break;
						case ExpansionPortDevice::FamilyTrainerMat: device = DEVICE_FAMILYTRAINER; break;
						case ExpansionPortDevice::HoriTrack: device = DEVICE_HORITRACK; break;
						case ExpansionPortDevice::KonamiHyperShot: device = DEVICE_KONAMIHYPERSHOT; break;
						case ExpansionPortDevice::OekaKidsTablet: device = DEVICE_OEKAKIDS; break;
						case ExpansionPortDevice::Pachinko: device = DEVICE_PACHINKO; break;
						case ExpansionPortDevice::PartyTap: device = DEVICE_PARTYTAP; break;
						case ExpansionPortDevice::Zapper: device = DEVICE_ZAPPER; break;
						case ExpansionPortDevice::BattleBox: device = DEVICE_BATTLEBOX; break;
						case ExpansionPortDevice::AsciiTurboFile: device = DEVICE_ASCIITURBOFILE; break;
						case ExpansionPortDevice::FourPlayerAdapter: device = DEVICE_FOURPLAYERADAPTER; break;
						default: return;
					}
				}
			}

			if(device == DEVICE_GAMEPAD || device == DEVICE_SNESGAMEPAD) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
				if(device == DEVICE_SNESGAMEPAD) {
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "X");
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Y");
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "L");
					addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "R");
				} else {
					if(_shiftButtonsClockwise) {
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "A");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "B");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Turbo A");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Turbo B");
					} else {
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "A");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "B");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Turbo A");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B");
					}

					if(port == 0) {
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "(FDS) Insert Next Disk");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "(FDS) Switch Disk Side");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_L2, "(VS) Insert Coin 1");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_R2, "(VS) Insert Coin 2");
						addDesc(port, RETRO_DEVICE_ID_JOYPAD_L3, "(Famicom) Microphone (P2)");
					}
				}
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select");
			} else if(device == DEVICE_EXCITINGBOXING) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Left Hook");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Right Hook");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Left Jab");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Right Jab");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Body");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Straight");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "Move Left");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Move Right");
			} else if(device == DEVICE_PARTYTAP) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Partytap P1");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Partytap P2");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Partytap P3");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Partytap P4");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Partytap P5");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Partytap P6");
			} else if(device == DEVICE_FAMILYTRAINER || device == DEVICE_POWERPAD) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Powerpad B1");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Powerpad B2");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Powerpad B3");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Powerpad B4");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Powerpad B5");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Powerpad B6");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "Powerpad B7");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Powerpad B8");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "Powerpad B9");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "Powerpad B10");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Powerpad B11");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Powerpad B12");
			} else if(device == DEVICE_PACHINKO) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Release Trigger");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Press Trigger");
			} else if(device == DEVICE_VBGAMEPAD) {
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_B, "Virtual Boy D-Pad 2 Down");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_Y, "Virtual Boy D-Pad 2 Left");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_SELECT, "Virtual Boy Select");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_START, "Virtual Boy Start");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_UP, "Virtual Boy D-Pad 1 Up");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "Virtual Boy D-Pad 1 Down");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "Virtual Boy D-Pad 1 Left");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Virtual Boy D-Pad 1 Right");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_A, "Virtual Boy D-Pad 2 Right");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_X, "Virtual Boy D-Pad 2 Up");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L, "Virtual Boy L");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R, "Virtual Boy R");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_L2, "Virtual Boy B");
				addDesc(port, RETRO_DEVICE_ID_JOYPAD_R2, "Virtual Boy A");
			}
		};
*/
		setupPlayerButtons(0);
		setupPlayerButtons(1);
		setupPlayerButtons(2);
		setupPlayerButtons(3);
		setupPlayerButtons(4);

		retro_input_descriptor end = { 0 };
		desc.push_back(end);

	// Avoid sending identical input descriptors repeatedly (RetroArch logs each SET_INPUT_DESCRIPTORS call).
	// Build a lightweight representation we can compare to the last one and skip the env_cb if unchanged.
	struct SimpleDesc { unsigned port; unsigned device; unsigned index; unsigned id; std::string name; };
	static std::vector<SimpleDesc> lastDesc;
	std::vector<SimpleDesc> curDesc;
	curDesc.reserve(desc.size());
	for(size_t i = 0; i < desc.size(); ++i) {
		const retro_input_descriptor &d = desc[i];
		if(d.description == nullptr) break; // end sentinel
		curDesc.push_back({ d.port, d.device, d.index, d.id, std::string(d.description) });
	}

	bool same = (curDesc.size() == lastDesc.size());
	if(same) {
		for(size_t i = 0; i < curDesc.size(); ++i) {
			const SimpleDesc &a = curDesc[i];
			const SimpleDesc &b = lastDesc[i];
			if(a.port != b.port || a.device != b.device || a.index != b.index || a.id != b.id || a.name != b.name) { same = false; break; }
		}
	}

	if(!same) {
		env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc.data());
		lastDesc.swap(curDesc);
	}
	}

	void update_core_controllers()
	{
		// Map libretro-selected devices into the emulator's settings.
		// Guard against being called before a console exists.
		if(!_console) return;

		// Ensure ports 0/1 default to gamepads when still "auto"
		if(_inputDevices[0] == DEVICE_AUTO) _inputDevices[0] = DEVICE_GAMEPAD;
		if(_inputDevices[1] == DEVICE_AUTO) _inputDevices[1] = DEVICE_GAMEPAD;
		// make sure other ports have a sane default
		for(int port = 2; port < 5; ++port) {
			if(_inputDevices[port] == DEVICE_AUTO) _inputDevices[port] = RETRO_DEVICE_NONE;
		}

		// Map ports 0..3 to ControllerType using available enums in SettingTypes.h
		for(int port = 0; port <= 3; ++port) {
			ControllerType type = ControllerType::NesController;
			switch(_inputDevices[port]) {
				case RETRO_DEVICE_NONE: type = ControllerType::None; break;
				case DEVICE_GAMEPAD: type = ControllerType::NesController; break;
				case DEVICE_ZAPPER: type = ControllerType::NesZapper; break;
				case DEVICE_POWERPAD: type = ControllerType::PowerPadSideA; break; // default to side A
				case DEVICE_ARKANOID: type = ControllerType::NesArkanoidController; break;
				case DEVICE_SNESGAMEPAD: type = ControllerType::SnesController; break;
				case DEVICE_SNESMOUSE: type = ControllerType::SnesMouse; break;
				case DEVICE_VBGAMEPAD: type = ControllerType::VirtualBoyController; break;
				default: /* leave as default */ break;
			}
			// Emu settings now expose NesConfig; update that and write it back.
			NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
			if(port == 0) nesCfg.Port1.Type = type;
			else if(port == 1) nesCfg.Port2.Type = type;
			// Ports 2/3 map to subports; leave them alone for now to avoid mismapping.
			_emu->GetSettings()->SetNesConfig(nesCfg);
		}

		// Map port 4 selection to one of the ControllerType expansion/device enums where applicable
		ControllerType expType = ControllerType::None;
		switch(_inputDevices[4]) {
			case RETRO_DEVICE_NONE: expType = ControllerType::None; break;
			case DEVICE_FAMILYTRAINER: expType = ControllerType::FamilyTrainerMatSideA; break;
			case DEVICE_PARTYTAP: expType = ControllerType::PartyTap; break;
			case DEVICE_PACHINKO: expType = ControllerType::Pachinko; break;
			case DEVICE_EXCITINGBOXING: expType = ControllerType::ExcitingBoxing; break;
			case DEVICE_KONAMIHYPERSHOT: expType = ControllerType::KonamiHyperShot; break;
			case DEVICE_OEKAKIDS: expType = ControllerType::OekaKidsTablet; break;
			case DEVICE_BANDAIHYPERSHOT: expType = ControllerType::BandaiHyperShot; break;
			case DEVICE_ARKANOID: expType = ControllerType::NesArkanoidController; break;
			case DEVICE_HORITRACK: expType = ControllerType::HoriTrack; break;
			case DEVICE_ASCIITURBOFILE: expType = ControllerType::AsciiTurboFile; break;
			case DEVICE_BATTLEBOX: expType = ControllerType::BattleBox; break;
			case DEVICE_FOURPLAYERADAPTER: expType = ControllerType::FourPlayerAdapter; break;
			default: break;
		}
		// Map expansion/device selection into the NesConfig ExpPort
		NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
		nesCfg.ExpPort.Type = expType;
		_emu->GetSettings()->SetNesConfig(nesCfg);

		// Set HasFourScore if expansion or extra controllers indicate it
		bool hasFourScore = false;
	// Consider expansion port (now stored in NesConfig.ExpPort) or extra controllers
	NesConfig nesCfg2 = _emu->GetSettings()->GetNesConfig();
	if(nesCfg2.ExpPort.Type != ControllerType::None) hasFourScore = true;
	// We can't reliably map ports 2/3 without the higher-level helper; skip them here.
	// Do not set a non-existent EmulationFlags::HasFourScore; let higher-level code infer it.
/*		//Setup all "auto" ports
		RomInfo romInfo = _console->GetRomInfo();
		if(romInfo.IsInDatabase || romInfo.IsNes20Header) {
			_console->GetSettings()->InitializeInputDevices(romInfo.InputType, romInfo.System, true);
		} else {
			_console->GetSettings()->InitializeInputDevices(GameInputType::StandardControllers, GameSystem::NesNtsc, true);
		}

		for(int port = 0; port < 5; port++) {
			if(_inputDevices[port] != DEVICE_AUTO) {
				if(port <= 3) {
					ControllerType type = ControllerType::StandardController;
					switch(_inputDevices[port]) {
						case RETRO_DEVICE_NONE: type = ControllerType::None; break;
						case DEVICE_GAMEPAD: type = ControllerType::StandardController; break;
						case DEVICE_ZAPPER: type = ControllerType::Zapper; break;
						case DEVICE_POWERPAD: type = ControllerType::PowerPad; break;
						case DEVICE_ARKANOID: type = ControllerType::ArkanoidController; break;
						case DEVICE_SNESGAMEPAD: type = ControllerType::SnesController; break;
						case DEVICE_SNESMOUSE: type = ControllerType::SnesMouse; break;
						case DEVICE_VBGAMEPAD: type = ControllerType::VbController; break;
					}
					_console->GetSettings()->SetControllerType(port, type);
				} else {
					ExpansionPortDevice type = ExpansionPortDevice::None;
					switch(_inputDevices[port]) {
						case RETRO_DEVICE_NONE: type = ExpansionPortDevice::None; break;
						case DEVICE_FAMILYTRAINER: type = ExpansionPortDevice::FamilyTrainerMat; break;
						case DEVICE_PARTYTAP: type = ExpansionPortDevice::PartyTap; break;
						case DEVICE_PACHINKO: type = ExpansionPortDevice::Pachinko; break;
						case DEVICE_EXCITINGBOXING: type = ExpansionPortDevice::ExcitingBoxing; break;
						case DEVICE_KONAMIHYPERSHOT: type = ExpansionPortDevice::KonamiHyperShot; break;
						case DEVICE_OEKAKIDS: type = ExpansionPortDevice::OekaKidsTablet; break;
						case DEVICE_BANDAIHYPERSHOT: type = ExpansionPortDevice::BandaiHyperShot; break;
						case DEVICE_ARKANOID: type = ExpansionPortDevice::ArkanoidController; break;
						case DEVICE_HORITRACK: type = ExpansionPortDevice::HoriTrack; break;
						case DEVICE_ASCIITURBOFILE: type = ExpansionPortDevice::AsciiTurboFile; break;
						case DEVICE_BATTLEBOX: type = ExpansionPortDevice::BattleBox; break;
						case DEVICE_FOURPLAYERADAPTER: type = ExpansionPortDevice::FourPlayerAdapter; break;
					}
					_console->GetSettings()->SetExpansionDevice(type);
				}
			}
		}

		bool hasFourScore = false;
		bool isFamicom = (_console->GetSettings()->GetExpansionDevice() != ExpansionPortDevice::None || romInfo.System == GameSystem::Famicom || romInfo.System == GameSystem::FDS || romInfo.System == GameSystem::Dendy);
		if(isFamicom) {
			_console->GetSettings()->SetConsoleType(ConsoleType::Famicom);
			if(_console->GetSettings()->GetExpansionDevice() == ExpansionPortDevice::FourPlayerAdapter) {
				hasFourScore = true;
			}
		} else {
			_console->GetSettings()->SetConsoleType(ConsoleType::Nes);
			if(_console->GetSettings()->GetControllerType(2) != ControllerType::None || _console->GetSettings()->GetControllerType(3) != ControllerType::None) {
				hasFourScore = true;
			}
		}

		_console->GetSettings()->SetFlagState(EmulationFlags::HasFourScore, hasFourScore);
*/	}
	
	void retro_set_memory_maps()
	{
		// The console/Memory APIs were refactored. Provide an empty memory map for now.
		retro_memory_map memoryMap = {};
		memoryMap.descriptors = nullptr;
		memoryMap.num_descriptors = 0;
		env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &memoryMap);
/*		//Expose internal RAM and work/save RAM for retroachievements
		retro_memory_descriptor descriptors[256] = {};
		retro_memory_map memoryMap = {};

		int count = 0;
		for(int i = 0; i <= 0xFFFF; i += 0x100) {
			uint8_t* ram = _console->GetRamBuffer(i);
			if(ram) {
				descriptors[count].ptr = ram;
				descriptors[count].start = i;
				descriptors[count].len = 0x100;
				count++;
			}
		}

		memoryMap.descriptors = descriptors;
		memoryMap.num_descriptors = count;

		env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &memoryMap);
*/	}

	RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
	{
		if(port < 5 && _inputDevices[port] != device) {
			_inputDevices[port] = device;
		update_core_controllers();
		update_input_descriptors();
		}
	}

	RETRO_API bool retro_load_game(const struct retro_game_info *game)
	{
		char *saveFolder;
		char *systemFolder;
		if(!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemFolder) || !systemFolder)
			return false;

		if(!env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveFolder)) {
			logMessage(RETRO_LOG_ERROR, "Could not find save directory.\n");
		}

		enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
		if(!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
			logMessage(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
			return false;
		}

		//Expect the following structure:
		// /system/disksys.rom
		// /system/HdPacks/*
		// /saves/*.sav
		FolderUtilities::SetHomeFolder(systemFolder);
		// SetFolderOverrides signature expects 4 strings (save, savestate, screenshot, firmware).
		FolderUtilities::SetFolderOverrides(saveFolder, std::string(""), std::string(""), std::string(""));
		update_settings();

		// Controller/Settings API changed; skip initial controller setup here
		//Plug in 2 standard controllers by default, game database will switch the controller types for recognized games
/*		_console->GetSettings()->SetMasterVolume(10.0);
		_console->GetSettings()->SetControllerType(0, ControllerType::StandardController);
		_console->GetSettings()->SetControllerType(1, ControllerType::StandardController);
		_console->GetSettings()->SetControllerType(2, ControllerType::None);
		_console->GetSettings()->SetControllerType(3, ControllerType::None);
*/
		// Attempt to fetch extended game info
		const struct retro_game_info_ext *gameExt = NULL;
		const void *gameData = NULL;
		size_t gameSize = 0;
		string gamePath("");
		if (env_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &gameExt)) {
			gameData = gameExt->data;
			gameSize = gameExt->size;
			if (gameExt->file_in_archive) {
				// We don't have a 'physical' file in this
				// case, but the core still needs a filename
				// in order to detect associated content
				// (i.e. HdPacks). We therefore fake it, using
				// the content directory, canonical content
				// name, and content file extension
#if defined(_WIN32)
				char slash = '\\';
#else
				char slash = '/';
#endif
				gamePath = string(gameExt->dir) +
							  string(1, slash) +
							  string(gameExt->name) +
							  "." +
							  string(gameExt->ext);
			} else {
				gamePath = gameExt->full_path;
			}
		} else {
			// No extended game info; all we have is the
			// content fullpath from the retro_game_info
			// struct
			gamePath = game->path;
		}

/*		// Load content
		VirtualFile romData(gameData, gameSize, gamePath);
		bool result = _console->Initialize(romData);

		if(result) {
			//Set default dipswitches for some VS System games
			switch(_console->GetRomInfo().Hash.PrgCrc32) {
				case 0x8850924B: _console->GetSettings()->SetDipSwitches(32); break; //VS Tetris
				case 0xE1AA8214: _console->GetSettings()->SetDipSwitches(32); break; //StarLuster
				default: _console->GetSettings()->SetDipSwitches(0); break;
			}

			update_core_controllers();
			update_input_descriptors();

			//Savestates in Mesen may change size over time
			//Retroarch doesn't like this for netplay or rewinding - it requires the states to always be the exact same size
			//So we need to send a large enough size to Retroarch to ensure Mesen's state will always fit within that buffer.
			std::stringstream ss;
			_console->GetSaveStateManager()->SaveState(ss);

			//Round up to the next 1kb multiple
			_saveStateSize = ((ss.str().size() * 2) + 0x400) & ~0x3FF;
			retro_set_memory_maps();
		}

		return result;
*/
		// Build a VirtualFile for the ROM (either from memory or from a path)
		VirtualFile romData;
		if(gameData && gameSize > 0) {
			romData = VirtualFile(gameData, gameSize, gamePath);
		} else {
			romData = VirtualFile(gamePath);
		}

		// Set HD pack configuration BEFORE loading ROM (LoadHdPack checks this during ROM load)
		{
			NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
			nesCfg.EnableHdPacks = _hdPacksEnabled;
			_emu->GetSettings()->SetNesConfig(nesCfg);
		}

		// Attempt to load the ROM via the Emulator API
		bool result = false;
		try {
			// Do not instruct the emulator to stop any existing ROM here - letting it avoid
			// the Stop() path which can trigger complex shutdown behavior inside a libretro host.
			fprintf(stderr, "[libretro] Loading ROM: %s (HD packs %s)\n", gamePath.c_str(), _hdPacksEnabled ? "enabled" : "disabled");
			result = _emu->LoadRom(romData, VirtualFile(), false, false);
			fprintf(stderr, "[libretro] ROM load result: %s\n", result ? "SUCCESS" : "FAILED");
		} catch(...) {
			fprintf(stderr, "[libretro] Exception during ROM load\n");
			result = false;
		}

		if(result) {
			// Update the concrete console shared_ptr now that a ROM is loaded
			auto consoleIf = _emu->GetConsole();
			_console = std::dynamic_pointer_cast<NesConsole>(consoleIf);

			// Inform the LibretroKeyManager of the concrete console/emulator so it can
			// safely mark itself ready to be polled and access emulator data (mouse pos, etc.).
			if(_keyManager) {
				_keyManager->SetConsole(consoleIf, _emu.get());
			}

			// Initialize audio settings to enable sound output
			// Ensure audio is enabled and channel volumes are set
			{
				AudioConfig audioCfg = _emu->GetSettings()->GetAudioConfig();
				audioCfg.EnableAudio = true;
				audioCfg.MasterVolume = 100;
				_emu->GetSettings()->SetAudioConfig(audioCfg);
				
				NesConfig nesCfg = _emu->GetSettings()->GetNesConfig();
				// Initialize all channel volumes to 100 (full volume) if they're zero
				for(size_t i = 0; i < 11; ++i) {
					if(nesCfg.ChannelVolumes[i] == 0) {
						nesCfg.ChannelVolumes[i] = 100;
					}
					// Initialize panning to center (50) if zero
					if(nesCfg.ChannelPanning[i] == 0) {
						nesCfg.ChannelPanning[i] = 50;
					}
				}
				_emu->GetSettings()->SetNesConfig(nesCfg);
			}

			// Allow the game/db-specific controller initialization to run
			update_core_controllers();
			update_input_descriptors();

			// Compute a safe save state size to report to the frontend
			std::stringstream ss;
			_emu->GetSaveStateManager()->SaveState(ss);
			_saveStateSize = ((ss.str().size() * 2) + 0x400) & ~0x3FF;

			// Update memory maps (still a stubbed implementation for now)
			retro_set_memory_maps();

			// Register the libretro audio device now that the emulator is initialized and consoles are ready
			if(_audioDevice && _emu && _emu->GetSoundMixer()) {
				fprintf(stderr, "[libretro] Registering audio device with SoundMixer\n");
				_emu->GetSoundMixer()->RegisterAudioDevice(_audioDevice.get());
				fprintf(stderr, "[libretro] Audio device registered successfully\n");
			} else {
				fprintf(stderr, "[libretro] WARNING: Cannot register audio device - _audioDevice=%p, _emu=%p, mixer=%p\n",
					_audioDevice.get(), _emu.get(), _emu ? _emu->GetSoundMixer() : nullptr);
			}

			// Update geometry if HD packs are loaded (they may change the resolution)
			if(_console && _console->GetHdData()) {
				fprintf(stderr, "[libretro] HD pack detected - updating geometry\n");
				retro_system_av_info avInfo = {};
				retro_get_system_av_info(&avInfo);
				env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &avInfo);
			}

			// Forward any saved input callbacks into the key manager now that console/key manager are initialized.
			if(_keyManager) {
				if(_savedGetInputState) _keyManager->SetGetInputState(_savedGetInputState);
				if(_savedPollInput) _keyManager->SetPollInput(_savedPollInput);
				// Re-check bitmask support
				if (env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
					_keyManager->SetSupportsInputBitmasks(true);
			}

			// Diagnostic probe: call saved callbacks once from this thread and log their values.
			// This uses the shared probe helper so we don't duplicate the implementation.
			if(_savedPollInput || _savedGetInputState) {
				// forward-declare the shared probe helper (defined later in this file)
				if(getenv("MESEN_LIBRETRO_VERBOSE_INPUT")) {
					void libretro_probe_inputs(const char* tag);
					libretro_probe_inputs("retro_load_game");
				}
			}
		} else {
			logMessage(RETRO_LOG_ERROR, "retro_load_game: Failed to load ROM via Emulator::LoadRom.\n");
			_saveStateSize = 0;
		}

		return result;
	}

	RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
	{
		return false;
	}

	RETRO_API void retro_unload_game()
	{
	}

	RETRO_API unsigned retro_get_region()
	{
	/*	NesModel model = _console->GetModel();
		return model == NesModel::NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
	*/
		// The NesConsole / Emulator APIs were refactored. Return NTSC for now.
		return RETRO_REGION_NTSC;	
	}

	RETRO_API void retro_get_system_info(struct retro_system_info *info)
	{
		// TODO: Replace with real version string when available
   	static std::string version = "2.0.0";
   	_mesenVersion = version;
		//_mesenVersion = EmulationSettings::GetMesenVersionString();

		info->library_name = "Mesen2-NES";
		info->library_version = _mesenVersion.c_str();
		// need_fullpath is required since HdPacks are
		// identified via the rom file name
		info->need_fullpath = true;
		info->valid_extensions = "nes|fds|unf|unif";
		info->block_extract = false;
	}

	RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
	{
		memset(info, 0, sizeof(*info));
		
		uint32_t width = 256;
		uint32_t height = 240;
		
		// Get actual filtered frame size if available
		if(_emu && _emu->GetVideoDecoder()) {
			FrameInfo frameSize = _emu->GetVideoDecoder()->GetFrameInfo();
			width = frameSize.Width;
			height = frameSize.Height;
		}
		
		// Check if HD packs are loaded and apply additional scaling
		uint32_t hscale = 1;
		uint32_t vscale = 1;
		
		if(_console) {
			auto hdData = _console->GetHdData().lock();
			if(hdData) {
				hscale = hdData->Scale;
				vscale = hdData->Scale;
				fprintf(stderr, "[libretro] HD pack detected: scale = %u\n", hscale);
			}
		}
		
		// Apply HD scale to final dimensions
		width *= hscale;
		height *= vscale;
		
		info->geometry.base_width = width;
		info->geometry.base_height = height;
		info->geometry.max_width = 256 * 8 * 4; // generous max (8x scale + 4x HD)
		info->geometry.max_height = 240 * 8 * 4;
		info->geometry.aspect_ratio = 4.0f / 3.0f;
		info->timing.fps = 60.0988;
		info->timing.sample_rate = 44100.0;
		
		fprintf(stderr, "[libretro] AV info: %ux%u (aspect %.2f, fps %.4f)\n", 
			width, height, info->geometry.aspect_ratio, info->timing.fps);
	}

	RETRO_API void *retro_get_memory_data(unsigned id)
	{
/*		BaseMapper* mapper = _console->GetMapper();
		switch(id) {
			case RETRO_MEMORY_SAVE_RAM: return mapper->GetSaveRam();
			case RETRO_MEMORY_SYSTEM_RAM: return _console->GetMemoryManager()->GetInternalRAM();
		}*/
		// Mapper and memory APIs were refactored. Provide safe fallbacks:
		BaseMapper* mapper = (_console) ? _console->GetMapper() : nullptr;
		switch(id) {
			case RETRO_MEMORY_SAVE_RAM:
				// Save-ram accessor moved/hidden in BaseMapper; return nullptr until mapped to new API.
				(void)mapper;
				return nullptr;
			case RETRO_MEMORY_SYSTEM_RAM:
				// Use the renamed GetInternalRam() if available on NesMemoryManager.
				if(_console && _console->GetMemoryManager()) {
					return _console->GetMemoryManager()->GetInternalRam();
				}
				return nullptr;
		}
		return nullptr;
	}

	RETRO_API size_t retro_get_memory_size(unsigned id)
	{
		switch(id) {
			case RETRO_MEMORY_SAVE_RAM: //return mapper->GetMemorySize(DebugMemoryType::SaveRam);
			   // Mapper API changed; save-RAM access moved. Return 0 for now.
            // TODO: replace with mapper->GetSaveRam() equivalent when API is available.
            return 0;
			case RETRO_MEMORY_SYSTEM_RAM: //return MemoryManager::InternalRAMSize;
            // Memory manager API renamed to GetInternalRam()
            // Use the new method if available; otherwise return 0.
            return 0;		
		}
		return 0;
	}
}
