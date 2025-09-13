#pragma once
#include "libretro.h"
#include "../Core/Shared/Interfaces/IKeyManager.h"
#include "../Core/Shared/KeyManager.h"
#include "../Core/Shared/SystemActionManager.h"
//#include "../Core/VsSystemActionManager.h"
#include "../Core/Shared/Interfaces/IConsole.h"

class LibretroKeyManager : public IKeyManager
{
private:
	std::shared_ptr<IConsole> _console;
	retro_input_state_t _getInputState = nullptr;
	int16_t _joypadButtons[5] = { };
	retro_input_poll_t _pollInput = nullptr;
	bool _mouseButtons[3] = { false, false, false };
	bool _supportsInputBitmasks = false;
	bool _wasPushed[16] = { };

	bool ProcessAction(uint32_t button)
	{
		bool buttonPressed = false;
		if (_supportsInputBitmasks)
			buttonPressed = (_joypadButtons[0] & (1 << button)) != 0;
		else
			buttonPressed = _getInputState(0, RETRO_DEVICE_JOYPAD, 0, button) != 0;

		if(buttonPressed) {
			if(!_wasPushed[button]) {
				//Newly pressed, process action
				_wasPushed[button] = true;
				return true;
			}
		} else {
			_wasPushed[button] = false;
		}
		return false;
	}
	
public:
    LibretroKeyManager(std::shared_ptr<IConsole> console)
    {
        _console = console;
        KeyManager::RegisterKeyManager(this);
    }

    ~LibretroKeyManager()
    {
        KeyManager::RegisterKeyManager(nullptr);
    }

    void SetGetInputState(retro_input_state_t getInputState)
    {
        _getInputState = getInputState;
    }

    void SetPollInput(retro_input_poll_t pollInput)
    {
        _pollInput = pollInput;
    }

    void SetSupportsInputBitmasks(bool supportsInputBitmasks)
    {
        _supportsInputBitmasks = supportsInputBitmasks;
    }

    // Inherited via IKeyManager
    virtual void RefreshState() override
    {
        if(_pollInput) {
            _pollInput();
        }

        if(_getInputState) {
            int32_t x = _getInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
            int32_t y = _getInputState(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

            x += 0x8000;
            y += 0x8000;

            // New KeyManager API expects a MousePosition struct
            MousePosition mp;
            mp.X = 0;
            mp.Y = 0;
            mp.RelativeX = (double)x / 0x10000;
            mp.RelativeY = (double)y / 0x10000;
            // KeyManager::SetMousePosition now takes (Emulator*, double x, double y).
            // Pass nullptr for the Emulator* if the key manager doesn't have a pointer to it.
            KeyManager::SetMousePosition(nullptr, (double)x / 0x10000, (double)y / 0x10000);

            int16_t dx = _getInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
            int16_t dy = _getInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
            KeyManager::SetMouseMovement(dx, dy);

            if (_supportsInputBitmasks)
            ;
            
            _mouseButtons[(int)MouseButton::LeftButton] = _getInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
            _mouseButtons[(int)MouseButton::RightButton] = _getInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
            _mouseButtons[(int)MouseButton::MiddleButton] = _getInputState(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;
        }
    }

    // Implementations required by the current IKeyManager interface:

    // update device list / poll devices (no-op here, keep compatibility)
    virtual void UpdateDevices() override
    {
        // libretro backend drives input via RefreshState(), so nothing needed
    }

    // Return currently pressed keys (not used by libretro backend; return empty)
    virtual vector<uint16_t> GetPressedKeys() override
    {
        return {};
    }

    // IsKeyPressed signature updated to match IKeyManager
    virtual bool IsKeyPressed(uint16_t keyCode) override
    {
        if(keyCode > 0 && _getInputState)
        {
            // old implementation assumed (port<<8) | (retroKey+1)
            int port = (keyCode >> 8) & 0xFF;
            int retroKey = (keyCode & 0xFF) - 1;
            if(retroKey < 0) return false;
            if(_supportsInputBitmasks) {
                // bitmask path
                return (_joypadButtons[port] & (1 << retroKey)) != 0;
            }
            return _getInputState(port, RETRO_DEVICE_JOYPAD, 0, retroKey) != 0;
        }
        return false;
    }

    virtual bool IsMouseButtonPressed(MouseButton button) override
    {
        return _mouseButtons[(int)button];
    }

    // GetKeyName/GetKeyCode signatures updated to match IKeyManager
    virtual string GetKeyName(uint16_t keyCode) override
    {
        // Not used; return empty string
        return string();
    }

    virtual uint16_t GetKeyCode(string keyName) override
    {
        // Not used; return 0
        return 0;
    }

    // New control methods required by IKeyManager
    virtual bool SetKeyState(uint16_t scanCode, bool state) override
    {
        // Not supported in this backend
        (void)scanCode; (void)state;
        return false;
    }

    virtual void ResetKeyState() override
    {
        // No persistent key state to clear
    }

    virtual void SetDisabled(bool disabled) override
    {
        // No-op for libretro backend
        (void)disabled;
    }

    // Optional force feedback (kept as no-op)
    virtual void SetForceFeedback(uint16_t magnitudeRight, uint16_t magnitudeLeft) override
    {
        (void)magnitudeRight; (void)magnitudeLeft;
    }
};
