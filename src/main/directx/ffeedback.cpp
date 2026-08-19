/***************************************************************************
    Force Feedback (aka Haptic) Support — patched
    
    Linux: uses evdev (/dev/input/event*) with capability checks, stable
           device open/close, and a small bank of pre-uploaded effects.
    Windows: uses DirectInput 8; picks device via env var FF_TARGET_VIDPID
             (e.g., "0x046d:0xc24f") or first FF-capable device; non-exclusive
             cooperative level to avoid SDL conflicts; reacquires on loss.

    Copyright (c) 2025
***************************************************************************/

#include "ffeedback.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>

#ifdef __linux__
// --------------------------- Linux (evdev) ---------------------------
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <vector>
#include <string>


namespace forcefeedback {

static int fd = -1;
static bool g_supported = false;
static int g_max_force = 10000;
static int g_min_force = 0;
static int g_force_duration = 20;
static struct ff_effect effects[6]; // 0 unused, 1..5 = soft..strong (or vice versa)

static bool has_bit(const unsigned long* bits, int bit)
{
    return (bits[bit / (sizeof(unsigned long)*8)] >> (bit % (sizeof(unsigned long)*8))) & 1UL;
}

bool init(int max_force, int min_force, int duration_ms)
{
    if (fd >= 0) { g_supported = true; return true; }

    const char* devpath = "/dev/input/event";
    char device_file_name[64] = {0};

    for (int idx = 0; idx < 100; ++idx)
    {
        std::snprintf(device_file_name, sizeof(device_file_name), "%s%d", devpath, idx);
        int tmp = ::open(device_file_name, O_RDWR | O_CLOEXEC);
        if (tmp < 0) continue;

        // check this is a FF-capable device
        const size_t __BPL = sizeof(unsigned long) * 8;
        unsigned long ev_bits[(EV_MAX + __BPL - 1) / __BPL] = {};
        if (ioctl(tmp, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) == -1) { ::close(tmp); continue; }
        if (!has_bit(ev_bits, EV_FF)) { ::close(tmp); continue; }

        unsigned long ff_bits[(FF_MAX + __BPL - 1) / __BPL] = {};
        if (ioctl(tmp, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) == -1) { ::close(tmp); continue; }
        bool have_rumble  = has_bit(ff_bits, FF_RUMBLE);
        bool have_period  = has_bit(ff_bits, FF_PERIODIC);
        if (!have_rumble && !have_period) { ::close(tmp); continue; }

        fd = tmp;
        break;
    }

    if (fd < 0) { g_supported = false; return false; }

    // Attempt to set overall gain (ignore failures)
    struct input_event ie{};
    ie.type = EV_FF;
    ie.code = FF_GAIN;
    ie.value = 0x7fff;
    (void)write(fd, &ie, sizeof(ie));

    // Upload a small bank of effects: 1..5
    for (int j = 1; j <= 5; ++j)
    {
        struct ff_effect e{};
        e.type = FF_RUMBLE;
        e.id = -1;
        e.u.rumble.strong_magnitude = (unsigned short)std::max(0, std::min(0x7fff, max_force - (j-1) * (max_force - min_force) / 4));
        e.u.rumble.weak_magnitude   = e.u.rumble.strong_magnitude / 2;
        e.replay.length = std::max(10, duration_ms);
        e.replay.delay  = 0;

        if (ioctl(fd, EVIOCSFF, &e) == -1)
        {
            // try periodic sine as fallback
            std::memset(&e, 0, sizeof(e));
            e.type = FF_PERIODIC;
            e.id = -1;
            e.u.periodic.waveform = FF_SINE;
            e.u.periodic.magnitude = (unsigned short)std::max(0, std::min(0x7fff, max_force - (j-1) * (max_force - min_force) / 4));
            e.u.periodic.period = 50;
            e.u.periodic.offset = 0;
            e.u.periodic.phase = 0;
            e.replay.length = std::max(10, duration_ms);
            e.replay.delay  = 0;

            if (ioctl(fd, EVIOCSFF, &e) == -1)
            {
                // give up on this device
                ::close(fd); fd = -1; g_supported = false;
                return false;
            }
        }
        effects[j] = e;
    }

    g_supported = true;
    return true;
}

int set(int command, int force) // command is unused; keep for ABI compatibility
{
    if (!g_supported || fd < 0) return -1;
    int idx = std::max(1, std::min(5, force));

    struct input_event play{};
    play.type = EV_FF;
    play.code = effects[idx].id;
    play.value = 1;
    if (write(fd, &play, sizeof(play)) == -1) return -1;

    return 0;
}

void set_tyre_slip(bool)
{
}

void stop()
{
    if (g_pEffect)
        g_pEffect->Stop();
}

void close()
{
    if (fd >= 0)
    {
        // Try to stop all uploaded effects
        for (int j = 1; j <= 5; ++j)
        {
            if (effects[j].id >= 0)
            {
                struct input_event stop{};
                stop.type = EV_FF;
                stop.code = effects[j].id;
                stop.value = 0;
                (void)write(fd, &stop, sizeof(stop));
            }
        }
        ::close(fd);
        fd = -1;
    }
    g_supported = false;
}

bool is_supported()
{
    return g_supported;
}

} // namespace forcefeedback

#elif defined(_WIN32)

// --------------------------- Windows (DirectInput 8) ---------------------------

#define DIRECTINPUT_VERSION 0x0800
#define NOMINMAX

#include <windows.h>
#include <dinput.h>
#include <SDL.h>
#include <SDL_syswm.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace forcefeedback
{
    
    static LPDIRECTINPUT8       g_pDI = nullptr;
    static LPDIRECTINPUTDEVICE8 g_pDevice = nullptr;
    static LPDIRECTINPUTEFFECT  g_pEffect = nullptr;
    static LPDIRECTINPUTEFFECT  g_pSpringEffect = nullptr;
    static LPDIRECTINPUTEFFECT  g_pTyreSlipEffect = nullptr;
    static bool g_supported = false;
    static bool g_enabled = true;
    static bool g_tyre_slip_active = false;
    static bool g_tyre_slip_create_attempted = false;
    static int  g_centering_percent = 30;
    static int  g_gain_percent = 100;
    static int g_max_force = 10000;
    static int g_min_force = 0;
    static int g_force_duration = 20;

    static DWORD g_num_ff_axes = 0;

    void set_enabled(bool enabled)
    {
        g_enabled = enabled;

        if (!g_enabled)
        {
            if (g_pEffect)
                g_pEffect->Stop();

            if (g_pTyreSlipEffect)
                g_pTyreSlipEffect->Stop();

            g_tyre_slip_active = false;
        }
    }

    void set_gain(int percent)
    {
        if (percent < 10)
            percent = 10;
        else if (percent > 100)
            percent = 100;

        g_gain_percent = percent;
    }

    void set_centering_strength(int percent)
    {
        if (percent < 0)
            percent = 0;
        else if (percent > 100)
            percent = 100;

        g_centering_percent = percent;

        // Tyre slip temporarily lightens the current steering load while
        // retaining the configured/dynamic spring value for instant recovery.
        const int effective_percent =
            g_tyre_slip_active
            ? (g_centering_percent * 80 + 50) / 100
            : g_centering_percent;


        // Spring not created yet.
        // The stored percentage will be used by create_spring_effect().
        if (!g_pSpringEffect)
            return;


        // 0% means completely disable centering.
        if (effective_percent == 0)
        {
            g_pSpringEffect->Stop();
            return;
        }


        DICONDITION condition{};

        condition.lOffset = 0;


        const LONG strength =
            DI_FFNOMINALMAX *
            effective_percent /
            100;


        condition.lPositiveCoefficient =
            strength;

        condition.lNegativeCoefficient =
            strength;

        condition.dwPositiveSaturation =
            DI_FFNOMINALMAX;

        condition.dwNegativeSaturation =
            DI_FFNOMINALMAX;

        condition.lDeadBand = 0;


        DWORD axis =
            DIJOFS_X;


        LONG direction[1] =
        {
            1
        };


        DIEFFECT effect{};

        effect.dwSize =
            sizeof(DIEFFECT);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;

        effect.cAxes =
            1;

        effect.rgdwAxes =
            &axis;

        effect.rglDirection =
            direction;

        effect.cbTypeSpecificParams =
            sizeof(DICONDITION);

        effect.lpvTypeSpecificParams =
            &condition;


        HRESULT hr =
            g_pSpringEffect->SetParameters(
                &effect,
                DIEP_DIRECTION |
                DIEP_TYPESPECIFICPARAMS |
                DIEP_START);


        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED ||
            hr == DIERR_NOTEXCLUSIVEACQUIRED)
        {
            g_pDevice->Unacquire();

            if (SUCCEEDED(g_pDevice->Acquire()))
            {
                hr =
                    g_pSpringEffect->SetParameters(
                        &effect,
                        DIEP_DIRECTION |
                        DIEP_TYPESPECIFICPARAMS |
                        DIEP_START);
            }
        }


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: unable to update centering strength: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;
        }
    }

    static SDL_Window* g_ffb_window = nullptr;


    // -----------------------------------------------------------------------------
    // VID / PID filter
    // -----------------------------------------------------------------------------

    static unsigned short parse_hex4(const char* s)
    {
        unsigned v = 0;

        if (!s)
            return 0;

        std::sscanf(s, "%x", &v);

        return (unsigned short)v;
    }


    static bool match_target_vidpid(LPDIRECTINPUTDEVICE8 dev)
    {
        const char* env =
            std::getenv("FF_TARGET_VIDPID");

        // No target specified:
        // accept the first actual FFB-capable DirectInput device.
        if (!env)
            return true;


        DIPROPDWORD dp{};

        dp.diph.dwSize =
            sizeof(DIPROPDWORD);

        dp.diph.dwHeaderSize =
            sizeof(DIPROPHEADER);

        dp.diph.dwHow =
            DIPH_DEVICE;


        if (FAILED(
            dev->GetProperty(
                DIPROP_VIDPID,
                &dp.diph)))
        {
            return true;
        }


        unsigned short vid =
            LOWORD(dp.dwData);

        unsigned short pid =
            HIWORD(dp.dwData);


        unsigned short target_vid = 0;
        unsigned short target_pid = 0;


        const char* colon =
            std::strchr(env, ':');


        if (colon)
        {
            target_vid =
                parse_hex4(env);

            target_pid =
                parse_hex4(colon + 1);
        }


        return
            (!target_vid || target_vid == vid) &&
            (!target_pid || target_pid == pid);
    }


    // -----------------------------------------------------------------------------
    // DirectInput device enumeration
    //
    // IMPORTANT:
    // Do NOT configure or create effects here.
    // Only select the DirectInput FFB device.
    // -----------------------------------------------------------------------------

    static BOOL CALLBACK EnumFFDevicesCallback(
        const DIDEVICEINSTANCE* instance,
        VOID*)
    {
        if (!g_pDI || g_pDevice)
            return DIENUM_STOP;


        LPDIRECTINPUTDEVICE8 device =
            nullptr;


        HRESULT hr =
            g_pDI->CreateDevice(
                instance->guidInstance,
                &device,
                nullptr);


        if (FAILED(hr))
            return DIENUM_CONTINUE;


        if (!match_target_vidpid(device))
        {
            device->Release();

            return DIENUM_CONTINUE;
        }


        // Device accepted.
        g_pDevice = device;


        return DIENUM_STOP;
    }


    // -----------------------------------------------------------------------------
    // Count actual force-feedback actuator axes
    // -----------------------------------------------------------------------------

    static BOOL CALLBACK EnumAxesCallback(
        const DIDEVICEOBJECTINSTANCE* object,
        VOID* context)
    {
        DWORD* count =
            static_cast<DWORD*>(context);


        if (object->dwFlags & DIDOI_FFACTUATOR)
            (*count)++;


        return DIENUM_CONTINUE;
    }


    // -----------------------------------------------------------------------------
    // Create initial constant-force effect
    // -----------------------------------------------------------------------------

    static bool create_force_effect()
    {
        DWORD axes[2] =
        {
            DIJOFS_X,
            DIJOFS_Y
        };


        LONG direction[2] =
        {
            0,
            0
        };


        DICONSTANTFORCE constant_force{};

        constant_force.lMagnitude = 0;


        DIEFFECT effect{};

        effect.dwSize =
            sizeof(DIEFFECT);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;


        if (g_force_duration <= 0)
            g_force_duration = 20;


        effect.dwDuration =
            DI_SECONDS /
            g_force_duration;


        effect.dwSamplePeriod = 0;

        effect.dwGain =
            DI_FFNOMINALMAX;

        effect.dwTriggerButton =
            DIEB_NOTRIGGER;

        effect.dwTriggerRepeatInterval = 0;


        effect.cAxes =
            g_num_ff_axes;

        effect.rgdwAxes =
            axes;

        effect.rglDirection =
            direction;


        effect.lpEnvelope =
            nullptr;


        effect.cbTypeSpecificParams =
            sizeof(DICONSTANTFORCE);

        effect.lpvTypeSpecificParams =
            &constant_force;


        effect.dwStartDelay = 0;


        HRESULT hr =
            g_pDevice->CreateEffect(
                GUID_ConstantForce,
                &effect,
                &g_pEffect,
                nullptr);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: CreateEffect failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        if (!g_pEffect)
        {
            std::cout
                << "DirectInput: CreateEffect returned no effect"
                << std::endl;

            return false;
        }


        return true;
    }

    static bool create_spring_effect()
    {
        if (!g_pDevice)
            return false;

        DICONDITION condition{};

        condition.lOffset = 0;

        const int effective_percent =
            g_tyre_slip_active
            ? (g_centering_percent * 80 + 50) / 100
            : g_centering_percent;

        const LONG strength =
            DI_FFNOMINALMAX * effective_percent / 100;

        // DirectInput spring coefficient
        condition.lPositiveCoefficient = strength;
        condition.lNegativeCoefficient = strength;

        condition.dwPositiveSaturation =
            DI_FFNOMINALMAX;

        condition.dwNegativeSaturation =
            DI_FFNOMINALMAX;

        condition.lDeadBand = 0;


        DWORD axis = DIJOFS_X;

        LONG direction[1] =
        {
            1
        };


        DIEFFECT effect{};

        effect.dwSize =
            sizeof(DIEFFECT);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;

        effect.dwDuration =
            INFINITE;

        effect.dwSamplePeriod =
            0;

        effect.dwGain =
            DI_FFNOMINALMAX;

        effect.dwTriggerButton =
            DIEB_NOTRIGGER;

        effect.dwTriggerRepeatInterval =
            0;

        effect.cAxes =
            1;

        effect.rgdwAxes =
            &axis;

        effect.rglDirection =
            direction;

        effect.lpEnvelope =
            nullptr;

        effect.cbTypeSpecificParams =
            sizeof(DICONDITION);

        effect.lpvTypeSpecificParams =
            &condition;

        effect.dwStartDelay =
            0;


        HRESULT hr =
            g_pDevice->CreateEffect(
                GUID_Spring,
                &effect,
                &g_pSpringEffect,
                nullptr);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: Create spring failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            g_pSpringEffect = nullptr;
            return false;
        }


        hr =
            g_pSpringEffect->Start(
                1,
                0);


        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED ||
            hr == DIERR_NOTEXCLUSIVEACQUIRED)
        {
            g_pDevice->Unacquire();

            HRESULT acquire_hr =
                g_pDevice->Acquire();

            if (SUCCEEDED(acquire_hr))
            {
                hr =
                    g_pSpringEffect->Start(
                        1,
                        0);
            }
        }


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: unable to start centering spring: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            g_pSpringEffect->Release();
            g_pSpringEffect = nullptr;

            return false;
        }


        return true;
    }

    // -----------------------------------------------------------------------------
    // Tyre-slip vibration
    // -----------------------------------------------------------------------------

    static bool create_tyre_slip_effect()
    {
        if (!g_pDevice)
            return false;

        DWORD axis = DIJOFS_X;
        LONG direction[1] = { 1 };

        DIPERIODIC periodic{};
        periodic.dwMagnitude = 0;
        periodic.lOffset = 0;
        periodic.dwPhase = 0;
        periodic.dwPeriod = DI_SECONDS / 22;

        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwSamplePeriod = 0;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.dwTriggerButton = DIEB_NOTRIGGER;
        effect.dwTriggerRepeatInterval = 0;
        effect.cAxes = 1;
        effect.rgdwAxes = &axis;
        effect.rglDirection = direction;
        effect.lpEnvelope = nullptr;
        effect.cbTypeSpecificParams = sizeof(DIPERIODIC);
        effect.lpvTypeSpecificParams = &periodic;
        effect.dwStartDelay = 0;

        HRESULT hr =
            g_pDevice->CreateEffect(
                GUID_Sine,
                &effect,
                &g_pTyreSlipEffect,
                nullptr);

        if (FAILED(hr) || !g_pTyreSlipEffect)
        {
            std::cout
                << "DirectInput: tyre-slip sine effect unavailable: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            g_pTyreSlipEffect = nullptr;
            return false;
        }

        return true;
    }

    void set_tyre_slip(bool active)
    {
        if (!g_supported || !g_enabled || !g_pDevice)
            active = false;

        if (active == g_tyre_slip_active)
            return;

        g_tyre_slip_active = active;

        // Reapply the last requested spring immediately. set_centering_strength()
        // applies the temporary 20% reduction while tyre slip is active.
        set_centering_strength(g_centering_percent);

        if (!active)
        {
            if (g_pTyreSlipEffect)
                g_pTyreSlipEffect->Stop();

            return;
        }

        if (!g_pTyreSlipEffect)
        {
            if (g_tyre_slip_create_attempted)
                return;

            g_tyre_slip_create_attempted = true;

            if (!create_tyre_slip_effect())
                return;
        }

        DIPERIODIC periodic{};
        periodic.dwMagnitude =
            static_cast<DWORD>(
                (static_cast<long long>(DI_FFNOMINALMAX) * 12 * g_gain_percent) /
                10000);
        periodic.lOffset = 0;
        periodic.dwPhase = 0;
        periodic.dwPeriod = DI_SECONDS / 22;

        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.cbTypeSpecificParams = sizeof(DIPERIODIC);
        effect.lpvTypeSpecificParams = &periodic;

        HRESULT hr =
            g_pTyreSlipEffect->SetParameters(
                &effect,
                DIEP_TYPESPECIFICPARAMS |
                DIEP_START);

        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED ||
            hr == DIERR_NOTEXCLUSIVEACQUIRED)
        {
            g_pDevice->Unacquire();

            if (SUCCEEDED(g_pDevice->Acquire()))
            {
                hr =
                    g_pTyreSlipEffect->SetParameters(
                        &effect,
                        DIEP_TYPESPECIFICPARAMS |
                        DIEP_START);
            }
        }

        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: unable to start tyre-slip effect: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;
        }
    }

    // -----------------------------------------------------------------------------
    // Initialise
    // -----------------------------------------------------------------------------

    bool init(
        int max_force,
        int min_force,
        int force_duration)
    {
        if (g_supported)
            return true;


        g_max_force =
            max_force;

        g_min_force =
            min_force;

        g_force_duration =
            force_duration;


        // -------------------------------------------------------------------------
        // DirectInput FFB requires a valid HWND.
        //
        // This is the same basic approach used by original CannonBall:
        // create a tiny hidden SDL window and retrieve its native Windows handle.
        // -------------------------------------------------------------------------

        if (!g_ffb_window)
        {
            g_ffb_window =
                SDL_CreateWindow(
                    "CannonBall FFB",
                    SDL_WINDOWPOS_UNDEFINED,
                    SDL_WINDOWPOS_UNDEFINED,
                    1,
                    1,
                    SDL_WINDOW_HIDDEN);


            if (!g_ffb_window)
            {
                std::cout
                    << "DirectInput: Could not create FFB window: "
                    << SDL_GetError()
                    << std::endl;

                return false;
            }
        }


        SDL_SysWMinfo wm_info{};

        SDL_VERSION(
            &wm_info.version);


        if (!SDL_GetWindowWMInfo(
            g_ffb_window,
            &wm_info))
        {
            std::cout
                << "DirectInput: SDL_GetWindowWMInfo failed: "
                << SDL_GetError()
                << std::endl;

            return false;
        }


        HWND hwnd =
            wm_info.info.win.window;


        // -------------------------------------------------------------------------
        // Create DirectInput
        // -------------------------------------------------------------------------

        HRESULT hr =
            DirectInput8Create(
                GetModuleHandle(nullptr),
                DIRECTINPUT_VERSION,
                IID_IDirectInput8,
                reinterpret_cast<void**>(&g_pDI),
                nullptr);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: DirectInput8Create failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        // -------------------------------------------------------------------------
        // Find an attached force-feedback device
        // -------------------------------------------------------------------------

        hr =
            g_pDI->EnumDevices(
                DI8DEVCLASS_GAMECTRL,
                EnumFFDevicesCallback,
                nullptr,
                DIEDFL_ATTACHEDONLY |
                DIEDFL_FORCEFEEDBACK);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: device enumeration failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        if (!g_pDevice)
        {
            std::cout
                << "DirectInput: No FFB device found"
                << std::endl;

            return false;
        }


        // -------------------------------------------------------------------------
        // Standard joystick format.
        //
        // Original CannonBall uses c_dfDIJoystick rather than c_dfDIJoystick2.
        // -------------------------------------------------------------------------

        hr =
            g_pDevice->SetDataFormat(
                &c_dfDIJoystick);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: SetDataFormat failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        // -------------------------------------------------------------------------
        // Force feedback requires exclusive access.
        // -------------------------------------------------------------------------

        hr =
            g_pDevice->SetCooperativeLevel(
                hwnd,
                DISCL_EXCLUSIVE |
                DISCL_BACKGROUND);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: SetCooperativeLevel failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        // -------------------------------------------------------------------------
        // Disable driver-controlled centering spring
        // -------------------------------------------------------------------------

        DIPROPDWORD autocenter{};

        autocenter.diph.dwSize =
            sizeof(DIPROPDWORD);

        autocenter.diph.dwHeaderSize =
            sizeof(DIPROPHEADER);

        autocenter.diph.dwObj =
            0;

        autocenter.diph.dwHow =
            DIPH_DEVICE;

        autocenter.dwData =
            DIPROPAUTOCENTER_OFF;


        hr =
            g_pDevice->SetProperty(
                DIPROP_AUTOCENTER,
                &autocenter.diph);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: warning - unable to disable autocenter: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            // Not fatal.
        }


        // -------------------------------------------------------------------------
        // Determine actual FFB axes
        // -------------------------------------------------------------------------

        g_num_ff_axes = 0;


        hr =
            g_pDevice->EnumObjects(
                EnumAxesCallback,
                &g_num_ff_axes,
                DIDFT_AXIS);


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: EnumObjects failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << std::endl;

            return false;
        }


        if (g_num_ff_axes == 0)
        {
            std::cout
                << "DirectInput: device has no FFB actuator axes"
                << std::endl;

            return false;
        }


        if (g_num_ff_axes > 2)
            g_num_ff_axes = 2;


        // -------------------------------------------------------------------------
        // Acquire
        // -------------------------------------------------------------------------

        hr =
            g_pDevice->Acquire();


        if (FAILED(hr))
        {
            std::cout
                << "DirectInput: initial Acquire failed: 0x"
                << std::hex
                << (unsigned long)hr
                << std::dec
                << " - will retry later"
                << std::endl;
        }


        // -------------------------------------------------------------------------
        // Create constant-force effect
        // -------------------------------------------------------------------------

        if (!create_force_effect())
        {
            return false;
        }


        // -------------------------------------------------------------------------
        // Create native centering spring
        // -------------------------------------------------------------------------

        if (!create_spring_effect())
        {
            std::cout
                << "DirectInput: native centering spring unavailable"
                << std::endl;

            // Not fatal. Constant-force FFB remains available.
        }


        g_supported = true;


        return true;
}
    // -----------------------------------------------------------------------------
    // Send force
    //
    // xdirection:
    // < 8 = left
    //   8 = centre
    // > 8 = right
    //
    // Larger force number = softer force.
    // -----------------------------------------------------------------------------

    int set(
        int xdirection,
        int force)
    {
        if (!g_supported ||
            !g_pDevice ||
            !g_pEffect ||
            force < 0)
        {
            return -1;
        }

        LONG direction[2] =
        {
            0,
            0
        };


        if (xdirection > 0x08)
            direction[0] = 1;

        else if (xdirection < 0x08)
            direction[0] = -1;


        if (force > 7)
            force = 7;


        LONG magnitude =
            g_max_force -
            (((g_max_force - g_min_force) / 7) * force);

        // Apply independent master FFB strength
        magnitude =
            static_cast<LONG>(
                (static_cast<long long>(magnitude) * g_gain_percent) / 100
                );

        if (magnitude > DI_FFNOMINALMAX)
            magnitude = DI_FFNOMINALMAX;

        else if (magnitude < -DI_FFNOMINALMAX)
            magnitude = -DI_FFNOMINALMAX;


        DICONSTANTFORCE constant_force{};

        constant_force.lMagnitude =
            magnitude;


        DIEFFECT effect{};

        effect.dwSize =
            sizeof(DIEFFECT);

        effect.dwFlags =
            DIEFF_CARTESIAN |
            DIEFF_OBJECTOFFSETS;

        effect.cAxes =
            g_num_ff_axes;

        effect.rglDirection =
            direction;

        effect.cbTypeSpecificParams =
            sizeof(DICONSTANTFORCE);

        effect.lpvTypeSpecificParams =
            &constant_force;


        HRESULT hr =
            g_pEffect->SetParameters(
                &effect,
                DIEP_DIRECTION |
                DIEP_TYPESPECIFICPARAMS |
                DIEP_START);

        // Device temporarily lost or no longer exclusively acquired?
        if (hr == DIERR_INPUTLOST ||
            hr == DIERR_NOTACQUIRED ||
            hr == DIERR_NOTEXCLUSIVEACQUIRED)
        {
            // Make absolutely sure we're starting from a clean state
            g_pDevice->Unacquire();

            HRESULT acquire_hr =
                g_pDevice->Acquire();

            if (SUCCEEDED(acquire_hr))
            {
                hr =
                    g_pEffect->SetParameters(
                        &effect,
                        DIEP_DIRECTION |
                        DIEP_TYPESPECIFICPARAMS |
                        DIEP_START);
            }
            else
            {
                std::cout
                    << "DirectInput: FFB reacquire failed: 0x"
                    << std::hex
                    << (unsigned long)acquire_hr
                    << std::dec
                    << std::endl;
            }
        }


        return FAILED(hr)
            ? -1
            : 0;
    }


    void stop()
    {
        if (g_pEffect)
            g_pEffect->Stop();
    }

    // -----------------------------------------------------------------------------
    // Close
    // -----------------------------------------------------------------------------

    void close()
    {
        if (g_pTyreSlipEffect)
        {
            g_pTyreSlipEffect->Stop();
            g_pTyreSlipEffect->Release();
            g_pTyreSlipEffect = nullptr;
        }

        if (g_pSpringEffect)
        {
            g_pSpringEffect->Stop();
            g_pSpringEffect->Release();
            g_pSpringEffect = nullptr;
        }

        if (g_pEffect)
        {
            g_pEffect->Stop();
            g_pEffect->Release();
            g_pEffect = nullptr;
        }


        if (g_pDevice)
        {
            g_pDevice->Unacquire();
            g_pDevice->Release();
            g_pDevice = nullptr;
        }


        if (g_pDI)
        {
            g_pDI->Release();
            g_pDI = nullptr;
        }


        if (g_ffb_window)
        {
            SDL_DestroyWindow(
                g_ffb_window);

            g_ffb_window = nullptr;
        }


        g_num_ff_axes = 0;
        g_tyre_slip_active = false;
        g_tyre_slip_create_attempted = false;

        g_supported = false;
    }


    bool is_supported()
    {
        return g_supported;
    }

} // namespace forcefeedback

#else

// Stubs for other platforms
namespace forcefeedback {
bool init(int, int, int) { return false; }
int  set(int, int) { return -1; }
void set_tyre_slip(bool) {}
void close() {}
bool is_supported() { return false; }
}

#endif