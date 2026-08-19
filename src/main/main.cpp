/***************************************************************************
    Cannonball Main Entry Point.

    Copyright (c) Chris White.
    See license.txt for more details.

    This version for CannonBall-SE incorporates revisions Copyright (c)
    2025 James Pearce:
    - Threaded operation
    - Automatic 30/60fps operation
    - Rumble on tire smoke
    - Hardware watchdog support
    - Records play count and runtime hours
***************************************************************************/

#include <cstring>
#include <iostream>

// SDL Library
#include <SDL.h>

// SDL Specific Code
#include "sdl2/timer.hpp"
#include "sdl2/input.hpp"

#include "video.hpp"

#include "romloader.hpp"
#include "trackloader.hpp"
#include "stdint.hpp"
#include "main.hpp"
#include "engine/outrun.hpp"
#include "frontend/config.hpp"
#include "frontend/menu.hpp"

#include "engine/oinputs.hpp"
#include "engine/ooutputs.hpp"
#include "engine/omusic.hpp"

// Direct X Haptic Support.
// Fine to include on non-windows builds as dummy functions used.
#include "directx/ffeedback.hpp"

// Multi-threading support
// This implementation runs three parts of frame rendering in parallel to enable 60fps operation even on
// Raspberry Pi Zero 2W (requires 450MHz GPU clock).
#include <thread>
#include <mutex>
#include <chrono>
#include <semaphore>
#include <omp.h>
#include <condition_variable>
#include <cstdio>
#include <algorithm>
#include <atomic>

#ifdef _WIN32
  #include <thread>
#else
  #include <pthread.h>
#endif

#ifndef _WIN32
  #include <sys/resource.h>
  #include <sys/syscall.h>   // for SYS_gettid
  #include <unistd.h>        // for syscall()
#endif

#ifdef PROFILE_WITH_GPERFTOOLS
#include <gperftools/profiler.h> // performance profiling
static uint32_t perf_end_frame = 0;
#endif

#include "singlecorepi.hpp" // detects if running on a single-core RaspberryPi

#ifdef _WIN32
  #include <thread>
  #define sched_yield() std::this_thread::yield()
#else
  #include <pthread.h>
  #include <sched.h>
#endif

// ------------------------------------------------------------------------------------------------
// Watchdog close handler (prevents hardware reset on e.g. seg fault
// In that scenario, systemd should restart cannonball. Watchdog is there to catch code or system
// lock-up.
// ------------------------------------------------------------------------------------------------
#ifdef __linux__
  #include <signal.h>
  #include <sys/ioctl.h>
  #include <linux/watchdog.h>

  // 1) Global watchdog FD
  static int g_watchdog_fd = -1;

  // 2) Cleanup: disable & close
  static void cleanup_watchdog()
  {
      if (g_watchdog_fd >= 0) {
          int disable = WDIOS_DISABLECARD;
          if (ioctl(g_watchdog_fd, WDIOC_SETOPTIONS, &disable) < 0) {
              std::cerr << "Could not disable watchdog" << std::endl;
          }
          close(g_watchdog_fd);
          g_watchdog_fd = -1;
          std::cout << "Watchdog disabled and closed" << std::endl;
      }
  }

  // 3) Signal handler
  static void watchdog_signal_handler(int signum)
  {
      cleanup_watchdog();
      // restore default handler and re-raise so core dump / default behavior still happens
      signal(signum, SIG_DFL);
      raise(signum);
  }

  // Helper to register for all the “fatal” signals
  static void register_watchdog_signal_handlers()
  {
      struct sigaction sa;
      sa.sa_handler   = watchdog_signal_handler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags     = SA_RESETHAND;  // reset handler to default after first catch

      int signals[] = { SIGINT, SIGTERM, SIGABRT,
                        SIGSEGV, SIGFPE, SIGILL, SIGBUS };
      for (int s : signals) {
          sigaction(s, &sa, nullptr);
      }
  }
#endif


// ------------------------------------------------------------------------------------------------
// Diagnostics - displays info on crash
// ------------------------------------------------------------------------------------------------
#ifdef __linux__
    #include <execinfo.h>
    #include <signal.h>
    #include <unistd.h>
    #include <cstdio>

    static void segv_handler(int sig) {
        void* buf[64];
        int n = backtrace(buf, 64);
        fprintf(stderr, "Fatal signal %d — stack:\n", sig);
        backtrace_symbols_fd(buf, n, STDERR_FILENO);
        _Exit(128 + sig);
    }

    void install_segv_handler() {
        struct sigaction sa{};
        sa.sa_handler = segv_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        sigaction(SIGSEGV, &sa, nullptr);
    }
#endif



// ------------------------------------------------------------------------------------------------
// Initialize Shared Variables
// ------------------------------------------------------------------------------------------------
using namespace cannonball;

int     cannonball::state               = STATE_BOOT;
double  cannonball::frame_ms            = 0;
int     cannonball::frame               = 0;
bool    cannonball::tick_frame          = true;
int     cannonball::fps_counter         = 0;
int     cannonball::fps_lock            = 0; // 0=no lock (auto), 30(fps), 60(fps)
bool    cannonball::singlecore_detect   = true;
bool    cannonball::singlecore_mode     = false;
// fps_eval_period is the interval at which 30/60 fps is evaluated in auto mode. Starts at
// 4 (seconds), and is doubled every time a switch back to 30fps happens
long    cannonball::fps_eval_period     = 4;
int     cannonball::game_threads        = omp_get_max_threads();
bool    cannonball::perftest            = false;

// ------------------------------------------------------------------------------------------------
// Main Variables and Pointers
// ------------------------------------------------------------------------------------------------
Audio cannonball::audio;
Menu* menu;
bool pause_engine;

// Short transient applied after normal game FFB when changing gear.
static bool ffb_shift_active = false;
static int ffb_shift_direction = 1;
static Uint32 ffb_shift_start_ms = 0;

// Short mechanical detent when moving between music-selection positions.
static bool ffb_music_detent_active = false;
static int ffb_music_last_selection = -1;
static int ffb_music_last_steering = 0;
static int ffb_music_detent_direction = 0;
static Uint32 ffb_music_detent_start_ms = 0;

static void set_music_detent_force(int direction, int gain_percent)
{
    int scaled_gain =
        (config.controls.ffb_strength * gain_percent + 50) / 100;

    if (scaled_gain < 10)
        scaled_gain = 10;
    else if (scaled_gain > 100)
        scaled_gain = 100;

    forcefeedback::set_gain(scaled_gain);
    forcefeedback::set(direction, 0);
    forcefeedback::set_gain(config.controls.ffb_strength);
}


// ------------------------------------------------------------------------------------------------

static void quit_func(int code)
{
    audio.stop_audio();
    input.close_joy();
    forcefeedback::close();
    if (menu) delete menu;
    SDL_Quit();
    _Exit(code); // exit without invoking atexit bindings; this prevents seg fault caused by uninitialised SDLgpu.
    //exit(code);
}

static void process_events(void)
{
    SDL_Event event;

    // Grab all events from the queue.
    while(SDL_PollEvent(&event)) {
        switch(event.type) {
            case SDL_KEYDOWN:
                // Handle key presses.
                if (event.key.keysym.sym == config.master_break_key)
                    cannonball::state = STATE_QUIT;
                else
                    input.handle_key_down(&event.key.keysym);
                break;

            case SDL_KEYUP:
                input.handle_key_up(&event.key.keysym);
                break;

            case SDL_JOYAXISMOTION:
                input.handle_joy_axis(&event.jaxis);
                break;

            case SDL_JOYBUTTONDOWN:
                input.handle_joy_down(&event.jbutton);
                break;

            case SDL_JOYBUTTONUP:
                input.handle_joy_up(&event.jbutton);
                break;

            case SDL_CONTROLLERAXISMOTION:
                input.handle_controller_axis(&event.caxis);
                break;

            case SDL_CONTROLLERBUTTONDOWN:
                input.handle_controller_down(&event.cbutton);
                break;

            case SDL_CONTROLLERBUTTONUP:
                input.handle_controller_up(&event.cbutton);
                break;

            case SDL_JOYHATMOTION:
                input.handle_joy_hat(&event.jhat);
                break;

            case SDL_JOYDEVICEADDED:
                input.add_joystick(event.jdevice.which);
                break;

            case SDL_JOYDEVICEREMOVED:
                input.remove_joystick(event.jdevice.which);
                break;

            case SDL_QUIT:
                // Handle quit requests (like Ctrl-c).
                cannonball::state = STATE_QUIT;
                break;
        }
    }
}

static void tick()
{
    frame++;

    // Determine whether to tick certain logic for the current frame.
    tick_frame = (config.fps == 60) ?
                 (((frame & 1) == 0) ? 1 : 0)
                 : 1;

    process_events();

    if (tick_frame) {
        oinputs.tick();           // Do Controls

        const bool gear_before = oinputs.gear;
        oinputs.do_gear();        // Digital Gear

        if (oinputs.gear != gear_before &&
            config.controls.haptic &&
            forcefeedback::is_supported() &&
            cannonball::state == STATE_GAME &&
            outrun.game_state == GS_INGAME)
        {
            ffb_shift_active = true;
            ffb_shift_direction = oinputs.gear ? 1 : -1;
            ffb_shift_start_ms = SDL_GetTicks();
        }
    }

    switch (cannonball::state) {
        case STATE_GAME: {
            if (tick_frame) {
                if (input.has_pressed(Input::TIMER)) outrun.freeze_timer = !outrun.freeze_timer;
                if (input.has_pressed(Input::PAUSE)) pause_engine = !pause_engine;
                if (input.has_pressed(Input::MENU))  cannonball::state = STATE_INIT_MENU;
            }

            if (!pause_engine || input.has_pressed(Input::STEP))
                outrun.tick(tick_frame);

            if (tick_frame) input.frame_done();
        }
        break;

        case STATE_INIT_GAME: {
            if (config.engine.jap && !roms.load_japanese_roms()) {
                std::cerr << "Japanese ROMs not loaded." << std::endl;
                cannonball::state = STATE_QUIT;
            } else {
                tick_frame = true;
                pause_engine = false;
                outrun.init();
                cannonball::state = STATE_GAME;
            }
        }
        break;

        case STATE_MENU:
            menu->tick();
            input.frame_done();
            break;

        case STATE_INIT_MENU:
            oinputs.init();
            outrun.outputs->init();
            menu->init();
            cannonball::state = STATE_MENU;
            break;
    }

    // Report output state for SmartyPi
    outrun.outputs->writeDigitalToConsole();

    if (tick_frame) {
        // Controller Rumble. Only rumble in-game.
        // JJP - Enhancement - high frequency rumble 'ticks' when skidding on road
        if (outrun.SkiddingOnRoad() && outrun.game_state == GS_INGAME)
            input.set_rumble(true, config.controls.rumble, 1);
        else
            input.set_rumble(outrun.outputs->is_set(OOutputs::D_MOTOR), config.controls.rumble, 0);
    }

    // Music-selection wheel detent. This deliberately runs after normal output
    // processing so the stationary-car FFB path cannot stop it in the same frame.
    if (config.controls.haptic &&
        forcefeedback::is_supported() &&
        cannonball::state == STATE_GAME &&
        outrun.game_state == GS_MUSIC)
    {
        const int selection = omusic.get_music_selected();
        const int steering = oinputs.steering_adjust;

        if (ffb_music_last_selection < 0)
        {
            // Initialise tracking without kicking the wheel when the screen opens.
            ffb_music_last_selection = selection;
            ffb_music_last_steering = steering;
        }
        else if (selection != ffb_music_last_selection)
        {
            const int steering_delta =
                steering - ffb_music_last_steering;

            if (steering_delta > 0)
                ffb_music_detent_direction = 1;
            else if (steering_delta < 0)
                ffb_music_detent_direction = -1;
            else
                ffb_music_detent_direction =
                    selection > ffb_music_last_selection ? 1 : -1;

            ffb_music_detent_active = true;
            ffb_music_detent_start_ms = SDL_GetTicks();
            ffb_music_last_selection = selection;
        }

        ffb_music_last_steering = steering;

        if (ffb_music_detent_active)
        {
            const Uint32 elapsed =
                SDL_GetTicks() - ffb_music_detent_start_ms;

            // On this DirectInput path 0x07 physically pulls the current wheel
            // right and 0x09 left. First resist the player's movement, then
            // clearly snap into the newly selected position.
            const int resistance_direction =
                ffb_music_detent_direction > 0 ? 0x09 : 0x07;
            const int snap_direction =
                ffb_music_detent_direction > 0 ? 0x07 : 0x09;

            if (elapsed < 110)
            {
                // Deliberately strong: prove the detent is physically present.
                // At the default 50% master this runs at 65% effective gain.
                set_music_detent_force(resistance_direction, 130);
            }
            else if (elapsed < 170)
            {
                set_music_detent_force(snap_direction, 100);
            }
            else
            {
                forcefeedback::stop();
                ffb_music_detent_active = false;
            }
        }
    }
    else
    {
        if (ffb_music_detent_active)
            forcefeedback::stop();

        ffb_music_detent_active = false;
        ffb_music_last_selection = -1;
        ffb_music_last_steering = oinputs.steering_adjust;
        ffb_music_detent_direction = 0;
    }

    // Gear shift kick. This runs after normal game FFB so the on-road code
    // cannot stop the transient in the same frame.
    if (ffb_shift_active)
    {
        if (!config.controls.haptic ||
            !forcefeedback::is_supported() ||
            cannonball::state != STATE_GAME ||
            outrun.game_state != GS_INGAME)
        {
            ffb_shift_active = false;
        }
        else
        {
            const Uint32 elapsed = SDL_GetTicks() - ffb_shift_start_ms;
            const int primary_direction = ffb_shift_direction > 0 ? 0x09 : 0x07;
            const int rebound_direction = ffb_shift_direction > 0 ? 0x07 : 0x09;

            if (elapsed < 45)
            {
                forcefeedback::set(primary_direction, 0);
            }
            else if (elapsed < 80)
            {
                forcefeedback::set(rebound_direction, 7);
            }
            else
            {
                forcefeedback::stop();
                ffb_shift_active = false;
            }
        }
    }
}


#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

// Combined thread: updates play stats and kicks the watchdog (on linux)
static void play_stats_and_watchdog_updater() {
    // this thread saves play stats periodically to the associated config file
    // e.g. runtime and number of plays
    // Updates every minute in a seperate thread since SD-card access can be slow
    // and we don't want to hold up the game engine.
    // Also kicks system watchdog when compiled for Linux

#ifdef __linux__
    // Open the watchdog device. Note - device name is defined in globals.hpp
    g_watchdog_fd = open(SYSTEM_WATCHDOG, O_WRONLY);
    if (g_watchdog_fd < 0) {
        std::cerr << "Note: Could not open " << SYSTEM_WATCHDOG << " - proceeding without system watchdog." << std::endl;
    } else {
        // Set the watchdog timeout (in seconds)
        int timeout = 15; // 15 seconds is the maximum supported on Raspberry Pi
        if (ioctl(g_watchdog_fd, WDIOC_SETTIMEOUT, &timeout) < 0) {
            std::cerr << "Note: Unable to set watchdog timeout - proceeding without system watchdog." << std::endl;
            close(g_watchdog_fd);
            g_watchdog_fd = -1;
        } else {
            std::cout << "Watchdog timeout set to " << timeout << " seconds" << std::endl;
        }
    }
#endif

    Timer run_time;
    run_time.start();
    while (cannonball::state != STATE_QUIT) {
        if ((run_time.get_ticks() >= 60000) &&
            (cannonball::state == STATE_GAME) ) {
            config.stats.runtime++;  // increment machine run-time by 1 (minute)
            config.save_stats();     // save stats file
            run_time.start();        // reset timer
        }
        SDL_Delay(500); // wait 0.5 seconds before next check

#ifdef __linux__
        // Kick the watchdog device if it's open.
        if (g_watchdog_fd >= 0) {
            if (write(g_watchdog_fd, "\0", 1) < 0) {
                std::cerr << "Failed to write to " << SYSTEM_WATCHDOG << std::endl;
            }
        }
#endif
    }
#ifdef __linux__
    // Disable the watchdog on exit.
    cleanup_watchdog();
#endif
}


// Semaphore based threading for video rendering
// used to spread the load across all available cores

static std::binary_semaphore renderReady0{0};
static std::binary_semaphore renderReady1{0};
static std::binary_semaphore renderDone0{0};
static std::binary_semaphore renderDone1{0};
static std::binary_semaphore* const renderReady[2] = { &renderReady0, &renderReady1 };
static std::binary_semaphore* const renderDone [2] = { &renderDone0,  &renderDone1  };

static std::binary_semaphore prepareReady{0};
static std::binary_semaphore prepareDone {0};

static std::atomic<bool> running{true};

void render_thread(int renderthreads, int id)
{
    while (running.load(std::memory_order_acquire)) {
        renderReady[id]->acquire();
        if (!running.load(std::memory_order_acquire)) break;
        video.render_frame((renderthreads==1 ? -1 : id));
        renderDone[id]->release();
    }
}

void prepare_thread() {
    while (running.load(std::memory_order_acquire)) {
        prepareReady.acquire();
        if (!running.load(std::memory_order_acquire)) break;
        tick();
        audio.tick();
        video.prepare_frame();
        prepareDone.release();
    }
}


void pin_thread_to_core(std::thread& t, int core_id) {
#ifdef _WIN32
        std::cerr << "Error setting affinity for thread to core: "
                  << "CannonBall-SE does not support thread pinning on Windows" << "\n";
    return;
#else
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t handle = t.native_handle();
    int err = pthread_setaffinity_np(handle, sizeof(cpuset), &cpuset);
    if (err != 0) {
        std::cerr << "Error setting affinity for thread to core "
                  << core_id << ": " << std::strerror(err) << "\n";
    }
#endif
}


static void main_loop() {
    // Determine frame rate. Use auto (30/60) unless override was set on command-line
    int configured_fps = (cannonball::fps_lock == 60 ? 60 : 30);
    config.video.fps   = (configured_fps == 30 ? 0 : 2);
    config.set_fps(config.video.fps);
    double targetFPS   = static_cast<double>(configured_fps);

    // Track whether vsync is enabled
    bool vsync = false;
    // Determine if we can rely on vsync (for 60fps mode)
    SDL_DisplayMode displayMode;
    if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
        // Can retrieve monitor refresh rate
        vsync = (displayMode.refresh_rate == configured_fps) && SDL_GL_GetSwapInterval() && (config.video.vsync == 1);
        std::cout << "INFO: ";
        if (config.video.vsync != 1)
            std::cout << "VSync is disabled by setting in config.xml. ";
        std::cout << "Display reports refresh rate is " << displayMode.refresh_rate << "Hz.";
        if ((config.video.vsync == 1) && (displayMode.refresh_rate == 60) && (cannonball::fps_lock != 30))
            std::cout << " VSync will be used for 60fps mode.\n";
        else
            std::cout << "\n";
    }

    // Determine if we'll be using threaded or sequential rendering
    int threads = cannonball::game_threads;

#ifdef WIN32
    // On Windows, input must be on the main thread. We therefore cap the thread-count to 3,
    // thereby setting prepare_threads to 0 (below)
    if (threads > 3) threads = 3;
#else
    // On Linux, we can use up to 4 threads
    if (threads > 4) threads = 4; // max used by cannonball-se
#endif

    int using_threading = (threads > 1);
    int render_threads = (threads > 2 ? 2 : 1);
    int prepare_threads = (threads >= 4);

    std::thread t0;
    std::thread t1;
    std::thread t2;
    if (using_threading) {
        // Create worker threads.
        std::cout << "Using " << threads << " threads (" << render_threads << " renderer threads)" << std::endl;
        if (prepare_threads) {
            // prepare_thread produces the S16 output - road, sky, sprites etc
            // Note - this thread also launches the sound callback, so don't pin this thread to one core.
            t0 = std::thread(prepare_thread);
        }
        // render_threads process with S16 output with Blargg filter, if enabled,
        // or convert the S16 palette-based output to RGB otherwise
        t1 = std::thread(render_thread,render_threads,0);
        // pin_thread_to_core(t1,2);
        if (render_threads == 2) {
            t2 = std::thread(render_thread,render_threads,1);
            // pin_thread_to_core(t2,3);
        }
    }

    SDL_Delay(500); // let system stabalise

    // Duration per frame (in seconds)
    auto frameDuration = std::chrono::duration<double>(1.0 / targetFPS);

    // Set the next frame time to now + frameDuration
    // we do x10 for the first frame so the special start frame can be seen.
    auto nextFrameTime = std::chrono::steady_clock::now() + frameDuration;

    // For diagnostics:
    int frameCounter = 0;
    int renderedFrames = 0;
    int droppedFrames = 0;
    auto fpsTimer = std::chrono::steady_clock::now();

    // Performance check variables (10-second evaluation).
    auto performanceCheckStart = std::chrono::steady_clock::now();
    int totalRenderedFramesForCheck = 0;
    // For 30 FPS, accumulate sleep time and frame count.
    std::chrono::duration<double> totalSleepTime(0);
    int frameCountForSleep = 0;

    // Go!

    while (cannonball::state != STATE_QUIT) {
        #ifdef PROFILE_WITH_GPERFTOOLS
        if ((outrun.game_state == GS_START1) && (perf_end_frame==0)) {
            perf_end_frame = frameCounter + (5 * configured_fps);
            ProfilerStart("cpu-profile.pb");
        } else if (frameCounter >= perf_end_frame) {
            // stop collecting
            ProfilerStop();
        }
        #endif

        frameCounter++;
        auto now = std::chrono::steady_clock::now();

        // If running at 30 FPS, count this frame for sleep evaluation.
        frameCountForSleep += (configured_fps == 30);

        // If we're behind schedule and not forcing a render, drop this frame
        // always render every 4th frame at least
        bool forceRender = ((frameCounter & 3) == 3);
        if (!forceRender && now > nextFrameTime) {
            // Update game logic for missed frame
            tick();
            audio.tick();
            ++droppedFrames;
            nextFrameTime = (now + frameDuration); // reset time next frame is due
            // Skip heavy work this frame; return to top of while loop
            continue;
        }

        // ---- LAUNCH WORKER TASKS & RENDERING ----

        renderedFrames++;
        totalRenderedFramesForCheck++;  // For performance evaluation

        if (using_threading) {
            // Set NTSC filter to work on the last complete frame immediately
            renderReady0.release();
            if (render_threads==2) renderReady1.release();

            // Set sprite gen to work if we're threading that too
            if (prepare_threads) {
                prepareReady.release(); // sprites for last frame
            } else {
                // tick game logic
                tick();
                audio.tick();
                video.prepare_frame();
            }

            // Run the GPU-bound work on the main thread (SDL limitation)
            video.present_frame();

            // await thread completion
            renderDone0.acquire();
            if (render_threads==2) renderDone1.acquire();
            if (prepare_threads)   prepareDone.acquire();
        } else {
            // 1 Game Thread. Run logic sequentially
            tick();
            audio.tick();
            video.prepare_frame();
            video.render_frame(-1);
            video.present_frame();
        }

        // Swap the buffers for the next frame.
        video.swap_buffers();

        // Check to see if anything happened needing a video restart
        if (config.videoRestartRequired) {
            video.disable();
            config.video.hires = config.video.hires_next;
            video.init(&roms, &config.video);
            video.sprite_layer->set_x_clip(false);
            config.videoRestartRequired = false;
            // reset timers as video restart can take a while
            nextFrameTime = std::chrono::steady_clock::now();
        }
        // If we're ahead of schedule, sleep until it's time.
        if (perftest) {
            // we're trying to run the engine as fast as possible, simply update the next frame time
            nextFrameTime = std::chrono::steady_clock::now();
        } else if (!vsync) {
            if (now < nextFrameTime) {
                auto sleepDuration = nextFrameTime - now;
                // Only record sleep if running at 30 FPS.
                totalSleepTime += sleepDuration * (configured_fps == 30);

                std::this_thread::sleep_for(sleepDuration);
                now = std::chrono::steady_clock::now();
            }
        }

        // Update the next frame time.
        nextFrameTime += frameDuration;

        // Record FPS info on console (every 2 seconds)
        auto elapsed = std::chrono::steady_clock::now() - fpsTimer;
        if (elapsed >= std::chrono::seconds(2)) {
            int fps = renderedFrames / 2;
            int totalFrames = renderedFrames + droppedFrames;
            int droppedPercent = (totalFrames > 0) ? (droppedFrames * 100 / totalFrames) : 0;
            printf("\r%i FPS (dropped: %i%%)    ", fps, droppedPercent);
            fflush(stdout);
            fps_counter = fps;
            renderedFrames = 0;
            droppedFrames = 0;
            fpsTimer = std::chrono::steady_clock::now();
        }

        // ---- PERFORMANCE EVALUATION (every 10 seconds) ----
        if (cannonball::fps_lock==0) {
            auto performanceElapsed = std::chrono::steady_clock::now() - performanceCheckStart;
            if (performanceElapsed >= std::chrono::seconds(cannonball::fps_eval_period)) {
                double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(performanceElapsed).count();
                if (configured_fps == 60) {
                    // Evaluate average FPS at 60 FPS.
                    double avgFPS = totalRenderedFramesForCheck / seconds;
                    if (avgFPS < 50.0) {
                        printf("\nPerformance check: average FPS %.2f too low. Switching to 30 FPS.\n", avgFPS);
                        config.video.fps = 0; // 0 = 30 fps
                        config.set_fps(config.video.fps);
                        cannonball::fps_eval_period *= 2; // double eval period for next try
                    }
                }
                else if (configured_fps == 30) {
                    // Evaluate average sleep fraction at 30 FPS.
                    double avgSleepFraction = 0.0;
                    if (frameCountForSleep > 0) {
                        avgSleepFraction = totalSleepTime.count() / (frameDuration.count() * frameCountForSleep);
                    }
                    if (avgSleepFraction > 0.6) {
                        printf("\nPerformance check: average sleep fraction %.2f%%. Switching to 60 FPS.\n", avgSleepFraction * 100.0);
                        config.video.fps = 2; // 2 = 60 fps
                        config.set_fps(config.video.fps);
                    }
                }
                // Reset performance evaluation counters.
                performanceCheckStart = std::chrono::steady_clock::now();
                totalRenderedFramesForCheck = 0;
                totalSleepTime = std::chrono::duration<double>(0);
                frameCountForSleep = 0;
            }
            // Update control variables if there is an FPS change
            if (config.fps != configured_fps) {
                configured_fps = config.fps;
                targetFPS = static_cast<double>(configured_fps);
                frameDuration = std::chrono::duration<double>(1.0 / targetFPS);
                nextFrameTime = std::chrono::steady_clock::now() + frameDuration;

                // Determine if we can rely on vsync (for 60fps mode)
                SDL_DisplayMode displayMode;
                if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0) {
                    // Can retrieve monitor refresh rate
                    vsync = (displayMode.refresh_rate == configured_fps) && SDL_GL_GetSwapInterval() && (config.video.vsync == 1);
                    std::cout << "INFO: ";
                    std::cout << "Display reports refresh rate is " << displayMode.refresh_rate << "Hz.";
                    if (vsync)
                        std::cout << " VSync enabled.\n";
                    else
                        std::cout << " VSync disabled.\n";
                }
            }
        }
    }
    // Signal the worker threads to quit.
    if (using_threading) {
        running.store(false);
        if (prepare_threads)   { prepareReady.release(); t0.join(); }
                                 renderReady0.release(); t1.join();
        if (render_threads==2) { renderReady1.release(); t2.join(); }
    }

    // Stop audio
    audio.stop_audio();

    // we're done
    printf("\n");
}


// Very (very) simple command line parser.
// Returns true if everything is ok to proceed with launching the game engine.
static bool parse_command_line(int argc, char* argv[]) {
    bool fps_set = false;
    for (int i = 0; i < argc; i++) {
        if ( (strcmp(argv[i], "-list-sound-devices") == 0) ||
                  (strcmp(argv[i], "-list-audio-devices") == 0) ) {
            // list out SDL audio devices available:
            cannonball::audio.start_audio(true);
            _Exit(0);
        }
        else if (strcmp(argv[i], "-cfgfile") == 0 && i+1 < argc) {
            config.set_config_file(argv[i+1]);
        }
        else if (strcmp(argv[i], "-file") == 0 && i+1 < argc) {
            if (!trackloader.set_layout_track(argv[i+1]))
                return false;
        }
        else if (strcmp(argv[i], "-30") == 0) {
            cannonball::fps_lock = 30;
            std::cout << "Game set to 30fps. Automatic frame-rate selection disabled.\n";
            fps_set = true;
        }
        else if (strcmp(argv[i], "-60") == 0) {
            cannonball::fps_lock = 60;
            std::cout << "Game set to 60fps. Automatic frame-rate selection disabled.\n";
            fps_set = true;
        }
        else if (strcmp(argv[i], "-t") == 0 && i+1 < argc) {
            std::string arg = argv[i + 1];
            if (arg.size() == 1 && arg[0] >= '1' && arg[0] <= '4') {
                cannonball::game_threads = arg[0] - '0';  // convert char to int
                std::cout << "Game will use " << cannonball::game_threads << " threads.\n";
            } else {
                std::cerr << "-t: specified threads must be between 1 and 4.\n";
            }
        }
        else if (strcmp(argv[i], "-x") == 0) {
            cannonball::singlecore_detect = false;
            std::cout << "Single-core Pi detection disabled.\n";
        }
        else if (strcmp(argv[i], "-1") == 0) {
            cannonball::singlecore_mode = true;
            std::cout << "Using single-core mode.\n";
        }
        else if (strcmp(argv[i], "-perftest") == 0) {
            cannonball::perftest = true;
            std::cout << "Running in performance test mode.\n";
        }
        else if (   (strcmp(argv[i], "-help") == 0)  ||
                    (strcmp(argv[i], "--help") == 0) ||
                    (strcmp(argv[i], "-h") == 0)     ||
                    (strcmp(argv[i], "--h") == 0)    ||
                    (strcmp(argv[i], "-?") == 0) )
            {
            std::cout << "Command Line Options:\n\n" <<
                         "-cfgfile             : Location and name of config.xml\n" <<
                         "-file                : LayOut Editor track data to load\n" <<
                         "-list-audio-devices  : Lists available playback devices then quit\n" <<
                         "-30                  : Lock to 30fps\n" <<
                         "-60                  : Lock to 60fps\n" <<
                         "-t x                 : Number of game threads (1-4)\n" <<
                         "-x                   : Disable single-core RaspberryPi board detection\n" <<
                         "-1                   : Use single-core mode\n" <<
                         "-perftest            : Assess max frame rate possible on this platform\n\n" <<
                         "CannonBall-SE man page is in the res folder. Open it with 'man -l docs/cannonball-se.6'" << std::endl;
            _Exit(0);
        }
    }
    if (!fps_set)
        std::cout << "Automatic frame-rate selection enabled.\n";
  
    return true;
}


int main(int argc, char* argv[]) {
#ifdef __linux__
    install_segv_handler();
#endif
    std::cout << "CannonBall-SE " << CANNONBALL_SE_VERSION << "\n";
    std::cout << "  An enhanced build of the SEGA Outrun engine by Chris White (https://github.com/djyt/cannonball)\n";
    std::cout << "  CannonBall-SE is Copyright (c) 2025, James Pearce (https://github.com/J1mbo/cannonball)\n";
    std::cout << std::endl;

    // Parse command line arguments (config file location, LayOut data)
    bool ok = parse_command_line(argc, argv);

    if (ok) {
        std::cout << "DEBUG: BEFORE CONFIG LOAD" << std::endl;

        config.load();

        std::cout << "DEBUG: AFTER CONFIG LOAD" << std::endl;
        ok = roms.load_revb_roms(config.sound.fix_samples);

        if (cannonball::singlecore_detect || cannonball::singlecore_mode) {
            if (singleCorePi() || cannonball::singlecore_mode) {
                // we're on a Raspberry Pi with a single-core CPU. Set modes accordingly.
                // user can override these in-game through the menu system
                if (!cannonball::singlecore_mode)
                    std::cout << "Single-core RaspberryPi detected. Setting parameters for optimal performance\n";
                cannonball::game_threads   =  1;        // disabled multi-threading
                config.video.hires         =  0;        // run at original arcade res (half the normal CannonBall-SE res)
                config.video.blargg        =  0;        // disable Blargg NTSC filter
                config.video.shader_mode   =  2;        // full glsl shader (VideoCore IV can handle it easily at 30fps)
                config.video.shadow_mask   =  2;        // glsl shader based overlay (looks better)
                config.video.crt_shape     =  1;        // enable shape overlay
                config.video.noise         =  10;       // as Blargg filter is disabled, add more analogue noise
                config.sound.rate          =  22050;    // 22kHz audio rate
                config.sound.callback_rate =  1;        // 16ms sound callbacks
                if (cannonball::fps_lock == 0) cannonball::fps_lock = 30; // lock to 30fps unless user has overriden
            }
        }
    }
    if (!ok) {
        quit_func(1);
        return 0;
    }

    // Display help text around custom music if none was found
    if (config.sound.custom_tracks_loaded == 0) {
        std::cout << "Custom Music: Put .WAV, .MP3, or .YM files in res/ folder named as:" << std::endl;
        std::cout << "[01-99]_Track_Display_Name.[wav|mp3|ym] - e.g. 04_AHA_Take_On_Me.mp3" << std::endl;
        std::cout << "Indexes 01-03 will replace the built-in tracks (01=Magical Sound Shower), higher indexes add tracks." \
                  << std::endl;
    }

    // Load machine stats e.g. playcount and runtime (defaults to 0 each if file not present)
    config.load_stats();

    // Load gamecontrollerdb.txt mappings
    if (SDL_GameControllerAddMappingsFromFile((config.data.res_path + "gamecontrollerdb.txt").c_str()) == -1)
        std::cout << "Warning: Unable to load game controller mapping file." << std::endl;

    // Initialize timer and video systems
    // On Linux with RPi4 in desktop environment, x11 is default but will cap full-screen to 30fps with vsync enabled.
    // Wayland allows 60fps full-screen.
#ifdef __linux__
    //SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    //#define SDL_HINT_QTWAYLAND_WINDOW_FLAGS "StaysOnTop BypassWindowManager"
    SDL_SetHint(SDL_HINT_QTWAYLAND_WINDOW_FLAGS,
            "StaysOnTop BypassWindowManager");
    if (const char* drv = std::getenv("SDL_VIDEODRIVER"); drv && std::strcmp(drv, "wayland") != 0) {
        std::cout << "\nCannonball requires wayland video driver for 60fps operation under desktop environment. Start cannonball like:" << std::endl;
        std::cout << "$ SDL_VIDEODRIVER=""wayland"" build/cannonball" << std::endl;
    }
#endif
    SDL_SetHint(SDL_HINT_APP_NAME, "Cannonball");
    //SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    if (SDL_Init(   SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                    SDL_INIT_HAPTIC | SDL_INIT_EVENTS) == -1) {
        std::cerr << "SDL Initialization Failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    std::cout << "\nAvailable SDL video drivers: ";
    int ndri = SDL_GetNumVideoDrivers();
    for (int i = 0; i < ndri; ) {
        std::cout << SDL_GetVideoDriver(i);
        if (++i < ndri) std::cout << ", ";
    }
    std::cout << std::endl;

    // Load patched widescreen tilemaps
    if (!omusic.load_widescreen_map(config.data.res_path))
        std::cerr << "Unable to load widescreen tilemaps" << std::endl;

    // Initialize SDL Video
    config.set_fps(config.video.fps);
    if (!video.init(&roms, &config.video))
        quit_func(1);

    cannonball::state = config.menu.enabled ? STATE_INIT_MENU : STATE_INIT_GAME;

    /* Initalize SDL Controls */

    // If a controller has Rumble feature that is supported by SDL, it will be enabled here.
    // If the controller has Rumble not supported by SDL, it can be used anyway via /dev/hidraw.
    // In that case, Input::set_rumble() needs to be amended for the specific controller.
    // Controller Rumble function is controlled in tick() above.

    input.init(config.controls.pad_id,
               config.controls.keyconfig, config.controls.padconfig,
               config.controls.analog,    config.controls.axis, config.controls.invert, config.controls.asettings);

    input.scan_joysticks();
    input.restore_axis_bindings();
    input.sync_bound_axes();

    // Regardless to rumble, if haptic is enabled in config.xml, this is handled via ffeedback.cpp using either
    // DirectX (Windows) or /dev/input/event on Linux. This also includes control of real cabinet hardware via
    // SmartyPi. Therefore, haptic takes priority over simple rumble.

    if (config.controls.haptic)
    {
        config.controls.haptic =
            forcefeedback::init(
                config.controls.max_force,
                config.controls.min_force,
                config.controls.force_duration);

        if (config.controls.haptic)
        {
            forcefeedback::set_gain(
                config.controls.ffb_strength);

            forcefeedback::set_centering_strength(
                config.controls.centering_strength);

            forcefeedback::set_enabled(true);
        }
    }

    // Populate menus
    menu = new Menu();
    menu->populate();

    // start the game threads
#ifdef __linux__
    register_watchdog_signal_handlers();
#endif
    std::thread stats(play_stats_and_watchdog_updater); // Play stats file updater thread

    // Now start the main game loop, which includes SDL video and input
    audio.init();
    main_loop();

    // Wait for threads to finish
    //sound.join();
    stats.join();
    quit_func(0);

    // Never Reached
    return 0;
}
