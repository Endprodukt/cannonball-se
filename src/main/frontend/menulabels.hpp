#pragma once

// ------------------------------------------------------------------------------------------------
// Text Labels for menus
// ------------------------------------------------------------------------------------------------

// Back Labels
const static char* ENTRY_BACK = "BACK";

// Main Menu
const static char* ENTRY_PLAYGAME           = "PLAY GAME";
const static char* ENTRY_GAMEMODES          = "GAME MODES";
const static char* ENTRY_SETTINGS           = "SETTINGS";
const static char* ENTRY_ABOUT              = "ABOUT";
const static char* ENTRY_EXIT               = "EXIT";

// Main Menu (cabinet)
const static char* ENTRY_DIPS               = "DIP SWITCHES";
const static char* ENTRY_CABTESTS           = "HARDWARE TESTS";
const static char* ENTRY_EXSETTINGS         = "EXTRA SETTINGS";

// Game Modes Menu
const static char* ENTRY_ENHANCED           = "SET ENHANCED MODE AND PLAY NOW";
const static char* ENTRY_ORIGINAL           = "SET ORIGINAL MODE AND PLAY NOW";
const static char* ENTRY_CONT               = "PLAY NOW IN CONTINUOUS MODE";
const static char* ENTRY_TIMETRIAL          = "PLAY NOW IN TIME TRIAL MODE";

// Time Trial Menu
const static char* ENTRY_START              = "START TIME TRIAL";
const static char* ENTRY_LAPS               = "NO OF LAPS ";

// Continuous Menu
const static char* ENTRY_START_CONT         = "START CONTINUOUS MODE";

// Settings Menu
const static char* ENTRY_VIDEO              = "VIDEO";
const static char* ENTRY_SOUND              = "SOUND";
const static char* ENTRY_CONTROLS           = "CONTROLS";
const static char* ENTRY_ENGINE             = "GAME ENGINE";
const static char* ENTRY_SCORES             = "CLEAR HISCORES";
const static char* ENTRY_ENHANCE            = "ENHANCEMENTS";
const static char* ENTRY_MASTER_BREAK       = "MASTER BREAK KEY ";
const static char* ENTRY_SAVE               = "SAVE AND RETURN";

// SMARTYPI Extra Settings
const static char* ENTRY_S_CAB              = "CABINET TYPE ";
const static char* ENTRY_FREEPLAY           = "FREEPLAY ";
const static char* ENTRY_S_MOTOR            = "MOTOR TEST";
const static char* ENTRY_S_INPUTS           = "INPUT TEST";
const static char* ENTRY_S_OUTPUTS          = "OUTPUT TEST";
const static char* ENTRY_S_CRT              = "CRT TEST";
const static char* ENTRY_TIMER              = "TIMING FIXES ";
const static char* ENTRY_S_BUGS             = "BUG FIXES ";

// Video Menu
const static char* ENTRY_FPS_COUNTER        = "FPS COUNTER ";
const static char* ENTRY_FULLSCREEN         = "FULL SCREEN ";   // no longer in menu but available in config.xml
const static char* ENTRY_SCREENMODE         = "SCREEN MODE ";   // no longer in menu but available in config.xml
const static char* ENTRY_WIDESCREEN			= "ASPECT RATIO ";
const static char* ENTRY_SCALE              = "WINDOW SCALE ";  // no longer in menu but available in config.xml
const static char* ENTRY_X_OFFSET           = "X POSITION ";    // JJP - allows user to shift image left or right
const static char* ENTRY_Y_OFFSET           = "Y POSITION ";    // JJP - allows user to shift image up or down
const static char* ENTRY_CRT_SHADER1        = "VIDEO EMULATION... "; // JJP - Submenu for the CRT mask overlays
const static char* ENTRY_BLARGG_FILTER      = "BLARGG CRT FILTER... "; // JJP - Submenu for the Blargg CRT filter settings

// JJP - CRT Shader Menu
const static char* ENTRY_CRT_SHADER_MODE    = "SHADER MODE ";
const static char* ENTRY_CRT_SHAPE_SETTINGS = "CRT SHAPE... ";
const static char* ENTRY_MASK_SETTINGS      = "MASK SETTINGS...";
const static char* ENTRY_CRT_SHADER2        = "NOISE AND DESATURATION...";

// JJP - CRT Shape Settings...
const static char* ENTRY_CRT_SHAPE          = "CRT SHAPE ";
const static char* ENTRY_VIGNETTE           = "VIGNETTE ";
const static char* ENTRY_WARPX              = "WARP X ";
const static char* ENTRY_WARPY              = "WARP Y ";

// JJP - CRT Shader Mask Settings...
const static char* ENTRY_SHADOW_MASK        = "SHADOW MASK ";
const static char* ENTRY_MASK_DIM           = "MASK DIM LEVEL ";
const static char* ENTRY_MASK_BOOST         = "MASK BOOST LEVEL ";
const static char* ENTRY_MASK_SIZE          = "MASK SIZE ";
const static char* ENTRY_SCANLINES          = "SCANLINE INTENSITY ";

// JJP - CRT Shader More...
const static char* ENTRY_NOISE              = "NOISE ";
const static char* ENTRY_DESATURATE         = "DESATURATION ";
const static char* ENTRY_DESATURATE_EDGES   = "EDGE DESATURATION ";
const static char* ENTRY_BRIGHTNESS_BOOST   = "BRIGHT. BOOST ";

// JJP - Blargg CRT filter sub-menu
const static char* ENTRY_BLARGG             = "BLARGG NTSC FILTER ";
const static char* ENTRY_SATURATION         = "SATURATION ";
const static char* ENTRY_CONTRAST           = "CONTRAST ";
const static char* ENTRY_BRIGHTNESS         = "BRIGHTNESS ";
const static char* ENTRY_SHARPNESS          = "SHARPNESS ";
const static char* ENTRY_RESOLUTION         = "RESOLUTION ";
const static char* ENTRY_GAMMA              = "GAMMA ";
const static char* ENTRY_HUE                = "HUE ";

// Sound Menu
const static char* ENTRY_MUTE               = "SOUND ";
const static char* ENTRY_BGM                = "BGM VOL ";
const static char* ENTRY_SFX                = "SFX VOL ";
const static char* ENTRY_ADVERTISE          = "ATTRACT SOUND ";
const static char* ENTRY_PREVIEWSND         = "PREVIEW MUSIC ";
const static char* ENTRY_FIXSAMPLES         = "FIX SAMPLES ";
const static char* ENTRY_MUSICTEST          = "MUSIC... ";

// Controls Menu
const static char* ENTRY_GEAR               = "GEAR ";
const static char* ENTRY_CONFIGUREGP        = "CONFIGURE GAMEPAD";
const static char* ENTRY_REDEFKEY           = "REDEFINE KEYS";
const static char* ENTRY_DSTEER             = "DIGITAL STEER SPEED ";
const static char* ENTRY_DPEDAL             = "DIGITAL PEDAL SPEED ";

// GamePad Menu
const static char* ENTRY_ANALOG             = "ANALOG ";
const static char* ENTRY_INVERT_ACCEL       = "INVERT GAS ";
const static char* ENTRY_INVERT_BRAKE       = "INVERT BRAKE ";
const static char* ENTRY_FFB = "FORCE FEEDBACK ";
const static char* ENTRY_FFB_STRENGTH = "FFB STRENGTH ";
const static char* ENTRY_CENTERING_STRENGTH = "CENTERING STRENGTH ";
const static char* ENTRY_RUMBLE = "RUMBLE STRENGTH ";
const static char* ENTRY_REDEFJOY           = "CONFIGURE CONTROLS";

// Game Engine Menu
const static char* ENTRY_TRACKS             = "TRACKS ";
const static char* ENTRY_TIME               = "TIME ";
const static char* ENTRY_TRAFFIC            = "TRAFFIC ";
const static char* ENTRY_SUB_ENHANCEMENTS   = "ENHANCEMENTS";
const static char* ENTRY_SUB_HANDLING       = "CAR SETUP";

// Game Engine: Enhancements Sub-Menu
const static char* ENTRY_HIRES              = "GAME ENGINE RESOLUTION ";
const static char* ENTRY_SPRITERES          = "SPRITE RESOLUTION ";
const static char* ENTRY_ATTRACT            = "NEW ATTRACT ";
const static char* ENTRY_PROTOTYPE          = "PROTOTYPE STAGE 1 ";
const static char* ENTRY_OBJECTS            = "OBJECTS ";

// Game Engine: Car Setup Sub-Menu
const static char* ENTRY_GRIP               = "GRIPPY TYRES ";
const static char* ENTRY_OFFROAD            = "OFFROAD TYRES ";
const static char* ENTRY_BUMPER             = "STRONG BUMPER ";
const static char* ENTRY_TURBO              = "FASTER CAR ";
const static char* ENTRY_COLOR              = "COLOR ";

// Music Test Menu
const static char* ENTRY_MUSIC1             = "PLAY TRACK";
const static char* ENTRY_MUSIC2             = "TRACK - ";
const static char* ENTRY_MUSIC3             = "LAST WAVE";
const static char* ENTRY_WAVEVOLUME         = "CUSTOM MUSIC PLAYBACK LEVEL "; // JJP .WAV FILE VOLUME ADJUST
const static char* ENTRY_CALLBACK_RATE      = "CALLBACK RATE ";        // JJP - 8ms or 16ms callback rates

const static char* COLOR_LABELS[5] = { "RED", "BLUE", "YELLOW", "GREEN", "CYAN" };
const static char* DIP_DIFFICULTY[4] = { "EASY", "NORMAL", "HARD", "HARDEST" };
const static char* GEAR_LABELS[4] = {"MANUAL", "MANUAL CABINET", "MANUAL 2 BUTTONS", "AUTOMATIC"};
const static char* FPS_LABELS[3] = { "30 FPS", "ORIGINAL", "60 FPS" };
const static char* ANALOG_LABELS[3] = { "OFF", "ON", "ON WHEEL ONLY" };
const static char* VIDEO_LABELS[3] = { "OFF", "ON", "STRETCH" };
const static char* ASPECT_LABELS[3] = { "4-3", "16-9", "21-9" };
const static char* RUMBLE_LABELS[5] = { "OFF", "LOW", "MED", "HIGH", "FULL" };
const static char* CAB_LABELS[3] = { "MOVING", "UPRIGHT", "MINI" };
