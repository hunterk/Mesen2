#include "pch.h"
#include "Shared/Interfaces/IKeyManager.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/Video/VideoRenderer.h"

std::shared_ptr<IKeyManager> KeyManager::_keyManager = nullptr;
MousePosition KeyManager::_mousePosition = { 0, 0 };
double KeyManager::_xMouseMovement;
double KeyManager::_yMouseMovement;
EmuSettings* KeyManager::_settings = nullptr;
SimpleLock KeyManager::_lock;
std::atomic<bool> KeyManager::_backendReady { false };

void KeyManager::RegisterKeyManager(std::shared_ptr<IKeyManager> keyManager)
{
	_xMouseMovement = 0;
	_yMouseMovement = 0;
	std::atomic_store(&_keyManager, keyManager);
}

void KeyManager::RefreshKeyState()
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		// Do not call into the backend until it reports ready to be polled by
		// background threads. Backends must call KeyManager::SetBackendReady(true)
		// once they have initialized their internal pointers/callbacks to avoid
		// init-order races.
		if(!_backendReady.load()) return;
		return km->RefreshState();
	}
}

bool KeyManager::IsReady()
{
	// Readiness is now a KeyManager-level flag set by the backend when ready.
	return _backendReady.load();
}

void KeyManager::SetSettings(EmuSettings *settings)
{
	_settings = settings;
}

void KeyManager::SetBackendReady(bool ready)
{
	_backendReady.store(ready);
}

bool KeyManager::IsKeyPressed(uint16_t keyCode)
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		return _settings && _settings->IsInputEnabled() && km->IsKeyPressed(keyCode);
	}
	return false;
}

optional<int16_t> KeyManager::GetAxisPosition(uint16_t keyCode)
{
	auto km = std::atomic_load(&_keyManager);
	if(km && _settings && _settings->IsInputEnabled()) {
		return km->GetAxisPosition(keyCode);
	}
	return std::nullopt;
}

bool KeyManager::IsMouseButtonPressed(MouseButton button)
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		return _settings && _settings->IsInputEnabled() && km->IsMouseButtonPressed(button);
	}
	return false;
}

vector<uint16_t> KeyManager::GetPressedKeys()
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		return km->GetPressedKeys();
	}
	return vector<uint16_t>();
}

string KeyManager::GetKeyName(uint16_t keyCode)
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		return km->GetKeyName(keyCode);
	}
	return "";
}

uint16_t KeyManager::GetKeyCode(string keyName)
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		return km->GetKeyCode(keyName);
	}
	return 0;
}

void KeyManager::UpdateDevices()
{
	auto km = std::atomic_load(&_keyManager);
	if(km) {
		km->UpdateDevices();
	}
}

void KeyManager::SetMouseMovement(int16_t x, int16_t y)
{
	auto lock = _lock.AcquireSafe();
	_xMouseMovement += x;
	_yMouseMovement += y;
}

MouseMovement KeyManager::GetMouseMovement(Emulator* emu, uint32_t mouseSensitivity)
{
	constexpr double divider[10] = { 0.25, 0.33, 0.5, 0.66, 0.75, 1, 1.5, 2, 3, 4 };
	FrameInfo rendererSize = emu->GetVideoRenderer()->GetRendererSize();
	FrameInfo frameSize = emu->GetVideoDecoder()->GetFrameInfo();
	double scale = (double)rendererSize.Width / frameSize.Width;
	double factor = scale / divider[mouseSensitivity];

	MouseMovement mov = {};

	auto lock = _lock.AcquireSafe();
	mov.dx = (int16_t)(_xMouseMovement / factor);
	mov.dy = (int16_t)(_yMouseMovement / factor);
	_xMouseMovement -= (mov.dx * factor);
	_yMouseMovement -= (mov.dy * factor);

	return mov;
}

void KeyManager::SetMousePosition(Emulator* emu, double x, double y)
{
	if(x < 0 || y < 0) {
		_mousePosition.X = -1;
		_mousePosition.Y = -1;
		_mousePosition.RelativeX = -1;
		_mousePosition.RelativeY = -1;
	} else {
		OverscanDimensions overscan = emu->GetSettings()->GetOverscan();
		FrameInfo frame = emu->GetVideoDecoder()->GetBaseFrameInfo(true);
		_mousePosition.X = (int32_t)(x*frame.Width + overscan.Left);
		_mousePosition.Y = (int32_t)(y*frame.Height + overscan.Top);
		_mousePosition.RelativeX = x;
		_mousePosition.RelativeY = y;
	}
}

MousePosition KeyManager::GetMousePosition()
{
	return _mousePosition;
}

void KeyManager::SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft)
{
	auto km = std::atomic_load(&_keyManager);
	if(km && _settings) {
		double intensity = _settings->GetInputConfig().ForceFeedbackIntensity;
		km->SetForceFeedback(magnitudeRight * intensity, magnitudeLeft * intensity);
	}
}

void KeyManager::SetForceFeedback(uint16_t magnitude)
{
	SetForceFeedback(magnitude, magnitude);
}
