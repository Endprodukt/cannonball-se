/***************************************************************************
    Process Outputs.

    - Cabinet Vibration & Hydraulic Movement
    - Brake & Start Lamps
    - Coin Chute Outputs
    
    The Deluxe Motor code is also used by the force-feedback haptic system.

    One thing to note is that this code was originally intended to drive
    a moving hydraulic cabinet, not to be mapped to a haptic device.

    Therefore, it's not perfect when used in this way, but the results
    aren't bad :)
    
    Copyright Chris White.
    See license.txt for more details.
***************************************************************************/

#include <iostream>
#include <cstdlib> // abs

#include "utils.hpp"

#include "engine/outrun.hpp"
#include "engine/oanimseq.hpp"
#include "engine/ocrash.hpp"
#include "engine/oferrari.hpp"
#include "engine/ohud.hpp"
#include "engine/oinputs.hpp"
#include "engine/ooutputs.hpp"
#include "directx/ffeedback.hpp"

namespace
{
    enum CrashFfbType
    {
        CRASH_FFB_NONE = 0,
        CRASH_FFB_BUMP,
        CRASH_FFB_SPIN,
        CRASH_FFB_FLIP,
    };

    static int g_crash_ffb_type = CRASH_FFB_NONE;
    static int g_crash_ffb_last_state = -1;
    static int g_crash_ffb_last_spin_count = -1;
    static int g_crash_ffb_direction = 1;
    static int g_crash_ffb_landing_frames = 0;

    static bool g_start_sequence_ffb_active = false;
    static bool g_prestart_sine_active = false;
    static int g_prestart_sine_gain = 0;
    static int g_prestart_sine_applied_gain = 0;

    static int crash_ffb_command(int direction)
    {
        return direction < 0 ? 0x07 : 0x09;
    }

    static void reset_crash_ffb_tracking()
    {
        g_crash_ffb_type = CRASH_FFB_NONE;
        g_crash_ffb_last_state = -1;
        g_crash_ffb_last_spin_count = -1;
        g_crash_ffb_direction = 1;
        g_crash_ffb_landing_frames = 0;
    }

    static void reset_start_sequence_ffb_tracking()
    {
        g_start_sequence_ffb_active = false;
        g_prestart_sine_active = false;
        g_prestart_sine_gain = 0;
        g_prestart_sine_applied_gain = 0;
    }

    static void set_start_intro_force(int gain_percent)
    {
        if (gain_percent < 10)
            gain_percent = 10;

        // Never let the automatic intro steering exceed the user's master
        // FFB strength. This keeps the effect deliberately restrained.
        if (gain_percent > config.controls.ffb_strength)
            gain_percent = config.controls.ffb_strength;

        forcefeedback::set_gain(gain_percent);
        forcefeedback::set(0x09, 7);
        forcefeedback::set_gain(config.controls.ffb_strength);
    }

    static void update_prestart_sine(int pedal)
    {
        if (pedal < 0)
            pedal = 0;
        else if (pedal > 0xFF)
            pedal = 0xFF;

        // Reuse the tyre-slip sine effect, but ramp its effective gain with
        // the throttle. At full pedal it can only reach the configured master
        // FFB strength, so it remains lighter than the crash forces.
        int target_gain =
            (pedal * config.controls.ffb_strength + 0x7F) /
            0xFF;

        if (target_gain < 10)
            target_gain = 0;

        // Add a short mechanical build-up even if the pedal is stamped down.
        // At the default 50% master gain this takes about a third of a second.
        if (g_prestart_sine_gain < target_gain)
        {
            g_prestart_sine_gain += 5;
            if (g_prestart_sine_gain > target_gain)
                g_prestart_sine_gain = target_gain;
        }
        else if (g_prestart_sine_gain > target_gain)
        {
            g_prestart_sine_gain -= 7;
            if (g_prestart_sine_gain < target_gain)
                g_prestart_sine_gain = target_gain;
        }

        if (g_prestart_sine_gain < 10)
        {
            if (g_prestart_sine_active)
            {
                forcefeedback::set_tyre_slip(false);
                g_prestart_sine_active = false;
                g_prestart_sine_applied_gain = 0;
            }

            return;
        }

        // set_tyre_slip() only rebuilds the sine parameters when its active
        // state changes. Restart it only when the ramp actually reaches a new
        // gain value; once the pedal is steady the effect simply keeps running.
        if (!g_prestart_sine_active ||
            g_prestart_sine_gain != g_prestart_sine_applied_gain)
        {
            if (g_prestart_sine_active)
                forcefeedback::set_tyre_slip(false);

            forcefeedback::set_gain(g_prestart_sine_gain);
            forcefeedback::set_tyre_slip(true);
            forcefeedback::set_gain(config.controls.ffb_strength);
            g_prestart_sine_active = true;
            g_prestart_sine_applied_gain = g_prestart_sine_gain;
        }
    }

    static bool apply_start_sequence_ffb()
    {
        const bool intro_driving =
            outrun.game_state == GS_START1 &&
            oferrari.state == OFerrari::FERRARI_SEQ2;

        const bool waiting_for_start =
            outrun.game_state == GS_START2 ||
            outrun.game_state == GS_START3 ||
            (outrun.game_state == GS_START1 &&
             oferrari.state == OFerrari::FERRARI_LOGIC);

        if (!intro_driving && !waiting_for_start)
        {
            if (g_start_sequence_ffb_active)
            {
                if (g_prestart_sine_active)
                    forcefeedback::set_tyre_slip(false);

                forcefeedback::set_gain(config.controls.ffb_strength);
                forcefeedback::stop();
                reset_start_sequence_ffb_tracking();
            }

            return false;
        }

        g_start_sequence_ffb_active = true;

        if (intro_driving)
        {
            if (g_prestart_sine_active)
            {
                forcefeedback::set_tyre_slip(false);
                g_prestart_sine_active = false;
                g_prestart_sine_gain = 0;
                g_prestart_sine_applied_gain = 0;
            }

            // The intro is an animation rather than normal steering physics.
            // Follow the actual Ferrari sprite position: while the car arcs in
            // from the right, apply a restrained right-hand steering load and
            // progressively relax it as the sprite reaches the centre line.
            const int sprite_x =
                oanimseq.anim_ferrari.sprite
                ? std::abs(static_cast<int>(oanimseq.anim_ferrari.sprite->x))
                : 0;

            if (oferrari.car_state == OFerrari::CAR_ANIM_SEQ &&
                sprite_x > 4)
            {
                int intro_gain = 10 + (sprite_x / 10);
                if (intro_gain > 18)
                    intro_gain = 18;

                set_start_intro_force(intro_gain);
            }
            else
            {
                forcefeedback::stop();
            }

            return true;
        }

        // Ferrari is parked on the grid. The real pedal value remains live
        // during the countdown, so use it to build a light engine/rev shake.
        forcefeedback::stop();
        update_prestart_sine(oinputs.acc_adjust);
        return true;
    }

    static void prepare_crash_ffb_tracking()
    {
        if (!ocrash.crash_counter)
        {
            if (g_crash_ffb_type != CRASH_FFB_NONE)
                forcefeedback::stop();

            reset_crash_ffb_tracking();
            return;
        }

        if (g_crash_ffb_type != CRASH_FFB_NONE)
            return;

        if (ocrash.is_flip())
        {
            g_crash_ffb_type = CRASH_FFB_FLIP;
        }
        else if (ocrash.crash_state == 1 &&
                 (oinitengine.car_increment >> 16) == 0)
        {
            // Low-speed scenery collision. collide_slow() explicitly clears
            // the integer car speed when it enters the bump state.
            g_crash_ffb_type = CRASH_FFB_BUMP;
        }
        else
        {
            g_crash_ffb_type = CRASH_FFB_SPIN;
        }

        if (oferrari.car_x_diff < 0)
            g_crash_ffb_direction = -1;
        else if (oferrari.car_x_diff > 0)
            g_crash_ffb_direction = 1;
        else
            g_crash_ffb_direction =
                (oinitengine.car_x_pos < 0) ? -1 : 1;

        g_crash_ffb_last_state = -1;
        g_crash_ffb_last_spin_count = ocrash.crash_spin_count;
        g_crash_ffb_landing_frames = 0;
    }

    static void apply_crash_ffb_force()
    {
        if (!ocrash.crash_counter ||
            g_crash_ffb_type == CRASH_FFB_NONE)
        {
            return;
        }

        const int state = ocrash.crash_state;
        const int spin_count = ocrash.crash_spin_count;
        const int age = ocrash.crash_counter;

        const int primary =
            crash_ffb_command(g_crash_ffb_direction);
        const int rebound =
            crash_ffb_command(-g_crash_ffb_direction);

        const bool spin_changed =
            g_crash_ffb_last_spin_count >= 0 &&
            spin_count != g_crash_ffb_last_spin_count;

        // -----------------------------------------------------------------
        // Low-speed bump: one blunt hit followed by a small rebound.
        // -----------------------------------------------------------------
        if (g_crash_ffb_type == CRASH_FFB_BUMP)
        {
            if (age <= 2)
                forcefeedback::set(primary, 2);
            else if (age == 3)
                forcefeedback::set(rebound, 7);
            else
                forcefeedback::stop();
        }
        // -----------------------------------------------------------------
        // Medium-speed spin: strong initial contact followed by clear,
        // sustained left/right yanks while the car rotates/slides.
        // -----------------------------------------------------------------
        else if (g_crash_ffb_type == CRASH_FFB_SPIN)
        {
            if (age <= 2)
            {
                forcefeedback::set(primary, 1);
            }
            else if (state >= 1 && state <= 4)
            {
                // Hold each direction for several game ticks. The previous
                // two-tick/soft pattern felt like impacts rather than a wheel
                // being physically torn from side to side.
                const bool phase =
                    ((age / 4) & 1) != 0;

                forcefeedback::set(
                    phase ? primary : rebound,
                    1);
            }
            else
            {
                forcefeedback::stop();
            }
        }
        // -----------------------------------------------------------------
        // High-speed flip:
        //   state 1 = initial collision / pre-flip spin
        //   state 2 = airborne flip
        //   state 5+ = landing / post-crash recovery
        // -----------------------------------------------------------------
        else if (g_crash_ffb_type == CRASH_FFB_FLIP)
        {
            if (state <= 1)
            {
                if (age <= 2)
                {
                    forcefeedback::set(primary, 0);
                }
                else
                {
                    // Stronger, longer pre-flip side loads make the initial
                    // spin visibly translate into steering-wheel movement.
                    const bool phase =
                        ((age / 4) & 1) != 0;

                    forcefeedback::set(
                        phase ? primary : rebound,
                        1);
                }
            }
            else if (state == 2)
            {
                // Keep a real lateral load on the wheel throughout the flip.
                // crash_spin_count changes with the flip animation, so every
                // new rotation reverses the pull. The transition frame gets a
                // full-strength kick, then the pull remains strong until the
                // next rotation instead of disappearing after one tick.
                if (spin_count > 0)
                {
                    const int direction =
                        (spin_count & 1) ? primary : rebound;

                    forcefeedback::set(
                        direction,
                        spin_changed ? 0 : 2);
                }
                else
                {
                    forcefeedback::stop();
                }
            }
            else
            {
                // A flip transitions directly from the airborne state to the
                // post-crash state. Treat that edge as the landing impact.
                if (g_crash_ffb_last_state == 2 && state >= 5)
                    g_crash_ffb_landing_frames = 3;

                if (g_crash_ffb_landing_frames == 3)
                {
                    forcefeedback::set(primary, 0);
                    g_crash_ffb_landing_frames--;
                }
                else if (g_crash_ffb_landing_frames == 2)
                {
                    forcefeedback::set(rebound, 3);
                    g_crash_ffb_landing_frames--;
                }
                else if (g_crash_ffb_landing_frames == 1)
                {
                    forcefeedback::set(primary, 7);
                    g_crash_ffb_landing_frames--;
                }
                else
                {
                    forcefeedback::stop();
                }
            }
        }

        g_crash_ffb_last_state = state;
        g_crash_ffb_last_spin_count = spin_count;
    }
}

OOutputs::OOutputs(void)
{
    mode = MODE_DISABLED;

    chute1.output_bit  = D_COIN1_SUCC;
    chute2.output_bit  = D_COIN2_SUCC;

    col1               = 0;
    col2               = 0;
    limit_left         = 0;
    limit_right        = 0;
    motor_enabled      = true;
}

OOutputs::~OOutputs(void)
{
}


// Initalize Moving Cabinet Motor
// Source: 0xECE8
void OOutputs::init()
{
    motor_state        = STATE_INIT;
    hw_motor_control   = MOTOR_OFF;
    hw_motor_control_old = MOTOR_OFF;
    dig_out            = 0;
    dig_out_old        = -1;
    motor_control      = 0;
    motor_movement     = 0;
    is_centered        = false;
    motor_change_latch = 0;
    speed              = 0;
    curve              = 0;
    counter            = 0;
    vibrate_counter    = 0;
    was_small_change   = false;
    movement_adjust1   = 0;
    movement_adjust2   = 0;
    movement_adjust3   = 0;
    skid_ffb_active = false;
    offroad_pull_direction = 0;
    ffb_centering_strength = -1;
    chute1.counter[0]  = 0;
    chute1.counter[1]  = 0;
    chute1.counter[2]  = 0;
    chute2.counter[0]  = 0;
    chute2.counter[1]  = 0;
    chute2.counter[2]  = 0;

    reset_crash_ffb_tracking();
    reset_start_sequence_ffb_tracking();

    // Always restore the configured base spring when outputs are reset,
    // for example when returning to the menu after a high-speed curve.
    if (mode == MODE_FFEEDBACK && forcefeedback::is_supported())
    {
        forcefeedback::set_tyre_slip(false);
        forcefeedback::set_centering_strength(
            config.controls.centering_strength);
        ffb_centering_strength =
            config.controls.centering_strength;
    }
}

void OOutputs::set_mode(int m)
{
    mode = m;
}

void OOutputs::update_centering_strength()
{
    if (!forcefeedback::is_supported())
        return;

    const int base_strength =
        config.controls.centering_strength;

    int target_strength =
        base_strength;

    // Dynamic spring is only an on-road cornering effect.
    // Do not stack it with skid, crash or off-road constant-force effects.
    const int road_curve =
        std::abs(static_cast<int>(oinitengine.road_curve));

    if (outrun.game_state == GS_INGAME &&
        !ocrash.crash_counter &&
        !ocrash.skid_counter &&
        oferrari.wheel_state == OFerrari::WHEELS_ON &&
        road_curve > 0 &&
        road_curve <= 0x5A)
    {
        const uint16_t car_inc =
            oinitengine.car_increment >> 16;

        // Scale continuously instead of using discrete speed/curve bands.
        // This keeps the response immediate while avoiding noticeable steps.
        const int speed_start = 0x64;
        const int speed_full  = 0xF0;
        const int speed_span  = speed_full - speed_start;

        int speed_factor =
            static_cast<int>(car_inc) - speed_start;

        if (speed_factor < 0)
            speed_factor = 0;
        else if (speed_factor > speed_span)
            speed_factor = speed_span;

        // Preserve some extra weight even in gentle curves, while sharper
        // curves progressively approach the full boost.
        int curve_percent =
            50 + ((0x5A - road_curve) * 50) / 0x59;

        if (curve_percent < 50)
            curve_percent = 50;
        else if (curve_percent > 100)
            curve_percent = 100;

        // Scale the dynamic cornering load with the configured spring.
        // At 30% this preserves the current maximum (+30 points), while
        // lower spring settings reduce both centering and cornering load.
        const int max_boost_points = base_strength;

        const int boost_points =
            (max_boost_points * speed_factor * curve_percent +
             (speed_span * 50)) /
            (speed_span * 100);

        target_strength += boost_points;

        if (target_strength > 100)
            target_strength = 100;
    }

    // Accident-specific steering weight. Dynamic cornering is already disabled
    // in these states, so scale from the user's base spring value.
    if (outrun.game_state == GS_INGAME)
    {
        int crash_spring_percent = 100;

        if (ocrash.crash_counter)
        {
            if (g_crash_ffb_type == CRASH_FFB_BUMP)
            {
                crash_spring_percent = 65;
            }
            else if (g_crash_ffb_type == CRASH_FFB_SPIN)
            {
                crash_spring_percent =
                    ocrash.crash_state <= 4 ? 35 : 70;
            }
            else if (g_crash_ffb_type == CRASH_FFB_FLIP)
            {
                if (ocrash.crash_state <= 1)
                    crash_spring_percent = 45;
                else if (ocrash.crash_state == 2)
                    crash_spring_percent = 10;
                else if (ocrash.crash_state <= 4)
                    crash_spring_percent = 25;
                else if (ocrash.crash_state == 5)
                    crash_spring_percent = 45;
                else
                    crash_spring_percent = 70;
            }
        }
        else if (ocrash.skid_counter)
        {
            // Traffic collision / spin: keep some steering weight, but make
            // the loss of control clearly different from normal cornering.
            crash_spring_percent = 50;
        }

        if (crash_spring_percent < 100)
        {
            target_strength =
                (base_strength * crash_spring_percent + 50) /
                100;
        }
    }

    // Avoid sending identical DirectInput parameter updates every frame.
    if (target_strength == ffb_centering_strength)
        return;

    forcefeedback::set_centering_strength(
        target_strength);

    ffb_centering_strength =
        target_strength;
}

void OOutputs::tick(int16_t input_motor)
{
    switch (mode)
    {
        case MODE_DISABLED:
            break;

        // Force Feedback Steering Wheels
        case MODE_FFEEDBACK:
        {
            const bool start_sequence_ffb =
                apply_start_sequence_ffb();

            const bool tyre_slip =
                outrun.game_state == GS_INGAME &&
                outrun.SkiddingOnRoad() &&
                !ocrash.crash_counter &&
                !ocrash.skid_counter &&
                oferrari.wheel_state == OFerrari::WHEELS_ON;

            // During the countdown the same sine channel is temporarily used
            // for the throttle/rev shake. Normal tyre-slip owns it again as
            // soon as the game enters GS_INGAME.
            if (!start_sequence_ffb)
                forcefeedback::set_tyre_slip(tyre_slip);

            prepare_crash_ffb_tracking();
            update_centering_strength();

            if (start_sequence_ffb)
            {
                hw_motor_control = MOTOR_OFF;
                skid_ffb_active = false;
            }
            else if (ocrash.crash_counter)
            {
                // The modern FFB crash sequence replaces the old moving-cabinet
                // vibration table while a scenery crash is active.
                hw_motor_control = MOTOR_OFF;
                skid_ffb_active = false;
                apply_crash_ffb_force();
            }
            else
            {
                do_motors(mode, input_motor);   // Use X-Position of wheel instead of motor position
                motor_output(hw_motor_control); // Force Feedback Handling
            }
            break;
        }

        // SMARTYPI: Real Cabinet
        case MODE_CABINET:
            if (config.smartypi.cabinet == Config::CABINET_MOVING)
            {
                do_motors(mode, input_motor);
            }
            else
            {
                if (config.smartypi.cabinet == Config::CABINET_UPRIGHT)
                    do_vibrate_upright();
                else if (config.smartypi.cabinet == Config::CABINET_MINI)
                    do_vibrate_mini();
            }
            break;

        // GamePad: Basic Rumble
        case MODE_RUMBLE:
            do_vibrate_upright();
            break;
    }
}

void OOutputs::writeDigitalToConsole()
{
    if (config.smartypi.enabled && config.smartypi.ouputs)
    {
        if ((dig_out & D_BRAKE_LAMP) != (dig_out_old & D_BRAKE_LAMP))
            std::cout << "brake_lamp = " << is_set(D_BRAKE_LAMP) << std::endl;
        if ((dig_out & D_START_LAMP) != (dig_out_old & D_START_LAMP))
            std::cout << "start_lamp = " << is_set(D_START_LAMP) << std::endl;
        if ((dig_out & D_MOTOR) != (dig_out_old & D_MOTOR))
            std::cout << "wheel_motor = " << is_set(D_MOTOR) << std::endl;

        if (hw_motor_control != hw_motor_control_old)
            std::cout << "bank_motor_speed = " << (int) hw_motor_control << std::endl;

        dig_out_old = dig_out;
        hw_motor_control_old = hw_motor_control;
    }
}

// ------------------------------------------------------------------------------------------------
// Digital Outputs
// ------------------------------------------------------------------------------------------------

void OOutputs::set_digital(uint8_t output)
{
    dig_out |= output;   
}

void OOutputs::clear_digital(uint8_t output)
{
    dig_out &= ~output;
}

int OOutputs::is_set(uint8_t output)
{
    return (dig_out & output) ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------
// Motor Diagnostics
// Source: 0x1885E
// ------------------------------------------------------------------------------------------------

bool OOutputs::diag_motor(int16_t input_motor, uint8_t hw_motor_limit)
{
    switch (motor_state)
    {
        // Initalize
        case STATE_INIT:
            col1 = 10;
            col2 = 27;
            ohud.blit_text_new(col1, 9, "LEFT LIMIT");
            ohud.blit_text_new(col1, 11, "RIGHT LIMIT");
            ohud.blit_text_new(col1, 13, "CENTRE");
            ohud.blit_text_new(col1, 16, "MOTOR POSITION");
            ohud.blit_text_new(col1, 18, "LIMIT B3 LEFT");
            ohud.blit_text_new(col1, 19, "LIMIT B4 CENTRE");
            ohud.blit_text_new(col1, 20, "LIMIT B5 RIGHT");
            counter          = COUNTER_RESET;
            motor_centre_pos = 0;
            motor_enabled    = true;
            motor_state = STATE_LEFT;
            break;

        case STATE_LEFT:
            diag_left(input_motor, hw_motor_limit);
            break;

        case STATE_RIGHT:
            diag_right(input_motor, hw_motor_limit);
            break;

        case STATE_CENTRE:
            diag_centre(input_motor, hw_motor_limit);
            break;

        case STATE_DONE:
            diag_done();
            break;
    }

    // Print Motor Position & Limit Switch
    ohud.blit_text_new(col2, 16, "  H", 0x80);
    ohud.blit_text_new(col2, 16, Utils::to_hex_string(input_motor).c_str(), 0x80);
    ohud.blit_text_new(col2, 18, (hw_motor_limit & BIT_5) ? "OFF" : "ON ", 0x80);
    ohud.blit_text_new(col2, 19, (hw_motor_limit & BIT_4) ? "OFF" : "ON ", 0x80);
    ohud.blit_text_new(col2, 20, (hw_motor_limit & BIT_3) ? "OFF" : "ON ", 0x80);
    return motor_state == STATE_DONE;
}

void OOutputs::diag_left(int16_t input_motor, uint8_t hw_motor_limit)
{
    // If Right Limit Reached, Move Left
    if (hw_motor_limit & BIT_5)
    {
        if (--counter >= 0)
        {
            hw_motor_control = MOTOR_LEFT;
            return;
        }
        // Counter Expired, Left Limit Still Not Reached
        else
            ohud.blit_text_new(col2, 9, "FAIL 1", 0x80);
    }
    // Left Limit Reached
    else if (hw_motor_limit & BIT_3)
    {
        ohud.blit_text_new(col2, 9, "  H", 0x80);
        ohud.blit_text_new(col2, 9, Utils::to_hex_string(input_motor).c_str(), 0x80);
    }
    else
        ohud.blit_text_new(col2, 9, "FAIL 2", 0x80);

    counter          = COUNTER_RESET;
    motor_state      = STATE_RIGHT;
}


void OOutputs::diag_right(int16_t input_motor, uint8_t hw_motor_limit)
{
    if (motor_centre_pos == 0 && (hw_motor_limit & BIT_4) == 0)
        motor_centre_pos = input_motor;
   
    // If Left Limit Set, Move Right
    if (hw_motor_limit & BIT_3)
    {
        if (--counter >= 0)
        {
            hw_motor_control = MOTOR_RIGHT; // Move Right
            return;
        }
        // Counter Expired, Right Limit Still Not Reached
        else
            ohud.blit_text_new(col2, 11, "FAIL 1", 0x80);
    }
    // Right Limit Reached
    else if (hw_motor_limit & BIT_5)
    {
        ohud.blit_text_new(col2, 11, "  H", 0x80);
        ohud.blit_text_new(col2, 11, Utils::to_hex_string(input_motor).c_str(), 0x80);
    }
    else
    {
        ohud.blit_text_new(col2, 11, "FAIL 2", 0x80);
        motor_enabled = false;
        motor_state   = STATE_DONE;
        return;
    }

    motor_state  = STATE_CENTRE;
    counter      = COUNTER_RESET;
}


void OOutputs::diag_centre(int16_t input_motor, uint8_t hw_motor_limit)
{
    if (hw_motor_limit & BIT_4)
    {
        if (--counter >= 0)
        {
            hw_motor_control = (counter <= COUNTER_RESET/2) ? MOTOR_RIGHT : MOTOR_LEFT; // Move Right
            return;
        }
        else
        {
            ohud.blit_text_new(col2, 13, "FAIL", 0x80);
        }  
    }
    else
    {
        ohud.blit_text_new(col2, 13, "  H", 0x80);
        ohud.blit_text_new(col2, 13, Utils::to_hex_string((input_motor + motor_centre_pos) >> 1).c_str(), 0x86);
        hw_motor_control = MOTOR_OFF; // switch off
        counter          = 32;
        motor_state      = STATE_DONE;
    }
}

void OOutputs::diag_done()
{
    if (counter > 0)
        counter--;

    if (counter == 0)
        hw_motor_control = MOTOR_CENTRE;
}

// ------------------------------------------------------------------------------------------------
// Calibrate Motors
// ------------------------------------------------------------------------------------------------

bool OOutputs::calibrate_motor(int16_t input_motor, uint8_t hw_motor_limit)
{
    switch (motor_state)
    {
        // Initalize
        case STATE_INIT:
            col1 = 11;
            col2 = 25;
            ohud.blit_text_big(      2,  "MOTOR CALIBRATION");
            ohud.blit_text_new(col1, 10, "MOVE LEFT   -");
            ohud.blit_text_new(col1, 12, "MOVE RIGHT  -");
            ohud.blit_text_new(col1, 14, "MOVE CENTRE -");
            counter          = 25;
            motor_centre_pos = 0;
            motor_enabled    = true;
            motor_state++;
            break;

        // Just a delay to wait for the serial for safety
        case STATE_DELAY:
            if (--counter == 0)
            {
                counter = COUNTER_RESET;
                motor_state++;
            }
            break;

        // Calibrate Left Limit
        case STATE_LEFT:
            calibrate_left(input_motor, hw_motor_limit);
            break;

        // Calibrate Right Limit
        case STATE_RIGHT:
            calibrate_right(input_motor, hw_motor_limit);
            break;

        // Return to Centre
        case STATE_CENTRE:
            calibrate_centre(input_motor, hw_motor_limit);
            break;

        // Clear Screen & Exit Calibration
        case STATE_DONE:
            calibrate_done();
            break;

        case STATE_EXIT:
            return true;
    }

    return false;
}

void OOutputs::calibrate_left(int16_t input_motor, uint8_t hw_motor_limit)
{
    // If Right Limit Set, Move Left
    if (hw_motor_limit & BIT_5)
    {
        if (--counter >= 0)
        {
            hw_motor_control = MOTOR_LEFT;
            return;
        }
        // Counter Expired, Left Limit Still Not Reached
        else
        {
            ohud.blit_text_new(col2, 10, "FAIL 1");
            motor_centre_pos = 0;
            limit_left       = input_motor; // Set Left Limit
            hw_motor_control = MOTOR_LEFT;  // Move Left
            counter          = COUNTER_RESET;
            motor_state      = STATE_RIGHT;
        }
    }
    // Left Limit Reached
    else if (hw_motor_limit & BIT_3)
    {
        ohud.blit_text_new(col2, 10, Utils::to_hex_string(input_motor).c_str(), 0x80);
        motor_centre_pos = 0;
        limit_left       = input_motor; // Set Left Limit
        hw_motor_control = MOTOR_LEFT;  // Move Left
        counter          = COUNTER_RESET; 
        motor_state      = STATE_RIGHT;
    }
    else
    {
        ohud.blit_text_new(col2, 10, "FAIL 2");
        ohud.blit_text_new(col2, 12, "FAIL 2");
        motor_enabled = false; 
        counter       = COUNTER_RESET;
        motor_state   = STATE_CENTRE;
    }
}

void OOutputs::calibrate_right(int16_t input_motor, uint8_t hw_motor_limit)
{
    if (motor_centre_pos == 0 && ((hw_motor_limit & BIT_4) == 0))
    {
        motor_centre_pos = input_motor;
    }
   
    // If Left Limit Set, Move Right
    if (hw_motor_limit & BIT_3)
    {
        if (--counter >= 0)
        {
            hw_motor_control = MOTOR_RIGHT; // Move Right
            return;
        }
        // Counter Expired, Right Limit Still Not Reached
        else
        {
            ohud.blit_text_new(col2, 12, "FAIL 1");
            limit_right  = input_motor;
            motor_state  = STATE_CENTRE;
            counter      = COUNTER_RESET;
        }
    }
    // Right Limit Reached
    else if (hw_motor_limit & BIT_5)
    {
        ohud.blit_text_new(col2, 12, Utils::to_hex_string(input_motor).c_str(), 0x80);
        limit_right   = input_motor; // Set Right Limit
        motor_state   = STATE_CENTRE;
        counter       = COUNTER_RESET;
    }
    else
    {
        ohud.blit_text_new(col2, 12, "FAIL 2");
        motor_enabled = false;
        motor_state   = STATE_CENTRE;
        counter       = COUNTER_RESET;
    }
}

void OOutputs::calibrate_centre(int16_t input_motor, uint8_t hw_motor_limit)
{
    bool fail = false;

    if (hw_motor_limit & BIT_4)
    {
        if (--counter >= 0)
        {
            hw_motor_control = (counter <= COUNTER_RESET/2) ? MOTOR_RIGHT : MOTOR_LEFT; // Move Right
            return;
        }
        else
        {
            ohud.blit_text_new(col2, 14, "FAIL SW");
            fail = true;
            // Fall through to EEB6
        }  
    }
  
    // 0xEEB6:
    motor_centre_pos = (input_motor + motor_centre_pos) >> 1;
  
    int16_t d0 = limit_right - motor_centre_pos;
    int16_t d1 = motor_centre_pos  - limit_left;
  
    // set both to left limit
    if (d0 > d1)
        d1 = d0;

    d0 = d1;
  
    limit_left  = d0 - 6;
    limit_right = -d1 + 6;
    
    if (std::abs(motor_centre_pos - 0x80) > 0x20)
    {
        ohud.blit_text_new(col2, 14, "FAIL DIST");
        motor_enabled = false;
    }
    else if (!fail)
    {
        ohud.blit_text_new(col2, 14, Utils::to_hex_string(motor_centre_pos).c_str(), 0x80);
    }

    ohud.blit_text_new(13, 17, "TESTS COMPLETE!", 0x82);

    hw_motor_control = MOTOR_OFF; // switch off
    counter          = 90;
    motor_state      = STATE_DONE;
}

void OOutputs::calibrate_done()
{
    if (counter > 0)
        counter--;
    else
        motor_state = STATE_EXIT;
}

// ------------------------------------------------------------------------------------------------
// Moving Cabinet Code
// ------------------------------------------------------------------------------------------------

const static uint8_t MOTOR_VALUES[] = 
{
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3,
    2, 2, 3, 3, 4, 4, 5, 5, 2, 3, 4, 5, 6, 7, 7, 7
};

const static uint8_t MOTOR_VALUES_STATIONARY[] = 
{
    2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4
};

const static uint8_t MOTOR_VALUES_OFFROAD1[] = 
{
    0x8, 0x8, 0x5, 0x5, 0x8, 0x8, 0xB, 0xB, 0x8, 0x8, 0x4, 0x4, 0x8, 0x8, 0xC, 0xC, 
    0x8, 0x8, 0x3, 0x3, 0x8, 0x8, 0xD, 0xD, 0x8, 0x8, 0x2, 0x2, 0x8, 0x8, 0xE, 0xE,
};

const static uint8_t MOTOR_VALUES_OFFROAD2[] = 
{
    0x8, 0x5, 0x8, 0xB, 0x8, 0x5, 0x8, 0xB, 0x8, 0x4, 0x8, 0xC, 0x8, 0x4, 0x8, 0xC,
    0x8, 0x3, 0x8, 0xD, 0x8, 0x3, 0x8, 0xD, 0x8, 0x2, 0x8, 0xE, 0x8, 0x2, 0x8, 0xE,
};

const static uint8_t MOTOR_VALUES_OFFROAD3[] = 
{
    0x8, 0x5, 0x5, 0x8, 0xB, 0x8, 0x0, 0x8, 0x8, 0x4, 0x4, 0x8, 0xC, 0x8, 0x0, 0x8,
    0x8, 0x3, 0x3, 0x8, 0xD, 0x8, 0x0, 0x8, 0x8, 0x2, 0x2, 0x8, 0xE, 0x8, 0x0, 0x8,
};

const static uint8_t MOTOR_VALUES_OFFROAD4[] = 
{
    0x8, 0xB, 0xB, 0x8, 0x5, 0x8, 0x0, 0x8, 0x8, 0xC, 0xC, 0x8, 0x4, 0x8, 0x0, 0x8,
    0x8, 0xD, 0xD, 0x8, 0x3, 0x8, 0x0, 0x8, 0x8, 0xE, 0xE, 0x8, 0x2, 0x8, 0x0, 0x8,
};


// Process Motor Code.
// Note, that only the Deluxe Moving Motor Code is ported for now.
// Source: 0xE644
void OOutputs::do_motors(int MODE, int16_t input_motor)
{
    motor_x_change = -(input_motor - (MODE == MODE_FFEEDBACK ? CENTRE_POS : motor_centre_pos));

    if (!motor_enabled)
    {
        done();
        return;
    }

    // In-Game: Test for crash, skidding, whether car is moving
    if (outrun.game_state == GS_INGAME)
    {
        if (ocrash.crash_counter)
        {
            skid_ffb_active = false;

            if ((oinitengine.car_increment >> 16) <= 0x14)
                car_stationary();
            else
                do_motor_crash();
        }
        else if (ocrash.skid_counter)
        {
            if (MODE == MODE_FFEEDBACK)
            {
                if (!skid_ffb_active)
                {
                    const int skid = ocrash.skid_counter;

                    // Positive skid = car skids left.
                    // Negative skid = car skids right.
                    const int direction =
                        skid > 0 ? 0x07 : 0x09;

                    // forcefeedback::set():
                    // 0 = strongest
                    // 7 = weakest
                    const int force = 0;

                    forcefeedback::set(
                        direction,
                        force);

                    skid_ffb_active = true;
                }
            }
            else
            {
                // Preserve original non-FFB behaviour
                do_motor_crash();
            }
        }
        else
        {
            skid_ffb_active = false;

            if ((oinitengine.car_increment >> 16) <= 0x14)
            {
                if (!was_small_change)
                    done();
                else
                    car_stationary();
            }
            else
            {
                if (MODE == MODE_FFEEDBACK &&
                    oferrari.wheel_state == OFerrari::WHEELS_ON)
                {
                    hw_motor_control = MOTOR_OFF;
                    forcefeedback::stop();
                }
                else
                {
                    car_moving(MODE);
                }
            }
        }
    }
    // Not In-Game: Act as though car is stationary / moving slow
    else
    {
        car_stationary();
    }
}

// Source: 0xE6DA
void OOutputs::car_moving(const int MODE)
{
    // Motor is currently moving
    if (motor_movement)
    {
        hw_motor_control = motor_control;
        adjust_motor();
        return;
    }
    
    // Motor is not currently moving. Setup new movement as necessary.
    if (oferrari.wheel_state != OFerrari::WHEELS_ON)
    {
        do_motor_offroad();
        return;
    }

    const uint16_t car_inc = oinitengine.car_increment >> 16;
    if (car_inc <= 0x64)                    speed = 0;
    else if (car_inc <= 0xA0)               speed = 1 << 3;
    else if (car_inc <= 0xDC)               speed = 2 << 3;
    else                                    speed = 3 << 3;

    if (oinitengine.road_curve == 0)         curve = 0;
    else if (oinitengine.road_curve <= 0x3C) curve = 2; // sharp curve
    else if (oinitengine.road_curve <= 0x5A) curve = 1; // gentle curve
    else                                     curve = 0;

    int16_t steering = oinputs.steering_adjust;
    steering += (movement_adjust1 + movement_adjust2 + movement_adjust3);
    steering >>= 2;
    movement_adjust3 = movement_adjust2;                   // Trickle down values
    movement_adjust2 = movement_adjust1;
    movement_adjust1 = oinputs.steering_adjust;

    // Veer Left
    if (steering >= 0)
    {
        steering = (steering >> 4) - 1;
        if (steering < 0)
        {
            car_stationary();
            return;
        }
                
        if (steering > 0)
            steering--;

        uint8_t motor_value = MOTOR_VALUES[speed + curve];

        if (MODE == MODE_FFEEDBACK)
        {
            hw_motor_control = motor_value + 8;
        }
        else
        {
            int16_t change = motor_x_change + (motor_value << 1);
            // Latch left movement
            if (change >= limit_left)
            {
                hw_motor_control   = MOTOR_CENTRE;
                motor_movement     = 1;
                motor_control      = 7;
                motor_change_latch = motor_x_change;
            }
            else
            {
                hw_motor_control = motor_value + 8;
            }
        }
        
        done();
    }
    // Veer Right
    else
    {
        steering = -steering;
        steering = (steering >> 4) - 1;
        if (steering < 0)
        {
            car_stationary();
            return;
        }

        if (steering > 0)
            steering--;

        uint8_t motor_value = MOTOR_VALUES[speed + curve];

        if (MODE == MODE_FFEEDBACK)
        {
            hw_motor_control = -motor_value + 8;
        }
        else
        {
            int16_t change = motor_x_change - (motor_value << 1);
            // Latch right movement
            if (change <= limit_right)
            {
                hw_motor_control   = MOTOR_CENTRE;
                motor_movement     = -1;
                motor_control      = 9;
                motor_change_latch = motor_x_change;
            }
            else
            {
                hw_motor_control = -motor_value + 8;
            }
        }
        
        done();
    }
}

// Source: 0xE822
void OOutputs::car_stationary()
{
    if (mode == MODE_FFEEDBACK)
    {
        hw_motor_control = MOTOR_OFF;
        forcefeedback::stop();
        return;
    }

    int16_t change = std::abs(motor_x_change);

    const int centre_deadzone = 8;

    if (change <= centre_deadzone)
    {
        if (!is_centered)
        {
            hw_motor_control = MOTOR_CENTRE;
            is_centered = true;
        }
        else
        {
            hw_motor_control = MOTOR_OFF;
            is_centered = false;
            done();
        }
    }
    else
    {
        int8_t motor_value =
            MOTOR_VALUES_STATIONARY[change >> 3];

        if (motor_x_change >= 0)
            motor_value = -motor_value;

        hw_motor_control = motor_value + 8;
        done();
    }
}

// Source: 0xE8DA
void OOutputs::adjust_motor()
{
    int16_t change = motor_change_latch; // d1
    motor_change_latch = motor_x_change;
    change -= motor_x_change;
    if (change < 0) 
        change = -change;

    // no movement
    if (change <= 2)
    {
        motor_movement = 0;
        is_centered    = true;
    }
    // moving right
    else if (motor_movement < 0)
    {
        if (++motor_control > 10)
            motor_control = 10;
    }
    // moving left
    else 
    {
        if (--motor_control < 6)
            motor_control = 6;
    }

    done();
}

// Adjust motor during crash/skid state
// Source: 0xE994
void OOutputs::do_motor_crash()
{
    if (oferrari.car_x_diff == 0)
        set_value(MOTOR_VALUES_OFFROAD1, 3);
    else if (oferrari.car_x_diff < 0)
        set_value(MOTOR_VALUES_OFFROAD4, 3);
    else
        set_value(MOTOR_VALUES_OFFROAD3, 3);
}

// Adjust motor when wheels are off-road
// Source: 0xE9BE
void OOutputs::do_motor_offroad()
{
    // For FFB, keep the denser vibration pattern even when all wheels
    // are off-road. Cabinet/non-FFB modes retain the original table choice.
    const uint8_t* table =
        (mode == MODE_FFEEDBACK ||
         oferrari.wheel_state != OFerrari::WHEELS_OFF)
        ? MOTOR_VALUES_OFFROAD2
        : MOTOR_VALUES_OFFROAD1;

    const uint16_t car_inc =
        oinitengine.car_increment >> 16;

    uint8_t index;

    if (car_inc <= 0x32)
        index = 0;
    else if (car_inc <= 0x50)
        index = 1;
    else if (car_inc <= 0x6E)
        index = 2;
    else
        index = 3;

    // Preserve original behaviour for cabinet / non-FFB modes.
    if (mode != MODE_FFEEDBACK)
    {
        set_value(table, index);
        return;
    }

    // ---------------------------------------------------------------------
    // FFB off-road handling
    //
    // Keep the original vibration pattern and add a constant force that
    // pulls the wheel further away from the road.
    // ---------------------------------------------------------------------

    const uint8_t cmd =
        table[(index << 3) + (counter & 7)];

    counter++;

    // Remember which side of the road we left.
    if (oferrari.wheel_state == OFerrari::WHEELS_LEFT_OFF)
    {
        offroad_pull_direction = 1;
    }
    else if (oferrari.wheel_state == OFerrari::WHEELS_RIGHT_OFF)
    {
        offroad_pull_direction = -1;
    }
    else if (oferrari.wheel_state == OFerrari::WHEELS_OFF &&
        offroad_pull_direction == 0)
    {
        // Fallback if both wheels leave the road in a single frame.
        offroad_pull_direction =
            (oinitengine.car_x_pos >= 0) ? -1 : 1;
    }

    // Convert original motor command into a signed force level:
    //
    // -7 = strong left
    //  0 = centre
    // +7 = strong right
    int rumble_force = 0;

    if (cmd != MOTOR_OFF &&
        cmd != MOTOR_CENTRE)
    {
        rumble_force =
            static_cast<int>(cmd) - MOTOR_CENTRE;
    }

    const bool fully_offroad =
        oferrari.wheel_state == OFerrari::WHEELS_OFF;

    // With one wheel off-road, keep the existing restrained vibration so
    // the outward pull remains dominant. With all four wheels off-road,
    // restore the full vibration amplitude so it cannot be masked by pull.
    const int rumble_percent =
        fully_offroad ? 100 : 50;

    rumble_force =
        (rumble_force * rumble_percent) / 100;

    // Constant outward pull. Once the whole car is off-road the bias is
    // deliberately reduced, leaving enough headroom for alternating rumble.
    int pull_force =
        fully_offroad ? 2 : 3;

    pull_force *= offroad_pull_direction;

    // Combine vibration and outward pull.
    int combined_force =
        rumble_force + pull_force;

    if (combined_force > 7)
        combined_force = 7;
    else if (combined_force < -7)
        combined_force = -7;

    // No resulting constant force.
    if (combined_force == 0)
    {
        hw_motor_control = MOTOR_CENTRE;
        forcefeedback::stop();
        done();
        return;
    }

    // Convert back to CannonBall motor command.
    hw_motor_control =
        static_cast<uint8_t>(
            MOTOR_CENTRE + combined_force);

    done();
}

void OOutputs::set_value(const uint8_t* table, uint8_t index)
{
    hw_motor_control = table[(index << 3) + (counter & 7)];
    counter++;
    done();
}

// Source: 0xE94E
void OOutputs::done()
{
    const int centre_deadzone =
        (mode == MODE_FFEEDBACK) ? 1 : 8;

    if (std::abs(motor_x_change) <= centre_deadzone)
    {
        was_small_change = true;
        motor_control = MOTOR_CENTRE;
    }
    else
    {
        was_small_change = false;
    }
}
// Send output commands to motor hardware
// This is the equivalent to writing to register 0x140003
void OOutputs::motor_output(uint8_t cmd)
{
    if (cmd == MOTOR_OFF || cmd == MOTOR_CENTRE)
        return;

    int8_t force = 0;

    if (cmd < MOTOR_CENTRE)      // left
        force = cmd - 1;
    else if (cmd > MOTOR_CENTRE) // right
        force = 15 - cmd;

    forcefeedback::set(cmd, force);
}

// ------------------------------------------------------------------------------------------------
// Deluxe Upright: Steering Wheel Movement
// ------------------------------------------------------------------------------------------------

// Deluxe Upright: Vibration Enable Table. 4 Groups of vibration values.
const static uint8_t VIBRATE_LOOKUP[] = 
{
    // SLOW SPEED --------   // MEDIUM SPEED ------
    1, 0, 0, 0, 1, 0, 0, 0,  1, 1, 0, 0, 1, 1, 0, 0,
    // FAST SPEED --------   // VERY FAST SPEED ---
    1, 1, 1, 0, 1, 1, 1, 0,  1, 1, 1, 1, 1, 1, 1, 1,
};

// Source: 0xEAAA
void OOutputs::do_vibrate_upright()
{
    if (outrun.game_state != GS_INGAME)
    {
        clear_digital(D_MOTOR);
        return;
    }

    const uint16_t speed = oinitengine.car_increment >> 16;
    uint16_t index = 0;

    // Car Crashing: Diable Motor once speed below 10
    if (ocrash.crash_counter)
    {
        if (speed <= 10)
        {
            clear_digital(D_MOTOR);
            return;
        }
    }
    // Car Normal
    else if (!ocrash.skid_counter)
    {
        // 0xEAE2: Disable Vibration once speed below 30 or wheels on-road
        if (speed < 30 || oferrari.wheel_state == OFerrari::WHEELS_ON)
        {
            clear_digital(D_MOTOR);
            return;
        }

        // 0xEAFC: Both wheels off-road. Faster the car speed, greater the chance of vibrating
        if (oferrari.wheel_state == OFerrari::WHEELS_OFF)
        {
            if (speed > 220)      index = 3;
            else if (speed > 170) index = 2;
            else if (speed > 120) index = 1;
        }
        // 0xEB38: One wheel off-road. Faster the car speed, greater the chance of vibrating
        else
        {
            if (speed > 270)      index = 3;
            else if (speed > 210) index = 2;
            else if (speed > 150) index = 1;
        }

        if (VIBRATE_LOOKUP[ (vibrate_counter & 7) + (index << 3) ])
            set_digital(D_MOTOR);
        else
            clear_digital(D_MOTOR);

        vibrate_counter++;
        return;
    }
    // 0xEB68: Car Crashing or Skidding
    if (speed > 140)      index = 3;
    else if (speed > 100) index = 2;
    else if (speed > 60)  index = 1;

    if (VIBRATE_LOOKUP[ (vibrate_counter & 7) + (index << 3) ])
        set_digital(D_MOTOR);
    else
        clear_digital(D_MOTOR);

    vibrate_counter++;
}

// ------------------------------------------------------------------------------------------------
// Mini Upright: Steering Wheel Movement
// ------------------------------------------------------------------------------------------------

void OOutputs::do_vibrate_mini()
{
    if (outrun.game_state != GS_INGAME)
    {
        clear_digital(D_MOTOR);
        return;
    }

    const uint16_t speed = oinitengine.car_increment >> 16;
    uint16_t index = 0;

    // Car Crashing: Disable Motor once speed below 10
    if (ocrash.crash_counter)
    {
        if (speed <= 10)
        {
            clear_digital(D_MOTOR);
            return;
        }
    }
    // Car Normal
    else if (!ocrash.skid_counter)
    {
        if (speed < 10 || oferrari.wheel_state == OFerrari::WHEELS_ON)
        {
            clear_digital(D_MOTOR);
            return;
        }  

        if (speed > 140)      index = 5;
        else if (speed > 100) index = 4;
        else if (speed > 60)  index = 3;
        else if (speed > 20)  index = 2;
        else                  index = 1;

        if (index > vibrate_counter)
        {
            vibrate_counter = 0;
            clear_digital(D_MOTOR);
        }
        else
        {
            vibrate_counter++;
            set_digital(D_MOTOR);
        }
        return;
    }

    // 0xEC7A calc_crash_skid:
    if (speed > 90)      index = 4;
    else if (speed > 70) index = 3;
    else if (speed > 50) index = 2;
    else if (speed > 30) index = 1;
    if (index > vibrate_counter)
    {
        vibrate_counter = 0;
        clear_digital(D_MOTOR);
    }
    else
    {
        vibrate_counter++;
        set_digital(D_MOTOR);
    }
}

// ------------------------------------------------------------------------------------------------
// Coin Chute Output
// Source: 0x6F8C
// ------------------------------------------------------------------------------------------------

void OOutputs::coin_chute_out(CoinChute* chute, bool insert)
{
    // Initalize counter if coin inserted
    chute->counter[2] = insert ? 1 : 0;

    if (chute->counter[0])
    {
        if (--chute->counter[0] != 0)
            return;
        chute->counter[1] = 6;
        clear_digital(chute->output_bit);
    }
    else if (chute->counter[1])
    {
        chute->counter[1]--;
    }
    // Coin first inserted. Called Once. 
    else if (chute->counter[2])
    {
        chute->counter[2]--;
        chute->counter[0] = 6;
        set_digital(chute->output_bit);
    }
}