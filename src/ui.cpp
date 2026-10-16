#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "nesrt/recomp.h"
}

ui_settings ui_config = {80, false, FILTER_NONE, false, false};
bool ui_cheats_available = true;

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static bool g_open;
static std::string g_settings_path;

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
        else if (!strncmp(key, "cheat.", 6))
            for (int i = 0; i < nes_cheat_count; i++)
                if (!strcmp(key + 6, nes_cheat_defs[i].id)) nes_cheats[i] = value != 0;
    }
    fclose(f);
}

static void save_settings() {
    FILE *f = fopen(g_settings_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "volume=%d\ninteger_scale=%d\nfilter=%d\nremove_slowdown=%d\nframe_interp=%d\n",
            ui_config.volume, ui_config.integer_scale, ui_config.filter, ui_config.remove_slowdown,
            ui_config.frame_interp);
    for (int i = 0; i < nes_cheat_count; i++) fprintf(f, "cheat.%s=%d\n", nes_cheat_defs[i].id, nes_cheats[i] ? 1 : 0);
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

void ui_draw(void) {
    if (!g_open) return;
    begin_frame();

    /* Dim the paused game behind the menu. */
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 150));

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Darkwing Duck - Options", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);

    ImGui::SeparatorText("Cheats");
    if (!ui_cheats_available) ImGui::TextDisabled("Cheats need the supported ROM version (Europe).");
    else if (nes_cheat_count == 0) ImGui::TextDisabled("No cheats defined in the game config.");
    for (int i = 0; ui_cheats_available && i < nes_cheat_count; i++) {
        bool on = nes_cheats[i] != 0;
        if (ImGui::Checkbox(nes_cheat_defs[i].name, &on)) nes_cheats[i] = on;
        help_marker(nes_cheat_defs[i].description);
    }

    ImGui::SeparatorText("Video");
    bool fullscreen = SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN;
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) SDL_SetWindowFullscreen(g_window, fullscreen);
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
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::Combo("Filter", &ui_config.filter, filters, FILTER_COUNT);
    help_marker("Scale2x/Scale3x redraw pixel-art edges at higher resolution. CRT imitates the "
                "scanlines of an old TV, which the art was designed for.");

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
    if (ImGui::Button("Resume")) ui_toggle();
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) nes_cpu_state.quit = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Esc / F1 / Select+Start");

    ImGui::End();
    end_frame();
}
