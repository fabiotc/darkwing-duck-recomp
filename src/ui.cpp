#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "bezel.h"
#include "rom_select.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "nesrt/recomp.h"
}

/* Defaults for a new player: everything that changes the game is off; only
 * the bezel and the performance-safe slowdown removal are on. */
static const ui_settings UI_DEFAULTS = {
    80,            /* volume */
    false,         /* integer_scale */
    FILTER_NONE,   /* filter */
    true,          /* remove_slowdown: game and music speed are unchanged */
    false,         /* frame_interp (experimental) */
    false,         /* no_flicker */
    1,             /* state_slot */
    BEZEL_NIGHT_CITY, /* bezel */
    BEZEL_FIT_AUTO, /* bezel_fit */
    false,          /* quick_skip */
    true,           /* fullscreen */
};
ui_settings ui_config = UI_DEFAULTS;
bool ui_cheats_available = true;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static bool g_open;
static std::string g_settings_path;
static int g_request;
static char g_toast[160];
static uint64_t g_toast_until;

int ui_take_request(void) {
    int r = g_request;
    g_request = UI_REQ_NONE;
    return r;
}

void ui_toast(const char *message) {
    snprintf(g_toast, sizeof g_toast, "%s", message);
    g_toast_until = SDL_GetTicks() + 2000;
}

void ui_state_path(int slot, char *out, size_t len) {
    const char *dir = app_data_dir();
    snprintf(out, len, "%sstate%d.sav", dir ? dir : "", slot);
}

/* ---- Settings file: simple "key=value" lines in the user's pref dir --- */

static void load_settings() {
    FILE *f = fopen(g_settings_path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[128];
        int value;
        if (sscanf(line, "%127[^=]=%d", key, &value) != 2) continue;
        if (!strcmp(key, "volume")) ui_config.volume = value < 0 ? 0 : value > 100 ? 100 : value;
        else if (!strcmp(key, "integer_scale")) ui_config.integer_scale = value != 0;
        else if (!strcmp(key, "filter")) ui_config.filter = value >= 0 && value < FILTER_COUNT ? value : 0;
        else if (!strcmp(key, "remove_slowdown")) ui_config.remove_slowdown = value != 0;
        else if (!strcmp(key, "frame_interp")) ui_config.frame_interp = value != 0;
        else if (!strcmp(key, "no_flicker")) ui_config.no_flicker = value != 0;
        else if (!strcmp(key, "bezel")) ui_config.bezel = value >= 0 && value < BEZEL_COUNT ? value : BEZEL_PATTERN;
        else if (!strcmp(key, "quick_skip")) ui_config.quick_skip = value != 0;
        else if (!strcmp(key, "fullscreen")) ui_config.fullscreen = value != 0;
        else if (!strcmp(key, "bezel_fit")) ui_config.bezel_fit = value >= 0 && value < BEZEL_FIT_COUNT ? value : 0;
        else if (!strcmp(key, "state_slot")) ui_config.state_slot = value >= 1 && value <= UI_STATE_SLOTS ? value : 1;
        else if (!strncmp(key, "cheat.", 6))
            for (int i = 0; i < nes_cheat_count; i++)
                if (!strcmp(key + 6, nes_cheat_defs[i].id)) {
                    int max = nes_cheat_defs[i].choice_count ? nes_cheat_defs[i].choice_count : 1;
                    nes_cheats[i] = (uint8_t)(value >= 0 && value <= max ? value : 0);
                }
    }
    fclose(f);
}

static void save_settings() {
    FILE *f = fopen(g_settings_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "volume=%d\ninteger_scale=%d\nfilter=%d\nremove_slowdown=%d\nframe_interp=%d\nno_flicker=%d\n"
               "state_slot=%d\nbezel=%d\nbezel_fit=%d\nquick_skip=%d\nfullscreen=%d\n",
            ui_config.volume, ui_config.integer_scale, ui_config.filter, ui_config.remove_slowdown,
            ui_config.frame_interp, ui_config.no_flicker, ui_config.state_slot, ui_config.bezel, ui_config.bezel_fit, ui_config.quick_skip, ui_config.fullscreen);
    for (int i = 0; i < nes_cheat_count; i++) fprintf(f, "cheat.%s=%d\n", nes_cheat_defs[i].id, nes_cheats[i]);
    fclose(f);
}

/* ---- Public API -------------------------------------------------------- */

bool ui_init(SDL_Window *window, SDL_Renderer *renderer) {
    g_window = window;
    g_renderer = renderer;
    if (char *pref = SDL_GetPrefPath("darkwing-duck-recomp", "darkwing-duck")) {
        g_settings_path = std::string(pref) + "settings.ini";
        SDL_free(pref);
        load_settings();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.WindowPadding = ImVec2(14, 12);
    style.ItemSpacing = ImVec2(8, 8);
    float dpi = SDL_GetWindowDisplayScale(window);
    if (dpi > 0) {
        style.ScaleAllSizes(dpi);
        io.FontGlobalScale = dpi;
    }
    return ImGui_ImplSDL3_InitForSDLRenderer(window, renderer) && ImGui_ImplSDLRenderer3_Init(renderer);
}

void ui_shutdown(void) {
    save_settings();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ui_process_event(const SDL_Event *e) { ImGui_ImplSDL3_ProcessEvent(e); }
bool ui_is_open(void) { return g_open; }

void ui_toggle(void) {
    g_open = !g_open;
    if (!g_open) save_settings();
}

static void help_marker(const char *text) {
    if (!text || !*text) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void begin_frame() {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

static void end_frame() {
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
}

int ui_rom_picker(const char *message, bool can_use_anyway, bool warn, const char *data_folder) {
    int action = UI_PICK_NONE;
    begin_frame();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.85f, 0));
    ImGui::Begin("Welcome", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextWrapped("Darkwing Duck - native recompilation");
    ImGui::Separator();
    ImGui::TextWrapped("This game needs your own copy of the Darkwing Duck NES cartridge, as a ROM "
                       "file (.nes). Choose it once and it will be remembered.");
    ImGui::Spacing();
    if (ImGui::Button("Choose ROM file...")) action = UI_PICK_BROWSE;
    ImGui::SameLine();
    ImGui::TextDisabled("or drag the file onto this window");
    if (message && *message) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, warn ? ImVec4(1.0f, 0.8f, 0.3f, 1) : ImVec4(1.0f, 0.45f, 0.45f, 1));
        ImGui::TextWrapped("%s", message);
        ImGui::PopStyleColor();
        if (can_use_anyway && ImGui::Button("Use it anyway")) action = UI_PICK_USE_ANYWAY;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("No file dialog (e.g. Steam Deck game mode)? Put the ROM next to the app, named "
                       "darkwing-duck.nes, or copy it into the data folder:");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", data_folder);
    if (ImGui::Button("Open data folder")) action = UI_PICK_OPEN_FOLDER;
    ImGui::SameLine();
    if (ImGui::Button("Quit")) action = UI_PICK_QUIT;
    ImGui::End();
    end_frame();
    return action;
}

static void draw_toast() {
    if (SDL_GetTicks() >= g_toast_until) return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 12, vp->Pos.y + vp->Size.y - 12), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(g_toast);
    ImGui::End();
}

void ui_draw(void) {
    if (!g_open) {
        if (SDL_GetTicks() >= g_toast_until) return;
        begin_frame();
        draw_toast();
        end_frame();
        return;
    }
    begin_frame();

    /* Dim the paused game behind the menu. */
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 150));

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Darkwing Duck - Options", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    ImGui::SeparatorText("Save states");
    for (int slot = 1; slot <= UI_STATE_SLOTS; slot++) {
        char path[1200], label[96];
        ui_state_path(slot, path, sizeof path);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(path, &info)) {
            SDL_DateTime dt;
            SDL_TimeToDateTime(info.modify_time, &dt, true);
            snprintf(label, sizeof label, "Slot %d  (%04d-%02d-%02d %02d:%02d)", slot, dt.year, dt.month, dt.day,
                     dt.hour, dt.minute);
        } else {
            snprintf(label, sizeof label, "Slot %d  (empty)", slot);
        }
        ImGui::RadioButton(label, &ui_config.state_slot, slot);
    }
    if (ImGui::Button("Save state")) { g_request = UI_REQ_SAVE; ui_toggle(); }
    ImGui::SameLine();
    if (ImGui::Button("Load state")) { g_request = UI_REQ_LOAD; ui_toggle(); }
    ImGui::SameLine();
    ImGui::TextDisabled("F5 save, F9 load, F6 next slot");

    ImGui::SeparatorText("Cheats");
    if (!ui_cheats_available) ImGui::TextDisabled("Cheats need the supported ROM version (Europe).");
    else if (nes_cheat_count == 0) ImGui::TextDisabled("No cheats defined in the game config.");
    for (int i = 0; ui_cheats_available && i < nes_cheat_count; i++) {
        const nes_cheat_def *def = &nes_cheat_defs[i];
        if (def->choice_count > 0) {
            /* Dropdown: "Off" plus the cheat's choices (stored as index + 1). */
            int sel = nes_cheats[i] <= def->choice_count ? nes_cheats[i] : 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::BeginCombo(def->name, sel ? def->choices[sel - 1] : "Off")) {
                if (ImGui::Selectable("Off", sel == 0)) nes_cheats[i] = 0;
                for (int k = 0; k < def->choice_count; k++)
                    if (ImGui::Selectable(def->choices[k], sel == k + 1)) nes_cheats[i] = (uint8_t)(k + 1);
                ImGui::EndCombo();
            }
        } else {
            bool on = nes_cheats[i] != 0;
            if (ImGui::Checkbox(def->name, &on)) nes_cheats[i] = on;
        }
        help_marker(def->description);
    }

    ImGui::SeparatorText("Convenience");
    ImGui::Checkbox("Quick skip (Tab / R1)", &ui_config.quick_skip);
    help_marker("Press Tab (or R1/RB on a gamepad) during the intro, briefings or dialogue boxes to race "
                "through them to the next screen where you play or choose: the map or the stage. Any "
                "game button stops it.");

    ImGui::SeparatorText("Video");
    if (ImGui::Checkbox("Fullscreen", &ui_config.fullscreen)) SDL_SetWindowFullscreen(g_window, ui_config.fullscreen);
    bool fullscreen = ui_config.fullscreen;
    ImGui::Checkbox("Integer scaling", &ui_config.integer_scale);
    help_marker("Only scale the picture by whole numbers, so every NES pixel is the same size.");
    if (!fullscreen) {
        ImGui::TextUnformatted("Window size:");
        for (int s = 1; s <= 5; s++) {
            ImGui::SameLine();
            char label[8];
            snprintf(label, sizeof label, "%dx", s);
            if (ImGui::Button(label)) SDL_SetWindowSize(g_window, NES_WIDTH * s, NES_HEIGHT * s);
        }
    }

    static const char *filters[FILTER_COUNT] = {"None (sharp pixels)", "Scale2x (smooth edges)",
                                                 "Scale3x (smooth edges)", "CRT scanlines"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
    ImGui::Combo("Filter", &ui_config.filter, filters, FILTER_COUNT);
    help_marker("Scale2x/Scale3x redraw pixel-art edges at higher resolution. CRT imitates the "
                "scanlines of an old TV, which the art was designed for.");

    /* Menu order differs from the stored values (new modes are appended). */
    static const struct { int mode; const char *name; } bezels[] = {
        {BEZEL_NIGHT_CITY, "Night city (built-in)"}, {BEZEL_PATTERN, "Pattern (built-in)"},
        {BEZEL_CUSTOM, "Custom image"},             {BEZEL_BLACK, "Black bars"}};
    int prev_bezel = ui_config.bezel;
    const char *current = bezels[0].name;
    for (const auto &b : bezels)
        if (b.mode == ui_config.bezel) current = b.name;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
    if (ImGui::BeginCombo("Background", current)) {
        for (const auto &b : bezels)
            if (ImGui::Selectable(b.name, b.mode == ui_config.bezel)) ui_config.bezel = b.mode;
        ImGui::EndCombo();
    }
    if (ui_config.bezel == BEZEL_CUSTOM && prev_bezel != BEZEL_CUSTOM) bezel_reload_custom();
    help_marker("What fills the space around the picture. \"Night city\" is \"Pixel night city\" by "
                "fridaruiz (CC0, opengameart.org). For a custom image, put bezel.png or bezel.jpg in the "
                "game's data folder.");
    if (ui_config.bezel == BEZEL_CUSTOM && bezel_custom_available()) {
        static const char *fits[BEZEL_FIT_COUNT] = {"Auto", "Fill (artwork)", "Repeat (patterns)"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 15);
        ImGui::Combo("Image fit", &ui_config.bezel_fit, fits, BEZEL_FIT_COUNT);
        help_marker("Fill scales the image to cover the window (best for posters and artwork). "
                    "Repeat fits its height and tiles it sideways (best for patterns).");
    }
    if (ui_config.bezel == BEZEL_CUSTOM && !bezel_custom_available()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1));
        ImGui::TextWrapped("No bezel.png / bezel.jpg found, showing the pattern. Put one in:");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", app_data_dir());
        if (ImGui::Button("Open data folder")) {
            char url[1100];
            snprintf(url, sizeof url, "file://%s", app_data_dir());
            SDL_OpenURL(url);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload")) bezel_reload_custom();
    }
    ImGui::Checkbox("Remove sprite flicker", &ui_config.no_flicker);
    help_marker("The NES can only show 8 sprites per line, so games flicker when more are on screen. "
                "This draws all of them. Gameplay is unaffected.");

    ImGui::SeparatorText("Performance");
    ImGui::Checkbox("Remove slowdown", &ui_config.remove_slowdown);
    help_marker("Gives the NES CPU twice the time per frame, so scenes with many enemies no longer "
                "slow down. Game speed and music are unchanged.");
    ImGui::Checkbox("Frame interpolation (experimental)", &ui_config.frame_interp);
    {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
        float game_hz = (float)nes_frame_hz();
        char tip[256];
        snprintf(tip, sizeof tip,
                 "Shows a generated frame between every two real ones (%.0f -> %.0f fps) by sliding "
                 "sprites and scrolling halfway, for smoother motion. Adds half a frame of delay and "
                 "can wobble on some objects. Your display runs at %.0f Hz%s.",
                 game_hz, game_hz * 2, mode ? mode->refresh_rate : 0.0f,
                 mode && mode->refresh_rate + 1 < game_hz * 2 ? ", too slow to show it" : "");
        help_marker(tip);
    }

    ImGui::SeparatorText("Audio");
    ImGui::SliderInt("Volume", &ui_config.volume, 0, 100, "%d%%");

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        int slot = ui_config.state_slot;
        ui_config = UI_DEFAULTS;
        ui_config.state_slot = slot;
        memset(nes_cheats, 0, (size_t)nes_cheat_count);
        SDL_SetWindowFullscreen(g_window, ui_config.fullscreen);
        ui_toast("Settings reset to defaults (all cheats off)");
    }
    ImGui::SameLine();
    help_marker("Turns all cheats and enhancements off, except the background and slowdown removal.");
    if (ImGui::Button("Resume")) ui_toggle();
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) {
        fprintf(stderr, "quit from the menu\n");
        nes_cpu_state.quit = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Esc / F1 / Select+Start");

    ImGui::End();
    draw_toast();
    end_frame();
}
