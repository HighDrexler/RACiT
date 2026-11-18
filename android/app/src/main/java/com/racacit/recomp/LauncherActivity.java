package com.racacit.recomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Bitmap;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The launcher: a first-run setup guide (your game, controls, a graphics
 * preset), then Play (RecompActivity, in its own process); settings in
 * categories with what each one does, a controls page with a live button
 * tester and the touch layout, mods, saves, help. Built in code in the game's
 * look (Acit) and driven entirely by touch or a controller:
 *   D-pad / left stick  move        Cross / A   select       Circle / B  back
 *   left/right on a setting  change it     L1 / R1  previous/next category
 *   Start  Play (home)     Select  Help
 */
public final class LauncherActivity extends Activity {
    private static final int REQ_COPY = 1, REQ_PLACE = 2, REQ_MOD = 3, REQ_SAVE_OUT = 4, REQ_SAVE_IN = 5, REQ_REPAIR = 6, REQ_EBOOT = 7,
            REQ_ISO = 8, REQ_DKEY = 9, REQ_MIGRATION = 10, REQ_UPDATE = 11, REQ_INSTALL_PERMISSION = 12;

    private LauncherMigration.Preview pendingMigration;
    private OfflineUpdate.Candidate pendingUpdate;
    private Companion companionPreview;

    private Store store;
    private FrameLayout root;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean cancel;
    /* A copy into the app is running (static: it outlives a recreated launcher in
     * this process). Its screen has no way out but Cancel: Start, Back and a
     * return to the launcher leave it alone, and a leftover PS3_GAME.incoming is
     * only cleared while none runs. */
    private static volatile boolean busy;
    private long lastPlay;                 /* a second Play within 3 s is the same press */
    private Runnable back;                 /* B / Back on this screen; null on the home screen */
    private Runnable prevTab, nextTab;     /* L1 / R1 on this screen */
    private TextView tester;               /* the controls page's live readout */
    private int guideStep = -1;            /* the setup guide's step while it is open */
    private Acit.Background bg;
    private ImageView homeIcon;
    private String screenKey;              /* the screen on show: the same one rebuilt keeps its focus, no slide-in */
    private LinearLayout column;           /* its column of buttons */
    private int keepFocus = -1;            /* the column row to focus after a rebuild */
    private FrameLayout modal;             /* an open dialog, over the screen */
    private Runnable modalBack, modalPrev, modalNext;   /* the screen's keys while it is open */
    private EbootFile.Candidate pendingEboot;
    private Uri pendingIso;
    /* The screen to rebuild when a controller connects or disconnects (home and controls show them); null elsewhere. */
    private Runnable padScreen;
    private InputManager.InputDeviceListener padListener;
    private final Runnable padRefresh = () -> { if (padScreen != null && modal == null && !gone()) padScreen.run(); };

    /* The screen on show, rebuilt when the window changes size class (a foldable opened, split screen, another
     * display): set by each screen that can be rebuilt as it is; null on progress screens (a copy running). */
    private Runnable redraw;
    private boolean builtWide;
    private int builtPad;

    /* Secondary text in the launcher: brighter than the companion's (DIM). */
    private static final int DIM = Acit.LTEXT_DIM, OFF = Acit.LTEXT_OFF;

    /* ---- settings: what they are and what they do ---------------------------- */

    static final class Def {
        final String cat, key, label, def, help; final String[] values, labels;
        Def(String cat, String key, String label, String def, String help, String... choices) {
            this.cat = cat; this.key = key; this.label = label; this.def = def; this.help = help;
            values = new String[choices.length]; labels = new String[choices.length];
            for (int i = 0; i < choices.length; i++) {
                int c = choices[i].indexOf(':');
                values[i] = choices[i].substring(0, c); labels[i] = choices[i].substring(c + 1);
            }
        }
        String labelOf(String v) { for (int i = 0; i < values.length; i++) if (values[i].equals(v)) return labels[i]; return v; }
        int indexOf(String v) { for (int i = 0; i < values.length; i++) if (values[i].equals(v)) return i; return Math.max(0, indexOfDef()); }
        private int indexOfDef() { for (int i = 0; i < values.length; i++) if (values[i].equals(def)) return i; return 0; }
    }

    static final String[] CATS = { "Graphics", "Display", "Performance", "Audio", "Memory", "Controls", "DualSense", "Gameplay", "Dual screen" };

    /** The categories this device shows: Dual screen only with a second display (the AYN Thor's lower screen). */
    private String[] cats() {
        if (secondDisplay()) return CATS;
        List<String> out = new ArrayList<>();
        for (String c : CATS) if (!c.equals("Dual screen")) out.add(c);
        return out.toArray(new String[0]);
    }

    static final Def[] SETTINGS = {
        /* Graphics, Display and Performance are the Windows build's in-game Settings menu
         * (rsx_host_ui.c): the same keys, values and wording. */
        new Def("Graphics", "RACACIT_RES_SCALE", "Internal resolution", "1",
                "The resolution the game draws at. 100% is the PS3's own 960x704. Lower is lighter on the GPU "
                + "(phones, long sessions); higher is sharper but much heavier. Takes effect the next time the game starts.",
                "0.5:50%  (480x352)", "0.75:75%  (720x528)", "1:100%  (960x704, PS3)", "1.5:150%  (1440x1056)",
                "2:200%  (1920x1408)", "3:300%  (2880x2112)", "4:400%  (3840x2816)"),
        new Def("Graphics", "RACACIT_UPSCALE", "Upscaling", "bilinear",
                "How the picture is scaled to your screen. Sharp is bicubic with a sharpening pass (a little GPU time); "
                + "Nearest keeps hard pixel edges.",
                "bilinear:Bilinear", "sharp:Sharp (bicubic + sharpen)", "nearest:Nearest (pixelated)"),
        new Def("Graphics", "RACACIT_SHARPNESS", "Sharpness", "5",
                "How strong the Sharp upscaler is. Only used with Upscaling: Sharp.",
                "0:0%", "1:10%", "2:20%", "3:30%", "4:40%", "5:50%", "6:60%", "7:70%", "8:80%", "9:90%", "10:100%"),
        new Def("Graphics", "RACACIT_BRIGHTNESS", "Brightness", "0",
                "Brightens or darkens the whole picture, for a dim or very bright screen. Default is the game's own.",
                "-5:-5", "-4:-4", "-3:-3", "-2:-2", "-1:-1", "0:Default", "1:+1", "2:+2", "3:+3", "4:+4", "5:+5"),
        new Def("Graphics", "RACACIT_TEX_QUALITY", "Texture quality", "high",
                "High is every texture at full detail, as on the PS3. Medium and Low leave out the largest texture "
                + "levels: less memory and bandwidth, a softer look. Takes effect the next time the game starts.",
                "high:High (original)", "medium:Medium", "low:Low"),
        new Def("Graphics", "RACACIT_ANISO", "Anisotropic filtering", "0",
                "Keeps floors and walls sharp at an angle. Costs a little GPU time; 4x is a good middle.",
                "0:Off", "2:2x", "4:4x", "8:8x", "16:16x"),
        new Def("Graphics", "RACACIT_TRILINEAR", "Trilinear filtering", "0",
                "Smooths the visible step between texture detail levels in the distance. Cheap.",
                "0:Off", "1:On"),
        new Def("Graphics", "RACACIT_SHADOW_RES", "Shadow resolution", "",
                "Shadow-map detail relative to the scene resolution. Higher values cost GPU time and memory. "
                + "Automatic follows the native GPU default. Changes apply on the next launch.",
                ":Automatic (GPU default)", "1:1x (scene resolution)", "1.5:1.5x", "2:2x", "3:3x", "4:4x"),
        new Def("Graphics", "RACACIT_SHADOW_MB", "Enhanced shadow budget", "",
                "Memory budget for enhanced small shadow maps. When exhausted, additional maps use the scene's resolution. "
                + "This changes shadow sharpness, not gameplay. Applies on the next launch.",
                ":Native default", "32:32 MB", "64:64 MB", "96:96 MB", "128:128 MB", "256:256 MB", "512:512 MB"),
        new Def("Display", "RACACIT_PRESENT_MODE", "VSync", "mailbox",
                "Off (no tearing) shows each frame as soon as it is ready without tearing (recommended). On waits for "
                + "the screen: never tears, a frame more delay. Off (allow tearing) has the least delay.",
                "mailbox:Off (no tearing)", "fifo:On", "immediate:Off (allow tearing)"),
        new Def("Display", "RACACIT_ASPECT", "Aspect ratio", "keep",
                "16:9 shows the game as designed, with bars on a wider phone screen. Stretch fills the screen "
                + "and distorts the picture.",
                "keep:16:9", "stretch:Stretch to the screen"),
        new Def("Display", "RACACIT_REFRESH", "Screen refresh", "max",
                "Asks for the screen's highest refresh rate while the game runs (120 Hz on the AYN Thor). At 60 Hz a "
                + "frame that is a little late stays on screen twice as long; at 120 Hz it shows after 25 ms instead "
                + "of 33, so dips below 60 look smooth instead of stuttering. Android still keeps it within the "
                + "system's refresh rate setting (Settings > Display). Takes effect the next time the game starts.",
                "max:Highest the screen allows", "system:As the system sets it"),
        new Def("Display", "RACACIT_FPS", "FPS counter", "0",
                "Shows the frame rate in the top-left corner while you play.",
                "0:Off", "1:On"),
        new Def("Performance", "RACACIT_PRESENT_PACE", "Frame pacing", "1",
                "Spreads frames evenly so motion looks steady at 60 Hz. Leave on unless you are testing.",
                "1:On (even 60 Hz)", "0:Off"),
        new Def("Performance", "RACACIT_CPU_MODE", "CPU usage", "efficient",
                "Maximum keeps the game's SPU worker threads spinning between jobs instead of resting: slightly faster "
                + "on a desktop, but on a phone it mostly makes heat, and heat lowers the clocks. Efficient is recommended.",
                "efficient:Efficient", "max:Maximum (spin on all cores)"),
        new Def("Performance", "RACACIT_GPU_SUBMIT_DRAWS", "GPU work hand-off", "1024",
                "How often the game hands recorded drawing to the GPU during a frame. Often lets the GPU work while the CPU "
                + "is still recording the rest of the frame (a phone GPU needs that); once a frame is the old behaviour, "
                + "for comparing.",
                "512:Every 512 draws", "1024:Every 1024 draws (default)", "2048:Every 2048 draws", "0:Once a frame (old)"),
        new Def("Performance", "RACACIT_CORE_SWAP", "Core swap", "1",
                "While the game waits on an SPU job, lends the fastest core to the job and waits on a small core. "
                + "Measured on the AYN Thor: 2-4 ms a frame less waiting, a few frames a second more in busy places. "
                + "Takes effect the next time the game starts.",
                "1:On", "0:Off"),
        new Def("Performance", "RACACIT_PERF_HINTS", "Performance hints", "1",
                "Tells Android how long each frame takes, so it raises the CPU clocks when the game needs them instead "
                + "of guessing. Android 13 and later; does nothing on older versions.",
                "1:On", "0:Off"),
        new Def("Performance", "RACACIT_SUSTAINED", "Sustained performance", "0",
                "Holds the clocks the device can keep up for a whole session, instead of peaking and then throttling "
                + "when it gets hot. Steadier but a lower peak. Only on devices that support it.",
                "0:Off (full speed)", "1:On (cooler, steadier)"),
        new Def("Performance", "RACACIT_COLL_FIBER", "Collision on the main thread", "0",
                "Runs the game's collision checks on the main thread while it waits for them, instead of handing each one "
                + "to a worker thread on a busy core. Measured on the AYN Thor: about 1 fps more in heavy fights. "
                + "Experimental. Takes effect the next time the game starts.",
                "0:Off", "1:On (experimental)"),
        new Def("Audio", "RACACIT_AAUDIO", "Audio output", "1",
                "AAudio is Android's own low-latency audio path: the game's sound plays without gaps on the AYN Thor. "
                + "If you hear no sound or crackling on your device, try Compatibility (OpenSL ES, through SDL). "
                + "Takes effect the next time the game starts.",
                "1:AAudio (recommended)", "0:Compatibility (OpenSL ES)"),
        new Def("Memory", "RSX_ENG_TEX_MB", "Texture cache target", "",
                "A soft target for cached textures, not a total memory limit. Lower targets may cause more uploads. "
                + "The native default can follow GPU memory. Changes apply on the next launch.",
                ":Native default", "128:128 MB", "256:256 MB", "384:384 MB", "512:512 MB", "640:640 MB", "1024:1024 MB", "2048:2048 MB"),
        new Def("Memory", "RSX_ENG_RT_MB", "Color-target cache", "",
                "Soft base target for cached color render targets. Multiplied by scene scale above 1x. "
                + "Live rendering resources may exceed it. Lower values can increase GPU allocation work. Next launch.",
                ":Native default", "256:256 MB", "384:384 MB", "512:512 MB", "768:768 MB", "1024:1024 MB", "1536:1536 MB"),
        new Def("Memory", "RSX_ENG_Z_MB", "Depth-target cache", "",
                "Soft base target for cached depth and shadow targets. Multiplied by scene scale above 1x. "
                + "Live rendering resources may exceed it. Verify demanding scenes after reducing this target. Next launch.",
                ":Native default", "256:256 MB", "384:384 MB", "512:512 MB", "768:768 MB", "1024:1024 MB", "1536:1536 MB"),
        new Def("Memory", "RSX_VK_RECYCLE_MB", "Released-image pool", "",
                "Retain only already-released Vulkan images for reuse. Lower limits free memory sooner, but may increase "
                + "allocation overhead. Zero disables this pool. Next launch.",
                ":Native default", "0:Disabled", "32:32 MB", "64:64 MB", "128:128 MB", "256:256 MB"),
        new Def("Controls", "RACACIT_DS_SWAP", "Fire button", "1",
                "R2 fires and R1 takes the PS3 R2 action. L2 strafes; L1 keeps the PS3 look action. "
                + "Quick Select and hoverboots follow the v97 context controls. R1: the PS3's own layout. The touch controls "
                + "keep the PS3 layout. Also in the game's Settings menu (Controls).",
                "1:R2 (Rift Apart layout)", "0:R1 (PS3 layout)"),
        new Def("Controls", "RACACIT_DS_RUMBLE", "Rumble", "100",
                "How strongly the controller vibrates, on controllers that can.",
                "0:Off", "30:30%", "50:50%", "70:70%", "100:100%"),
        new Def("Controls", "RACACIT_PROMPT_SWAP", "Button prompts", "1",
                "The game's on-screen button prompts follow your Fire button choice (R2 to fire shows R2). "
                + "Off shows the PS3's original prompts.",
                "1:Match the Fire button", "0:PS3 originals"),
        new Def("Controls", "RACACIT_KBM", "Keyboard + mouse", "1",
                "Bluetooth or USB keyboard and mouse use the v97 bindings. Click the game view to capture the mouse. "
                + "F1 releases it and opens Settings. After changing apps or losing focus, click the game view to capture again.",
                "1:On", "0:Off"),
        new Def("Controls", "RACACIT_MOUSE_SENS", "Mouse sensitivity", "1",
                "Camera speed for a captured mouse. The in-game Settings menu allows finer adjustment.",
                "0.1:10%", "0.25:25%", "0.5:50%", "0.75:75%", "1:100%", "1.5:150%", "2:200%", "3:300%", "4:400%", "5:500%"),
        new Def("Controls", "RACACIT_MOUSE_INVERT_Y", "Invert mouse camera Y", "0",
                "Reverse vertical mouse movement while the game captures the mouse.", "0:Off", "1:On"),
        new Def("Controls", "RACACIT_TOUCH", "On-screen controls", "1",
                "A PS3 pad drawn over the game for playing by touch: sticks, face buttons, d-pad, L1-L3, R1-R3, "
                + "Select and Start. Auto hides them while a controller is connected; Always keeps them on screen.",
                "1:Auto (hide with a controller)", "2:Always on", "0:Off"),
        new Def("Updates", "RACIT_AUTO_UPDATE", "Check for updates", "1",
                "When the launcher opens, look for a newer Android release on the project's GitHub page and offer to download it. "
                + "Nothing is downloaded or installed without your OK, and Android asks again before installing.",
                "1:On", "0:Off"),
        new Def("Controls", "RACACIT_TOUCH_FIRE", "Fire and aim button", "1",
                "A crosshair button by your right thumb: hold it to fire, and slide the same thumb to turn the camera while you shoot.",
                "1:Shown", "0:Hidden"),
        new Def("Controls", "RACACIT_TOUCH_DPAD", "D-pad", "1",
                "The on-screen d-pad (weapon and gadget shortcuts).", "1:Shown", "0:Hidden"),
        new Def("Controls", "RACACIT_TOUCH_SHOULDERS", "Shoulder buttons", "1",
                "L1, L2, R1 and R2 on the screen's edges.", "1:Shown", "0:Hidden"),
        new Def("Controls", "RACACIT_TOUCH_CLICKS", "Stick clicks", "1",
                "L3 and R3, under the shoulder buttons.", "1:Shown", "0:Hidden"),
        new Def("Controls", "RACACIT_TOUCH_MENU", "Select, Start and menu", "1",
                "The three small buttons along the top. Hidden, the controller's or the device's own buttons open the menus.",
                "1:Shown", "0:Hidden"),
        new Def("Controls", "RACACIT_TOUCH_INSET", "Move toward the middle", "0",
                "Moves the left and right button groups in from the screen's edges, for a better reach on a wide screen.",
                "0:At the edges", "4:A little", "8:Halfway", "12:Far in"),
        new Def("Controls", "RACACIT_TOUCH_RAISE", "Raise the lower buttons", "0",
                "Lifts the face buttons, d-pad and stick up from the bottom of the screen.",
                "0:At the bottom", "5:A little", "10:Higher", "15:Highest"),
        new Def("Controls", "RACACIT_TOUCH_SIZE", "Touch button size", "100",
                "Makes the on-screen buttons and their touch areas bigger or smaller. The preview on the Controls page shows the result.",
                "70:Small", "85:Medium-small", "100:Medium", "115:Large", "130:Larger", "145:Largest"),
        new Def("Controls", "RACACIT_TOUCH_OPACITY", "Touch button opacity", "100",
                "How visible the on-screen buttons are over the game.",
                "30:Faint", "50:Light", "70:Medium", "100:Full"),
        new Def("Controls", "RACACIT_TOUCH_CAM_SENS", "Touch camera speed", "100",
                "How far the camera turns when you drag on the right half of the screen with the on-screen controls.",
                "50:50%", "75:75%", "100:100%", "125:125%", "150:150%", "200:200%", "250:250%", "300:300%"),
        new Def("DualSense", "RACACIT_DUALSENSE_PLUS", "DualSense Plus", "0",
                "Enables the native DualSense extras available through a compatible USB HID connection. "
                + "Android device and controller driver support varies. Speaker and audio haptics require a separate "
                + "compatible USB audio output; that path has not been verified on Android. Standard rumble remains available.",
                "0:Off", "1:On (compatible USB controllers)"),
        new Def("DualSense", "RACACIT_DS_SPEAKER", "Controller speaker", "1",
                "Routes game sounds only if the native audio backend finds the DualSense USB audio output. Android support is unverified.",
                "1:On when available", "0:Off"),
        new Def("DualSense", "RACACIT_DS_SPK_VOL", "Speaker volume", "49",
                "Volume of the controller speaker, when a compatible USB audio output is available.",
                "0:0%", "10:10%", "20:20%", "30:30%", "40:40%", "49:49% (default)", "50:50%", "60:60%", "70:70%", "80:80%", "90:90%", "100:100%"),
        new Def("DualSense", "RACACIT_DS_HAPTICS", "Audio haptics", "1",
                "Uses the controller's actuator audio channels when a compatible four-channel USB output is available. Android support is unverified.",
                "1:On when available", "0:Off (standard rumble)"),
        new Def("DualSense", "RACACIT_DS_HAPTIC_VOL", "Haptic strength", "50",
                "Strength of audio haptics when the controller audio path is available.",
                "0:0%", "10:10%", "20:20%", "30:30%", "40:40%", "50:50%", "60:60%", "70:70%", "80:80%", "90:90%", "100:100%"),
        new Def("DualSense", "RACACIT_DS_TRIGGERS", "Adaptive triggers", "1",
                "Trigger resistance with DualSense Plus on, when the USB HID driver supports output reports.",
                "1:On", "0:Off"),
        new Def("DualSense", "RACACIT_DS_TRIG_STR", "Trigger strength", "100",
                "Strength of supported DualSense trigger effects.",
                "10:10%", "20:20%", "30:30%", "40:40%", "50:50%", "60:60%", "70:70%", "80:80%", "90:90%", "100:100%"),
        new Def("DualSense", "RACACIT_DS_LIGHT", "Light bar", "1",
                "Game light-bar effects with DualSense Plus on and a compatible USB HID driver.",
                "1:On", "0:Off"),
        new Def("DualSense", "RACACIT_DS_MOTION", "Motion sensors", "1",
                "Pass controller motion to the game's Sixaxis input when the driver exposes the sensors.",
                "1:On", "0:Off"),
        new Def("Gameplay", "RACACIT_LANGUAGE", "Language", "",
                "The disc's English, French, Spanish, German, Italian or Dutch text and speech. Applies when the game starts.",
                ":English", "fr:Fran\u00e7ais (French)", "es:Espa\u00f1ol (Spanish)", "de:Deutsch (German)",
                "it:Italiano (Italian)", "nl:Nederlands (Dutch)"),
        new Def("Gameplay", "RACACIT_FOV", "Field of view", "100",
                "How wide the camera sees, in percent of the game's own. Wider shows more around Ratchet. "
                + "Also in the game's Settings menu (Camera), where it applies at once.",
                "75:75%", "80:80%", "85:85%", "90:90%", "95:95%", "100:100% (as shipped)", "105:105%", "110:110%",
                "115:115%", "120:120%", "125:125%", "130:130%", "140:140%", "150:150%"),
        new Def("Gameplay", "RACACIT_CAM_DIST", "Camera distance", "100",
                "How far behind Ratchet the camera follows, in percent of the game's own. "
                + "Also in the game's Settings menu (Camera).",
                "50:50%", "60:60%", "70:70%", "80:80%", "90:90%", "100:100% (as shipped)", "110:110%", "120:120%",
                "130:130%", "140:140%", "150:150%", "175:175%", "200:200%"),
        new Def("Gameplay", "RACACIT_TOD_DISCOUNT", "Tools of Destruction discount", "1",
                "The weapon vendor discount a PS3 gives with Tools of Destruction installed. Applies to each save as it loads.",
                "1:On", "0:Off"),
        new Def("Gameplay", "RACACIT_POWER_PACK", "Power pack (pre-order bonus)", "1",
                "The pre-order Power pack: extra health, the CryoMine Glove early and an armor variant. Off takes the health back from a save.",
                "1:On", "0:Off"),
        new Def("Gameplay", "RACACIT_CHEATS", "Cheat menu", "0",
                "Puts a Cheats page in the in-game Settings menu, where Trophies is: bolts, weapon levels, god mode, teleport. "
                + "While it is on, no skill points or trophies are earned; a save a cheat was used in earns none from then on, "
                + "even with the menu off (an older copy of the save from before still does).",
                "0:Off", "1:On"),
        new Def("Gameplay", "RACACIT_SAVE_UI", "Save list screen", "1",
                "Show the game's save-slot list for loading, saving and deleting saves.",
                "1:On", "0:Off"),
        new Def("Gameplay", "RACACIT_SKIP_INTRO", "Logos and notices at boot", "0",
                "Skip goes straight to PRESS START. Show keeps the game's own cards, the health and seizure warning included.",
                "0:Show", "1:Skip"),
        new Def("Gameplay", "RACACIT_START_SKIPS_MOVIES", "START skips movies", "1",
                "Start skips a playing movie, except the title backdrop and vendor previews.",
                "1:Yes", "0:No"),
        new Def("Gameplay", "RACACIT_ALL_CUTSCENES_SKIPPABLE", "Every cutscene skippable", "1",
                "Lets Start skip any cutscene, not only the ones the game allows.",
                "1:Yes", "0:No"),
        new Def("Gameplay", "RACACIT_SKIP_CLANK", "Skip Clank's sections", "0",
                "Skips the Great Clock puzzle sections. For replays.",
                "0:No", "1:Yes"),
        new Def("Gameplay", "RACACIT_SPACE_FREE_LANDING", "Land anywhere in space", "1",
                "Lets the ship land on any planet or moon in space, not only the ones the game marks.",
                "0:No (as the game)", "1:Yes"),
        new Def("Dual screen", "RACACIT_COMPANION", "Dual-screen companion", "1",
                "Opens automatically on the lower display in dual-screen mode. Drag around the quick-select ring, "
                + "then release to select; the game's upper-screen wheel shows the actual weapons and pages.",
                "1:On", "0:Off"),
        new Def("Dual screen", "RACACIT_COMPANION_PAGE", "Companion opens on", "quick",
                "The companion page shown first. Clock is the Great Clock with the time, the date, how long you have played "
                + "and the battery: a calm page to leave on the lower screen.",
                "quick:Quick Select", "map:Map", "actions:Actions", "status:Status", "clock:Clock"),
        new Def("Dual screen", "RACACIT_COMPANION_DIM", "Dim when not touched", "120",
                "Dims the companion after this long without a touch (the Status page and the clock keep updating, darker). "
                + "A touch wakes the screen without activating a control.",
                "0:Never", "30:After 30 seconds", "60:After 1 minute", "120:After 2 minutes", "300:After 5 minutes"),
        new Def("Dual screen", "RACACIT_PERF_LOG", "Performance recording", "0",
                "Records frame-rate samples, CPU/GPU readings and heat to perf_log.csv while the companion is visible. "
                + "Leave off for normal play; the Status page still shows live readings when opened.",
                "0:Off", "1:On"),
    };

    /* Graphics presets, the Windows build's (rsx_host_ui.c k_presets): name, what it is, key=value pairs. */
    static final String[][] PRESETS = {
        { "Low", "75% resolution, plain filtering, lower texture detail. A conservative starting point for older GPUs and long sessions.",
          "RACACIT_RES_SCALE=0.75", "RACACIT_ANISO=0", "RACACIT_TRILINEAR=0", "RACACIT_TEX_QUALITY=low" },
        { "Medium", "The PS3's resolution with 2x filtering and medium textures. Balanced image quality and memory use.",
          "RACACIT_RES_SCALE=1", "RACACIT_ANISO=2", "RACACIT_TRILINEAR=1", "RACACIT_TEX_QUALITY=medium" },
        { "High", "150% resolution, 4x filtering, full textures. Needs a strong GPU.",
          "RACACIT_RES_SCALE=1.5", "RACACIT_ANISO=4", "RACACIT_TRILINEAR=1", "RACACIT_TEX_QUALITY=high" },
        { "Ultra", "200% resolution, 16x filtering, full textures. The sharpest picture; a very strong GPU only.",
          "RACACIT_RES_SCALE=2", "RACACIT_ANISO=16", "RACACIT_TRILINEAR=1", "RACACIT_TEX_QUALITY=high" },
        { "Original (PS3)", "Exactly the PS3's image: 960x704, plain filtering, every texture level.",
          "RACACIT_RES_SCALE=1", "RACACIT_ANISO=0", "RACACIT_TRILINEAR=0", "RACACIT_TEX_QUALITY=high" },
    };

    // Explicit selections only: never overwrite saved custom budgets when the app starts.
    static final String[][] MEMORY_PRESETS = {
        { "Experimental conservative", "256 MB texture/color/depth targets; 64 MB released-image pool. Untested reductions can cause stalls or rendering errors.",
          "RSX_ENG_TEX_MB=256", "RSX_ENG_RT_MB=256", "RSX_ENG_Z_MB=256", "RSX_VK_RECYCLE_MB=64" },
        { "Experimental balanced", "384 MB texture/color/depth targets; 128 MB released-image pool. Check demanding scenes for rendering errors.",
          "RSX_ENG_TEX_MB=384", "RSX_ENG_RT_MB=384", "RSX_ENG_Z_MB=384", "RSX_VK_RECYCLE_MB=128" },
        { "High", "640 MB texture, 768 MB color/depth targets; 256 MB reuse pool. More retention for devices with memory to spare.",
          "RSX_ENG_TEX_MB=640", "RSX_ENG_RT_MB=768", "RSX_ENG_Z_MB=768", "RSX_VK_RECYCLE_MB=256" },
        { "Native defaults", "Remove these four overrides and let the runtime choose its default targets. Keeps graphics and shadow choices.",
          "RSX_ENG_TEX_MB=", "RSX_ENG_RT_MB=", "RSX_ENG_Z_MB=", "RSX_VK_RECYCLE_MB=" },
    };

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (b != null && b.getString("pendingIso") != null) pendingIso = Uri.parse(b.getString("pendingIso"));
        store = new Store(this);
        new Thread(store::repairFolderModes).start();   /* a save import before .52 left savedata private */
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        root = new FrameLayout(this);
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        final int side = Math.max(dm.widthPixels, dm.heightPixels);
        bg = new Acit.Background(Art.cachedKeyArt());
        root.setBackground(bg);
        root.setOnApplyWindowInsetsListener((v, ins) -> { applySafeInsets(v, ins); return ins; });
        setContentView(root);
        immersive();
        new Thread(() -> {
            // Decode from slow storage after the first interactive frame. A
            // recreated Activity can immediately use the existing cached art.
            if (!Art.present(store) && store.gameStatus().equals("ok")) Art.refresh(store);
            Bitmap a = Art.keyArt(store, side);
            Bitmap icon = Art.icon(store);
            ui.post(() -> {
                if (isDestroyed()) return;
                bg.setArt(a);
                if (homeIcon != null) {
                    homeIcon.setImageBitmap(icon);
                    homeIcon.setVisibility(icon == null ? View.GONE : View.VISIBLE);
                }
            });
        }, "launcher-art").start();
        if (!setupDone()) {
            // A new install starts on what suits the device; the guide then offers the rest.
            if (store.settings().isEmpty()) applyPreset(PRESETS[recommendedPreset()]);
            showGuide(0);
        } else showHome();
        if (!busy) new Thread(() -> Importer.cleanupInterruptedCopy(store.base, () -> busy),
                "launcher-import-cleanup").start();
    }

    /**
     * Pads the launcher clear of a camera cutout and of rounded screen corners, on any phone: from Android 15 an app
     * targeting it is drawn edge to edge, under both. The key art behind still fills the screen.
     */
    private void applySafeInsets(View v, WindowInsets ins) {
        int l = 0, t = 0, r = 0, b = 0;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets c = ins.getInsets(WindowInsets.Type.displayCutout());
            l = c.left; t = c.top; r = c.right; b = c.bottom;
        } else if (Build.VERSION.SDK_INT >= 28 && ins.getDisplayCutout() != null) {
            android.view.DisplayCutout c = ins.getDisplayCutout();
            l = c.getSafeInsetLeft(); t = c.getSafeInsetTop(); r = c.getSafeInsetRight(); b = c.getSafeInsetBottom();
        }
        if (Build.VERSION.SDK_INT >= 31) {
            int rad = 0;
            for (int pos : new int[] { android.view.RoundedCorner.POSITION_TOP_LEFT, android.view.RoundedCorner.POSITION_TOP_RIGHT,
                    android.view.RoundedCorner.POSITION_BOTTOM_LEFT, android.view.RoundedCorner.POSITION_BOTTOM_RIGHT }) {
                android.view.RoundedCorner rc = ins.getRoundedCorner(pos);
                if (rc != null) rad = Math.max(rad, rc.getRadius());
            }
            /* what a corner of that radius cuts off near the page's own padding */
            int side = Math.round(rad * 0.3f), edge = Math.round(rad * 0.15f);
            l = Math.max(l, side); r = Math.max(r, side); t = Math.max(t, edge); b = Math.max(b, edge);
        }
        if (v.getPaddingLeft() != l || v.getPaddingTop() != t || v.getPaddingRight() != r || v.getPaddingBottom() != b) {
            v.setPadding(l, t, r, b);
            ui.post(this::relayoutIfNeeded);
        }
    }

    @Override public void onConfigurationChanged(android.content.res.Configuration c) {
        super.onConfigurationChanged(c);
        ui.post(this::relayoutIfNeeded);
    }

    /** The window's size class or margins changed (and no dialog is open): build the screen again for it. */
    private void relayoutIfNeeded() {
        if (gone() || redraw == null || modal != null) return;
        boolean w = wide();
        if (w != builtWide || Math.abs(sidePad(w ? 20 : 12) - builtPad) > dp(8)) redraw.run();
    }

    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersive(); }

    private void immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private File setupFlag() { return new File(store.base, ".setup_done"); }
    private boolean setupDone() { return setupFlag().exists(); }

    /* ---- building blocks ---------------------------------------------------- */

    private int dp(float v) { return (int) Acit.dp(this, v); }

    private TextView text(String s, float sp, int color, boolean title) {
        TextView t = new TextView(this);
        t.setText(s); t.setTextColor(color); t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTypeface(title ? Acit.titleFace() : Acit.bodyFace());
        if (title) t.setAllCaps(true);
        t.setShadowLayer(dp(2.5f), 0, dp(1), 0xD0000000);   /* readable over any part of the key art */
        return t;
    }

    private Acit.Button button(String label, String sub, Runnable r) {
        Acit.Button b = new Acit.Button(this, label, sub, r);
        b.setLayoutParams(rowParams(sub == null ? 60 : 72));
        return b;
    }

    /** The system font size setting, as a factor for row heights (1 to 1.7). */
    private float fontScale() { return Math.max(1f, Math.min(1.7f, getResources().getConfiguration().fontScale)); }

    /** A size heightDp at the default font size, larger with a larger one. */
    private int rowH(float heightDp) { return dp(heightDp * fontScale()); }

    private LinearLayout.LayoutParams rowParams(int heightDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, rowH(heightDp));
        lp.bottomMargin = dp(5);
        return lp;
    }

    private Bitmap glyph(int cp) { return Glyphs.get(store.glyphs(), cp); }

    /** Room for the list and a detail panel side by side: a phone or the Thor's top screen in landscape (not its lower screen). */
    private boolean wide() {
        return usableWidthDp() / fontScale() >= 640;
    }

    /** The width the launcher has, in dp, inside the cutout and corner padding. */
    private float usableWidthDp() {
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        int w = root != null && root.getWidth() > 0 ? root.getWidth() - root.getPaddingLeft() - root.getPaddingRight() : dm.widthPixels;
        return w / dm.density;
    }

    /** Side padding: at least the base, more on a very wide display so the content stays at most 1280 dp across. */
    private int sidePad(int baseDp) {
        return dp(Math.max(baseDp, (usableWidthDp() - 1280) / 2));
    }

    /**
     * A screen, laid out for a handheld (v113-android launcher rework, 2026-11-16): a slim top bar with a
     * Back button for touch, the title, where you are and the time; then the screen's list (the
     * actions and settings, focused first) and its detail panel (what the focused thing is, status,
     * progress) -- side by side when wide(), else one column with the list first. Controller hints
     * along the bottom only while a controller is connected. A new screen slides in; the same one
     * rebuilt (a setting changed) keeps the focused row. Returns { detail panel, list }.
     */
    private LinearLayout[] screen(String subtitle, String title, Runnable onBack) {
        String key = title + "\n" + subtitle;
        boolean same = key.equals(screenKey) && column != null;
        keepFocus = -1;
        if (same) {
            View f = getCurrentFocus();
            for (int i = 0; f != null && i < column.getChildCount(); i++) if (column.getChildAt(i) == f) keepFocus = i;
        }
        screenKey = key;
        modal = null;
        back = onBack; prevTab = null; nextTab = null; tester = null; padScreen = null; hintBar = null; infoHint = false;
        root.removeAllViews();
        homeIcon = null;
        final boolean wide = wide();
        redraw = null; builtWide = wide; builtPad = sidePad(wide ? 20 : 12);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(sidePad(wide ? 20 : 12), dp(8), sidePad(wide ? 20 : 12), dp(6));

        /* the top bar */
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        if (onBack != null) {
            Acit.Button bk = new Acit.Button(this, "Back", null, onBack);
            bk.setFocusable(false);   /* a controller has B; the D-pad stays in the list */
            LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(rowH(112), rowH(50));
            bl.rightMargin = dp(10);
            head.addView(bk, bl);
        }
        LinearLayout names = new LinearLayout(this);
        names.setOrientation(LinearLayout.VERTICAL);
        TextView t = text(title, 24, Acit.TEXT, true);
        t.setShadowLayer(dp(8), 0, 0, Acit.GLOW);
        t.setSingleLine(true); t.setEllipsize(TextUtils.TruncateAt.END);
        names.addView(t);
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView st = text(subtitle, 14, DIM, false);
            st.setSingleLine(true); st.setEllipsize(TextUtils.TruncateAt.END);
            names.addView(st);
        }
        head.addView(names, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        android.widget.TextClock clock = new android.widget.TextClock(this);
        clock.setFormat24Hour("H:mm"); clock.setFormat12Hour("h:mm a");
        clock.setTextColor(Acit.ORANGE); clock.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18); clock.setTypeface(Acit.titleFace());
        clock.setPadding(dp(10), 0, dp(4), 0);
        head.addView(clock);
        page.addView(head, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        /* the list and the detail panel */
        ScrollView sc = new ScrollView(this);
        sc.setVerticalScrollBarEnabled(false);
        sc.setVerticalFadingEdgeEnabled(true); sc.setFadingEdgeLength(dp(18));
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(0, dp(2), 0, dp(6));
        final LinearLayout info = new LinearLayout(this);
        info.setOrientation(LinearLayout.VERTICAL);
        info.setPadding(dp(18), dp(12), dp(18), dp(12));
        final View infoBox;
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1);
        rp.topMargin = dp(8);
        if (wide) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            sc.addView(col);
            row.addView(sc, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.12f));
            ScrollView isc = new ScrollView(this);
            isc.setVerticalScrollBarEnabled(false);
            isc.setFocusable(false);
            isc.setVerticalFadingEdgeEnabled(true); isc.setFadingEdgeLength(dp(18));
            isc.addView(info);
            isc.setBackground(new Acit.Panel(this));
            LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.88f);
            ip.leftMargin = dp(14);
            row.addView(isc, ip);
            infoBox = isc;
            page.addView(row, rp);
        } else {
            LinearLayout one = new LinearLayout(this);
            one.setOrientation(LinearLayout.VERTICAL);
            one.addView(col);
            info.setBackground(new Acit.Panel(this));
            LinearLayout.LayoutParams ip = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            ip.topMargin = dp(10);
            one.addView(info, ip);
            sc.addView(one);
            infoBox = info;
            page.addView(sc, rp);
        }
        /* an empty detail panel is not drawn (it fills later on some screens: shown again then) */
        infoBox.setVisibility(View.GONE);
        info.setOnHierarchyChangeListener(new ViewGroup.OnHierarchyChangeListener() {
            @Override public void onChildViewAdded(View parent, View child) { infoBox.setVisibility(View.VISIBLE); }
            @Override public void onChildViewRemoved(View parent, View child) { if (info.getChildCount() == 0) infoBox.setVisibility(View.GONE); }
        });
        if (!controllers().isEmpty()) page.addView(hints(onBack != null));
        root.addView(page);
        column = col;
        if (!same) {
            page.setAlpha(0f); page.setTranslationX(dp(18));
            page.animate().alpha(1f).translationX(0).setDuration(170).setInterpolator(new android.view.animation.DecelerateInterpolator()).start();
        }
        return new LinearLayout[] { info, col };
    }

    /** The bottom bar: the version on the left, what the controller buttons do here on the right (the game's own icons). */
    private View hints(boolean canBack) {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        bar.setPadding(dp(2), dp(6), 0, 0);
        TextView ver = text("RACiT " + version() + "  ·  " + Build.MODEL, 12, OFF, false);
        bar.addView(ver, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        hintBar = bar; infoHint = false;
        hint(bar, GlyphCore.CROSS, "A", "Select");
        if (canBack) hint(bar, GlyphCore.CIRCLE, "B", "Back");
        hint(bar, GlyphCore.L1, "L1", null); hint(bar, GlyphCore.R1, "R1", "Category");
        hint(bar, GlyphCore.START, "Start", back == null ? "Play" : null);
        hint(bar, GlyphCore.SELECT, "Select", "Help");
        return bar;
    }

    private void hint(LinearLayout bar, int cp, String fallback, String what) {
        Bitmap g = glyph(cp);
        if (g != null) {
            ImageView iv = new ImageView(this);
            iv.setImageBitmap(g);
            int h = dp(20), w = Math.round(h * (float) g.getWidth() / Math.max(1, g.getHeight()));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(w, h);
            lp.leftMargin = dp(what == null ? 14 : 4);
            bar.addView(iv, lp);
        } else {
            TextView t = text(fallback, 13, Acit.EDGE, true);
            t.setPadding(dp(what == null ? 14 : 4), 0, 0, 0);
            bar.addView(t);
        }
        if (what != null) {
            TextView t = text(what, 14, DIM, false);
            t.setPadding(dp(5), 0, dp(10), 0);
            bar.addView(t);
        }
    }

    /* The hint bar of the screen on show (null without a controller), and whether it names Triangle yet. */
    private LinearLayout hintBar;
    private boolean infoHint;
    /** A screen with settings: Triangle (Y) tells what the focused one does. */
    private void addInfoHint() {
        if (hintBar == null || infoHint) return;
        infoHint = true;
        hint(hintBar, GlyphCore.TRIANGLE, "Y", "About");
    }

    private String version() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception e) { return "?"; }
    }

    private TextView infoLine(LinearLayout info, String label, String value, int color) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.VERTICAL);
        r.setPadding(0, dp(3), 0, dp(8));
        r.addView(text(label, 13, Acit.EDGE, true));
        TextView v = text(value, 16, color, false);
        v.setLineSpacing(0, 1.1f);
        r.addView(v);
        info.addView(r);
        return v;
    }

    /** An info line with a status hexagon beside it: state 1 ready, -1 needs you, 0 neither. Returns the value's TextView. */
    private TextView statusLine(LinearLayout info, int state, String label, String value, int color) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout.LayoutParams mp = new LinearLayout.LayoutParams(dp(14), dp(16));
        mp.topMargin = dp(4); mp.rightMargin = dp(10);
        r.addView(new Acit.Mark(this, state), mp);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        r.addView(body, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        TextView v = infoLine(body, label, value, color);
        info.addView(r);
        return v;
    }

    /** A section heading in the button column: amber capitals over a faint line. */
    private void heading(LinearLayout col, String s) {
        TextView t = text(s, 15, Acit.ORANGE, true);
        t.setLetterSpacing(0.08f);
        t.setPadding(dp(4), dp(col.getChildCount() == 0 ? 2 : 14), 0, dp(6));
        t.setBackground(new Acit.Underline());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(4);
        col.addView(t, lp);
    }

    /** Focus the first row of col, or after a rebuild of the same screen, the row that had it. */
    private void focusFirst(LinearLayout col) {
        if (col == column && keepFocus >= 0 && keepFocus < col.getChildCount() && col.getChildAt(keepFocus).isFocusable()) {
            col.getChildAt(keepFocus).requestFocus(); keepFocus = -1; return;
        }
        for (int i = 0; i < col.getChildCount(); i++)
            if (col.getChildAt(i).isFocusable()) { col.getChildAt(i).requestFocus(); return; }
    }

    private boolean gone() { return isFinishing() || isDestroyed(); }

    private boolean canChangeGameFiles() {
        android.app.ActivityManager manager = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<android.app.ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes) {
            if ((getPackageName() + ":game").equals(process.processName)) {
                toast("Close the game first", "Quit the running game before importing files, changing its program, or transferring saves.");
                return false;
            }
        }
        return true;
    }

    /** A message box; nothing once the launcher is closing (a worker thread may finish after it). */
    private void toast(String title, String msg) { dialog(title, msg, new String[] { "OK" }, new Runnable[] { null }); }

    /**
     * A dialog in the launcher's look over the current screen, driven like the
     * rest: D-pad, A to choose, B to close (the same as the last choice when
     * that is Cancel). More than two choices are a list. Focus stays in it.
     */
    private void dialog(String title, String msg, String[] labels, Runnable[] actions) { dialog(title, msg, labels, actions, 0); }

    /** The same, with choice number focus focused (a setting's picker opens on its current value). */
    private void dialog(String title, String msg, String[] labels, Runnable[] actions, int focus) {
        if (gone()) return;
        closeDialog();
        final View page = root.getChildCount() > 0 ? root.getChildAt(0) : null;
        if (page instanceof ViewGroup) ((ViewGroup) page).setDescendantFocusability(ViewGroup.FOCUS_BLOCK_DESCENDANTS);
        modalBack = back; modalPrev = prevTab; modalNext = nextTab;
        prevTab = null; nextTab = null;
        back = this::closeDialog;

        FrameLayout shade = new FrameLayout(this);
        shade.setBackgroundColor(0xB8030A0D);
        shade.setClickable(true);   /* nothing under it takes a tap */
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(26), dp(20), dp(26), dp(16));
        box.setBackground(new Acit.Panel(this));
        TextView t = text(title, 20, Acit.ORANGE, true);
        t.setLetterSpacing(0.05f);
        box.addView(t);
        final boolean list = labels.length > 2;
        final int screenH = getResources().getDisplayMetrics().heightPixels;
        if (msg != null && !msg.isEmpty()) {
            ScrollView ms = new MaxScroll((int) (screenH * (list ? 0.26f : 0.5f)));
            ms.setFocusable(false);
            TextView m = text(msg, 16, Acit.TEXT, false);
            m.setLineSpacing(0, 1.12f);
            m.setPadding(0, dp(8), 0, dp(10));
            ms.addView(m);
            box.addView(ms);
        }
        LinearLayout choices = new LinearLayout(this);
        choices.setOrientation(list ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        Acit.Button first = null;
        for (int i = 0; i < labels.length; i++) {
            final Runnable a = actions[i];
            Acit.Button b = new Acit.Button(this, labels[i], null, () -> { closeDialog(); if (a != null) a.run(); });
            if (first == null || i == focus) first = b;
            choices.addView(b, list ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, rowH(56))
                                    : new LinearLayout.LayoutParams(0, rowH(58), 1));
        }
        if (list) {
            ScrollView cs = new MaxScroll((int) (screenH * 0.5f));
            cs.setVerticalFadingEdgeEnabled(true);
            cs.addView(choices);
            box.addView(cs);
        } else box.addView(choices);
        int w = (int) Math.min(dp(640), getResources().getDisplayMetrics().widthPixels * (wide() ? 0.72f : 0.94f));
        shade.addView(box, new FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        modal = shade;
        root.addView(shade);
        box.setScaleX(0.96f); box.setScaleY(0.96f); box.setAlpha(0f);
        box.animate().scaleX(1f).scaleY(1f).alpha(1f).setDuration(140).start();
        if (first != null) first.requestFocus();
    }

    /** A ScrollView no taller than max pixels. */
    private final class MaxScroll extends ScrollView {
        private final int max;
        MaxScroll(int max) { super(LauncherActivity.this); this.max = max; }
        @Override protected void onMeasure(int w, int h) { super.onMeasure(w, View.MeasureSpec.makeMeasureSpec(max, View.MeasureSpec.AT_MOST)); }
    }

    private void closeDialog() {
        if (modal == null) return;
        root.removeView(modal);
        modal = null;
        back = modalBack; prevTab = modalPrev; nextTab = modalNext;
        View page = root.getChildCount() > 0 ? root.getChildAt(0) : null;
        if (page instanceof ViewGroup) {
            ((ViewGroup) page).setDescendantFocusability(ViewGroup.FOCUS_AFTER_DESCENDANTS);
            if (column != null && getCurrentFocus() == null) focusFirst(column);
        }
    }

    private boolean secondDisplay() {
        DisplayManager dm = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        if (dm == null) return false;
        for (Display d : dm.getDisplays()) if (d.getDisplayId() != Display.DEFAULT_DISPLAY) return true;
        return false;
    }

    /** Connected game controllers, by name. */
    private List<String> controllers() {
        List<String> out = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                if (!out.contains(d.getName())) out.add(d.getName());
        }
        return out;
    }

    private String presetName() {
        Map<String, String> cur = store.settings();
        for (String[] p : PRESETS) {
            boolean all = true;
            for (int i = 2; i < p.length && all; i++) {
                String k = p[i].substring(0, p[i].indexOf('=')), v = p[i].substring(p[i].indexOf('=') + 1);
                String have = cur.containsKey(k) ? cur.get(k) : def(k).def;
                all = v.equals(have);
            }
            if (all) return p[0];
        }
        return "Custom";
    }

    /** Medium with the memory and the cores to carry it, Low otherwise. Android says nothing useful
     *  about the GPU, so that is left to the player. */
    private int recommendedPreset() {
        android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
        android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (am != null) am.getMemoryInfo(mi);
        long top = 0; int fast = 0;
        long[] cap = new long[32];
        for (int c = 0; c < cap.length; c++) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/sys/devices/system/cpu/cpu" + c + "/cpu_capacity"))) {
                cap[c] = Long.parseLong(r.readLine().trim());
                top = Math.max(top, cap[c]);
            } catch (Exception e) { break; }
        }
        for (long v : cap) if (v * 2 >= top && v > 0) fast++;
        return mi.totalMem >= 5500L << 20 && (top == 0 || fast >= 4) ? 1 : 0;
    }

    private static Def def(String key) { for (Def d : SETTINGS) if (d.key.equals(key)) return d; return null; }

    private String value(Def d) { Map<String, String> cur = store.settings(); return cur.containsKey(d.key) ? cur.get(d.key) : d.def; }

    private void applyPreset(String[] p) {
        try {
            Map<String, String> changes = new java.util.LinkedHashMap<>();
            for (int i = 2; i < p.length; i++) {
                int eq = p[i].indexOf('=');
                String v = p[i].substring(eq + 1);
                changes.put(p[i].substring(0, eq), v.isEmpty() ? null : v);
            }
            store.putSettings(changes);
        } catch (Exception e) { toast("Settings", e.getMessage()); }
    }

    /* ---- home --------------------------------------------------------------- */

    private File lastPlayFile() { return new File(store.base, ".last_play"); }

    /** "just now", "12 min ago", "3 h ago", "yesterday", else the date. */
    private static String ago(long t) {
        long s = (System.currentTimeMillis() - t) / 1000;
        if (s < 90) return "just now";
        if (s < 3600) return (s / 60) + " min ago";
        if (s < 86400) return (s / 3600) + " h ago";
        if (s < 2 * 86400) return "yesterday";
        return DateFormat.getDateInstance(DateFormat.MEDIUM).format(new Date(t));
    }

    /**
     * Home, for a handheld: the game and a big Play first (focused), a few short status lines under it,
     * and everything else as large tiles -- beside it when wide(), below it otherwise.
     */
    private void showHome() {
        guideStep = -1;
        String st = store.gameStatus();
        boolean ok = st.equals("ok");
        long last = lastPlayFile().lastModified();
        LinearLayout[] s = screen(ok ? (last > 0 ? "Last played " + ago(last) : "Ready to play") : "Set up your game to play",
                "A Crack in Time", null);
        redraw = this::showHome;   /* rebuilt as it is when the window changes size */
        padScreen = this::showHome;
        LinearLayout info = s[0], col = s[1];
        List<String> pads = controllers();
        boolean touch = !"0".equals(store.setting("RACACIT_TOUCH", "1"));

        /* the game's own tile, and what it is */
        Bitmap icon = Art.cachedIcon();
        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(2), 0, 0, dp(8));
        homeIcon = new ImageView(this);
        homeIcon.setImageBitmap(icon);
        homeIcon.setScaleType(ImageView.ScaleType.FIT_CENTER);
        homeIcon.setVisibility(icon == null ? View.GONE : View.VISIBLE);
        LinearLayout.LayoutParams iconLayout = new LinearLayout.LayoutParams(dp(132), dp(132 * 176 / 320));
        iconLayout.rightMargin = dp(14);
        top.addView(homeIcon, iconLayout);
        LinearLayout what = new LinearLayout(this);
        what.setOrientation(LinearLayout.VERTICAL);
        what.addView(text("Ratchet & Clank Future", 13, DIM, true));
        what.addView(text("A Crack in Time", 22, Acit.TEXT, true));
        what.addView(text("RACiT " + version(), 13, DIM, false));
        top.addView(what);
        col.addView(top);

        Acit.Button play = button("Play", ok ? "Your saves are backed up first" : "Import your game first", this::play);
        play.accent = ok;
        play.glyph = glyph(GlyphCore.START);
        play.setLayoutParams(rowParams(84));
        col.addView(play);
        if (!ok) { Acit.Button g = button("Setup guide", "Your game, controls and graphics, step by step", () -> showGuide(0)); g.accent = true; col.addView(g); }

        /* what is ready, one short line each */
        LinearLayout stat = new LinearLayout(this);
        stat.setOrientation(LinearLayout.VERTICAL);
        stat.setPadding(dp(8), dp(10), dp(8), 0);
        TextView gameLine = statusLine(stat, ok ? 1 : -1, "Game", ok ? "Ready" : st, ok ? Acit.TEXT_GOOD : Acit.TEXT_BAD);
        if (ok) {   /* every file there? (a copy without packed/game/game.psarc boots to a black screen) */
            final File root = store.gameRoot();
            new Thread(() -> {
                GameCheck c = GameCheck.run(this, root, store.eboot());
                if (c.known() && !c.complete()) ui.post(() -> {
                    gameLine.setText(c.line() + "\nImport game > Check game files copies in just the missing ones.");
                    gameLine.setTextColor(Acit.TEXT_BAD);
                });
            }).start();
        }
        statusLine(stat, !pads.isEmpty() || touch ? 1 : -1, "Controls", !pads.isEmpty() ? TextUtils.join(", ", pads)
                : touch ? "Touch controls (no controller connected)" : "No controller, and touch controls are off",
                !pads.isEmpty() || touch ? Acit.TEXT : Acit.TEXT_BAD);
        String rec = PRESETS[recommendedPreset()][0], now = presetName();
        statusLine(stat, 1, "Graphics", now + " preset" + (now.equals(rec) ? "" : "  ·  " + rec + " suits this device"), Acit.TEXT);
        boolean comp = store.companionEnabled(), two = secondDisplay();
        if (two) statusLine(stat, comp ? 1 : 0, "Dual screen", comp ? "Companion on the lower screen" : "Companion is off",
                comp ? Acit.TEXT_GOOD : DIM);
        List<File> slots = store.saveSlots();
        long newest = 0;
        for (File f : slots) newest = Math.max(newest, new File(f, "GAME.SAV").lastModified());
        int nm = store.enabledMods().size();
        statusLine(stat, 0, "Progress", (slots.isEmpty() ? "No saves yet" : slots.size() + (slots.size() == 1 ? " save slot" : " save slots")
                + (newest > 0 ? ", saved " + ago(newest) : "")) + (nm == 0 ? "" : "  ·  " + nm + (nm == 1 ? " mod on" : " mods on")), DIM);
        col.addView(stat);

        /* everything else, as tiles */
        heading(info, "Set up");
        info.addView(tiles(
                tile("Settings", "Graphics, display", () -> showSettings(0)),
                tile("Controls", pads.isEmpty() ? (touch ? "Touch controls" : "Controllers") : "Connected", this::showControls),
                tile("Saves", slots.isEmpty() ? "Import, restore" : "Back up, restore", this::showSaves),
                tile("Shaders", SHADER_SHORT[store.shaderStatus(version())], this::showShaders),
                tile("Game files", ok ? "Check, re-import" : "Start here", this::showImport),
                tile("Mods", nm == 0 ? "None on" : nm + " on", this::showMods)));
        heading(info, "More");
        info.addView(tiles(
                tile("Help", "Tips, problems", () -> showHelp(0)),
                ok ? tile("Diagnostics", store.debugFlag().exists() ? "Logging on" : "Logs", this::showLogs)
                   : tile("Setup guide", "Step by step", () -> showGuide(0)),
                tile("Update app", "Check, install", this::showUpdate),
                tile("Migrate data", "Saves, settings", this::showMigration)));
        focusFirst(col);
    }

    /** A home tile: a button with a title and one short line. */
    private Acit.Button tile(String label, String sub, Runnable r) {
        Acit.Button b = new Acit.Button(this, label, sub, r);
        b.tile = true;
        return b;
    }

    /** Tiles two to a row, each half the width. */
    private View tiles(Acit.Button... b) {
        LinearLayout grid = new LinearLayout(this);
        grid.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < b.length; i += 2) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            for (int k = i; k < i + 2; k++) {
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, rowH(96), 1);
                if (k == i) lp.rightMargin = dp(4); else lp.leftMargin = dp(4);
                if (k < b.length) row.addView(b[k], lp);
                else row.addView(new View(this), lp);
            }
            LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            rp.bottomMargin = dp(4);
            grid.addView(row, rp);
        }
        return grid;
    }

    private void play() {
        String st = store.gameStatus();
        if (!st.equals("ok")) { toast("Game not ready", st + "\n\nUse Import game first."); return; }
        long now = System.currentTimeMillis();
        if (busy || now - lastPlay < 3000) return;
        lastPlay = now;
        new Thread(() -> {
            GameCheck c = GameCheck.run(this, store.gameRoot(), store.eboot());
            if (c.known() && !c.complete()) {
                ui.post(() -> dialog("Game files missing",
                        c.line() + "\n\nThe game will stay on a black screen. Copy the missing files in from a complete copy first.",
                        new String[] { "Check and repair", "Play anyway", "Cancel" }, new Runnable[] { this::showCheck, this::launch, null }));
                return;
            }
            ui.post(this::launch);
        }).start();
    }

    /* ---- shaders ------------------------------------------------------------ */

    private static final String[] SHADER_SHORT = {
        "Ready", "None yet", "Not prepared", "Prepare again", "Prepare again" };

    private boolean onShaders;   /* back from a prepare: show its result (onResume) */
    private void leaveShaders() { onShaders = false; showHome(); }

    private void showShaders() {
        LinearLayout[] s = screen("Shaders", "Prepare shaders", this::leaveShaders);
        onShaders = true;
        redraw = this::showShaders;
        LinearLayout info = s[0], col = s[1];
        final int st = store.shaderStatus(version());
        final int n = store.shaderVariants();
        final String[] said = {
            "Ready: every recorded pipeline is built for this driver and this version.",
            "Nothing recorded yet. The game records each pipeline the first time it draws it; play a while, then come back.",
            "Not prepared on this device yet. Preparing builds them now so areas you have seen do not stutter the first time.",
            "The graphics driver changed since the last prepare: its pipelines have to be built again.",
            "Prepared by another app version. Prepare again to be sure." };
        infoLine(info, "Now", said[st], st == 0 ? Acit.TEXT_GOOD : st == 1 ? DIM : Acit.ORANGE);
        infoLine(info, "Recorded", n + (n == 1 ? " pipeline" : " pipelines"), Acit.TEXT);
        Map<String, String> stamp = store.shaderStamp();
        if (!stamp.isEmpty())
            infoLine(info, "Last prepare", stamp.get("built") + " of " + stamp.get("variants") + " built in " + stamp.get("seconds") + " s on "
                    + stamp.get("threads") + " threads (" + stamp.get("build") + ")"
                    + ("0".equals(stamp.get("absent")) || stamp.get("absent") == null ? "" : ", " + stamp.get("absent") + " without their shader"), DIM);
        infoLine(info, "What it does", "Opens the game screen without the game and builds every recorded pipeline on all cores, "
                + "with a progress bar, then closes. Takes from seconds to a few minutes; keep the device plugged in for a long one.", DIM);
        if (n > 0) col.addView(button("Prepare shaders", st == 0 ? "Build them again anyway" : "Build them now", this::prepareShaders));
        col.addView(button("Back", null, this::leaveShaders));
        focusFirst(col);
    }

    private void prepareShaders() {
        if (busy) return;
        busy = true;
        final String tag = version();
        new Thread(() -> {
            try {
                store.writeLaunchEnvPrepare(tag);
            } catch (Exception e) {
                ui.post(() -> { busy = false; if (!gone()) toast("Could not start", e.getMessage()); });
                return;
            }
            ui.post(() -> {
                busy = false;
                if (gone()) return;
                startActivity(new Intent(this, RecompActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                                .putExtra(RecompActivity.EXTRA_PREPARE, true),
                        android.app.ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle());
            });
        }).start();
    }

    private void launch() {
        if (busy) return;
        busy = true;
        new Thread(() -> {
            try {
                store.backupSaves("launch");
                store.writeLaunchEnv();
            } catch (Exception e) {
                ui.post(() -> { busy = false; if (!gone()) toast("Could not start", "Your saves or launch settings could not be prepared. " + e.getMessage()); });
                return;
            }
            try { Store.writeText(lastPlayFile(), Long.toString(System.currentTimeMillis())); } catch (Exception ignored) { }
            ui.post(() -> {
                busy = false;
                if (gone()) return;
                /* The game goes on the main screen (the Thor's top one) even when
                 * a frontend opened this launcher on the second screen, so the
                 * companion gets the other one. Its own task (taskAffinity) lets
                 * it sit on a different screen from the launcher. */
                startActivity(new Intent(this, RecompActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        android.app.ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY).toBundle());
            });
        }).start();
    }

    /* ---- setup guide -------------------------------------------------------- */

    private void showGuide(int step) {
        guideStep = step;
        final String[] titles = { "Welcome", "Your game", "Controls", "Graphics", "Ready" };
        LinearLayout[] s = screen("Setup  " + (step + 1) + " of 5", titles[step], step == 0 ? (setupDone() ? this::showHome : null) : () -> showGuide(step - 1));
        redraw = () -> showGuide(step);   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        String st = store.gameStatus();
        boolean ok = st.equals("ok");
        switch (step) {
            case 0:
                infoLine(info, "What this is", "Ratchet & Clank Future: A Crack in Time, recompiled to run natively on Android. "
                        + "It needs your own copy of the game: nothing from the game is included in this app.", Acit.TEXT);
                infoLine(info, "What you need", "Your decrypted dump of the USA release (BCUS98124): the folder with PS3_GAME, "
                        + "the decrypted EBOOT.elf in PS3_GAME/USRDIR. About 25 GB, so copy it to the device over USB or a card first.", Acit.TEXT);
                infoLine(info, "Three steps", "1. Import the game.  2. Check your controls (a controller, or the on-screen touch controls).  "
                        + "3. Pick a graphics preset for this device. Then play.", Acit.TEXT);
                infoLine(info, "Controller or touch", "Everything here works with a controller (D-pad, A to select, B to go back) or by touch.", DIM);
                col.addView(button("Start setup", null, () -> showGuide(1)));
                if (ok) col.addView(button("Skip to the home screen", "The game is already imported", () -> finishGuide(false)));
                break;
            case 1:
                infoLine(info, "Now", ok ? "Ready: " + store.gameRoot().getPath() : st, ok ? Acit.TEXT_GOOD : Acit.TEXT_BAD);
                infoLine(info, "Copy into the app", "Safest: the game goes into this app's own folder and works on every device. "
                        + "Needs about 25 GB free while it copies (it can take several minutes).", Acit.TEXT);
                infoLine(info, "Use where it is", "No second copy: the game is read from where you put it. Android asks you to "
                        + "allow \"All files access\" for this app.", Acit.TEXT);
                infoLine(info, "Which folder", "Pick the folder that contains PS3_GAME (or PS3_GAME itself).", DIM);
                if (ok) { Acit.Button n = button("Next: controls", null, () -> showGuide(2)); n.accent = true; col.addView(n); }
                col.addView(button("Copy into the app", "Pick the folder", () -> pickTree(REQ_COPY)));
                col.addView(button("Import a .iso", "Extract your own disc image; .dkey supported when needed", this::pickIso));
                col.addView(button("Use where it is", "Pick the folder", this::useInPlace));
                col.addView(button("Add EBOOT.elf", "Your decrypted PS3 program; game and program can be added in either order", this::pickEboot));
                break;
            case 2: {
                List<String> pads = controllers();
                infoLine(info, "Controllers", pads.isEmpty() ? "None connected right now." : TextUtils.join("\n", pads), pads.isEmpty() ? DIM : Acit.TEXT_GOOD);
                infoLine(info, "With a controller", "Xbox-style pads: A = Cross, B = Circle, X = Square, Y = Triangle, stick clicks L3/R3, "
                        + "View/Back = Select, Menu/Start = Start. PlayStation pads map one to one. Fire button: R2 by default (the "
                        + "v97 layout: R2 fires, R1 takes the PS3 R2 action, L2 strafes and L1 looks); R1 for the PS3's own layout, "
                        + "on the Controls page.", Acit.TEXT);
                infoLine(info, "By touch", "With no controller connected, a PS3 pad is drawn over the game: left stick where your left thumb lands, "
                        + "drag on the right half to look around, face buttons bottom right, shoulders in the top corners. Its size and opacity are on the Controls page.", Acit.TEXT);
                Acit.Button n = button("Next: graphics", null, () -> showGuide(3)); n.accent = true; col.addView(n);
                col.addView(button("Controls page", "Test your buttons, touch size and opacity", this::showControls));
                break;
            }
            case 3: {
                int rec = recommendedPreset();
                infoLine(info, "Presets", "The Windows build's graphics presets: each sets the resolution, texture quality and "
                        + "filtering together. Fine-tune them in Settings, or in the game's own Settings menu while you play.", Acit.TEXT);
                infoLine(info, "Starting point", PRESETS[rec][0] + " -- start light, then raise quality if frame times remain steady. "
                        + "High and Ultra suit faster GPUs; custom resolution ranges from 50% to 400%. Screen count does not determine performance.", Acit.TEXT_GOOD);
                for (int i = 0; i < PRESETS.length; i++) {
                    final String[] p = PRESETS[i];
                    Acit.Button b = button(p[0] + (i == rec ? "  (recommended)" : ""), p[1], () -> { applyPreset(p); showGuide(4); });
                    b.accent = i == rec;
                    col.addView(b);
                }
                break;
            }
            default: {
                infoLine(info, "Game", ok ? "Ready" : st, ok ? Acit.TEXT_GOOD : Acit.TEXT_BAD);
                infoLine(info, "Graphics", "Preset: " + presetName(), Acit.TEXT);
                infoLine(info, "While you play", "Start opens the game's pause menu. Graphics settings: the menu button between Select "
                        + "and Start on the touch controls, or L3 + R3 together on a controller. The first run builds its shader cache, so "
                        + "new places can stutter once and are smooth after that. Saves are backed up every time you press Play.", Acit.TEXT);
                Acit.Button p = button("Play", ok ? null : "Import your game first", () -> finishGuide(true));
                p.accent = ok; p.glyph = glyph(GlyphCore.START);
                col.addView(p);
                col.addView(button("Home screen", null, () -> finishGuide(false)));
                break;
            }
        }
        if (step > 0) col.addView(button("Back", null, () -> showGuide(step - 1)));
        focusFirst(col);
    }

    private void finishGuide(boolean play) {
        try { setupFlag().createNewFile(); } catch (Exception ignored) { }
        showHome();
        if (play) play();
    }

    /* ---- import ------------------------------------------------------------- */

    private void showImport() {
        LinearLayout[] s = screen("Import", "Your copy of the game", this::showHome);
        redraw = this::showImport;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        String st = store.gameStatus();
        infoLine(info, "Now", st.equals("ok") ? "Ready: " + store.gameRoot().getPath() : st, st.equals("ok") ? Acit.TEXT_GOOD : Acit.TEXT_BAD);
        infoLine(info, "What to pick", "The folder that contains PS3_GAME from your own decrypted dump of A Crack in Time (USA, BCUS98124), "
                 + "Add its decrypted EBOOT.elf separately below, or keep it in PS3_GAME/USRDIR. Either step can come first. Game data is never bundled with this app.", Acit.TEXT);
        infoLine(info, "Program", store.eboot().isFile() ? store.eboot().getPath() : "No EBOOT.elf selected yet.", DIM);
        infoLine(info, "Copy into the app", "Safest. Needs about 25 GB free; the copy can take several minutes.", DIM);
        infoLine(info, "Use where it is", "No copy, but needs \"All files access\" for this app.", DIM);
        col.addView(button("Copy into the app", "Pick the folder", () -> pickTree(REQ_COPY)));
        col.addView(button("Import a .iso", "Read-only source; validates and stages a complete game before replacing the app's copy", this::pickIso));
        col.addView(button("Use where it is", "Pick the folder", this::useInPlace));
        col.addView(button("Add EBOOT.elf", "Copies your decrypted program into this app; source game folder stays unchanged", this::pickEboot));
        col.addView(button("Check game files", "Is every file there? Copy in only what is missing", this::showCheck));
        col.addView(button("Refresh icons and art", "Button icons from the game's font, its key art", this::refreshGlyphs));
        col.addView(button("Back", null, this::showHome));
        focusFirst(col);
    }

    private void useInPlace() {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            toast("All files access", "Allow \"All files access\" for this app on the next screen, then come back and pick the folder again.");
            try { startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))); }
            catch (Exception e) { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
            return;
        }
        pickTree(REQ_PLACE);
    }

    private void pickEboot() {
        if (busy || !canChangeGameFiles()) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i, REQ_EBOOT);
    }

    private void pickIso() {
        if (busy || !canChangeGameFiles()) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, REQ_ISO);
    }

    private void pickDiscKey() {
        if (pendingIso == null) { toast("Select the ISO first", "Choose your .iso again, then its .dkey when requested."); return; }
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(i, REQ_DKEY);
    }

    /** Full-file descriptors only: pipes/cloud streams cannot provide random-access disc sectors. */
    private IsoImage openIso(Uri uri) throws java.io.IOException {
        android.os.ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(uri, "r");
        if (descriptor == null) throw new java.io.IOException("Could not open the selected ISO.");
        final java.io.FileInputStream stream = new android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor);
        final java.nio.channels.FileChannel channel = stream.getChannel();
        return IsoImage.open(new IsoImage.Source() {
            @Override public long size() throws java.io.IOException {
                try { return channel.size(); }
                catch (java.io.IOException e) { throw new java.io.IOException("The selected provider cannot seek this ISO. Save it to local device storage first.", e); }
            }
            @Override public void readFully(long position, byte[] bytes, int offset, int length) throws java.io.IOException {
                java.nio.ByteBuffer dst = java.nio.ByteBuffer.wrap(bytes, offset, length);
                while (dst.hasRemaining()) {
                    int n = channel.read(dst, position);
                    if (n <= 0) throw new java.io.IOException("The ISO ends early or is not seekable. Use a complete local image.");
                    position += n;
                }
            }
            @Override public void close() throws java.io.IOException { stream.close(); }
        }, () -> cancel);
    }

    private void runIso(Uri uri, Uri discKey) {
        if (busy) return;
        pendingIso = uri; busy = true; cancel = false;
        final int gs = guideStep;
        LinearLayout[] screen = screen("Import", "Importing your ISO", null);
        guideStep = gs;
        CopyProgress progress = new CopyProgress(screen[0], "The image is read without changes. Files are checked and staged before replacing this app's game. "
                + "Your existing game stays available if import fails or is cancelled. Keep the app open.");
        screen[1].addView(button("Cancel", null, () -> cancel = true));
        focusFirst(screen[1]);
        new Thread(() -> {
            try (IsoImage image = openIso(uri)) {
                if (discKey != null) {
                    try (InputStream key = getContentResolver().openInputStream(discKey)) {
                        if (key == null) throw new java.io.IOException("Could not read the selected .dkey.");
                        image.setDiscKey(new String(Store.readAll(key, 256), java.nio.charset.StandardCharsets.US_ASCII));
                    }
                }
                long copied = GameIsoImport.importInto(image, store.base, new GameIsoImport.Progress() {
                    @Override public boolean cancelled() { return cancel; }
                    @Override public void update(String path, long done, long total) { progress.update(path, done, total); }
                }, staged -> {
                    File elf = store.privateEboot().isFile() ? store.privateEboot() : new File(staged, "PS3_GAME/USRDIR/EBOOT.elf");
                    if (elf.isFile()) EbootFile.validate(elf);
                    GameCheck check = GameCheck.run(this, staged, elf);
                    if (!check.known()) throw new java.io.IOException("Could not validate the extracted game against this build's file list.");
                    for (String missing : check.missing) {
                        if (!missing.equals("PS3_GAME/USRDIR/EBOOT.elf")) throw new java.io.IOException("The ISO is incomplete for this build: missing " + missing);
                    }
                });
                store.putSetting("GAME", null);
                String status = store.gameStatus();
                String result = "Imported " + Store.size(copied) + ". " + (status.equals("ok") ? "Ready to play." : "Now use Add EBOOT.elf to select your decrypted program.");
                try { result += "\n" + Glyphs.extract(store.gameRoot(), store.glyphs()) + " button icons taken from your game."; } catch (Exception ignored) { }
                result += "\n" + takeArt();
                final String message = result;
                ui.post(() -> { busy = false; pendingIso = null; if (!gone()) afterImport(message); });
            } catch (GameIsoImport.KeyRequired needsKey) {
                ui.post(() -> {
                    busy = false;
                    if (gone()) return;
                    if (guideStep >= 0) showGuide(1); else showImport();
                    dialog("Disc key needed", "This ISO needs the 32-digit disc key from your own dump. Select its .dkey file. "
                            + "The key is used for this import and is not saved by the app.", new String[] { "Select .dkey", "Cancel" },
                            new Runnable[] { this::pickDiscKey, () -> pendingIso = null });
                });
            } catch (Exception failure) {
                final String message = cancel ? "Import cancelled. Your existing game was kept." : "ISO import failed: " + failure.getMessage();
                ui.post(() -> { busy = false; if (!gone()) afterImport(message); });
            }
        }).start();
    }

    private void importEboot(Uri uri) {
        if (busy) return;
        discardEboot(); busy = true;
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                EbootFile.Candidate candidate = EbootFile.stage(in, store.privateEboot().getParentFile());
                ui.post(() -> {
                    busy = false;
                    if (gone()) { candidate.close(); return; }
                    pendingEboot = candidate;
                    if (candidate.known()) installEboot(candidate);
                    else {
                        dialog("Check this EBOOT.elf", "This is a valid PS3 executable, but its SHA-256 differs from the USA BCUS98124 version 1.00 program "
                                + "used to build v97. A different region or updated program may not work.\n\nSHA-256: " + candidate.sha256,
                                new String[] { "Cancel", "Use this file" }, new Runnable[] { this::discardEboot, () -> installEboot(candidate) });
                        back = () -> { closeDialog(); discardEboot(); };
                    }
                });
            } catch (Exception e) {
                ui.post(() -> { busy = false; if (!gone()) toast("Could not add EBOOT.elf", e.getMessage()); });
            }
        }).start();
    }

    private void installEboot(EbootFile.Candidate candidate) {
        busy = true;
        new Thread(() -> {
            String result;
            try { candidate.install(store.privateEboot()); result = "EBOOT.elf added. " + (store.gameStatus().equals("ok") ? "Ready to play." : "Now select your game folder."); }
            catch (Exception e) { result = "Could not install EBOOT.elf: " + e.getMessage(); }
            finally { candidate.close(); }
            final String message = result;
            ui.post(() -> { pendingEboot = null; busy = false; if (!gone()) afterImport(message); });
        }).start();
    }

    private void discardEboot() { if (pendingEboot != null) { pendingEboot.close(); pendingEboot = null; } }

    private void pickTree(int req) {
        if (busy || !canChangeGameFiles()) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, req);
    }

    private void refreshGlyphs() {
        new Thread(() -> {
            String msg;
            try { msg = Glyphs.extract(store.gameRoot(), store.glyphs()) + " button icons taken from the game."; }
            catch (Exception e) { msg = "Could not read the game's font: " + e.getMessage(); }
            final String m = msg + "\n" + takeArt();
            ui.post(() -> { showImport(); toast("Icons and art", m); });
        }).start();
    }

    /** The game's key art and tile into assets/art, and onto the background (a worker thread). */
    private String takeArt() {
        String r = Art.refresh(store);
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        Bitmap a = Art.keyArt(store, Math.max(dm.widthPixels, dm.heightPixels));
        Bitmap icon = Art.icon(store);
        ui.post(() -> {
            if (isDestroyed()) return;
            bg.setArt(a);
            if (homeIcon != null) {
                homeIcon.setImageBitmap(icon);
                homeIcon.setVisibility(icon == null ? View.GONE : View.VISIBLE);
            }
        });
        return r;
    }

    /** After an import: back to the guide's step if it was open, else the import screen. */
    private void afterImport(String result) {
        if (guideStep >= 0) showGuide(1); else showImport();
        toast("Import", result);
    }

    /**
     * A progress screen's parts: a bar, how much of how much, the file now,
     * and the time left (from the rate so far).
     */
    private final class CopyProgress implements Importer.Progress {
        final Acit.Progress bar = new Acit.Progress(LauncherActivity.this);
        final TextView how = text("", 20, Acit.ORANGE, true), what = text("Starting...", 15, DIM, false),
                left = text("", 14, Acit.TEXT, false);
        private final long t0 = System.currentTimeMillis();
        private long lastUi;

        CopyProgress(LinearLayout info, String about) {
            infoLine(info, "What happens", about, Acit.TEXT);
            info.addView(text("Progress", 13, Acit.EDGE, true));
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(30));
            bp.topMargin = dp(6); bp.bottomMargin = dp(8);
            info.addView(bar, bp);
            info.addView(how); info.addView(left); info.addView(what);
            what.setSingleLine(true); what.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        }

        @Override public void update(String w, long done, long total) {
            long now = System.currentTimeMillis();
            if (now - lastUi < 250 && done < total) return;
            lastUi = now;
            long el = now - t0;
            String eta = total > 0 && done > 0 && el > 4000 && done < total
                    ? "About " + mins((long) ((total - done) * (double) el / done)) + " left" : "";
            ui.post(() -> {
                what.setText(w);
                bar.set(total > 0 ? (float) done / total : 0);
                how.setText(total > 0 ? Store.size(done) + " of " + Store.size(total) : "");
                left.setText(eta);
            });
        }
        @Override public boolean cancelled() { return cancel; }
    }

    private static String mins(long ms) {
        long m = (ms + 30000) / 60000;
        return m < 1 ? "less than a minute" : m == 1 ? "1 minute" : m < 90 ? m + " minutes" : String.format(Locale.US, "%.1f hours", m / 60f);
    }

    /** The copy, on a worker thread, with a progress screen and Cancel. */
    private void runCopy(Uri tree) {
        cancel = false; busy = true;
        final int gs = guideStep;
        LinearLayout[] s = screen("Import", "Copying your game", null);
        guideStep = gs;
        LinearLayout info = s[0], col = s[1];
        CopyProgress pr = new CopyProgress(info, "Your game is copied into this app's own folder. Keep the launcher open; "
                + "the device may get warm. Cancel removes what was copied so far.");
        col.addView(button("Cancel", null, () -> cancel = true));
        focusFirst(col);
        new Thread(() -> {
            String result;
            try {
                long n = new Importer(this, tree).copyInto(store.base, pr);
                store.putSetting("GAME", null);
                String st = store.gameStatus();
                result = "Copied " + Store.size(n) + ".\n\n" + (st.equals("ok") ? "The game is ready." : st);
                GameCheck gc = GameCheck.run(this, store.gameRoot(), store.eboot());
                if (gc.known() && !gc.complete())
                    result += "\n\nBut the folder you copied from was not complete: " + gc.line()
                            + "\nImport game > Check game files copies in just the missing ones from a complete copy.";
                if (st.equals("ok")) {
                    try { result += "\n" + Glyphs.extract(store.gameRoot(), store.glyphs()) + " button icons taken from the game's font."; }
                    catch (Exception e) { result += "\nButton icons: " + e.getMessage(); }
                    result += "\n" + takeArt();
                }
            } catch (Exception e) { result = cancel ? "Cancelled. Nothing was changed." : "The copy failed: " + e.getMessage(); }
            final String r = result;
            busy = false;
            ui.post(() -> afterImport(r));
        }).start();
    }

    private void usePlace(Uri tree) {
        new Thread(() -> {
            String result;
            try {
                String path = new Importer(this, tree).folderPath();
                if (path == null) throw new Exception("That folder is not on this device's storage. Copy it into the app instead.");
                String previous = store.setting("GAME", null);
                store.putSetting("GAME", path);
                String dataStatus = store.gameDataStatus();
                if (!dataStatus.equals("ok")) { store.putSetting("GAME", previous); throw new Exception(dataStatus); }
                String st = store.gameStatus();
                result = st.equals("ok") ? "Using the game at\n" + path : st;
                if (st.equals("ok")) {
                    try { result += "\n\n" + Glyphs.extract(store.gameRoot(), store.glyphs()) + " button icons taken from the game's font."; }
                    catch (Exception e) { result += "\n\nButton icons: " + e.getMessage(); }
                    result += "\n" + takeArt();
                }
            } catch (Exception e) { result = e.getMessage(); }
            final String r = result;
            ui.post(() -> afterImport(r));
        }).start();
    }

    /** Every file of the game there? The list, and a repair that copies only what is missing. */
    private void showCheck() {
        LinearLayout[] s = screen("Import", "Game files", this::showImport);
        redraw = this::showCheck;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        TextView t = text("Checking...", 13, Acit.TEXT, false);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        info.addView(t);
        infoLine(info, "Checked", store.gameRoot().getPath() + "/PS3_GAME, against the file list of the USA release "
                + "(BCUS98124: 790 files, 24.2 GB).", DIM);
        col.addView(button("Copy the missing files", "Pick a complete copy: only what is missing is copied", () -> pickTree(REQ_REPAIR)));
        col.addView(button("Check again", null, this::showCheck));
        col.addView(button("Back", null, this::showImport));
        focusFirst(col);
        final File root = store.gameRoot();
        new Thread(() -> {
            GameCheck c = GameCheck.run(this, root, store.eboot());
            ui.post(() -> { t.setText(c.report(40)); t.setTextColor(c.complete() ? Acit.TEXT_GOOD : c.known() ? Acit.TEXT_BAD : DIM); });
        }).start();
    }

    /** Copies from a complete copy only the files this one lacks (or has at another size). */
    private void runRepair(Uri tree) {
        cancel = false; busy = true;
        LinearLayout[] s = screen("Import", "Copying the missing files", null);
        LinearLayout info = s[0], col = s[1];
        CopyProgress pr = new CopyProgress(info, "Only the files your copy lacks (or has at another size) are copied in from the one you picked. "
                + "Cancel keeps what was copied so far.");
        col.addView(button("Cancel", null, () -> cancel = true));
        focusFirst(col);
        final File root = store.gameRoot();
        new Thread(() -> {
            String result;
            try {
                long n = new Importer(this, tree).repairInto(root, pr);
                GameCheck c = GameCheck.run(this, root, store.eboot());
                result = (n == 0 ? "Nothing to copy: the folder you picked has no file this copy lacks."
                                 : "Copied " + Store.size(n) + " of missing files.") + "\n\n" + c.line();
                if (n > 0 && c.complete()) {
                    try { Glyphs.extract(root, store.glyphs()); } catch (Exception ignored) { }
                }
            } catch (Exception e) { result = cancel ? "Cancelled. The files copied so far are kept." : "The copy failed: " + e.getMessage(); }
            final String r = result;
            busy = false;
            ui.post(() -> { showCheck(); toast("Game files", r); });
        }).start();
    }

    /* ---- settings ----------------------------------------------------------- */

    /**
     * One setting: its name on the left, its value on the right. Touch: a tap picks from the list of
     * values (a two-way setting just flips), a long press says what it does. Controller: left and
     * right step through the values, A opens the list, Triangle (Y) says what it does. On a wide
     * screen the detail panel (help) follows the focused row.
     */
    private class SettingRow extends Acit.Button {
        final Def d;
        final TextView help;
        SettingRow(Def d, TextView help) {
            super(LauncherActivity.this, d.label, "", null);
            this.d = d; this.help = help;
            refresh();
            action = this::choose;
            longAction = this::explain;
            setLayoutParams(rowParams(60));
            addInfoHint();
        }
        void refresh() { value = d.labelOf(value(d)); invalidate(); }
        /** After a change: the row, the help, and whatever else the screen shows from it. */
        void changed() { }
        void set(String v) {
            try { store.putSetting(d.key, v); } catch (Exception e) { toast("Settings", e.getMessage()); return; }
            refresh();
            showHelpFor();
            changed();
        }
        void step(int dir) {
            int i = d.indexOf(value(d)), n = d.values.length;
            set(d.values[((i + dir) % n + n) % n]);
        }
        void choose() {
            if (d.values.length == 2) { step(1); return; }
            final String cur = value(d);
            final int at = d.indexOf(cur);
            String[] labels = new String[d.values.length];
            Runnable[] acts = new Runnable[d.values.length];
            for (int i = 0; i < d.values.length; i++) {
                final String v = d.values[i];
                labels[i] = (i == at ? "✓  " : "     ") + d.labels[i] + (v.equals(d.def) && i != at ? "   (default)" : "");
                acts[i] = () -> { set(v); requestFocus(); };
            }
            dialog(d.label, d.help, labels, acts, at);
        }
        void explain() {
            dialog(d.label, d.help + "\n\nNow: " + d.labelOf(value(d)) + "      Default: " + d.labelOf(d.def),
                    new String[] { "OK" }, new Runnable[] { () -> requestFocus() });
        }
        void showHelpFor() {
            if (help != null) help.setText(d.label.toUpperCase(Locale.US) + "\n" + d.help + "\n\nNow: " + d.labelOf(value(d))
                    + "      Default: " + d.labelOf(d.def));
        }
        @Override public boolean onKeyDown(int code, KeyEvent e) {
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) { step(-1); return true; }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT) { step(1); return true; }
            return super.onKeyDown(code, e);
        }
        @Override protected void onFocusChanged(boolean f, int dir, android.graphics.Rect prev) {
            super.onFocusChanged(f, dir, prev);
            if (f) showHelpFor();
        }
    }

    private void showSettings(int cat) {
        final String[] cats = cats();
        final int c = (cat + cats.length) % cats.length;
        LinearLayout[] s = screen("Settings  ·  " + (c + 1) + " of " + cats.length, cats[c], this::showHome);
        redraw = () -> showSettings(c);   /* rebuilt as it is when the window changes size */
        prevTab = () -> showSettings(c - 1); nextTab = () -> showSettings(c + 1);
        LinearLayout info = s[0], col = s[1];
        col.addView(categoryStrip(c));

        /* the detail panel: what the focused setting does (wide screens; a narrow one shows it in the picker) */
        TextView help = null;
        if (wide()) {
            help = text("", 17, Acit.TEXT, false);
            help.setLineSpacing(0, 1.12f);
            info.addView(help);
            TextView how = text("\nTap a setting to choose its value; hold it to read about it. On a controller: left and right "
                    + "change it, Triangle tells what it does, L1 / R1 change the category.", 14, DIM, false);
            how.setLineSpacing(0, 1.08f);
            info.addView(how);
        }
        if (c <= 2) {   /* Graphics, Display, Performance: the game's own menu has them too */
            TextView where = text((help == null ? "" : "\n") + "Also in the game: L3 + R3 together on a controller, the menu button between "
                    + "Select and Start on the touch controls, or GRAPHICS on the Thor's companion.", 14, DIM, false);
            where.setLineSpacing(0, 1.08f);
            info.addView(where);
        }

        if (c == 0) {
            heading(col, "Presets");
            int rec = recommendedPreset();
            String now = presetName();
            for (int i = 0; i < PRESETS.length; i++) {
                final String[] p = PRESETS[i];
                Acit.Button b = button(p[0] + (i == rec ? "  (suits this device)" : "") + (p[0].equals(now) ? "  ✓" : ""), p[1], null);
                b.accent = p[0].equals(now);
                b.action = () -> { applyPreset(p); showSettings(c); };
                col.addView(b);
            }
            heading(col, "Fine-tune");
        }
        if (cats[c].equals("Memory")) {
            infoLine(info, "Choose explicitly", "These profiles change cache retention, not the graphics preset. "
                    + "Reduced targets are experimental and can cause rendering errors or stalls; reset Native defaults if they do. Live resources, game memory "
                    + "and driver allocations are additional. RAM size alone does not predict performance.", DIM);
            heading(col, "Memory profiles");
            for (String[] p : MEMORY_PRESETS) col.addView(button(p[0], p[1], () -> { applyPreset(p); showSettings(c); }));
            heading(col, "Fine-tune (next launch)");
        }
        if (cats[c].equals("Controls")) col.addView(button("Controller and touch page", "Connected controllers, button test, touch preview", this::showControls));
        if (cats[c].equals("Dual screen")) col.addView(button("Preview lower screen", "See the companion on a connected second display", this::previewCompanion));
        SettingRow first = null;
        int firstAt = -1;
        for (Def d : SETTINGS) {
            if (!d.cat.equals(cats[c])) continue;
            SettingRow r = new SettingRow(d, help);
            if (first == null) { first = r; firstAt = col.getChildCount(); }
            col.addView(r);
        }
        if (first != null) first.showHelpFor();
        heading(col, "This category");
        col.addView(button("Reset " + cats[c], "Every setting here back to its default", () -> dialog("Reset " + cats[c] + "?",
                "Every " + cats[c] + " setting goes back to its default.", new String[] { "Reset", "Cancel" }, new Runnable[] { () -> {
                    try {
                        Map<String, String> changes = new java.util.LinkedHashMap<>();
                        for (Def d : SETTINGS) if (d.cat.equals(cats[c])) changes.put(d.key, null);
                        store.putSettings(changes);
                    } catch (Exception e) { toast("Settings", e.getMessage()); }
                    showSettings(c);
                }, null })));
        if (keepFocus < 0 && firstAt >= 0) keepFocus = firstAt;   /* a new category: its first setting */
        focusFirst(col);
    }

    /**
     * The categories as a row of tabs at the top of the list, this one lit: tap one (touch), or L1 / R1
     * (controller). Not focusable, so the D-pad stays in the list. Scrolls sideways when narrow, with
     * the current tab in view.
     */
    private View categoryStrip(int cur) {
        final String[] cats = cats();
        LinearLayout strip = new LinearLayout(this);
        strip.setOrientation(LinearLayout.HORIZONTAL);
        strip.setGravity(Gravity.CENTER_VERTICAL);
        strip.setPadding(0, 0, 0, dp(4));
        android.graphics.Paint measure = Acit.text(Acit.TEXT, Acit.sp(this, 19), Acit.titleFace());
        View lit = null;
        for (int i = 0; i < cats.length; i++) {
            final int k = i;
            Acit.Button t = new Acit.Button(this, cats[i], null, () -> showSettings(k));
            t.accent = i == cur;
            t.setFocusable(false);
            int w = (int) (measure.measureText(cats[i]) + dp(62));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(w, rowH(50));
            lp.rightMargin = dp(2);
            strip.addView(t, lp);
            if (i == cur) lit = t;
        }
        final HorizontalStrip hs = new HorizontalStrip(strip);
        final View show = lit;
        if (show != null) hs.post(() -> hs.scrollTo(Math.max(0, show.getLeft() - dp(40)), 0));
        return hs;
    }

    /** A row that scrolls sideways if it is wider than the panel (a narrow phone). */
    private final class HorizontalStrip extends android.widget.HorizontalScrollView {
        HorizontalStrip(View child) { super(LauncherActivity.this); setHorizontalScrollBarEnabled(false); setFocusable(false); addView(child); }
    }

    /* ---- controls ----------------------------------------------------------- */

    /** The on-screen controls as the game draws them (touch_controls.c), at the chosen size and opacity. */
    private final class TouchPreview extends View {
        /* x, y (fractions of the frame), radius (of the height), kind: 0 face, 1 d-pad, 2 pill, 3 stick */
        private final float[][] L = {
            { 0.885f, 0.83f, 0.065f, 0 }, { 0.950f, 0.69f, 0.065f, 0 }, { 0.820f, 0.69f, 0.065f, 0 }, { 0.885f, 0.55f, 0.065f, 0 },
            { 0.330f, 0.73f, 0.050f, 1 }, { 0.330f, 0.91f, 0.050f, 1 }, { 0.285f, 0.82f, 0.050f, 1 }, { 0.375f, 0.82f, 0.050f, 1 },
            { 0.060f, 0.10f, 0.055f, 2 }, { 0.060f, 0.26f, 0.055f, 2 }, { 0.940f, 0.10f, 0.055f, 2 }, { 0.940f, 0.26f, 0.055f, 2 },
            { 0.400f, 0.06f, 0.035f, 2 }, { 0.600f, 0.06f, 0.035f, 2 }, { 0.500f, 0.06f, 0.030f, 2 },
            { 0.060f, 0.42f, 0.045f, 2 }, { 0.940f, 0.42f, 0.045f, 2 },
            { 0.130f, 0.72f, 0.140f, 3 }, { 0.700f, 0.80f, 0.080f, 4 } };
        private final int[] FACE = { 0xFF7DA8FF, 0xFFFF6B6B, 0xFFF29CE0, 0xFF70E0B5 };
        private float size, opacity, inset, raise;
        private boolean enabled, always;
        private final boolean[] hide = new boolean[19];
        TouchPreview() { super(LauncherActivity.this); refreshSettings(); }
        void refreshSettings() {
            Map<String, String> values = store.settings();
            size = previewPercent(values, "RACACIT_TOUCH_SIZE");
            opacity = previewPercent(values, "RACACIT_TOUCH_OPACITY");
            enabled = !"0".equals(values.get("RACACIT_TOUCH"));
            always = "2".equals(values.get("RACACIT_TOUCH"));
            inset = previewPercent(values, "RACACIT_TOUCH_INSET");
            raise = previewPercent(values, "RACACIT_TOUCH_RAISE");
            /* the groups touch_controls.c hides: d-pad 4-7, shoulders 8-11, menu row 12-14, stick clicks 15-16 */
            boolean dpad = !"0".equals(values.get("RACACIT_TOUCH_DPAD")), sh = !"0".equals(values.get("RACACIT_TOUCH_SHOULDERS"));
            boolean menu = !"0".equals(values.get("RACACIT_TOUCH_MENU")), clicks = !"0".equals(values.get("RACACIT_TOUCH_CLICKS"));
            boolean fire = !"0".equals(values.get("RACACIT_TOUCH_FIRE"));
            for (int i = 0; i < hide.length; i++)
                hide[i] = (i >= 4 && i <= 7 && !dpad) || (i >= 8 && i <= 11 && !sh) || (i >= 12 && i <= 14 && !menu) || (i >= 15 && i <= 16 && !clicks) || (i == 18 && !fire);
            invalidate();
        }
        private float previewPercent(Map<String, String> values, String key) {
            Def d = def(key);
            String raw = values.get(key);
            try {
                float percent = Float.parseFloat(raw == null ? d.def : raw);
                if (Float.isFinite(percent) && percent >= 0 && percent <= 200) return percent / 100f;
            } catch (NumberFormatException ignored) { }
            return Float.parseFloat(d.def) / 100f;
        }
        @Override protected void onDraw(android.graphics.Canvas cv) {
            /* a 16:9 frame that fits the view, centred, with a line for the caption under it */
            float room = getHeight() - dp(24);
            float w = Math.min(getWidth(), room * 16f / 9f), h = w * 9f / 16f, left = (getWidth() - w) / 2, top = (room - h) / 2;
            android.graphics.RectF fr = new android.graphics.RectF(left, top, left + w, top + h);
            cv.drawRect(fr, Acit.fill(0xFF1B2E48));
            cv.drawRect(fr, Acit.stroke(Acit.EDGE_DIM, dp(1)));
            // Rendering consumes a settings snapshot; it must never reopen settings.txt.
            float op = opacity;
            boolean on = enabled;
            int a = Math.round((on ? 255 : 70) * op);
            for (int i = 0; i < L.length; i++) {
                if (hide[i]) continue;
                float px = L[i][0], py = L[i][1];
                if (i < 12 || i >= 15) px += px < 0.5f ? inset : -inset;
                if (py > 0.5f) py -= raise;
                float cx = left + px * w, cy = top + py * h, r = L[i][2] * h * size;
                int kind = (int) L[i][3];
                android.graphics.Paint fillP = Acit.fill(0x38000000), edgeP = Acit.stroke(0x80FFFFFF, dp(1.2f));
                fillP.setAlpha(Math.round(0x38 * op * (on ? 1 : 0.4f))); edgeP.setAlpha(Math.round(0x80 * op * (on ? 1 : 0.4f)));
                if (kind == 2) {
                    android.graphics.RectF pr = new android.graphics.RectF(cx - r * 1.7f, cy - r, cx + r * 1.7f, cy + r);
                    cv.drawRoundRect(pr, r, r, fillP); cv.drawRoundRect(pr, r, r, edgeP);
                } else {
                    cv.drawCircle(cx, cy, r, fillP); cv.drawCircle(cx, cy, r, edgeP);
                    if (kind == 0) { android.graphics.Paint g = Acit.stroke(FACE[i], dp(1.5f)); g.setAlpha(a); cv.drawCircle(cx, cy, r * 0.45f, g); }
                    if (kind == 3) cv.drawCircle(cx, cy, r * 0.45f, edgeP);
                    if (kind == 4) {   /* fire and look: an amber crosshair */
                        android.graphics.Paint fp = Acit.stroke(Acit.ORANGE, dp(1.6f)); fp.setAlpha(a);
                        cv.drawCircle(cx, cy, r * 0.55f, fp);
                        cv.drawLine(cx, cy - r * 0.85f, cx, cy - r * 0.35f, fp); cv.drawLine(cx, cy + r * 0.35f, cx, cy + r * 0.85f, fp);
                        cv.drawLine(cx - r * 0.85f, cy, cx - r * 0.35f, cy, fp); cv.drawLine(cx + r * 0.35f, cy, cx + r * 0.85f, cy, fp);
                    }
                }
            }
            android.graphics.Paint t = Acit.text(DIM, dp(14), Acit.bodyFace());
            Acit.fitText(cv, !on ? "On-screen controls are off" : always ? "On-screen controls, always shown" : "On-screen controls (hidden while a controller is connected)",
                    getWidth() / 2f, top + h + dp(18), getWidth(), t);
        }
    }

    private static final int[][] PAD_ROWS = {
        { GlyphCore.CROSS }, { GlyphCore.CIRCLE }, { GlyphCore.SQUARE }, { GlyphCore.TRIANGLE },
        { GlyphCore.L1 }, { GlyphCore.R1 }, { GlyphCore.L2 }, { GlyphCore.R2 }, { GlyphCore.L3 }, { GlyphCore.R3 },
        { GlyphCore.SELECT }, { GlyphCore.START } };
    private static final String[][] PAD_TEXT = {
        { "Cross", "A (bottom)" }, { "Circle", "B (right)" }, { "Square", "X (left)" }, { "Triangle", "Y (top) -- hold for Quick Select" },
        { "L1", "Left bumper" }, { "R1", "Right bumper" }, { "L2", "Left trigger" }, { "R2", "Right trigger" },
        { "L3", "Press the left stick" }, { "R3", "Press the right stick" }, { "Select", "View / Back / Select -- the map" },
        { "Start", "Menu / Start -- pause, skip cutscenes" } };
    /* v97 keeps L1 for look and remaps L2 to strafe; contextual actions also apply. */
    private static final String[] SWAPPED = { "Left trigger -- strafe", "Right trigger -- fire", "Left bumper -- look", "Right bumper" };

    /**
     * Controls: the connected controllers (Bluetooth or USB; tap one to feel its rumble), a way to pair
     * one, then the controller, touch and keyboard settings with the touch layout's preview. The detail
     * panel has the live button test and what each PS3 button is on your controller. Rebuilt when a
     * controller connects or goes (padListener).
     */
    private void showControls() {
        final boolean swap = !"0".equals(value(def("RACACIT_DS_SWAP")));
        LinearLayout[] s = screen("Controls", "Controller and touch", this::showHome);
        redraw = this::showControls;   /* rebuilt as it is when the window changes size */
        padScreen = this::showControls;
        LinearLayout info = s[0], col = s[1];
        List<InputDevice> pads = controllerDevices();

        heading(col, "Controllers");
        if (pads.isEmpty()) {
            Acit.Button none = button("No controller connected", "The game uses the on-screen touch controls", null);
            none.enabled = false;
            none.setFocusable(false);
            col.addView(none);
        } else for (InputDevice d : pads) col.addView(button(d.getName(), padDetail(d), () -> rumble(d)));
        col.addView(button("Pair a Bluetooth controller", "Opens Android's Bluetooth settings", this::openBluetooth));

        heading(col, "Controller");
        addControlRows(col, swap, "RACACIT_DS_SWAP", "RACACIT_DS_RUMBLE", "RACACIT_PROMPT_SWAP");
        heading(col, "Touch controls");
        addControlRows(col, swap, "RACACIT_TOUCH", "RACACIT_TOUCH_SIZE", "RACACIT_TOUCH_OPACITY", "RACACIT_TOUCH_CAM_SENS",
                "RACACIT_TOUCH_FIRE", "RACACIT_TOUCH_INSET", "RACACIT_TOUCH_RAISE", "RACACIT_TOUCH_DPAD", "RACACIT_TOUCH_SHOULDERS", "RACACIT_TOUCH_CLICKS", "RACACIT_TOUCH_MENU");
        TouchPreview pv = new TouchPreview();
        pv.setTag("preview");
        pv.setFocusable(false);
        LinearLayout.LayoutParams pl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(200));
        pl.topMargin = dp(4); pl.bottomMargin = dp(10);
        col.addView(pv, pl);
        heading(col, "Keyboard and mouse");
        addControlRows(col, swap, "RACACIT_KBM", "RACACIT_MOUSE_SENS", "RACACIT_MOUSE_INVERT_Y");
        col.addView(button("Keyboard bindings", "Assign keyboard keys; restore any or all defaults", this::showKeyboardBindings));

        /* the detail panel: the button test and the map */
        info.addView(text("Button test", 13, Acit.EDGE, true));
        tester = text(pads.isEmpty() ? "Connect a controller, then press any button or move a stick."
                : "Press any button or move a stick.", 18, Acit.ORANGE, false);
        tester.setPadding(0, dp(4), 0, dp(12));
        info.addView(tester);
        info.addView(text("PS3 button  →  on your controller", 13, Acit.EDGE, true));
        for (int i = 0; i < PAD_ROWS.length; i++) {
            LinearLayout r = new LinearLayout(this);
            r.setOrientation(LinearLayout.HORIZONTAL);
            r.setGravity(Gravity.CENTER_VERTICAL);
            r.setPadding(0, dp(4), 0, dp(4));
            Bitmap g = glyph(PAD_ROWS[i][0]);
            if (g != null) {
                ImageView iv = new ImageView(this); iv.setImageBitmap(g);
                int h = dp(22); r.addView(iv, new LinearLayout.LayoutParams(Math.round(h * (float) g.getWidth() / Math.max(1, g.getHeight())), h));
            }
            TextView a = text(PAD_TEXT[i][0], 15, Acit.TEXT, true); a.setPadding(dp(8), 0, 0, 0);
            r.addView(a, new LinearLayout.LayoutParams(dp(96), ViewGroup.LayoutParams.WRAP_CONTENT));
            String where = PAD_TEXT[i][1];
            if (swap && i >= 4 && i <= 7) where = SWAPPED[i - 4];   /* Fire button: R2 (Rift Apart layout) */
            TextView w = text(where, 15, DIM, false);
            r.addView(w, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            info.addView(r);
        }
        TextView more = text("\nSticks: left moves, right turns the camera. Nintendo-layout pads use the positions above "
                + "(the bottom button is Cross). L3 + R3 together opens the game's Settings menu.", 15, DIM, false);
        more.setLineSpacing(0, 1.08f);
        info.addView(more);
        focusFirst(col);
    }

    /** The given Controls settings as rows; the fire layout redraws the map, the touch ones the preview. */
    private void addControlRows(LinearLayout col, boolean swap, String... keys) {
        for (String k : keys) {
            final Def d = def(k);
            if (d == null) continue;
            col.addView(new SettingRow(d, null) {
                @Override void changed() {
                    if (d.key.equals("RACACIT_DS_SWAP")) showControls();   /* the mapping table changes */
                    else {
                        TouchPreview preview = (TouchPreview) root.findViewWithTag("preview");
                        if (preview != null) preview.refreshSettings();
                    }
                }
            });
        }
    }

    /** Connected game controllers. */
    private List<InputDevice> controllerDevices() {
        List<InputDevice> out = new ArrayList<>();
        List<String> names = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if (((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) && !names.contains(d.getName())) {
                names.add(d.getName()); out.add(d);
            }
        }
        return out;
    }

    /** "Built-in controls  ·  battery 80%  ·  tap to test rumble", as far as Android tells (it does not say Bluetooth or USB). */
    private String padDetail(InputDevice d) {
        List<String> parts = new ArrayList<>();
        try { if (Build.VERSION.SDK_INT >= 29 && !d.isExternal()) parts.add("Built-in controls"); } catch (Throwable ignored) { }
        try {
            if (Build.VERSION.SDK_INT >= 31) {
                android.hardware.BatteryState b = d.getBatteryState();
                if (b != null && b.isPresent() && !Float.isNaN(b.getCapacity()))
                    parts.add("battery " + Math.round(b.getCapacity() * 100) + "%");
            }
        } catch (Throwable ignored) { }
        parts.add(hasRumble(d) ? "tap to test rumble" : "no rumble reported");
        return TextUtils.join("  ·  ", parts);
    }

    @SuppressWarnings("deprecation")
    private static boolean hasRumble(InputDevice d) {
        try { android.os.Vibrator v = d.getVibrator(); return v != null && v.hasVibrator(); } catch (Throwable t) { return false; }
    }

    @SuppressWarnings("deprecation")
    private void rumble(InputDevice d) {
        try {
            android.os.Vibrator v = d.getVibrator();
            if (v != null && v.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 26) v.vibrate(android.os.VibrationEffect.createOneShot(350, 255));
                else v.vibrate(350);
                if (tester != null) tester.setText("Rumble sent to " + d.getName() + ".");
                return;
            }
        } catch (Throwable ignored) { }
        toast(d.getName(), "Android reports no rumble motor for this controller. Some Bluetooth controllers rumble only in "
                + "the game, through its own driver.");
    }

    private void openBluetooth() {
        try { startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)); }
        catch (Exception e) {
            try { startActivity(new Intent(Settings.ACTION_SETTINGS)); }
            catch (Exception e2) { toast("Bluetooth", "Open Android's Settings > Connected devices to pair a controller."); }
        }
    }

    private void showKeyboardBindings() {
        LinearLayout[] s = screen("Controls", "Keyboard bindings", this::showControls);
        redraw = this::showKeyboardBindings;   /* rebuilt as it is when the window changes size */
        infoLine(s[0], "Format", "Enter up to four SDL key names separated by commas, such as Space,Return or Left Shift,F. " +
                "Use none to unbind a button; Restore default removes its override. Changes apply on the next game launch.", Acit.TEXT);
        infoLine(s[0], "Keyboard play", "The same key may trigger several buttons. R1 (fire) and L1 (strafe) default to mouse buttons; " +
                "assign keyboard keys here to use them without a mouse. These bindings do not remap a controller or touch pad.", DIM);
        infoLine(s[0], "Mouse", "Enable Keyboard + mouse, then click the game view to capture the mouse camera. " +
                "F1 releases the cursor and opens Settings. Click the game view again after focus loss. Mouse1 through Mouse5 " +
                "and WheelUp/WheelDown retain the v97 mappings. Sensitivity and invert Y are in Controls.", DIM);
        for (String[] row : KeyboardBindings.ALL) {
            String key = "RACACIT_BIND_" + row[0];
            String value = store.setting(key, row[2]);
            s[1].addView(button(row[1], value.isEmpty() ? row[2] : value, () -> editKeyboardBinding(row)));
        }
        s[1].addView(button("Restore all defaults", "Reset the listed buttons; keep other settings and unknown custom bindings", () ->
                dialog("Restore keyboard defaults?", "Overrides for the listed game buttons will be removed.", new String[] { "Restore", "Cancel" }, new Runnable[] {
                        () -> { try { store.putSettings(KeyboardBindings.defaults()); showKeyboardBindings(); }
                                catch (Exception e) { toast("Keyboard bindings", e.getMessage()); } }, null })));
        s[1].addView(button("Back", null, this::showControls));
        focusFirst(s[1]);
    }

    private void editKeyboardBinding(String[] row) {
        String key = "RACACIT_BIND_" + row[0];
        android.widget.EditText input = new android.widget.EditText(this);
        input.setSingleLine(true);
        input.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setText(store.setting(key, row[2]));
        input.setSelectAllOnFocus(true);
        android.app.AlertDialog editor = new android.app.AlertDialog.Builder(this)
                .setTitle(row[1])
                .setMessage("Up to four key names, separated by commas. Default: " + row[2] + ". Use none to unbind.")
                .setView(input).setPositiveButton("Save", null).setNegativeButton("Cancel", null)
                .setNeutralButton("Restore default", (d, which) -> {
                    try { store.putSetting(key, null); showKeyboardBindings(); }
                    catch (Exception e) { toast("Keyboard bindings", e.getMessage()); }
                }).create();
        editor.setOnShowListener(d -> editor.getButton(android.app.AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            try {
                String value = KeyboardBindings.normalize(input.getText().toString());
                store.putSetting(key, value != null && value.equals(row[2]) ? null : value);
                editor.dismiss(); showKeyboardBindings();
            } catch (Exception e) { input.setError(e.getMessage()); }
        }));
        editor.show();
        input.requestFocus();
        if (editor.getWindow() != null) editor.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
    }

    private static String keyName(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return "Cross (A)";
            case KeyEvent.KEYCODE_BUTTON_B: return "Circle (B)";
            case KeyEvent.KEYCODE_BUTTON_X: return "Square (X)";
            case KeyEvent.KEYCODE_BUTTON_Y: return "Triangle (Y)";
            case KeyEvent.KEYCODE_BUTTON_L1: return "L1";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R1";
            case KeyEvent.KEYCODE_BUTTON_L2: return "L2";
            case KeyEvent.KEYCODE_BUTTON_R2: return "R2";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
            case KeyEvent.KEYCODE_BUTTON_SELECT: return "Select";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start";
            case KeyEvent.KEYCODE_BUTTON_MODE: return "Home / PS (used by the system)";
            case KeyEvent.KEYCODE_DPAD_UP: return "D-pad up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "D-pad down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "D-pad left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "D-pad right";
            default: return KeyEvent.keyCodeToString(code).replace("KEYCODE_", "");
        }
    }

    /* ---- help --------------------------------------------------------------- */

    private static final String[][] HELP = {
        { "Getting started",
          "1. Put your own USA BCUS98124 game folder or ISO on local device storage, about 25 GB.\n"
          + "2. Import game: copy the folder, use it where it is, or import the ISO (select your own .dkey when requested). "
          + "Add your decrypted EBOOT.elf separately; either step can come first.\n"
          + "3. Controls: connect a controller, or play with the on-screen touch controls.\n"
          + "4. Settings: start with Low, then raise quality if frame times remain steady. Custom resolution spans 50% to 400%.\n"
          + "5. Play. The first visit to each place builds shaders and can stutter once.\n\n"
          + "While you play, the game's own Settings menu has the same graphics options as the Windows build: tap the "
          + "menu button between Select and Start on the touch controls, press L3 + R3 together on a controller, or "
          + "GRAPHICS on the Thor's companion. Changes there are saved for next time too." },
        { "Performance tips",
          "- Start with Low on an unfamiliar device; raise quality only if frame times stay steady during demanding scenes and longer sessions.\n"
          + "- CPU usage: keep it on Efficient on a phone; Maximum mostly makes heat.\n"
          + "- Keep the device cool: a hot phone lowers its clocks (the companion's Status page shows the heat state on a Thor).\n"
          + "- Performance hints should stay on (Android 13+).\n"
          + "- Close other apps; leave the charger off if the device gets very hot while charging.\n"
          + "- The first run of each area compiles shaders: give it a minute, later runs are smoother." },
        { "Dual screen (AYN Thor)",
          "Enable dual-screen mode on the device; the game opens on the main (top) screen and the companion opens below. "
          + "Quick Select: touch and hold the ring, slide toward a direction, then release to select. "
          + "Use the page controls while the ring is open; return to the centre to cancel. The upper-screen game wheel shows your actual inventory. "
          + "Actions provides map, pause, confirm, back and graphics controls. Status shows sampled frame rate, CPU/GPU readings and heat. "
          + "Clock is a quiet page for normal play. A first touch after dimming only wakes the screen. "
          + "Settings > Dual screen includes a preview and optional performance recording." },
        { "Files and folders",
          "Everything lives in this app's private external files folder (the Logs page shows its exact path):\n"
          + "PS3_GAME/ the imported game - settings/settings.txt your settings - mods/ mods - dev_hdd0/ the PS3 hard drive with your saves - "
          + "backups/ save backups (one per Play, the newest 8) - perf_log.csv the companion's performance log - log.txt when logging is on." },
        { "Something is wrong",
          "- \"Game data not found\": import again and pick the folder that contains PS3_GAME.\n"
          + "- \"EBOOT.elf is still encrypted\": use the decrypted EBOOT.elf from your dump.\n"
          + "- A controller does nothing: open Controls and press its buttons; if they show up there, they work in the game.\n"
          + "- To report a problem: Diagnostics > turn logging on, play until it happens, then send log.txt from the files folder." },
    };

    private void showHelp(int topic) {
        LinearLayout[] s = screen("Help", HELP[topic][0], this::showHome);
        redraw = () -> showHelp(topic);   /* rebuilt as it is when the window changes size */
        prevTab = () -> showHelp((topic + HELP.length - 1) % HELP.length);
        nextTab = () -> showHelp((topic + 1) % HELP.length);
        LinearLayout info = s[0], col = s[1];
        TextView body = text(HELP[topic][1], 15, Acit.TEXT, false);
        body.setLineSpacing(0, 1.15f);
        info.addView(body);
        String dev = Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                + (Build.VERSION.SDK_INT >= 31 ? ", " + Build.SOC_MODEL : "");
        info.addView(text("\nThis device: " + dev, 14, DIM, false));
        Acit.Button focus = null;
        for (int i = 0; i < HELP.length; i++) {
            final int k = i;
            Acit.Button b = button(HELP[i][0], null, () -> showHelp(k));
            b.accent = i == topic;
            if (i == topic) focus = b;
            col.addView(b);
        }
        col.addView(button("Run the setup guide", null, () -> showGuide(0)));
        col.addView(button("Back", null, this::showHome));
        if (focus != null) focus.requestFocus();
    }

    /* ---- mods --------------------------------------------------------------- */

    private void showMods() {
        LinearLayout[] s = screen("Files and settings for your game", "Mods", this::showHome);
        redraw = this::showMods;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        infoLine(info, "How", "Select a mod to turn it on or off. A mod replaces game files: mods/<id>/files/ is laid out like "
                 + "the game folder (PS3_GAME/USRDIR/...). It can also provide mod.json settings, including a settings-only mod. "
                 + "Mods turned on later win over earlier ones and override your settings on the next launch. Your saved settings remain intact.", Acit.TEXT);
        infoLine(info, "Folder", store.modsDir().getPath(), DIM);
        for (Store.Mod m : store.mods()) {
            boolean on = store.modEnabled(m);
            String sub = (on ? "ON" : "OFF") + (m.version.isEmpty() ? "" : "  v" + m.version) + (m.author.isEmpty() ? "" : "  " + m.author)
                    + (m.problem != null ? "  -- " + m.problem : "");
            Acit.Button b = button(m.name, sub, null);
            b.accent = on;
            b.action = () -> {
                try { store.setModEnabled(m, !store.modEnabled(m)); } catch (Exception e) { toast("Mods", e.getMessage()); return; }
                showMods();
            };
            col.addView(b);
            if (!m.description.isEmpty()) infoLine(info, m.name, m.description, DIM);
        }
        col.addView(button("Install a mod", "From a .zip with mod.json", () -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip");
            startActivityForResult(i, REQ_MOD);
        }));
        col.addView(button("Back", null, this::showHome));
        focusFirst(col);
    }

    /* ---- saves -------------------------------------------------------------- */

    private void showSaves() {
        LinearLayout[] s = screen("Saves", "Your saves", this::showHome);
        redraw = this::showSaves;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        DateFormat df = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT);
        List<File> slots = store.saveSlots();
        if (slots.isEmpty()) infoLine(info, "Slots", "No saves yet. To carry on from the Windows build, export its savedata folder "
                + "as a .zip and use Import saves.", DIM);
        for (File f : slots) infoLine(info, f.getName(), "Saved " + df.format(new Date(new File(f, "GAME.SAV").lastModified())), Acit.TEXT);
        File[] bk = store.backupList();
        infoLine(info, "Backups", bk.length == 0 ? "None yet (one is made each time you press Play)" : bk.length + ", newest " + bk[bk.length - 1].getName(), DIM);
        infoLine(info, "Folder", store.saves().getPath(), DIM);
        col.addView(button("Export saves", "To a .zip you choose", () -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip");
            i.putExtra(Intent.EXTRA_TITLE, "acit-saves.zip");
            startActivityForResult(i, REQ_SAVE_OUT);
        }));
        col.addView(button("Import saves", "From a .zip (your saves are backed up first)", () -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/zip");
            startActivityForResult(i, REQ_SAVE_IN);
        }));
        col.addView(button("Back up now", "Keep a backup on this device", () -> {
            if (busy || !canChangeGameFiles()) return;
            busy = true;
            new Thread(() -> {
                String result;
                try { File made = store.backupSaves("manual"); result = made == null ? "No saves to back up." : "Backed up to " + made.getName(); }
                catch (Exception e) { result = "Backup failed: " + e.getMessage(); }
                final String message = result;
                ui.post(() -> { busy = false; if (!gone()) { showSaves(); toast("Saves", message); } });
            }).start();
        }));
        if (bk.length > 0) col.addView(button("Restore a backup", "Choose one", this::chooseBackup));
        col.addView(button("Back", null, this::showHome));
        focusFirst(col);
    }

    private void chooseBackup() {
        if (busy || !canChangeGameFiles()) return;
        File[] bk = store.backupList();
        String[] names = new String[bk.length + 1];
        Runnable[] acts = new Runnable[bk.length + 1];
        DateFormat df = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT);
        for (int i = 0; i < bk.length; i++) {   /* newest first */
            final File f = bk[i];
            names[bk.length - 1 - i] = df.format(new Date(f.lastModified())) + "  ·  " + f.getName();
            acts[bk.length - 1 - i] = () -> new Thread(() -> {
                String r;
                try (InputStream in = new java.io.FileInputStream(f)) { store.importSaves(in); r = "Restored " + f.getName() + ". Your previous saves were backed up first."; }
                catch (Exception e) { r = "Restore failed: " + e.getMessage(); }
                final String m = r;
                ui.post(() -> { showSaves(); toast("Saves", m); });
            }).start();
        }
        names[bk.length] = "Cancel";
        dialog("Restore which backup?", "Your saves now are backed up first, so a restore can be undone.", names, acts);
    }

    /* ---- diagnostics -------------------------------------------------------- */

    private void showLogs() {
        LinearLayout[] s = screen("Diagnostics", "Logs and performance", this::showHome);
        redraw = this::showLogs;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        boolean on = store.debugFlag().exists();
        infoLine(info, "Logging", on ? "The game writes log.txt (a little slower)" : "Off: the game logs to logcat only", on ? Acit.ORANGE : Acit.TEXT);
        infoLine(info, "Log file", store.log().getPath() + (store.log().exists() ? "  (" + Store.size(store.log().length()) + ")" : ""), DIM);
        File perf = new File(store.base, "perf_log.csv");
        infoLine(info, "Performance log", perf.exists() ? perf.getPath() + "  (" + Store.size(perf.length()) + ", the last run)"
                : "Optional: Settings > Dual screen > Performance recording. Off by default.", DIM);
        infoLine(info, "Sending them", "Last game log shows what the game printed in its last run and shares it as text. "
                + "Over USB: Android/data/" + getPackageName() + "/files.", DIM);
        // Off by default (v113-android.36): the measuring builds .9-.35 had it on, and the sampler
        // alone took ~7% of the CPU time the game used (android_profiler.cpp).
        final boolean prof = "1".equals(store.setting("RACACIT_PROFILE", "0"));
        File pf = new File(store.base, "profile.txt");
        infoLine(info, "Performance profile", (prof ? "On: while the game runs it writes where its time goes every 30 s, and timing lines go into the game log (costs a few percent). " : "Off. Turn it on, play the slow part for a minute, quit, then Show the profile and share it. ")
                + (pf.exists() ? "Last profile: " + DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(new Date(pf.lastModified())) + "." : ""),
                prof ? Acit.ORANGE : DIM);
        col.addView(button("Last game log", "What the game printed -- view or share it", this::showGameLog));
        final boolean eng = "1".equals(store.setting("RSX_ENG_STATS", "0"));
        col.addView(button(eng ? "Renderer stats: on" : "Renderer stats: off", eng ? "Turn them off" : "Where the renderer waits on the GPU -- in the game log", () -> {
            try { store.putSetting("RSX_ENG_STATS", eng ? null : "1"); } catch (Exception e) { toast("Diagnostics", e.getMessage()); }
            showLogs();
        }));
        col.addView(button(prof ? "Performance profile: on" : "Performance profile: off", prof ? "Turn it off" : "Where the game's time goes -- turn on for the next run", () -> {
            try { store.putSetting("RACACIT_PROFILE", prof ? "0" : "1"); } catch (Exception e) { toast("Diagnostics", e.getMessage()); }
            showLogs();
        }));
        if (pf.exists()) col.addView(button("Show the profile", "View or share the last run's report", this::showProfile));
        col.addView(button(on ? "Turn logging off" : "Turn logging on", null, () -> {
            if (store.debugFlag().exists()) Store.deleteTree(store.debugFlag()); else store.debugFlag().mkdirs();
            showLogs();
        }));
        col.addView(button("Back", null, this::showHome));
        focusFirst(col);
    }

    /** The last run's log: a summary of what matters (device, GPU, errors, how far it got), then its end. */
    private void showGameLog() {
        LinearLayout[] s = screen("Diagnostics", "Last game log", this::showLogs);
        redraw = this::showGameLog;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        TextView t = text("Reading the log...", 11, Acit.TEXT, false);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        info.addView(t);
        final String[] share = { "" };
        col.addView(button("Share the log", "As text: a message, a note, an email", () -> {
            if (share[0].isEmpty()) return;
            Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                    .putExtra(Intent.EXTRA_SUBJECT, "A Crack in Time (Android) -- game log")
                    .putExtra(Intent.EXTRA_TEXT, share[0]);
            startActivity(Intent.createChooser(send, "Share the game log"));
        }));
        col.addView(button("Refresh", null, this::showGameLog));
        col.addView(button("Back", null, this::showLogs));
        focusFirst(col);
        new Thread(() -> {
            String all = readGameLog();
            String sum = summarizeLog(all);
            share[0] = sum + "\n\n---- the log's last part ----\n" + all.substring(Math.max(0, all.length() - 120000));
            ui.post(() -> t.setText(sum));
        }).start();
    }

    /** The built-in profiler's report (android_profiler.cpp): the last three 30 s windows, newest first. */
    private void showProfile() {
        LinearLayout[] s = screen("Diagnostics", "Performance profile", this::showLogs);
        redraw = this::showProfile;   /* rebuilt as it is when the window changes size */
        LinearLayout info = s[0], col = s[1];
        String text;
        File pf = new File(store.base, "profile.txt");
        try { text = new String(java.nio.file.Files.readAllBytes(pf.toPath()), java.nio.charset.StandardCharsets.UTF_8); }
        catch (Exception e) { text = "No profile yet: turn Performance profile on, play for a minute, quit, and come back."; }
        String head = "Device: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                + (Build.VERSION.SDK_INT >= 31 ? ", " + Build.SOC_MODEL : "");
        try { head += ", app " + getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) { }
        final String full = head + "\n" + presetName() + " preset, resolution " + value(def("RACACIT_RES_SCALE")) + "x\n\n" + text;
        TextView t = text(full, 11, Acit.TEXT, false);
        t.setTypeface(android.graphics.Typeface.MONOSPACE);
        t.setTextIsSelectable(true);
        info.addView(t);
        col.addView(button("Share the profile", "As text: a message, a note, an email", () -> {
            Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain")
                    .putExtra(Intent.EXTRA_SUBJECT, "A Crack in Time (Android) -- performance profile")
                    .putExtra(Intent.EXTRA_TEXT, full);
            startActivity(Intent.createChooser(send, "Share the profile"));
        }));
        col.addView(button("Back", null, this::showLogs));
        focusFirst(col);
    }

    /** log.txt when logging is on, else the game's own lines from logcat (an app may read its own). */
    private String readGameLog() {
        StringBuilder b = new StringBuilder();
        File lf = store.log();
        if (store.debugFlag().exists() && lf.exists() && lf.length() > 0) {
            try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(lf, "r")) {
                long start = Math.max(0, lf.length() - 600000);
                r.seek(start);
                byte[] buf = new byte[(int) (lf.length() - start)];
                r.readFully(buf);
                /* A native crash's backtrace is not in log.txt: Android's crash buffer
                 * has it. First, dated, so the log's own end stays the last lines. */
                try {
                    Process p = new ProcessBuilder("logcat", "-d", "-b", "crash", "-v", "time").redirectErrorStream(true).start();
                    try (java.io.BufferedReader cr = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                        String line; boolean first = true;
                        while ((line = cr.readLine()) != null && b.length() < 500000) {
                            if (first) { b.append("---- Android's crash buffer (dated; may be earlier runs) ----\n"); first = false; }
                            b.append(line).append('\n');
                        }
                        if (!first) b.append("---- log.txt ----\n");
                    }
                } catch (Exception ignored) { }
                b.append(new String(buf, java.nio.charset.StandardCharsets.UTF_8));
                return b.toString();
            } catch (Exception e) { b.append("(log.txt: ").append(e.getMessage()).append(")\n"); }
        }
        try {
            /* -b main,system,crash: a native crash's backtrace (DEBUG) is in the crash buffer */
            Process p = new ProcessBuilder("logcat", "-d", "-b", "main,system,crash", "-v", "time", "-s", "racacit:V", "SDL:V", "AndroidRuntime:E",
                    "DEBUG:V", "libc:F", "vulkan:V", "Adreno:W", "mali:W").redirectErrorStream(true).start();
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = r.readLine()) != null) {
                    b.append(line).append('\n');
                    if (b.length() > 2000000) b.delete(0, 500000);
                }
            }
        } catch (Exception e) { b.append("(logcat: ").append(e.getMessage()).append(")\n"); }
        return b.toString();
    }

    private String summarizeLog(String all) {
        StringBuilder s = new StringBuilder();
        s.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(", Android ").append(Build.VERSION.RELEASE);
        if (Build.VERSION.SDK_INT >= 31) s.append(", ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL);
        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        s.append("\nScreen: ").append(dm.widthPixels).append('x').append(dm.heightPixels);
        try { s.append("\nApp: ").append(getPackageManager().getPackageInfo(getPackageName(), 0).versionName); } catch (Exception ignored) { }
        String[] lines = all.split("\n");
        s.append("\nLog lines: ").append(lines.length);
        if (lines.length < 3) { s.append("\n\nThe log is empty: start the game once, then come back here."); return s.toString(); }
        java.util.regex.Pattern key = java.util.regex.Pattern.compile(
                "\\[android\\]|\\[RSX vulkan\\]|vulkan|Vulkan|\\[rsx\\]|error|Error|ERROR|fail|FAIL|fatal|FATAL|abort|signal |"
                + "DIALOG|Insufficient|not found|missing|exception|Exception|SIGSEGV|SIGABRT|backtrace|performance hints|\\[prof\\]|"
                + "\\[cellGame\\] title|\\[boot\\]|controller [0-9]|Out of memory|lost|exiting");
        s.append("\n\n---- what matters ----\n");
        int n = 0; String lastStats = null;
        for (String l : lines) {
            if (l.contains("[rsx-stats]")) { lastStats = l; continue; }
            if (key.matcher(l).find() && n < 80) { s.append(clip(l)).append('\n'); n++; }
        }
        if (lastStats != null) s.append("\n---- how far it got (the last frame counters) ----\n").append(clip(lastStats)).append('\n');
        s.append("\n---- the last lines ----\n");
        for (int i = Math.max(0, lines.length - 30); i < lines.length; i++) s.append(clip(lines[i])).append('\n');
        return s.toString();
    }

    private static String clip(String l) { return l.length() > 260 ? l.substring(0, 260) + "..." : l; }

    /* ---- portable launcher migration / local Android updates --------------- */

    private static String portableValue(String key, String value) {
        for (String[] binding : KeyboardBindings.ALL) if (key.equals("RACACIT_BIND_" + binding[0])) {
            try { return value.matches("[A-Za-z0-9 ,.+_\\-]+") ? KeyboardBindings.normalize(value) : null; }
            catch (IllegalArgumentException ignored) { return null; }
        }
        // Only settings presented by this Android launcher may migrate. Never copy paths,
        // launch commands, mod activation, desktop window state or driver/cache overrides.
        for (Def setting : SETTINGS) if (setting.key.equals(key)) {
            for (String allowed : setting.values) if (allowed.equals(value)) return value;
            double min = key.equals("RACACIT_MOUSE_SENS") ? 0.1 : 0.5;
            double max = key.equals("RACACIT_MOUSE_SENS") ? 5 : 4;
            if (key.equals("RACACIT_RES_SCALE") || key.equals("RACACIT_SHADOW_RES") || key.equals("RACACIT_MOUSE_SENS")) {
                try { double n = Double.parseDouble(value); if (Double.isFinite(n) && n >= min && n <= max) return value; }
                catch (NumberFormatException ignored) { }
            }
            return null;
        }
        return null;
    }

    private void chooseDocument(String mime, int request) {
        if (busy || !canChangeGameFiles()) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(mime);
        startActivityForResult(intent, request);
    }

    private void discardMigration() {
        if (pendingMigration != null) { pendingMigration.close(); pendingMigration = null; }
    }

    private void previewCompanion() {
        stopCompanionPreview();
        companionPreview = new Companion(this, true);
        companionPreview.start();
        if (!companionPreview.isShowing()) {
            stopCompanionPreview();
            toast("Second screen required", "Connect or enable the lower display to preview the companion. On an AYN Thor, use dual-screen mode.");
        }
    }

    private void stopCompanionPreview() {
        if (companionPreview != null) { companionPreview.stop(); companionPreview = null; }
    }

    private void leaveMigration() { if (!busy) { discardMigration(); showHome(); } }

    private void showMigration() {
        LinearLayout[] s = screen("Bring your progress with you", "Migrate launcher data", this::leaveMigration);
        redraw = this::showMigration;   /* rebuilt as it is when the window changes size */
        infoLine(s[0], "Your archive", "Select a ZIP you copied to this Android device. Include settings/settings.txt and " +
                "savedata/BCUS98124.../GAME.SAV, or the same savedata folder inside dev_hdd0/home/00000001. A containing launcher folder is supported.", Acit.TEXT);
        infoLine(s[0], "Portable data", "Review first, then import saves and settings separately. Computer paths, executables, shaders, caches and mod activation are excluded. " +
                "Game files and mods use their own import screens. Desktop graphics settings may need lowering for this device.", DIM);
        LauncherMigration.Preview preview = pendingMigration;
        if (preview != null) {
            infoLine(s[0], "Ready to review", preview.slots + " valid save slots · " + preview.settings.size() + " supported settings\n" +
                    preview.ignoredSettings + " settings and " + preview.ignoredFiles + " files excluded", Acit.TEXT_GOOD);
            if (!preview.settings.isEmpty()) {
                StringBuilder values = new StringBuilder();
                for (Map.Entry<String, String> item : preview.settings.entrySet()) values.append(item.getKey()).append(" = ").append(item.getValue()).append('\n');
                infoLine(s[0], "Settings to import", values.toString(), DIM);
            }
            if (preview.slots > 0) s[1].addView(button("Import reviewed saves", "Matching slots are replaced; current saves are backed up first", () -> applyMigration(false)));
            if (!preview.settings.isEmpty()) s[1].addView(button("Import reviewed settings", "Current settings are backed up; other Android settings are kept", () -> applyMigration(true)));
        }
        s[1].addView(button(preview == null ? "Select launcher ZIP" : "Select another ZIP", "Only the document you choose is read", () -> chooseDocument("application/zip", REQ_MIGRATION)));
        s[1].addView(button("Back", null, this::leaveMigration));
        focusFirst(s[1]);
    }

    private void readMigration(Uri uri) {
        if (busy) return;
        discardMigration(); busy = true;
        showDocumentWork("Reviewing launcher archive", "Reading and checking the selected document. Your current data is unchanged.");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                LauncherMigration.Preview preview = LauncherMigration.read(in, getCacheDir(), LauncherActivity::portableValue);
                ui.post(() -> { busy = false; if (gone()) { preview.close(); return; } pendingMigration = preview; showMigration(); });
            } catch (Exception error) {
                ui.post(() -> { busy = false; if (!gone()) { showMigration(); toast("Could not read launcher archive", error.getMessage()); } });
            }
        }, "launcher-migration-review").start();
    }

    private void applyMigration(boolean settings) {
        final LauncherMigration.Preview preview = pendingMigration;
        if (preview == null || busy || !canChangeGameFiles()) return;
        dialog(settings ? "Import these settings?" : "Import these saves?", settings ?
                "The reviewed values replace matching Android settings. A copy of your current settings is kept in settings/before-migration.txt." :
                "The reviewed save slots replace matching slots on this Android device. Current saves are backed up first.",
                new String[]{"Import", "Cancel"}, new Runnable[]{() -> {
                    if (busy || !canChangeGameFiles()) return;
                    busy = true;
                    showDocumentWork("Importing launcher data", "Preparing backups and applying the reviewed data. Keep this app open until the import finishes.");
                    new Thread(() -> {
                        String message;
                        try { if (settings) preview.importSettings(store); else preview.importSaves(store); message = settings ? "Settings imported. Review graphics and memory settings before playing." : "Saves imported. Your previous saves were backed up first."; }
                        catch (Exception error) { message = "Import failed: " + error.getMessage(); }
                        String result = message;
                        ui.post(() -> { busy = false; if (gone()) { preview.close(); return; } showMigration(); toast("Launcher migration", result); });
                    }, "launcher-migration-apply").start();
                }, null});
    }

    private void showUpdate() {
        LinearLayout[] s = screen("Independent Android build", "Update Android app", this::showHome);
        redraw = this::showUpdate;   /* rebuilt as it is when the window changes size */
        infoLine(s[0], "Local APK", "Choose a newer APK you have already downloaded. The launcher checks this app's package, version and signing identity. " +
                "Android verifies the package and asks you to confirm installation. Your game files, saves and settings remain in this app.", Acit.TEXT);
        infoLine(s[0], "Online", onlineRelease != null ? onlineRelease.name + " is on GitHub." : "When the launcher opens it looks for a newer Android release on GitHub (Settings > Updates turns this off).", DIM);
        if (onlineRelease != null && pendingUpdate == null)
            s[1].addView(button("Download " + onlineRelease.name, "From GitHub; Android asks before installing", () -> downloadOnline(onlineRelease)));
        s[1].addView(button("Check for updates now", "Asks GitHub for the newest Android release", () -> checkOnlineUpdate(true)));
        if (pendingUpdate != null) {
            infoLine(s[0], "Ready", pendingUpdate.version + "  ·  build " + pendingUpdate.code + "\nPackage and signing identity match this installation.", Acit.TEXT_GOOD);
            s[1].addView(button("Continue to Android installer", "Android asks before updating the app", this::installUpdate));
        }
        s[1].addView(button("Select newer APK", "Local Android package, up to 512 MB", () -> chooseDocument("application/vnd.android.package-archive", REQ_UPDATE)));
        s[1].addView(button("Back", null, this::showHome));
        focusFirst(s[1]);
    }

    private void readUpdate(Uri uri) {
        if (busy) return;
        pendingUpdate = null; busy = true;
        showDocumentWork("Checking Android update", "Copying the selected APK and checking its package identity. Installation requires your confirmation.");
        new Thread(() -> {
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                OfflineUpdate.Candidate candidate = OfflineUpdate.stage(this, in);
                ui.post(() -> { busy = false; if (!gone()) { pendingUpdate = candidate; showUpdate(); } });
            } catch (Exception error) {
                ui.post(() -> { busy = false; if (!gone()) { showUpdate(); toast("APK not accepted", error.getMessage()); } });
            }
        }, "android-update-review").start();
    }

    private void installUpdate() {
        if (pendingUpdate == null || busy || !canChangeGameFiles()) return;
        if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
            dialog("Allow Android app updates", "Android requires your permission for this app to open an APK installer. " +
                    "After choosing your preference, return here and select Continue to Android installer.",
                    new String[]{"Open Android settings", "Cancel"}, new Runnable[]{() -> {
                        try { startActivityForResult(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())), REQ_INSTALL_PERMISSION); }
                        catch (Exception error) { toast("Android settings", error.getMessage()); }
                    }, null});
            return;
        }
        try {
            Intent installer = new Intent(Intent.ACTION_INSTALL_PACKAGE).setData(pendingUpdate.uri)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            installer.setClipData(android.content.ClipData.newRawUri("Android update", pendingUpdate.uri));
            startActivity(installer);
        } catch (Exception error) { toast("Could not open Android installer", error.getMessage()); }
    }

    private void showDocumentWork(String title, String message) {
        LinearLayout[] s = screen("Please wait", title, () -> { });
        infoLine(s[0], title, message, Acit.TEXT);
    }

    /* ---- results, keys ------------------------------------------------------ */

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        if (busy || !canChangeGameFiles()) return;
        Uri u = data.getData();
        switch (req) {
            case REQ_MIGRATION: readMigration(u); break;
            case REQ_UPDATE: readUpdate(u); break;
            case REQ_COPY: runCopy(u); break;
            case REQ_PLACE: usePlace(u); break;
            case REQ_REPAIR: runRepair(u); break;
            case REQ_EBOOT: importEboot(u); break;
            case REQ_ISO:
                try { getContentResolver().takePersistableUriPermission(u, data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception ignored) { }
                runIso(u, null); break;
            case REQ_DKEY:
                if (pendingIso == null) toast("Select the ISO again", "The previous ISO selection is no longer available.");
                else runIso(pendingIso, u);
                break;
            case REQ_MOD:
                new Thread(() -> {
                    String r;
                    try (InputStream in = getContentResolver().openInputStream(u)) { r = "Installed " + store.installModZip(in) + ". Turn it on in the list."; }
                    catch (Exception e) { r = "Could not install the mod: " + e.getMessage(); }
                    final String m = r;
                    ui.post(() -> { showMods(); toast("Mods", m); });
                }).start();
                break;
            case REQ_SAVE_OUT:
                new Thread(() -> {
                    String r;
                    try (OutputStream o = getContentResolver().openOutputStream(u)) { store.exportSaves(o); r = "Saves exported."; }
                    catch (Exception e) { r = "Export failed: " + e.getMessage(); }
                    final String m = r;
                    ui.post(() -> toast("Saves", m));
                }).start();
                break;
            case REQ_SAVE_IN:
                new Thread(() -> {
                    String r;
                    try (InputStream in = getContentResolver().openInputStream(u)) { store.importSaves(in); r = "Saves imported. Your previous saves were backed up first."; }
                    catch (Exception e) { r = "Import failed: " + e.getMessage(); }
                    final String m = r;
                    ui.post(() -> { showSaves(); toast("Saves", m); });
                }).start();
                break;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        final int code = e.getKeyCode();
        if (tester != null && e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0
                && (KeyEvent.isGamepadButton(code) || code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN
                    || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT)) {
            InputDevice d = e.getDevice();
            tester.setText("Pressed: " + keyName(code) + (d != null ? "   (" + d.getName() + ")" : ""));
        }
        boolean mine = code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BUTTON_L1
                || code == KeyEvent.KEYCODE_BUTTON_R1 || code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_BUTTON_SELECT
                || code == KeyEvent.KEYCODE_BUTTON_Y;
        if (!mine) return super.dispatchKeyEvent(e);
        if (e.getAction() != KeyEvent.ACTION_UP) return true;
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: { View f = getCurrentFocus(); if (f != null) f.performClick(); break; }
            case KeyEvent.KEYCODE_BUTTON_B: if (back != null) back.run(); break;
            case KeyEvent.KEYCODE_BUTTON_L1: if (prevTab != null) prevTab.run(); break;
            case KeyEvent.KEYCODE_BUTTON_R1: if (nextTab != null) nextTab.run(); break;
            case KeyEvent.KEYCODE_BUTTON_START: if (back == null && guideStep < 0) play(); break;
            case KeyEvent.KEYCODE_BUTTON_SELECT: if (!busy && modal == null) showHelp(0); break;
            case KeyEvent.KEYCODE_BUTTON_Y: { View f = getCurrentFocus(); if (modal == null && f instanceof SettingRow) ((SettingRow) f).explain(); break; }
        }
        return true;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (tester != null && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            float lx = e.getAxisValue(MotionEvent.AXIS_X), ly = e.getAxisValue(MotionEvent.AXIS_Y);
            float rx = e.getAxisValue(MotionEvent.AXIS_Z), ry = e.getAxisValue(MotionEvent.AXIS_RZ);
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            if (Math.abs(lx) + Math.abs(ly) + Math.abs(rx) + Math.abs(ry) + lt + rt > 0.15f)
                tester.setText(String.format(Locale.US, "Left stick %+.2f %+.2f   Right stick %+.2f %+.2f\nL2 %.2f   R2 %.2f",
                        lx, ly, rx, ry, lt, rt));
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override public void onBackPressed() { if (back != null) back.run(); else if (!busy) super.onBackPressed(); }

    @Override protected void onStop() { stopCompanionPreview(); super.onStop(); }
    @Override protected void onDestroy() { stopCompanionPreview(); discardEboot(); if (!busy) discardMigration(); super.onDestroy(); }

    @Override protected void onSaveInstanceState(Bundle state) {
        if (pendingIso != null) state.putString("pendingIso", pendingIso.toString());
        super.onSaveInstanceState(state);
    }

    @Override protected void onResume() {
        super.onResume();
        InputManager im = (InputManager) getSystemService(INPUT_SERVICE);
        if (im != null && padListener == null) {
            padListener = new InputManager.InputDeviceListener() {
                @Override public void onInputDeviceAdded(int id) { padChanged(); }
                @Override public void onInputDeviceRemoved(int id) { padChanged(); }
                @Override public void onInputDeviceChanged(int id) { }
            };
            im.registerInputDeviceListener(padListener, ui);
        }
        if (back == null && guideStep < 0 && !busy) showHome();
        else if (onShaders && !busy) showShaders();
        if (!updateChecked) { updateChecked = true; checkOnlineUpdate(false); }
    }

    /* ---- online updates (OnlineUpdate.java) ---------------------------------- */
    private boolean updateChecked;   /* once a launcher session */
    private OnlineUpdate.Release onlineRelease;
    /** When the launcher opens (unless Settings > Updates is off), or from Update app (asked = true: say what was found). */
    private void checkOnlineUpdate(boolean asked) {
        if (!asked && "0".equals(store.setting("RACIT_AUTO_UPDATE", "1"))) return;
        final String mine = version();
        new Thread(() -> {
            OnlineUpdate.Release rel = null; String err = null;
            try { rel = OnlineUpdate.latest(); } catch (Exception e) { err = e.getMessage(); }
            final OnlineUpdate.Release r = rel; final String e = err;
            ui.post(() -> {
                if (gone()) return;
                if (r != null && OnlineUpdate.compare(r.tag, mine) > 0) {
                    onlineRelease = r;
                    if (busy) return;
                    dialog("Update available: " + r.name,
                            (r.notes.isEmpty() ? "" : r.notes + "\n\n") + "Download it now" + (r.size > 0 ? " (" + Store.size(r.size) + ")" : "")
                                    + "? Android asks before installing. Your game files, saves and settings stay.",
                            new String[]{"Download", "Not now", "Turn off update checks"},
                            new Runnable[]{() -> downloadOnline(r), null, () -> {
                                try { store.putSetting("RACIT_AUTO_UPDATE", "0"); } catch (Exception x) { toast("Updates", x.getMessage()); }
                                toast("Updates", "Update checks are off. Settings > Updates turns them back on.");
                            }});
                } else if (asked) {
                    toast("Updates", e != null ? "Could not reach GitHub: " + e : r == null ? "There is no Android release on GitHub yet." : "You have the newest version (" + mine + ").");
                }
            });
        }, "online-update-check").start();
    }

    private void downloadOnline(OnlineUpdate.Release r) {
        if (busy) return;
        pendingUpdate = null; busy = true;
        showDocumentWork("Downloading " + r.name, "Downloading the update from GitHub and checking its package and signing identity. Keep the launcher open.");
        new Thread(() -> {
            try (InputStream in = OnlineUpdate.open(r)) {
                OfflineUpdate.Candidate candidate = OfflineUpdate.stage(this, in);
                ui.post(() -> { busy = false; if (!gone()) { pendingUpdate = candidate; showUpdate(); installUpdate(); } });
            } catch (Exception error) {
                ui.post(() -> { busy = false; if (!gone()) { showHome(); toast("Update not downloaded", error.getMessage()); } });
            }
        }, "online-update-download").start();
    }

    @Override protected void onPause() {
        InputManager im = (InputManager) getSystemService(INPUT_SERVICE);
        if (im != null && padListener != null) im.unregisterInputDeviceListener(padListener);
        padListener = null;
        ui.removeCallbacks(padRefresh);
        super.onPause();
    }

    /** A controller connected or went: the screen that lists them, once things settle (a Bluetooth pad adds several devices). */
    private void padChanged() {
        ui.removeCallbacks(padRefresh);
        ui.postDelayed(padRefresh, 400);
    }
}
