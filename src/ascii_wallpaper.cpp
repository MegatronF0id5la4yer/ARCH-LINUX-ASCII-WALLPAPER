#include "audio_reactive.hpp"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_usr1_toggle{false};

constexpr const char* kAsciiRamp =
    " .'`^\",:;!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

struct RawRgba {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba; // straight R, G, B, A
};

struct MoodSpriteSet {
    std::string id;
    std::string label;
    RawRgba open;
    RawRgba blink;
};

struct Rgb {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
};

struct ThemePalette {
    std::string id;
    std::string label;
    Rgb bg_top;
    Rgb bg_bottom;
    Rgb accent;
    Rgb accent_soft;
    Rgb rain_color;
    Rgb hud_bg;
    Rgb hud_border;
    bool tint_sprite;
    double tint_strength;
    bool scanlines;
};

struct GlyphAtlas {
    double font_pt = 9.0;
    int cell_w = 8;
    int cell_h = 15;
    // 95 printable ASCII characters: code 32..126, each cell_w * cell_h alpha coverage [0..255]
    std::vector<uint8_t> masks;

    const uint8_t* glyph(char ch) const {
        int code = static_cast<unsigned char>(ch);
        if (code < 32 || code > 126) code = ' ';
        return &masks[static_cast<size_t>(code - 32) * static_cast<size_t>(cell_w * cell_h)];
    }
};

struct RainDrop {
    double x_norm = 0.0; // [0..1]
    double y_norm = 0.0; // [0..1]
    double speed = 0.08;
    double sway_phase = 0.0;
    char ch = '*';
    uint8_t alpha = 140;
};

struct Ripple {
    double x_px = 0.0;
    double y_px = 0.0;
    double age = 0.0;
    double max_age = 1.1;
    bool heart = false;
};

struct FloatingTag {
    double x_px = 0.0;
    double y_px = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double age = 0.0;
    double max_age = 1.6;
    std::string text;
    Rgb color;
};

struct HudButton {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::string action;
    std::string label;
};

static uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

static Rgb lerp_rgb(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return {
        clamp_u8(static_cast<int>(std::lround(a.r + (b.r - a.r) * t))),
        clamp_u8(static_cast<int>(std::lround(a.g + (b.g - a.g) * t))),
        clamp_u8(static_cast<int>(std::lround(a.b + (b.b - a.b) * t))),
    };
}

static std::string resolve_project_root() {
    if (const char* env_home = std::getenv("NATSUKI_SHIMEJI_HOME")) {
        if (*env_home && fs::exists(fs::path(env_home) / "assets" / "wallpaper")) {
            return env_home;
        }
    }
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        fs::path dir = exe.parent_path();
        if (fs::exists(dir / "assets" / "wallpaper")) return dir.string();
    }
    fs::path cwd = fs::current_path(ec);
    if (fs::exists(cwd / "assets" / "wallpaper")) return cwd.string();
    return ".";
}

static RawRgba load_png_straight(const std::string& path) {
    RawRgba out;
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (!pb) {
        if (err) g_error_free(err);
        return out;
    }
    if (!gdk_pixbuf_get_has_alpha(pb)) {
        GdkPixbuf* with_a = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
        g_object_unref(pb);
        pb = with_a;
    }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    const uint8_t* src_px = gdk_pixbuf_get_pixels(pb);
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w * h * 4));
    for (int y = 0; y < h; ++y) {
        std::memcpy(&out.rgba[static_cast<size_t>(y * w * 4)], src_px + y * rs, static_cast<size_t>(w * 4));
    }
    g_object_unref(pb);
    return out;
}

static RawRgba build_windows_xp_flag_sprite() {
    RawRgba out;
    out.width = 240;
    out.height = 220;
    out.rgba.assign(static_cast<size_t>(out.width * out.height * 4), 0);

    // Classic 4 glossy Windows XP wavy panes tilted slightly matching the Bliss Tux meme
    const Rgb kPaneCols[4] = {
        {242, 78, 34},   // Top-left: Red-Orange
        {118, 192, 22},  // Top-right: Green
        {18, 152, 240},  // Bottom-left: Blue
        {252, 188, 18},  // Bottom-right: Yellow-Gold
    };
    const double ang = -0.22; // tilted slightly forward on the hill
    const double ca = std::cos(ang), sa = std::sin(ang);

    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            double cx = (x - out.width * 0.5) / (out.width * 0.37);
            double cy = (y - out.height * 0.5) / (out.height * 0.37);
            // Perspective taper (wider top, narrower bottom) + rotation
            double rx = (cx * ca - cy * sa) / (1.0 - 0.14 * cy);
            double ry = (cx * sa + cy * ca);

            // Sinusoidal wave across horizontal & vertical flag axes
            double u = rx;
            double v = ry - 0.13 * std::sin(u * 2.8 + 0.35);

            if (std::abs(u) > 1.0 || std::abs(v) > 1.0) continue;
            // Cross gap between panes
            if (std::abs(u) < 0.085 || std::abs(v) < 0.085) continue;

            int pane = (v >= 0.0 ? 2 : 0) + (u >= 0.0 ? 1 : 0);
            double lu = (u < 0.0) ? (u + 1.0) / 0.915 : (u - 0.085) / 0.915;
            double lv = (v < 0.0) ? (v + 1.0) / 0.915 : (v - 0.085) / 0.915;

            double edge_d = std::min({lu, 1.0 - lu, lv, 1.0 - lv});
            Rgb base = kPaneCols[pane];
            // Glossy inner gradient + specular wave + crisp dark rim
            double shade = 0.78 + 0.28 * (1.0 - lv) + 0.16 * std::sin(lu * 3.14159) * (1.0 - lv);
            if (edge_d < 0.065) shade *= 0.35; // crisp dark outline around each curved pane
            double spec = std::exp(-std::pow((lu - 0.35) * 2.8, 2.0) - std::pow((lv - 0.25) * 3.4, 2.0)) * 75.0;

            size_t p = static_cast<size_t>(y * out.width + x) * 4;
            out.rgba[p + 0] = clamp_u8(static_cast<int>(base.r * shade + spec));
            out.rgba[p + 1] = clamp_u8(static_cast<int>(base.g * shade + spec));
            out.rgba[p + 2] = clamp_u8(static_cast<int>(base.b * shade + spec));
            out.rgba[p + 3] = 255;
        }
    }
    return out;
}

static GlyphAtlas build_glyph_atlas(double font_pt) {
    GlyphAtlas atlas;
    atlas.font_pt = font_pt;

    // Measure monospace cell geometry
    cairo_surface_t* tmp_surf = cairo_image_surface_create(CAIRO_FORMAT_A8, 64, 64);
    cairo_t* tmp_cr = cairo_create(tmp_surf);
    PangoLayout* layout = pango_cairo_create_layout(tmp_cr);
    char font_buf[96];
    std::snprintf(font_buf, sizeof(font_buf), "JetBrains Mono, DejaVu Sans Mono, Monospace Bold %.1f", font_pt);
    PangoFontDescription* desc = pango_font_description_from_string(font_buf);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, "@", 1);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    atlas.cell_w = std::max(2, lw);
    atlas.cell_h = std::max(4, lh - 1);
    g_object_unref(layout);
    cairo_destroy(tmp_cr);
    cairo_surface_destroy(tmp_surf);

    int cw = atlas.cell_w;
    int ch = atlas.cell_h;
    atlas.masks.assign(95ULL * static_cast<size_t>(cw * ch), 0);

    cairo_surface_t* cell_surf = cairo_image_surface_create(CAIRO_FORMAT_A8, cw, ch);
    int stride = cairo_image_surface_get_stride(cell_surf);
    cairo_t* cr = cairo_create(cell_surf);
    PangoLayout* gl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(gl, desc);

    for (int code = 32; code <= 126; ++code) {
        char buf[2] = {static_cast<char>(code), '\0'};
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        pango_layout_set_text(gl, buf, 1);
        cairo_move_to(cr, 0.0, 0.0);
        pango_cairo_show_layout(cr, gl);
        cairo_surface_flush(cell_surf);

        const uint8_t* data = cairo_image_surface_get_data(cell_surf);
        uint8_t* dst = &atlas.masks[static_cast<size_t>(code - 32) * static_cast<size_t>(cw * ch)];
        for (int y = 0; y < ch; ++y) {
            std::memcpy(dst + static_cast<size_t>(y * cw), data + static_cast<size_t>(y * stride), static_cast<size_t>(cw));
        }
    }

    pango_font_description_free(desc);
    g_object_unref(gl);
    cairo_destroy(cr);
    cairo_surface_destroy(cell_surf);
    return atlas;
}

static std::vector<ThemePalette> make_themes() {
    return {
        {
            "bliss_arch",
            "Bliss x Arch",
            {26, 86, 174},
            {48, 118, 34},
            {79, 212, 255},
            {255, 198, 224},
            {225, 244, 255},
            {12, 16, 26},
            {79, 212, 255},
            false,
            0.0,
            false,
        },
        {
            "cyber_pink",
            "Cyber Nya",
            {17, 10, 24},
            {34, 15, 38},
            {255, 118, 178},
            {255, 198, 224},
            {230, 96, 165},
            {24, 13, 31},
            {255, 120, 180},
            false,
            0.0,
            true,
        },
        {
            "club_pastel",
            "Pastel Noche",
            {22, 16, 28},
            {44, 24, 42},
            {255, 154, 198},
            {255, 226, 238},
            {255, 175, 210},
            {30, 19, 36},
            {255, 165, 205},
            false,
            0.0,
            false,
        },
        {
            "matrix_rose",
            "Matrix Sakura",
            {8, 12, 14},
            {18, 12, 22},
            {255, 92, 162},
            {158, 248, 208},
            {110, 235, 175},
            {12, 18, 20},
            {120, 240, 185},
            true,
            0.22,
            true,
        },
        {
            "mono_crt",
            "CRT Rosa",
            {12, 8, 14},
            {22, 12, 24},
            {255, 136, 192},
            {255, 214, 234},
            {210, 95, 150},
            {18, 10, 20},
            {240, 125, 182},
            true,
            0.78,
            true,
        },
        {
            "tokyo_otaku",
            "Tokyo Otaku",
            {11, 13, 24},
            {24, 16, 40},
            {255, 105, 180},
            {122, 234, 255},
            {115, 215, 255},
            {15, 18, 32},
            {122, 234, 255},
            false,
            0.0,
            true,
        },
    };
}

class AsciiWallpaperApp {
public:
    explicit AsciiWallpaperApp(bool self_test = false) : self_test_(self_test) {
        themes_ = make_themes();
        rng_.seed(static_cast<unsigned int>(std::time(nullptr)));
    }

    ~AsciiWallpaperApp() {
        audio_monitor_.stop();
        if (tick_id_) {
            g_source_remove(tick_id_);
            tick_id_ = 0;
        }
    }

    int run(int argc, char** argv) {
        if (!gtk_init_check(&argc, &argv)) {
            std::cerr << "[ascii-wallpaper] GTK display not available\n";
            return 1;
        }

        root_dir_ = resolve_project_root();
        fs::path nat_assets = fs::path(root_dir_) / "assets" / "natsuki";

        const std::array<std::pair<const char*, const char*>, 6> mood_defs = {{
            {"happy", "Feliz"},
            {"blush", "Sonrojada"},
            {"tsun", "Tsundere"},
            {"angry", "Enojada"},
            {"surprised", "Sorpresa"},
            {"thinking", "Pensando"},
        }};
        for (const auto& [mid, mlbl] : mood_defs) {
            MoodSpriteSet s;
            s.id = mid;
            s.label = mlbl;
            s.open = load_png_straight((nat_assets / (std::string(mid) + "_open.png")).string());
            s.blink = load_png_straight((nat_assets / (std::string(mid) + "_blink.png")).string());
            if (s.open.width <= 0 || s.open.height <= 0) {
                std::cerr << "[ascii-wallpaper] Failed to load jn_nya_cat mood PNG: " << mid << "\n";
                return 3;
            }
            if (s.blink.width <= 0) s.blink = s.open;
            sprites_.push_back(std::move(s));
        }

        atlases_.push_back(build_glyph_atlas(2.8));  // Hyper HD ASCII (~2x5 px)
        atlases_.push_back(build_glyph_atlas(4.0));  // Ultra HD ASCII
        atlases_.push_back(build_glyph_atlas(6.0));  // Fino
        atlases_.push_back(build_glyph_atlas(9.0));  // Medio (y HUD)
        atlases_.push_back(build_glyph_atlas(12.0)); // Retro

        fs::path wp_assets = fs::path(root_dir_) / "assets" / "wallpaper";
        tux_sprite_ = load_png_straight((wp_assets / "tux_bliss.png").string());
        lelouch_sprite_ = load_png_straight((wp_assets / "lelouch_arch.png").string());
        arch_sprite_ = load_png_straight((wp_assets / "arch_banner.png").string());
        win_flag_sprite_ = build_windows_xp_flag_sprite();
        if (tux_sprite_.width <= 0 || lelouch_sprite_.width <= 0 || arch_sprite_.width <= 0) {
            std::cerr << "[ascii-wallpaper] Missing wallpaper assets under " << wp_assets << "\n";
            return 4;
        }
        // Brighten neutral gray "arch" letters and vivid cyan "linux"/triangle in arch_sprite_
        for (size_t i = 0; i + 3 < arch_sprite_.rgba.size(); i += 4) {
            uint8_t a = arch_sprite_.rgba[i + 3];
            if (a < 20) continue;
            uint8_t r = arch_sprite_.rgba[i], g = arch_sprite_.rgba[i + 1], b = arch_sprite_.rgba[i + 2];
            if (b >= r + 15) {
                arch_sprite_.rgba[i] = clamp_u8(static_cast<int>(r * 0.6));
                arch_sprite_.rgba[i + 1] = clamp_u8(static_cast<int>(std::max<int>(g, 165) * 1.15));
                arch_sprite_.rgba[i + 2] = 255;
            } else if (std::max({r, g, b}) < 165) {
                arch_sprite_.rgba[i] = 244;
                arch_sprite_.rgba[i + 1] = 248;
                arch_sprite_.rgba[i + 2] = 255;
            }
        }

        init_rain();
        say("Bliss x Arch ASCII activo! Tux, Lelouch y yo cuidamos tu Linux~", "happy");

        if (self_test_) {
            return run_self_test();
        }

        setup_monitor_windows();
        GdkDisplay* dpy = gdk_display_get_default();
        if (dpy) {
            g_signal_connect(dpy, "monitor-added", G_CALLBACK(+[](GdkDisplay*, GdkMonitor*, gpointer d) {
                static_cast<AsciiWallpaperApp*>(d)->setup_monitor_windows();
            }), this);
            g_signal_connect(dpy, "monitor-removed", G_CALLBACK(+[](GdkDisplay*, GdkMonitor*, gpointer d) {
                static_cast<AsciiWallpaperApp*>(d)->setup_monitor_windows();
            }), this);
        }

        last_tick_us_ = g_get_monotonic_time();
        std::signal(SIGUSR1, [](int) { g_usr1_toggle.store(true); });
        tick_id_ = g_timeout_add(33, [](gpointer d) -> gboolean {
            return static_cast<AsciiWallpaperApp*>(d)->on_tick();
        }, this);

        gtk_main();
        return 0;
    }

private:
    struct MonitorWindow {
        GdkMonitor* monitor = nullptr;
        GtkWidget* window = nullptr;
        GtkWidget* area = nullptr;
        int width = 1366;
        int height = 768;
        cairo_surface_t* buffer = nullptr;
        std::array<int, 4> sprite_box = {0, 0, 0, 0}; // Natsuki hitbox
        std::array<int, 4> tux_box = {0, 0, 0, 0};    // Tux + Windows meme hitbox
        std::array<int, 4> lelouch_box = {0, 0, 0, 0}; // Lelouch Geass hitbox
        std::array<int, 4> arch_box = {0, 0, 0, 0};   // Arch Linux banner hitbox
        std::vector<HudButton> buttons;
    };

    bool self_test_ = false;
    std::string root_dir_;
    std::vector<MoodSpriteSet> sprites_;
    RawRgba tux_sprite_;
    RawRgba lelouch_sprite_;
    RawRgba arch_sprite_;
    RawRgba win_flag_sprite_;
    std::vector<ThemePalette> themes_;
    std::vector<GlyphAtlas> atlases_;
    std::vector<MonitorWindow> windows_;
    std::mt19937 rng_;

    int mood_idx_ = 0;       // 0..5, or auto cycles when auto_mood_
    bool auto_mood_ = true;
    int theme_idx_ = 0;
    int density_idx_ = 0;    // 0: Fino, 1: Medio, 2: Retro
    bool rain_enabled_ = true;
    bool right_aligned_ = false;
    bool tux_enabled_ = true;
    bool menu_open_ = false;
    double tux_z_ = 0.0, tux_vz_ = 0.0;
    double tux_flap_left_ = 0.0;
    double lelouch_geass_left_ = 0.0;
    bool audio_reactive_ = false;
    AudioReactiveMonitor audio_monitor_;
    AudioSnapshot audio_snap_;
    uint32_t last_beat_seq_ = 0;
    double sys_timer_ = 2.0;
    int mem_pct_ = 0;
    int bat_pct_ = -1;
    bool bat_charging_ = false;
    std::string load_str_ = "0.00";

    double time_s_ = 0.0;
    double auto_mood_timer_ = 0.0;
    double blink_timer_ = 2.8;
    double blink_left_ = 0.0;
    double ear_twitch_timer_ = 2.1;
    double ear_twitch_left_ = 0.0;
    int ear_twitch_dir_ = 1;

    // Interactive cursor & click physics
    double mouse_nx_ = 0.5;
    double mouse_ny_ = 0.5;
    double smooth_mx_ = 0.5;
    double smooth_my_ = 0.5;
    double spring_z_ = 0.0, spring_vz_ = 0.0;
    double spring_x_ = 0.0, spring_vx_ = 0.0;
    int total_clicks_ = 0;

    std::string bubble_text_;
    double bubble_timer_ = 0.0;

    std::vector<RainDrop> rain_;
    std::vector<Ripple> ripples_;
    std::vector<FloatingTag> tags_;

    guint tick_id_ = 0;
    gint64 last_tick_us_ = 0;
    double fps_est_ = 30.0;

    void init_rain() {
        rain_.clear();
        static const char* kRainChars = ".*:+~^nya=<>01";
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::uniform_real_distribution<double> uspd(0.05, 0.16);
        std::uniform_int_distribution<size_t> uch(0, std::strlen(kRainChars) - 1);
        for (int i = 0; i < 95; ++i) {
            RainDrop d;
            d.x_norm = u01(rng_);
            d.y_norm = u01(rng_);
            d.speed = uspd(rng_);
            d.sway_phase = u01(rng_) * 6.28318;
            d.ch = kRainChars[uch(rng_)];
            d.alpha = static_cast<uint8_t>(65 + (i % 130));
            rain_.push_back(d);
        }
    }

    int mood_index_by_id(const std::string& id) const {
        for (size_t i = 0; i < sprites_.size(); ++i) {
            if (sprites_[i].id == id) return static_cast<int>(i);
        }
        return 0;
    }

    void say(const std::string& msg, const std::string& mood_id, double duration = 5.5) {
        bubble_text_ = msg;
        bubble_timer_ = duration;
        mood_idx_ = mood_index_by_id(mood_id);
        auto_mood_timer_ = 0.0;
    }

    void trigger_pat_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 5> kPatLines = {{
            {"Nyaa~?! N-No me acaricies las orejas asi... idiota >///<", "blush"},
            {"Prrr... digo, NO estaba ronroneando! Solo me acomodaba el gorro!", "blush"},
            {"Hmph! Si sigues haciendome piojito en las orejas me voy a quedar aqui todo el dia~", "happy"},
            {"Meow~ Ok, un poquito mas ahi atras de la orejita no esta mal...", "happy"},
            {"O-Oye! Las orejas de gato son sensibles, baka!!", "tsun"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kPatLines.size();
        say(kPatLines[idx].first, kPatLines[idx].second, 6.0);
        spring_vz_ += 7.5;
        spring_vx_ += ((total_clicks_ & 1) ? 3.2 : -3.2);
        ear_twitch_left_ = 0.34;
        ear_twitch_dir_ = (total_clicks_ & 1) ? 1 : -1;
        spawn_tags(px, py, {"nya~", "<3", "^w^", "*purr*"}, themes_[theme_idx_].accent);
    }

    void trigger_bell_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 4> kBellLines = {{
            {"*Ring~!* Oye! Mi cascabel no es un timbre para llamarme!!", "tsun"},
            {"*Ding!* Nya?! Me asustaste con el cascabelito!", "surprised"},
            {"Tilín~ Quieres un cupcake con forma de gatito o que?", "happy"},
            {"No toques mi collar o te muerdo como gato callejero!! >:3", "angry"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kBellLines.size();
        say(kBellLines[idx].first, kBellLines[idx].second, 5.5);
        spring_vz_ -= 6.0;
        spring_vx_ += 4.5;
        spawn_tags(px, py, {"*ding!*", "~♪", ":3"}, themes_[theme_idx_].accent_soft);
    }

    void trigger_poke_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 5> kPokeLines = {{
            {"O-OYE!! Donde pones el cursor?! Baka!! >.<", "angry"},
            {"Nyaaa!! Quitame ese puntero de encima ahorita mismo!!", "angry"},
            {"Q-Que miras mi sueter de gatito?! No te burles!!", "tsun"},
            {"Hmph! Manga + gatito = obra de arte. Respeta!!", "surprised"},
            {"Grrr~ *arañazo ASCII* Te voy a borrar el wallpaper si sigues picando!", "angry"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kPokeLines.size();
        say(kPokeLines[idx].first, kPokeLines[idx].second, 5.5);
        spring_vz_ -= 9.5;
        spring_vx_ += ((total_clicks_ & 1) ? -5.8 : 5.8);
        spawn_tags(px, py, {"BAKA!", ">.<", "nya?!", "!"}, {255, 95, 130});
    }

    void trigger_bg_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 5> kBgLines = {{
            {"*mira tu cursor pasear por el escritorio* Hmph, hoy si vas a programar?", "thinking"},
            {"Nya~ Me veo mas tierna en ASCII que tu tema minimalista, admitelo!", "happy"},
            {"=^._.^= Vigilando tus ventanas en silencio... mas te vale no abrir basura!", "tsun"},
            {"*mueve la colita de gato* Tus letras ASCII brillan bonito hoy~", "blush"},
            {"Miaw~ Haz clic en mis orejas si necesitas animo para chambear!", "happy"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kBgLines.size();
        say(kBgLines[idx].first, kBgLines[idx].second, 5.0);
        spawn_tags(px, py, {"*", "nya", "+"}, themes_[theme_idx_].rain_color);
    }

    void trigger_tux_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 5> kTuxLines = {{
            {"*Tux aletea orgulloso* Dice que 'uses Arch btw', pero yo mando aqui!!", "tsun"},
            {"Hmph! Tux y yo cuidamos tu kernel mientras haces cosas de otaku~", "happy"},
            {"*Tux grazna en binario* Oye pinguino, no te robes mis cupcakes!!", "surprised"},
            {"sudo pacman -Syu cupcakes-de-gato... aprobada la orden por Tux y Natsuki!", "happy"},
            {"*Tux te saluda con la aleta* Bueno... admito que el pinguino es medio chad.", "blush"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kTuxLines.size();
        say(kTuxLines[idx].first, kTuxLines[idx].second, 5.5);
        tux_vz_ += 8.2;
        tux_flap_left_ = 0.55;
        spawn_tags(px, py - 18.0, {"arch btw", "sudo", "linux<3", "*flap*"}, {120, 225, 255});
    }

    void trigger_arch_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 4> kArchLines = {{
            {"I use Arch, btw!! Y con Natsuki neko + Lelouch en el wallpaper todavia mas chad~", "happy"},
            {"sudo pacman -Syu anime-girl... paquete instalado con exito!!", "blush"},
            {"Arch Linux + Hyprland + gatita tsundere = el setup definitivo!", "tsun"},
            {"Cuidado con romper el sistema en AUR... Tux y Lelouch te vigilamos!", "thinking"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kArchLines.size();
        say(kArchLines[idx].first, kArchLines[idx].second, 5.5);
        tux_vz_ += 4.5;
        spawn_tags(px, py - 16.0, {"arch btw", "AUR", "pacman", "chad"}, {79, 212, 255});
    }

    void trigger_lelouch_reaction(double px, double py) {
        static const std::array<std::pair<const char*, const char*>, 4> kLelouchLines = {{
            {"Lelouch vi Britannia te ordena: usa Arch Linux y dale mimos a Natsuki!!", "tsun"},
            {"*brilla su ojo Geass rojo* Windows ha sido destruido por orden de Zero.", "surprised"},
            {"All Hail Arch Linux!! Natsuki neko + Tux ahora dominan tu escritorio.", "happy"},
            {"Con el poder del Geass ni un kernel panic podra contigo, btw.", "thinking"},
        }};
        size_t idx = static_cast<size_t>(total_clicks_) % kLelouchLines.size();
        say(kLelouchLines[idx].first, kLelouchLines[idx].second, 5.8);
        lelouch_geass_left_ = 1.1;
        spawn_tags(px - 60.0, py - 24.0, {"GEASS!", "All Hail Arch", "ZERO", "btw"}, {255, 62, 108});
    }

    void spawn_tags(double px, double py, std::initializer_list<const char*> words, Rgb col) {
        std::uniform_real_distribution<double> uvx(-36.0, 36.0);
        std::uniform_real_distribution<double> uvy(-75.0, -35.0);
        int ti = 0;
        for (const char* w : words) {
            FloatingTag t;
            double side = (ti % 2 == 0) ? -1.0 : 1.0;
            t.x_px = std::clamp(px + side * (75.0 + (ti / 2) * 46.0), 42.0, 1280.0);
            t.y_px = std::max(28.0, py - 24.0 - ti * 16.0);
            t.vx = side * 22.0 + uvx(rng_) * 0.5;
            t.vy = uvy(rng_) - 12.0;
            t.age = 0.0;
            t.max_age = 1.45;
            t.text = w;
            t.color = col;
            tags_.push_back(std::move(t));
            ti++;
        }
    }

    void clear_windows() {
        for (auto& mw : windows_) {
            if (mw.buffer) {
                cairo_surface_destroy(mw.buffer);
                mw.buffer = nullptr;
            }
            if (mw.window) {
                gtk_widget_destroy(mw.window);
                mw.window = nullptr;
            }
        }
        windows_.clear();
    }

    void setup_monitor_windows() {
        clear_windows();
        GdkDisplay* dpy = gdk_display_get_default();
        int n = dpy ? gdk_display_get_n_monitors(dpy) : 1;
        n = std::max(1, n);

        for (int i = 0; i < n; ++i) {
            GdkMonitor* mon = dpy ? gdk_display_get_monitor(dpy, i) : nullptr;
            GdkRectangle geo{0, 0, 1366, 768};
            if (mon) gdk_monitor_get_geometry(mon, &geo);

            GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
            gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
            gtk_widget_set_app_paintable(win, TRUE);

            gtk_layer_init_for_window(GTK_WINDOW(win));
            gtk_layer_set_namespace(GTK_WINDOW(win), "natsuki-ascii-wallpaper");
            gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_BOTTOM);
            gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
            gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
            if (mon) gtk_layer_set_monitor(GTK_WINDOW(win), mon);
            gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
            gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);

            GtkWidget* area = gtk_drawing_area_new();
            gtk_widget_add_events(area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                            GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK);
            gtk_widget_set_can_focus(win, TRUE);
            gtk_container_add(GTK_CONTAINER(win), area);

            MonitorWindow mw;
            mw.monitor = mon;
            mw.window = win;
            mw.area = area;
            mw.width = std::max(320, geo.width);
            mw.height = std::max(240, geo.height);
            mw.buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mw.width, mw.height);
            windows_.push_back(mw);

            size_t idx = windows_.size() - 1;
            g_object_set_data(G_OBJECT(area), "mon_idx", GSIZE_TO_POINTER(idx));
            g_object_set_data(G_OBJECT(win), "mon_idx", GSIZE_TO_POINTER(idx));

            g_signal_connect(area, "size-allocate", G_CALLBACK(+[](GtkWidget* w, GdkRectangle* alloc, gpointer d) {
                auto* app = static_cast<AsciiWallpaperApp*>(d);
                size_t mi = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "mon_idx"));
                if (mi < app->windows_.size() && alloc->width > 10 && alloc->height > 10) {
                    auto& m = app->windows_[mi];
                    if (m.width != alloc->width || m.height != alloc->height) {
                        m.width = alloc->width;
                        m.height = alloc->height;
                        if (m.buffer) cairo_surface_destroy(m.buffer);
                        m.buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m.width, m.height);
                    }
                }
            }), this);

            g_signal_connect(area, "draw", G_CALLBACK(+[](GtkWidget* w, cairo_t* cr, gpointer d) -> gboolean {
                auto* app = static_cast<AsciiWallpaperApp*>(d);
                size_t mi = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "mon_idx"));
                if (mi < app->windows_.size()) {
                    app->render_monitor(app->windows_[mi]);
                    cairo_set_source_surface(cr, app->windows_[mi].buffer, 0.0, 0.0);
                    cairo_paint(cr);
                }
                return TRUE;
            }), this);

            g_signal_connect(area, "motion-notify-event", G_CALLBACK(+[](GtkWidget* w, GdkEventMotion* ev, gpointer d) -> gboolean {
                auto* app = static_cast<AsciiWallpaperApp*>(d);
                size_t mi = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "mon_idx"));
                if (mi < app->windows_.size()) {
                    app->on_pointer_move(app->windows_[mi], ev->x, ev->y);
                }
                return TRUE;
            }), this);

            g_signal_connect(area, "button-press-event", G_CALLBACK(+[](GtkWidget* w, GdkEventButton* ev, gpointer d) -> gboolean {
                auto* app = static_cast<AsciiWallpaperApp*>(d);
                size_t mi = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(w), "mon_idx"));
                if (mi < app->windows_.size()) {
                    gtk_widget_grab_focus(app->windows_[mi].window);
                    app->on_button_press(app->windows_[mi], ev->x, ev->y, ev->button);
                }
                return TRUE;
            }), this);

            g_signal_connect(area, "scroll-event", G_CALLBACK(+[](GtkWidget*, GdkEventScroll* ev, gpointer d) -> gboolean {
                auto* app = static_cast<AsciiWallpaperApp*>(d);
                app->on_scroll(ev->direction);
                return TRUE;
            }), this);

            g_signal_connect(win, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* ev, gpointer d) -> gboolean {
                return static_cast<AsciiWallpaperApp*>(d)->on_key_press(ev->keyval);
            }), this);

            gtk_widget_show_all(win);
        }
    }

    void on_pointer_move(const MonitorWindow& mw, double x, double y) {
        mouse_nx_ = std::clamp(x / std::max(1, mw.width), 0.0, 1.0);
        mouse_ny_ = std::clamp(y / std::max(1, mw.height), 0.0, 1.0);
    }

    std::string detect_click_zone(const MonitorWindow& mw, double x, double y) const {
        for (const auto& b : mw.buttons) {
            if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) {
                return "hud:" + b.action;
            }
        }
        const auto& tb = mw.tux_box;
        if (tux_enabled_ && tb[2] > 0 && tb[3] > 0 &&
            x >= tb[0] && x <= tb[0] + tb[2] &&
            y >= tb[1] && y <= tb[1] + tb[3]) {
            return "tux";
        }
        const auto& ab = mw.arch_box;
        if (tux_enabled_ && ab[2] > 0 && ab[3] > 0 &&
            x >= ab[0] && x <= ab[0] + ab[2] &&
            y >= ab[1] && y <= ab[1] + ab[3]) {
            return "arch";
        }
        const auto& lb = mw.lelouch_box;
        if (tux_enabled_ && lb[2] > 0 && lb[3] > 0 &&
            x >= lb[0] && x <= lb[0] + lb[2] &&
            y >= lb[1] && y <= lb[1] + lb[3]) {
            return "lelouch";
        }
        const auto& sb = mw.sprite_box;
        if (sb[2] > 0 && sb[3] > 0 &&
            x >= sb[0] && x <= sb[0] + sb[2] &&
            y >= sb[1] && y <= sb[1] + sb[3]) {
            double rx = (x - sb[0]) / static_cast<double>(sb[2]);
            double ry = (y - sb[1]) / static_cast<double>(sb[3]);
            if (ry < 0.27 && rx > 0.22 && rx < 0.78) return "ears";
            if (ry < 0.44 && rx > 0.28 && rx < 0.72) return "head";
            if (ry >= 0.44 && ry <= 0.56 && rx > 0.34 && rx < 0.66) return "bell";
            if (ry >= 0.50 && ry <= 0.96 && rx > 0.16 && rx < 0.84) return "poke";
        }
        return "bg";
    }

    void on_button_press(const MonitorWindow& mw, double x, double y, guint button) {
        if (button == 3) {
            theme_idx_ = (theme_idx_ + 1) % static_cast<int>(themes_.size());
            say("Tema ASCII cambiado a: " + themes_[theme_idx_].label + " ~", "happy", 3.2);
            queue_redraw();
            return;
        }
        total_clicks_++;
        std::string zone = detect_click_zone(mw, x, y);
        if (zone.rfind("hud:", 0) == 0) {
            handle_hud_action(zone.substr(4));
            queue_redraw();
            return;
        }
        if (menu_open_) {
            menu_open_ = false;
        }

        Ripple rp;
        rp.x_px = x;
        rp.y_px = y;
        rp.age = 0.0;
        rp.max_age = 1.05;
        rp.heart = (zone == "ears" || zone == "head");
        ripples_.push_back(rp);

        if (zone == "ears" || zone == "head") {
            trigger_pat_reaction(x, y);
        } else if (zone == "bell") {
            trigger_bell_reaction(x, y);
        } else if (zone == "poke") {
            trigger_poke_reaction(x, y);
        } else if (zone == "tux") {
            trigger_tux_reaction(x, y);
        } else if (zone == "arch") {
            trigger_arch_reaction(x, y);
        } else if (zone == "lelouch") {
            trigger_lelouch_reaction(x, y);
        } else {
            trigger_bg_reaction(x, y);
        }
        queue_redraw();
    }

    void handle_hud_action(const std::string& act) {
        if (act == "mood") {
            if (auto_mood_) {
                auto_mood_ = false;
                mood_idx_ = (mood_idx_ + 1) % static_cast<int>(sprites_.size());
            } else if (mood_idx_ + 1 < static_cast<int>(sprites_.size())) {
                mood_idx_++;
            } else {
                auto_mood_ = true;
                mood_idx_ = 0;
            }
        } else if (act == "theme") {
            theme_idx_ = (theme_idx_ + 1) % static_cast<int>(themes_.size());
        } else if (act == "density") {
            density_idx_ = (density_idx_ + 1) % static_cast<int>(atlases_.size());
        } else if (act == "rain") {
            rain_enabled_ = !rain_enabled_;
        } else if (act == "pos") {
            right_aligned_ = !right_aligned_;
        } else if (act == "tux") {
            tux_enabled_ = !tux_enabled_;
        } else if (act == "audio") {
            audio_reactive_ = !audio_reactive_;
            if (audio_reactive_) {
                audio_monitor_.start("monitor");
                say("Modo Audio ASCII activado~ Pon tu opening favorito!", "happy", 3.5);
            } else {
                audio_monitor_.stop();
                audio_snap_ = AudioSnapshot{};
                say("Audio ASCII pausado.", "thinking", 2.5);
            }
        } else if (act == "gear") {
            menu_open_ = !menu_open_;
        }
    }

    void on_scroll(GdkScrollDirection dir) {
        if (dir == GDK_SCROLL_UP) {
            density_idx_ = std::max(0, density_idx_ - 1);
        } else if (dir == GDK_SCROLL_DOWN) {
            density_idx_ = std::min(static_cast<int>(atlases_.size()) - 1, density_idx_ + 1);
        }
        queue_redraw();
    }

    gboolean on_key_press(guint keyval) {
        if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_6) {
            auto_mood_ = false;
            mood_idx_ = static_cast<int>(keyval - GDK_KEY_1) % static_cast<int>(sprites_.size());
            say("Expresion fijada: " + sprites_[mood_idx_].label, sprites_[mood_idx_].id, 3.0);
        } else if (keyval == GDK_KEY_0) {
            auto_mood_ = true;
            say("Modo de expresion automatico activado ~", "happy", 3.0);
        } else if (keyval == GDK_KEY_t || keyval == GDK_KEY_T) {
            handle_hud_action("theme");
        } else if (keyval == GDK_KEY_d || keyval == GDK_KEY_D) {
            handle_hud_action("density");
        } else if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
            handle_hud_action("rain");
        } else if (keyval == GDK_KEY_p || keyval == GDK_KEY_P) {
            handle_hud_action("pos");
        } else if (keyval == GDK_KEY_x || keyval == GDK_KEY_X) {
            handle_hud_action("tux");
        } else if (keyval == GDK_KEY_a || keyval == GDK_KEY_A) {
            handle_hud_action("audio");
        } else if (keyval == GDK_KEY_h || keyval == GDK_KEY_H) {
            handle_hud_action("gear");
        } else if (keyval == GDK_KEY_Escape) {
            menu_open_ = false;
        } else if (keyval == GDK_KEY_space) {
            total_clicks_++;
            trigger_pat_reaction(500.0, 260.0);
        } else {
            return FALSE;
        }
        queue_redraw();
        return TRUE;
    }

    void queue_redraw() {
        for (const auto& mw : windows_) {
            if (mw.area) gtk_widget_queue_draw(mw.area);
        }
    }

    void update_sys_stats() {
        if (FILE* f = std::fopen("/proc/loadavg", "r")) {
            char buf[64]{};
            if (std::fgets(buf, sizeof(buf), f)) {
                double l1 = 0.0;
                if (std::sscanf(buf, "%lf", &l1) == 1) {
                    char out[16];
                    std::snprintf(out, sizeof(out), "%.2f", l1);
                    load_str_ = out;
                }
            }
            std::fclose(f);
        }
        if (FILE* f = std::fopen("/proc/meminfo", "r")) {
            char line[128]{};
            long long total_kb = 0, avail_kb = 0;
            while (std::fgets(line, sizeof(line), f)) {
                long long v = 0;
                if (std::sscanf(line, "MemTotal: %lld kB", &v) == 1) total_kb = v;
                else if (std::sscanf(line, "MemAvailable: %lld kB", &v) == 1) avail_kb = v;
            }
            std::fclose(f);
            if (total_kb > 0) {
                mem_pct_ = static_cast<int>(std::clamp(((total_kb - avail_kb) * 100LL) / total_kb, 0LL, 100LL));
            }
        }
        bat_pct_ = -1;
        bat_charging_ = false;
        std::error_code ec;
        if (fs::exists("/sys/class/power_supply", ec)) {
            for (const auto& entry : fs::directory_iterator("/sys/class/power_supply", ec)) {
                std::string name = entry.path().filename().string();
                if (name.rfind("BAT", 0) != 0 && name.rfind("CMB", 0) != 0) continue;
                if (FILE* cf = std::fopen((entry.path() / "capacity").c_str(), "r")) {
                    int cap = -1;
                    if (std::fscanf(cf, "%d", &cap) == 1) {
                        bat_pct_ = std::clamp(cap, 0, 100);
                    }
                    std::fclose(cf);
                }
                if (FILE* sf = std::fopen((entry.path() / "status").c_str(), "r")) {
                    char st[32]{};
                    if (std::fgets(st, sizeof(st), sf)) {
                        bat_charging_ = (std::strncmp(st, "Charging", 8) == 0);
                    }
                    std::fclose(sf);
                }
                if (bat_pct_ >= 0) break;
            }
        }
    }

    void step_simulation(double dt) {
        time_s_ += dt;
        sys_timer_ += dt;
        if (sys_timer_ >= 1.5) {
            sys_timer_ = 0.0;
            update_sys_stats();
        }
        if (audio_reactive_) {
            audio_snap_ = audio_monitor_.snapshot();
            if (audio_snap_.beat_seq != last_beat_seq_) {
                last_beat_seq_ = audio_snap_.beat_seq;
                spring_vz_ += 3.4 * audio_snap_.beat_strength;
                ear_twitch_left_ = 0.16;
                ear_twitch_dir_ = -ear_twitch_dir_;
            }
        }
        smooth_mx_ += (mouse_nx_ - smooth_mx_) * std::min(1.0, dt * 8.0);
        smooth_my_ += (mouse_ny_ - smooth_my_) * std::min(1.0, dt * 8.0);

        // Underdamped bounce spring on Natsuki ASCII sprite
        double fz = (-spring_z_) * 84.0 - spring_vz_ * 4.4;
        spring_vz_ += fz * dt;
        spring_z_ += spring_vz_ * dt;

        double fx = (-spring_x_) * 72.0 - spring_vx_ * 4.8;
        spring_vx_ += fx * dt;
        spring_x_ += spring_vx_ * dt;

        double ftux = (-tux_z_) * 90.0 - tux_vz_ * 4.6;
        tux_vz_ += ftux * dt;
        tux_z_ += tux_vz_ * dt;
        if (tux_flap_left_ > 0.0) {
            tux_flap_left_ = std::max(0.0, tux_flap_left_ - dt);
        }
        if (lelouch_geass_left_ > 0.0) {
            lelouch_geass_left_ = std::max(0.0, lelouch_geass_left_ - dt);
        }

        if (blink_left_ > 0.0) {
            blink_left_ = std::max(0.0, blink_left_ - dt);
        } else {
            blink_timer_ -= dt;
            if (blink_timer_ <= 0.0) {
                blink_left_ = 0.16;
                blink_timer_ = 2.6 + static_cast<double>(rng_() % 24) * 0.12;
            }
        }

        if (ear_twitch_left_ > 0.0) {
            ear_twitch_left_ = std::max(0.0, ear_twitch_left_ - dt);
        } else {
            ear_twitch_timer_ -= dt;
            if (ear_twitch_timer_ <= 0.0) {
                ear_twitch_left_ = 0.24;
                ear_twitch_dir_ = -ear_twitch_dir_;
                ear_twitch_timer_ = 3.2 + static_cast<double>(rng_() % 28) * 0.15;
            }
        }

        if (bubble_timer_ > 0.0) {
            bubble_timer_ = std::max(0.0, bubble_timer_ - dt);
        }

        if (auto_mood_ && bubble_timer_ <= 0.0) {
            auto_mood_timer_ += dt;
            if (auto_mood_timer_ >= 8.5) {
                auto_mood_timer_ = 0.0;
                mood_idx_ = (mood_idx_ + 1) % static_cast<int>(sprites_.size());
            }
        }

        if (rain_enabled_) {
            for (auto& d : rain_) {
                d.y_norm += d.speed * dt;
                d.sway_phase += dt * 1.4;
                if (d.y_norm > 1.03) {
                    d.y_norm = -0.03;
                    d.x_norm = static_cast<double>(rng_() % 1000) / 1000.0;
                }
            }
        }

        for (auto it = ripples_.begin(); it != ripples_.end();) {
            it->age += dt;
            if (it->age >= it->max_age) {
                it = ripples_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = tags_.begin(); it != tags_.end();) {
            it->age += dt;
            if (it->age >= it->max_age) {
                it = tags_.erase(it);
            } else {
                it->x_px += it->vx * dt;
                it->y_px += it->vy * dt;
                it->vy += 26.0 * dt;
                ++it;
            }
        }
    }

    gboolean on_tick() {
        if (g_usr1_toggle.exchange(false)) {
            menu_open_ = !menu_open_;
        }
        gint64 now = g_get_monotonic_time();
        double dt = (last_tick_us_ > 0) ? (now - last_tick_us_) / 1000000.0 : (1.0 / 30.0);
        last_tick_us_ = now;
        dt = std::clamp(dt, 0.001, 0.05);
        fps_est_ = fps_est_ * 0.90 + (1.0 / dt) * 0.10;
        step_simulation(dt);
        queue_redraw();
        return G_SOURCE_CONTINUE;
    }

    static void draw_glyph(uint32_t* px, int buf_w, int buf_h, int stride_px,
                           const GlyphAtlas& atlas, char ch, int gx, int gy,
                           Rgb col, uint8_t opacity) {
        if (ch == ' ' || opacity == 0) return;
        const uint8_t* mask = atlas.glyph(ch);
        int cw = atlas.cell_w;
        int ch_px = atlas.cell_h;
        for (int dy = 0; dy < ch_px; ++dy) {
            int py = gy + dy;
            if (py < 0 || py >= buf_h) continue;
            uint32_t* row = px + static_cast<size_t>(py * stride_px);
            for (int dx = 0; dx < cw; ++dx) {
                int px_x = gx + dx;
                if (px_x < 0 || px_x >= buf_w) continue;
                uint8_t m = mask[dy * cw + dx];
                if (m == 0) continue;
                uint32_t a = (static_cast<uint32_t>(m) * static_cast<uint32_t>(opacity)) / 255U;
                if (a == 0) continue;
                uint32_t dst = row[px_x];
                uint32_t dr = (dst >> 16) & 0xff;
                uint32_t dg = (dst >> 8) & 0xff;
                uint32_t db = dst & 0xff;
                uint32_t inv = 255U - a;
                uint32_t nr = (col.r * a + dr * inv) / 255U;
                uint32_t ng = (col.g * a + dg * inv) / 255U;
                uint32_t nb = (col.b * a + db * inv) / 255U;
                row[px_x] = 0xff000000U | (nr << 16) | (ng << 8) | nb;
            }
        }
    }

    static void draw_ascii_text(uint32_t* px, int buf_w, int buf_h, int stride_px,
                                const GlyphAtlas& atlas, const std::string& text,
                                int start_x, int start_y, Rgb col, uint8_t opacity) {
        int x = start_x;
        for (unsigned char uc : text) {
            char ch = (uc >= 32 && uc <= 126) ? static_cast<char>(uc) : ' ';
            draw_glyph(px, buf_w, buf_h, stride_px, atlas, ch, x, start_y, col, opacity);
            x += atlas.cell_w;
        }
    }

    static std::string sanitize_ascii_box(const std::string& s) {
        // Keep printable ASCII cleanly; fold common Spanish accents so monospace glyph atlas prints crisply
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 32 && c <= 126) {
                out.push_back(static_cast<char>(c));
            } else if (c == 0xc3 && i + 1 < s.size()) {
                unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
                char rep = '?';
                switch (c2) {
                    case 0xa1: rep = 'a'; break;
                    case 0xa9: rep = 'e'; break;
                    case 0xad: rep = 'i'; break;
                    case 0xb3: rep = 'o'; break;
                    case 0xba: rep = 'u'; break;
                    case 0xb1: rep = 'n'; break;
                    case 0x81: rep = 'A'; break;
                    case 0x89: rep = 'E'; break;
                    case 0x8d: rep = 'I'; break;
                    case 0x93: rep = 'O'; break;
                    case 0x9a: rep = 'U'; break;
                    case 0x91: rep = 'N'; break;
                    default: rep = ' '; break;
                }
                out.push_back(rep);
                ++i;
            }
        }
        return out;
    }

    static void render_sprite_ascii(
        uint32_t* px, int w, int h, int stride_px,
        const GlyphAtlas& atlas, const ThemePalette& th,
        const RawRgba& sp, int ox_px, int oy_px, int draw_cols, int draw_rows,
        int ear_shift_px, double spring_x_val, bool face_boost, double geass_pulse)
    {
        if (sp.width <= 0 || sp.height <= 0 || draw_cols <= 0 || draw_rows <= 0) return;
        int cw = atlas.cell_w;
        int ch = atlas.cell_h;
        int ramp_len = static_cast<int>(std::strlen(kAsciiRamp));

        for (int r = 0; r < draw_rows; ++r) {
            double norm_r = static_cast<double>(r) / std::max(1, draw_rows - 1);
            int sy0 = (r * sp.height) / draw_rows;
            int sy1 = std::max(sy0 + 1, ((r + 1) * sp.height) / draw_rows);
            int sym = (sy0 + sy1) / 2;
            int row_shift = (ear_shift_px != 0 && norm_r < 0.25) ? ear_shift_px : 0;
            if (std::abs(spring_x_val) > 0.01 && norm_r >= 0.42 && norm_r <= 0.72) {
                row_shift += static_cast<int>(std::round(spring_x_val * 0.45)) * cw;
            }
            int gy = oy_px + r * ch;
            if (gy + ch < 0 || gy >= h) continue;

            for (int c = 0; c < draw_cols; ++c) {
                int sx0 = (c * sp.width) / draw_cols;
                int sx1 = std::max(sx0 + 1, ((c + 1) * sp.width) / draw_cols);
                int sxm = (sx0 + sx1) / 2;

                uint64_t sr = 0, sg = 0, sb = 0, sa = 0, wsum = 0, cnt = 0;
                double l_sum = 0.0, r_sum = 0.0, t_sum = 0.0, b_sum = 0.0;
                int l_cnt = 0, r_cnt = 0, t_cnt = 0, b_cnt = 0;
                double min_lum = 1.0, max_lum = 0.0;
                bool is_face_box = face_boost && (norm_r > 0.22 && norm_r < 0.48 && c > draw_cols / 4 && c < (draw_cols * 3) / 4);

                for (int py = sy0; py < sy1 && py < sp.height; ++py) {
                    const uint8_t* prow = &sp.rgba[static_cast<size_t>(py * sp.width) * 4];
                    bool is_cy = (std::abs(py - sym) <= std::max(1, (sy1 - sy0) / 4));
                    for (int px_i = sx0; px_i < sx1 && px_i < sp.width; ++px_i) {
                        uint8_t a = prow[px_i * 4 + 3];
                        double p_lum = 0.0;
                        if (a > 16) {
                            uint8_t rr = prow[px_i * 4 + 0];
                            uint8_t gg = prow[px_i * 4 + 1];
                            uint8_t bb = prow[px_i * 4 + 2];
                            bool is_cx = (std::abs(px_i - sxm) <= std::max(1, (sx1 - sx0) / 4));
                            uint64_t pw = (is_cx && is_cy)
                                ? (static_cast<uint64_t>(a) * (is_face_box ? 6U : 3U))
                                : static_cast<uint64_t>(a);
                            sr += static_cast<uint64_t>(rr) * pw;
                            sg += static_cast<uint64_t>(gg) * pw;
                            sb += static_cast<uint64_t>(bb) * pw;
                            wsum += pw;
                            double lum_px = (0.2126 * rr + 0.7152 * gg + 0.0722 * bb) / 255.0;
                            min_lum = std::min(min_lum, lum_px);
                            max_lum = std::max(max_lum, lum_px);
                            p_lum = lum_px * (a / 255.0);
                        }
                        sa += a;
                        cnt++;
                        if (px_i < sxm) { l_sum += p_lum; l_cnt++; } else { r_sum += p_lum; r_cnt++; }
                        if (py < sym) { t_sum += p_lum; t_cnt++; } else { b_sum += p_lum; b_cnt++; }
                    }
                }
                if (cnt == 0) continue;
                double avg_a = static_cast<double>(sa) / static_cast<double>(cnt);
                if (avg_a < 28.0 || wsum == 0) continue;

                uint8_t pr = static_cast<uint8_t>(std::min<uint64_t>(255, sr / wsum));
                uint8_t pg = static_cast<uint8_t>(std::min<uint64_t>(255, sg / wsum));
                uint8_t pb = static_cast<uint8_t>(std::min<uint64_t>(255, sb / wsum));

                double raw_lum = (0.2126 * pr + 0.7152 * pg + 0.0722 * pb) / 255.0;
                double sat = (std::max({pr, pg, pb}) - std::min({pr, pg, pb})) / 255.0;
                double local_contrast = max_lum - min_lum;
                double gx = (r_cnt > 0 ? r_sum / r_cnt : raw_lum) - (l_cnt > 0 ? l_sum / l_cnt : raw_lum);
                double gy_grad = (b_cnt > 0 ? b_sum / b_cnt : raw_lum) - (t_cnt > 0 ? t_sum / t_cnt : raw_lum);
                double grad = std::sqrt(gx * gx + gy_grad * gy_grad);

                bool is_geass_red = (geass_pulse > 0.0 && pr > pb + 42 && pr > pg + 30 && pr > 110);
                bool is_edge = (avg_a >= 160.0 && grad > 0.24);
                bool is_fine_dark = (avg_a >= 195.0 && min_lum < 0.20 && local_contrast > 0.36);
                bool is_dark_ink = (raw_lum < 0.26 || is_fine_dark);
                bool is_eye_spark = (!is_edge && avg_a >= 195.0 && max_lum > 0.90 && local_contrast > 0.35 && norm_r > 0.20 && norm_r < 0.46);
                bool is_pale_skin = (!is_edge && !is_fine_dark && raw_lum > 0.74 && sat < 0.28 && pr > pb);

                char ascii_ch = '#';
                if (is_geass_red) {
                    ascii_ch = (geass_pulse > 0.6) ? '@' : '*';
                } else if (is_eye_spark) {
                    ascii_ch = '*';
                } else if (is_edge) {
                    double ang = std::atan2(gy_grad, gx) * (180.0 / M_PI);
                    double abs_a = std::abs(ang);
                    if (abs_a < 22.5 || abs_a > 157.5) {
                        ascii_ch = '|';
                    } else if (abs_a > 67.5 && abs_a < 112.5) {
                        ascii_ch = (gy_grad > 0.0) ? '_' : '-';
                    } else if ((ang > 22.5 && ang < 67.5) || (ang < -112.5 && ang > -157.5)) {
                        ascii_ch = '\\';
                    } else {
                        ascii_ch = '/';
                    }
                } else if (is_dark_ink) {
                    static const char* kDarkInkGlyphs = "@#%:+*";
                    int di = std::clamp(static_cast<int>((0.30 - min_lum) * 20.0), 0, 5);
                    ascii_ch = kDarkInkGlyphs[di];
                } else if (is_pale_skin) {
                    static const char* kSkinGlyphs = ".:;~+";
                    int si = std::clamp(static_cast<int>((0.98 - raw_lum) * 20.0), 0, 4);
                    ascii_ch = kSkinGlyphs[si];
                } else {
                    double adj_lum = std::pow(std::clamp(raw_lum, 0.0, 1.0), 0.85);
                    double mapped = 0.32 + adj_lum * 0.64;
                    int ridx = std::clamp(static_cast<int>(std::round(mapped * (ramp_len - 1))), 10, ramp_len - 1);
                    ascii_ch = kAsciiRamp[ridx];
                }

                int cell_gx = ox_px + c * cw + row_shift;
                double back_mix = is_geass_red ? 0.94 : (is_dark_ink ? 0.88 : (is_pale_skin ? 0.90 : 0.88));
                if (avg_a > 40.0) {
                    Rgb bcol = th.tint_sprite
                        ? lerp_rgb({pr, pg, pb}, lerp_rgb(th.accent, th.accent_soft, raw_lum), th.tint_strength)
                        : Rgb{pr, pg, pb};
                    if (is_geass_red) {
                        bcol = {255, clamp_u8(static_cast<int>(40 + 60 * geass_pulse)), 90};
                    }
                    uint32_t bm = static_cast<uint32_t>(std::clamp(back_mix * (avg_a / 255.0) * 255.0, 0.0, 240.0));
                    uint32_t bi = 255U - bm;
                    for (int dy = 0; dy < ch; ++dy) {
                        int py = gy + dy;
                        if (py < 0 || py >= h) continue;
                        uint32_t* drow = px + static_cast<size_t>(py * stride_px);
                        for (int dx = 0; dx < cw; ++dx) {
                            int px_x = cell_gx + dx;
                            if (px_x < 0 || px_x >= w) continue;
                            uint32_t dst = drow[px_x];
                            uint32_t nr = (bcol.r * bm + ((dst >> 16) & 0xff) * bi) / 255U;
                            uint32_t ng = (bcol.g * bm + ((dst >> 8) & 0xff) * bi) / 255U;
                            uint32_t nb = (bcol.b * bm + (dst & 0xff) * bi) / 255U;
                            drow[px_x] = 0xff000000U | (nr << 16) | (ng << 8) | nb;
                        }
                    }
                }

                Rgb glyph_col = {pr, pg, pb};
                if (is_geass_red) {
                    glyph_col = {255, clamp_u8(static_cast<int>(45 + 110 * geass_pulse)), clamp_u8(static_cast<int>(80 + 70 * geass_pulse))};
                } else if (is_eye_spark) {
                    glyph_col = {255, 250, 255};
                } else if (is_dark_ink) {
                    glyph_col = is_fine_dark
                        ? Rgb{20, 12, 24}
                        : Rgb{
                            clamp_u8(static_cast<int>(pr * 0.92 + 26)),
                            clamp_u8(static_cast<int>(pg * 0.88 + 20)),
                            clamp_u8(static_cast<int>(pb * 0.94 + 30)),
                        };
                } else if (is_pale_skin) {
                    glyph_col = {
                        clamp_u8(static_cast<int>(pr)),
                        clamp_u8(static_cast<int>(pg * 0.95)),
                        clamp_u8(static_cast<int>(pb * 0.96)),
                    };
                } else {
                    glyph_col = {
                        clamp_u8(static_cast<int>(pr * 1.15)),
                        clamp_u8(static_cast<int>(pg * 1.10)),
                        clamp_u8(static_cast<int>(pb * 1.15)),
                    };
                }
                if (th.tint_sprite && !is_fine_dark && !is_geass_red) {
                    Rgb mono = lerp_rgb(th.accent, th.accent_soft, std::clamp(raw_lum + 0.15, 0.0, 1.0));
                    glyph_col = lerp_rgb(glyph_col, mono, th.tint_strength);
                }
                uint8_t alpha = static_cast<uint8_t>(std::clamp(
                    avg_a * 1.3 + (is_edge || is_fine_dark || is_geass_red ? 90.0 : 0.0) - (is_pale_skin ? 25.0 : 0.0),
                    115.0, 255.0));
                draw_glyph(px, w, h, stride_px, atlas, ascii_ch, cell_gx, gy, glyph_col, alpha);
            }
        }
    }

    void render_monitor(MonitorWindow& mw) {
        if (!mw.buffer) return;
        cairo_surface_flush(mw.buffer);
        uint32_t* px = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(mw.buffer));
        int stride_px = cairo_image_surface_get_stride(mw.buffer) / 4;
        int w = mw.width;
        int h = mw.height;
        const auto& th = themes_[theme_idx_];
        const auto& atlas = atlases_[density_idx_];
        int cw = atlas.cell_w;
        int ch = atlas.cell_h;
        int cols = std::max(20, w / cw);
        int rows = std::max(10, h / ch);
        const auto& ui_atlas = atlases_[std::min(static_cast<int>(atlases_.size()) - 1, std::max(3, density_idx_))];
        int ucw = ui_atlas.cell_w;
        int uch = ui_atlas.cell_h;

        double mx_px = smooth_mx_ * w;
        double my_px = smooth_my_ * h;
        const bool is_bliss = (th.id == "bliss_arch");

        // 1. Background: Windows XP Bliss ASCII landscape OR cyber/pastel dark gradient
        double anchor_x_ratio = tux_enabled_ ? (right_aligned_ ? 0.56 : 0.45) : (right_aligned_ ? 0.70 : 0.50);
        double glow_cx = w * anchor_x_ratio + (smooth_mx_ - 0.5) * 18.0;
        double glow_cy = h * 0.54 + (smooth_my_ - 0.5) * 10.0;

        auto bliss_hill_y = [&](double x_px) -> double {
            double nx = x_px / std::max(1, w);
            return h * (0.58 + 0.06 * std::sin(nx * 2.5 - 0.4) - 0.03 * std::cos(nx * 4.2));
        };

        if (is_bliss) {
            for (int y = 0; y < h; ++y) {
                uint32_t* row = px + static_cast<size_t>(y * stride_px);
                for (int x = 0; x < w; ++x) {
                    double hy = bliss_hill_y(x);
                    Rgb c;
                    if (y < hy) {
                        double st = std::clamp(static_cast<double>(y) / std::max(1.0, hy), 0.0, 1.0);
                        c = lerp_rgb({34, 102, 198}, {148, 206, 250}, st);
                    } else {
                        double gt = std::clamp((y - hy) / std::max(1.0, h - hy), 0.0, 1.0);
                        c = lerp_rgb({98, 174, 42}, {42, 104, 22}, gt);
                    }
                    row[x] = 0xff000000U | (static_cast<uint32_t>(c.r) << 16) |
                             (static_cast<uint32_t>(c.g) << 8) | static_cast<uint32_t>(c.b);
                }
            }
            // Fluffy drifting ASCII clouds in Bliss sky + lush ASCII grass texture on hill
            for (int r = 0; r < rows; ++r) {
                int gy = r * ch;
                for (int c = 0; c < cols; ++c) {
                    int gx = c * cw;
                    double hy = bliss_hill_y(gx);
                    if (gy < hy - ch) {
                        double nx = static_cast<double>(gx) / std::max(1, w);
                        double ny = static_cast<double>(gy) / std::max(1.0, hy);
                        double drift = nx - std::fmod(time_s_ * 0.006, 1.4);
                        double cloud_f = std::sin((drift) * 14.0 + ny * 4.5) * std::cos((drift) * 7.5 - ny * 6.0);
                        if (ny > 0.14 && ny < 0.88 && cloud_f > 0.42) {
                            double k = (cloud_f - 0.42) / 0.58;
                            char cc = (k > 0.65) ? '@' : ((k > 0.35) ? 'O' : ((k > 0.15) ? 'o' : '.'));
                            draw_glyph(px, w, h, stride_px, atlas, cc, gx, gy, {245, 250, 255},
                                       static_cast<uint8_t>(95 + k * 140));
                        }
                    } else if (((c * 7 + r * 13) % 5) == 0) {
                        char gc = ((c + r) % 3 == 0) ? '/' : (((c + r) % 3 == 1) ? '|' : ',');
                        Rgb gcol = ((c + r) % 2 == 0) ? Rgb{132, 210, 62} : Rgb{64, 146, 32};
                        draw_glyph(px, w, h, stride_px, atlas, gc, gx, gy, gcol, 130);
                    }
                }
            }
        } else {
            for (int y = 0; y < h; ++y) {
                double vy = static_cast<double>(y) / std::max(1, h - 1);
                Rgb base = lerp_rgb(th.bg_top, th.bg_bottom, vy);
                double scan = (th.scanlines && (y % 3 == 0)) ? 0.88 : 1.0;
                uint32_t* row = px + static_cast<size_t>(y * stride_px);
                int br = static_cast<int>(base.r * scan);
                int bg = static_cast<int>(base.g * scan);
                int bb = static_cast<int>(base.b * scan);
                uint32_t col32 = 0xff000000U | (static_cast<uint32_t>(br) << 16) |
                                 (static_cast<uint32_t>(bg) << 8) | static_cast<uint32_t>(bb);
                for (int x = 0; x < w; ++x) {
                    row[x] = col32;
                }
            }
            for (int r = 0; r < rows; ++r) {
                int gy = r * ch;
                for (int c = 0; c < cols; ++c) {
                    int gx = c * cw;
                    double dx = gx - glow_cx;
                    double dy = (gy - glow_cy) * 1.15;
                    double dist = std::sqrt(dx * dx + dy * dy);
                    double mdx = gx - mx_px;
                    double mdy = gy - my_px;
                    double mdist = std::sqrt(mdx * mdx + mdy * mdy);

                    if (mdist < 95.0) {
                        double k = 1.0 - mdist / 95.0;
                        char sym = (k > 0.68) ? '*' : ((k > 0.35) ? '+' : '.');
                        draw_glyph(px, w, h, stride_px, atlas, sym, gx, gy, th.accent_soft,
                                   static_cast<uint8_t>(45 + k * 145));
                    } else if (((c + r * 3) % 19) == 0 && dist < h * 0.62) {
                        double k = 1.0 - dist / (h * 0.62);
                        draw_glyph(px, w, h, stride_px, atlas, '.', gx, gy, th.accent,
                                   static_cast<uint8_t>(18 + k * 42));
                    }
                }
            }
        }

        // 2. Interactive falling ASCII rain / petals / matrix (swirls near cursor)
        if (rain_enabled_) {
            for (const auto& d : rain_) {
                double rx_px = d.x_norm * w + std::sin(d.sway_phase) * 12.0;
                double ry_px = d.y_norm * h;
                double dx = rx_px - mx_px;
                double dy = ry_px - my_px;
                double d2 = dx * dx + dy * dy;
                if (d2 > 1.0 && d2 < 16000.0) {
                    double push = (1.0 - std::sqrt(d2) / 126.5) * 34.0;
                    rx_px += (dx / std::sqrt(d2)) * push;
                    ry_px += (dy / std::sqrt(d2)) * push * 0.5;
                }
                int cell_x = (static_cast<int>(rx_px) / cw) * cw;
                int cell_y = (static_cast<int>(ry_px) / ch) * ch;
                draw_glyph(px, w, h, stride_px, atlas, d.ch, cell_x, cell_y, th.rain_color, d.alpha);
            }
        }

        // 3. Click ASCII ripples
        for (const auto& rp : ripples_) {
            double prog = rp.age / rp.max_age;
            double rad = 16.0 + prog * 185.0;
            uint8_t alp = static_cast<uint8_t>((1.0 - prog) * 235.0);
            int steps = 28;
            for (int s = 0; s < steps; ++s) {
                double ang = (2.0 * M_PI * s) / steps;
                int rx = static_cast<int>(rp.x_px + std::cos(ang) * rad);
                int ry = static_cast<int>(rp.y_px + std::sin(ang) * rad);
                char rc = rp.heart ? ((s % 2 == 0) ? '*' : '+') : ((prog < 0.4) ? 'O' : ((prog < 0.7) ? 'o' : '.'));
                draw_glyph(px, w, h, stride_px, atlas, rc, (rx / cw) * cw, (ry / ch) * ch, th.accent_soft, alp);
            }
        }

        // 4. Unified Crew Layout: Tux (+ Windows meme splash) on Left | Natsuki Center | Arch Logo Top | Lelouch Right
        mw.tux_box = {0, 0, 0, 0};
        mw.arch_box = {0, 0, 0, 0};
        mw.lelouch_box = {0, 0, 0, 0};

        // 4a. Natsuki jn_nya_cat
        const auto& mset = sprites_[mood_idx_];
        const RawRgba& sp = (blink_left_ > 0.0) ? mset.blink : mset.open;
        if (sp.width > 0 && sp.height > 0) {
            double target_px_h = h * (tux_enabled_ ? 0.72 : 0.90);
            int draw_rows = std::max(20, static_cast<int>(std::round(target_px_h / ch)));
            double aspect = static_cast<double>(sp.width) / static_cast<double>(sp.height);
            int draw_cols = std::max(20, static_cast<int>(std::round((draw_rows * ch * aspect) / cw)));

            double breath_y = std::sin(time_s_ * 2.1) * 3.0;
            double parallax_x = (smooth_mx_ - 0.5) * -14.0 + spring_x_ * 8.0;
            double parallax_y = (smooth_my_ - 0.5) * -7.0 - spring_z_ * 6.0 + breath_y;

            int ox_px = static_cast<int>(std::round(w * anchor_x_ratio - (draw_cols * cw) * 0.5 + parallax_x));
            int oy_px = static_cast<int>(std::round(h - uch * 3 - draw_rows * ch + parallax_y));
            ox_px = (ox_px / cw) * cw;
            oy_px = (oy_px / ch) * ch;
            mw.sprite_box = {ox_px, oy_px, draw_cols * cw, draw_rows * ch};

            int ear_shift_px = (ear_twitch_left_ > 0.0) ? (ear_twitch_dir_ * cw) : 0;
            render_sprite_ascii(px, w, h, stride_px, atlas, th, sp, ox_px, oy_px, draw_cols, draw_rows,
                                ear_shift_px, spring_x_, true, 0.0);
        }

        // 4b. Tux + "You are safe" + Windows Splash + Arch Linux Banner + Lelouch Geass
        if (tux_enabled_) {
            // Iconic Bliss meme header: "You are safe" stretched in upper sky cleanly centered
            if (is_bliss) {
                static const std::array<const char*, 5> kSafeText = {{
                    " Y   Y                         aaa                        sss             fff          ",
                    "  Y Y    ooo   u   u          a   a  r rrr  eee          s   s   aaa     f    eee      ",
                    "   Y    o   o  u   u          aaaaa  rr    e   e          sss   a   a   ffff e   e     ",
                    "   Y    o   o  u   u          a   a  r     eeeee             s  a   a    f   eeeee     ",
                    "   Y     ooo    uuuu          a   a  r      eee          ssss    aaaa    f    eee      ",
                }};
                int st_x = static_cast<int>(w * 0.28);
                int st_y = std::max(76, uch * 5);
                for (int r = 0; r < 5; ++r) {
                    draw_ascii_text(px, w, h, stride_px, ui_atlas, kSafeText[r], st_x + 2, st_y + r * uch + 2, {255, 255, 255}, 145);
                    draw_ascii_text(px, w, h, stride_px, ui_atlas, kSafeText[r], st_x, st_y + r * uch, {14, 16, 24}, 245);
                }
            }

            // Arch Linux official logo + wordmark banner (from uploaded Lelouch x Arch image)
            if (arch_sprite_.width > 0) {
                double arch_h_px = h * 0.135;
                int a_rows = std::max(8, static_cast<int>(std::round(arch_h_px / ch)));
                double a_asp = static_cast<double>(arch_sprite_.width) / static_cast<double>(arch_sprite_.height);
                int a_cols = std::max(14, static_cast<int>(std::round((a_rows * ch * a_asp) / cw)));
                int ax = static_cast<int>(std::round(w * 0.03 + (smooth_mx_ - 0.5) * -6.0));
                int ay = static_cast<int>(std::round(h * 0.16 + (smooth_my_ - 0.5) * -4.0));
                ax = (ax / cw) * cw;
                ay = (ay / ch) * ch;
                mw.arch_box = {ax, ay, a_cols * cw, a_rows * ch};
                render_sprite_ascii(px, w, h, stride_px, atlas, th, arch_sprite_, ax, ay, a_cols, a_rows, 0, 0.0, false, 0.0);
            }

            // Tux from Bliss meme on Left + Windows 4-pane logo getting splashed on the grass!
            if (tux_sprite_.width > 0) {
                double tux_h_px = h * 0.48;
                int t_rows = std::max(16, static_cast<int>(std::round(tux_h_px / ch)));
                double t_asp = static_cast<double>(tux_sprite_.width) / static_cast<double>(tux_sprite_.height);
                int t_cols = std::max(14, static_cast<int>(std::round((t_rows * ch * t_asp) / cw)));
                double tux_bob = std::sin(time_s_ * 2.4 + 0.9) * 2.5;
                int tx = static_cast<int>(std::round(w * 0.04 + (smooth_mx_ - 0.5) * -9.0));
                int ty = static_cast<int>(std::round(h - uch * 3 - t_rows * ch - tux_z_ * 6.0 + tux_bob));
                tx = (tx / cw) * cw;
                ty = (ty / ch) * ch;
                mw.tux_box = {tx, ty, t_cols * cw + ucw * 13, t_rows * ch};
                render_sprite_ascii(px, w, h, stride_px, atlas, th, tux_sprite_, tx, ty, t_cols, t_rows,
                                    (tux_flap_left_ > 0.0) ? cw : 0, 0.0, false, 0.0);

                // High-definition wavy 4-pane Windows XP flag on the grass + animated ASCII splash stream!
                double win_h_px = h * 0.24;
                int w_rows = std::max(10, static_cast<int>(std::round(win_h_px / ch)));
                double w_asp = static_cast<double>(win_flag_sprite_.width) / static_cast<double>(win_flag_sprite_.height);
                int w_cols = std::max(10, static_cast<int>(std::round((w_rows * ch * w_asp) / cw)));
                int wx0 = tx + t_cols * cw + ucw;
                int wy0 = ty + t_rows * ch - w_rows * ch - uch;
                mw.tux_box = {tx, ty, (wx0 + w_cols * cw) - tx, t_rows * ch};
                render_sprite_ascii(px, w, h, stride_px, atlas, th, win_flag_sprite_, wx0, wy0, w_cols, w_rows,
                                    0, 0.0, false, 0.0);

                // Animated ASCII splash arc from Tux onto the center of the wavy Windows flag
                int sx_start = tx + t_cols * cw - ucw;
                int sy_start = ty + static_cast<int>(t_rows * ch * 0.66);
                int wx_target = wx0 + (w_cols * cw) / 2;
                int wy_target = wy0 + (w_rows * ch) / 2;
                for (int si = 0; si < 11; ++si) {
                    double st = (si + std::fmod(time_s_ * 7.5, 1.0)) / 11.0;
                    int arc_x = static_cast<int>(sx_start + st * (wx_target - sx_start));
                    int arc_y = static_cast<int>(sy_start + st * st * (wy_target - sy_start) - std::sin(st * M_PI) * 18.0);
                    char sc = (si > 7) ? '*' : ((si % 2 == 0) ? '~' : 'o');
                    Rgb scol = (si > 7) ? Rgb{235, 252, 255} : Rgb{255, 236, 120};
                    draw_glyph(px, w, h, stride_px, ui_atlas, sc, arc_x, arc_y, scol, 235);
                }
            }

            // Lelouch (Zero) with Geass red eye pose on Right side (from uploaded Lelouch x Arch image)
            if (lelouch_sprite_.width > 0) {
                double l_h_px = h * 0.80;
                int l_rows = std::max(20, static_cast<int>(std::round(l_h_px / ch)));
                double l_asp = static_cast<double>(lelouch_sprite_.width) / static_cast<double>(lelouch_sprite_.height);
                int l_cols = std::max(20, static_cast<int>(std::round((l_rows * ch * l_asp) / cw)));
                double l_parx = (smooth_mx_ - 0.5) * -10.0;
                double l_pary = (smooth_my_ - 0.5) * -5.0 + std::sin(time_s_ * 1.7 + 0.5) * 2.5;
                int lx = static_cast<int>(std::round(w - l_cols * cw - 6.0 + l_parx));
                int ly = static_cast<int>(std::round(h - uch * 2 - l_rows * ch + l_pary));
                lx = (lx / cw) * cw;
                ly = (ly / ch) * ch;
                mw.lelouch_box = {lx, ly, l_cols * cw, l_rows * ch};
                double g_pulse = 0.45 + 0.35 * std::sin(time_s_ * 3.4) + std::clamp(lelouch_geass_left_, 0.0, 1.0) * 0.5;
                render_sprite_ascii(px, w, h, stride_px, atlas, th, lelouch_sprite_, lx, ly, l_cols, l_rows,
                                    0, 0.0, true, g_pulse);
            }
        }

        // 5. Floating ASCII tags/hearts
        for (const auto& t : tags_) {
            double fade = 1.0 - (t.age / t.max_age);
            uint8_t alp = static_cast<uint8_t>(std::clamp(fade * 255.0, 0.0, 255.0));
            draw_ascii_text(px, w, h, stride_px, ui_atlas, t.text,
                            static_cast<int>(t.x_px), static_cast<int>(t.y_px), t.color, alp);
        }

        // 6. ASCII dialogue box bubble
        if (bubble_timer_ > 0.0 && !bubble_text_.empty()) {
            std::string clean = sanitize_ascii_box(bubble_text_);
            int max_chars = std::clamp((w / ucw) / 3, 24, 38);
            std::vector<std::string> lines;
            size_t pos = 0;
            while (pos < clean.size()) {
                size_t take = std::min<size_t>(max_chars, clean.size() - pos);
                if (pos + take < clean.size()) {
                    size_t sp_pos = clean.rfind(' ', pos + take);
                    if (sp_pos != std::string::npos && sp_pos > pos) take = sp_pos - pos;
                }
                std::string chunk = clean.substr(pos, take);
                while (!chunk.empty() && chunk.front() == ' ') chunk.erase(chunk.begin());
                if (!chunk.empty()) lines.push_back(chunk);
                pos += take;
                while (pos < clean.size() && clean[pos] == ' ') pos++;
            }
            if (!lines.empty()) {
                int inner_w = 18;
                for (const auto& l : lines) inner_w = std::max(inner_w, static_cast<int>(l.size()));
                int box_cols = inner_w + 4;
                int box_rows = static_cast<int>(lines.size()) + 2;
                int bx = tux_enabled_
                    ? std::clamp(static_cast<int>(w * 0.18), ucw * 2, w - (box_cols + 2) * ucw)
                    : (right_aligned_
                        ? std::max(ucw * 2, mw.sprite_box[0] - (box_cols + 2) * ucw)
                        : std::min(w - (box_cols + 2) * ucw, mw.sprite_box[0] + mw.sprite_box[2] + ucw * 2));
                if (bx < ucw * 2) bx = ucw * 2;
                int by = tux_enabled_
                    ? std::max(uch * 12, static_cast<int>(h * 0.30))
                    : std::max(uch * 4, mw.sprite_box[1] + uch * 3);

                constexpr Rgb kMonoBg{18, 18, 22};
                constexpr Rgb kMonoBorder{196, 196, 204};
                constexpr Rgb kMonoText{244, 244, 248};

                uint8_t box_alpha = static_cast<uint8_t>(std::clamp(bubble_timer_ * 2.0, 0.0, 1.0) * 245.0);
                // Matte monochrome charcoal box backdrop for crisp readability
                for (int yy = std::max(0, by - 4); yy < std::min(h, by + box_rows * uch + 4); ++yy) {
                    uint32_t* row = px + static_cast<size_t>(yy * stride_px);
                    for (int xx = std::max(0, bx - 4); xx < std::min(w, bx + box_cols * ucw + 4); ++xx) {
                        uint32_t dst = row[xx];
                        uint32_t dr = (((dst >> 16) & 0xff) * 20U + kMonoBg.r * 80U) / 100U;
                        uint32_t dg = (((dst >> 8) & 0xff) * 20U + kMonoBg.g * 80U) / 100U;
                        uint32_t db = ((dst & 0xff) * 20U + kMonoBg.b * 80U) / 100U;
                        row[xx] = 0xff000000U | (dr << 16) | (dg << 8) | db;
                    }
                }

                std::string top = "+-- Natsuki.nya " + std::string(std::max(1, inner_w - 13), '-') + "+";
                std::string bot = "+" + std::string(inner_w + 2, '-') + "+";
                draw_ascii_text(px, w, h, stride_px, ui_atlas, top, bx, by, kMonoBorder, box_alpha);
                for (size_t li = 0; li < lines.size(); ++li) {
                    std::string padded = "| " + lines[li] + std::string(inner_w - lines[li].size(), ' ') + " |";
                    draw_ascii_text(px, w, h, stride_px, ui_atlas, padded,
                                    bx, by + static_cast<int>(li + 1) * uch, kMonoText, box_alpha);
                }
                draw_ascii_text(px, w, h, stride_px, ui_atlas, bot,
                                bx, by + static_cast<int>(lines.size() + 1) * uch, kMonoBorder, box_alpha);
            }
        }

        // 7. Monochrome UI Chrome: Tiny Gear Action Button (top-right corner or Super+Alt+P) + Monochrome ASCII Drawer
        constexpr Rgb kUiBg{18, 18, 22};
        constexpr Rgb kUiBorder{196, 196, 204};
        constexpr Rgb kUiText{244, 244, 248};
        constexpr Rgb kUiMuted{168, 168, 178};
        mw.buttons.clear();

        // Top-left minimal monochrome RAM / CPU / Battery status
        char sys_buf[128];
        if (bat_pct_ >= 0) {
            std::snprintf(sys_buf, sizeof(sys_buf), "RAM:%d%%  |  CPU:%s  |  BAT:%d%%%s",
                          mem_pct_, load_str_.c_str(), bat_pct_, bat_charging_ ? "+" : "");
        } else {
            std::snprintf(sys_buf, sizeof(sys_buf), "RAM:%d%%  |  CPU:%s",
                          mem_pct_, load_str_.c_str());
        }
        draw_ascii_text(px, w, h, stride_px, ui_atlas, sys_buf, 16, 72, kUiText, 195);

        // Audio-reactive ASCII spectrum bars when enabled
        if (audio_reactive_) {
            for (size_t bi = 0; bi < 8; ++bi) {
                double e = std::clamp(static_cast<double>(audio_snap_.bands[bi]), 0.0, 1.0);
                int bar_rows = 1 + static_cast<int>(std::round(e * 12.0));
                int bx = 18 + static_cast<int>(bi) * (ucw * 2);
                for (int br = 0; br < bar_rows; ++br) {
                    int by = h - 14 - (br + 1) * uch;
                    char bc = (br == bar_rows - 1) ? '*' : ((br > bar_rows / 2) ? '#' : '|');
                    draw_glyph(px, w, h, stride_px, ui_atlas, bc, bx, by,
                               (br > 7) ? th.accent_soft : th.accent, 215);
                }
            }
        }

        // Single ultra-tiny discreet monochrome cogwheel (gear) button in top-right corner
        const int gear_sz = 20;
        const int gear_x = w - gear_sz - 12;
        const int gear_y = 70;
        mw.buttons.push_back({gear_x - 3, gear_y - 3, gear_sz + 6, gear_sz + 6, "gear", "gear"});
        const bool gear_hover = (mx_px >= gear_x - 3 && mx_px <= gear_x + gear_sz + 3 &&
                                 my_px >= gear_y - 3 && my_px <= gear_y + gear_sz + 3);

        cairo_surface_mark_dirty(mw.buffer);
        {
            cairo_t* gcr = cairo_create(mw.buffer);
            double r_rad = 6.0;
            cairo_new_sub_path(gcr);
            cairo_arc(gcr, gear_x + gear_sz - r_rad, gear_y + r_rad, r_rad, -M_PI / 2.0, 0.0);
            cairo_arc(gcr, gear_x + gear_sz - r_rad, gear_y + gear_sz - r_rad, r_rad, 0.0, M_PI / 2.0);
            cairo_arc(gcr, gear_x + r_rad, gear_y + gear_sz - r_rad, r_rad, M_PI / 2.0, M_PI);
            cairo_arc(gcr, gear_x + r_rad, gear_y + r_rad, r_rad, M_PI, 3.0 * M_PI / 2.0);
            cairo_close_path(gcr);
            cairo_set_source_rgba(gcr,
                                  kUiBg.r / 255.0, kUiBg.g / 255.0, kUiBg.b / 255.0,
                                  (menu_open_ || gear_hover) ? 0.88 : 0.42);
            cairo_fill_preserve(gcr);
            cairo_set_line_width(gcr, 1.0);
            cairo_set_source_rgba(gcr, kUiBorder.r / 255.0, kUiBorder.g / 255.0, kUiBorder.b / 255.0,
                                  (menu_open_ || gear_hover) ? 0.94 : 0.46);
            cairo_stroke(gcr);

            // Micro monochrome vector cogwheel centered inside the 20x20 gear pill
            double cx = gear_x + gear_sz * 0.5;
            double cy = gear_y + gear_sz * 0.5;
            cairo_save(gcr);
            cairo_translate(gcr, cx, cy);
            cairo_rotate(gcr, menu_open_ ? 0.45 : (gear_hover ? time_s_ * 1.4 : 0.0));
            cairo_set_source_rgba(gcr,
                                  (menu_open_ || gear_hover) ? kUiText.r / 255.0 : kUiBorder.r / 255.0,
                                  (menu_open_ || gear_hover) ? kUiText.g / 255.0 : kUiBorder.g / 255.0,
                                  (menu_open_ || gear_hover) ? kUiText.b / 255.0 : kUiBorder.b / 255.0,
                                  (menu_open_ || gear_hover) ? 0.96 : 0.72);
            constexpr int kTeeth = 8;
            for (int ti = 0; ti < kTeeth; ++ti) {
                double a = (2.0 * M_PI * ti) / kTeeth;
                cairo_save(gcr);
                cairo_rotate(gcr, a);
                cairo_rectangle(gcr, -1.3, -6.8, 2.6, 2.5);
                cairo_fill(gcr);
                cairo_restore(gcr);
            }
            cairo_set_line_width(gcr, 1.8);
            cairo_arc(gcr, 0.0, 0.0, 3.8, 0.0, 2.0 * M_PI);
            cairo_stroke(gcr);
            cairo_restore(gcr);
            cairo_destroy(gcr);
        }
        cairo_surface_flush(mw.buffer);

        // Toggleable floating monochrome ASCII settings menu below the gear button
        if (menu_open_) {
            static const char* kDensLabels[] = {"Hyper", "Ultra", "Fino", "Medio", "Retro"};
            std::vector<std::pair<std::string, std::string>> defs = {
                {"mood", "Mood:    " + std::string(auto_mood_ ? "Auto/" : "") + sprites_[mood_idx_].label},
                {"theme", "Tema:    " + themes_[theme_idx_].label},
                {"density", "ASCII:   " + std::string(kDensLabels[density_idx_])},
                {"rain", std::string("Lluvia:  ") + (rain_enabled_ ? "ON" : "OFF")},
                {"pos", std::string("Pos:     ") + (right_aligned_ ? "Derecha" : "Centro")},
                {"tux", std::string("Tux+Arch:") + (tux_enabled_ ? "ON" : "OFF")},
                {"audio", std::string("Audio:   ") + (audio_reactive_ ? "ON" : "OFF")},
                {"gear", "Cerrar menu [x]"},
            };
            int max_lbl = 22;
            for (const auto& [a, l] : defs) {
                (void)a;
                max_lbl = std::max(max_lbl, static_cast<int>(l.size()) + 4);
            }
            int box_w = (max_lbl + 4) * ucw;
            int box_h = (static_cast<int>(defs.size()) + 3) * uch + 10;
            int mx0 = w - box_w - 12;
            int my0 = gear_y + gear_sz + 8;

            for (int yy = my0; yy < std::min(h, my0 + box_h); ++yy) {
                uint32_t* row = px + static_cast<size_t>(yy * stride_px);
                for (int xx = std::max(0, mx0); xx < std::min(w, mx0 + box_w); ++xx) {
                    uint32_t dst = row[xx];
                    uint32_t dr = (((dst >> 16) & 0xff) * 14U + kUiBg.r * 86U) / 100U;
                    uint32_t dg = (((dst >> 8) & 0xff) * 14U + kUiBg.g * 86U) / 100U;
                    uint32_t db = ((dst & 0xff) * 14U + kUiBg.b * 86U) / 100U;
                    row[xx] = 0xff000000U | (dr << 16) | (dg << 8) | db;
                }
            }
            std::string hdr = "+-- Ajustes ASCII " + std::string(std::max(1, max_lbl - 15), '-') + "+";
            std::string ftr = "+" + std::string(max_lbl + 2, '-') + "+";
            draw_ascii_text(px, w, h, stride_px, ui_atlas, hdr, mx0 + ucw, my0 + 4, kUiBorder, 245);
            for (size_t di = 0; di < defs.size(); ++di) {
                int ry = my0 + 6 + static_cast<int>(di + 1) * uch;
                std::string row_txt = "[ " + defs[di].second + " ]";
                int rw = static_cast<int>(row_txt.size()) * ucw;
                mw.buttons.push_back({mx0 + ucw * 2, ry - 2, rw, uch + 3, defs[di].first, defs[di].second});
                bool hov = (mx_px >= mx0 + ucw * 2 && mx_px <= mx0 + ucw * 2 + rw &&
                            my_px >= ry - 2 && my_px <= ry + uch + 1);
                draw_ascii_text(px, w, h, stride_px, ui_atlas, row_txt, mx0 + ucw * 2, ry,
                                hov ? kUiText : kUiBorder, hov ? 255 : 225);
            }
            std::time_t tnow = std::time(nullptr);
            std::tm tm_now{};
            localtime_r(&tnow, &tm_now);
            char clk[48];
            std::snprintf(clk, sizeof(clk), "clicks:%d  %02d:%02d:%02d", total_clicks_, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            int info_y = my0 + 8 + static_cast<int>(defs.size() + 1) * uch;
            draw_ascii_text(px, w, h, stride_px, ui_atlas, clk, mx0 + ucw * 2, info_y, kUiMuted, 200);
            draw_ascii_text(px, w, h, stride_px, ui_atlas, ftr, mx0 + ucw, info_y + uch, kUiBorder, 245);
        }

        cairo_surface_mark_dirty(mw.buffer);
    }

    int run_self_test() {
        update_sys_stats();
        MonitorWindow mw;
        mw.width = 1366;
        mw.height = 768;
        mw.buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mw.width, mw.height);
        render_monitor(mw);

        // Verify sprite hitbox bounding box and interactive zones
        if (mw.sprite_box[2] < 100 || mw.sprite_box[3] < 200) {
            std::cerr << "[self-test] FAIL: sprite_box too small\n";
            cairo_surface_destroy(mw.buffer);
            return 10;
        }
        double ear_x = mw.sprite_box[0] + mw.sprite_box[2] * 0.5;
        double ear_y = mw.sprite_box[1] + mw.sprite_box[3] * 0.16;
        if (detect_click_zone(mw, ear_x, ear_y) != "ears") {
            std::cerr << "[self-test] FAIL: ear zone detection mismatch\n";
            cairo_surface_destroy(mw.buffer);
            return 11;
        }
        double bell_y = mw.sprite_box[1] + mw.sprite_box[3] * 0.50;
        if (detect_click_zone(mw, ear_x, bell_y) != "bell") {
            std::cerr << "[self-test] FAIL: bell zone detection mismatch\n";
            cairo_surface_destroy(mw.buffer);
            return 12;
        }

        // Simulate click + spring step + theme toggle
        on_button_press(mw, ear_x, ear_y, 1);
        step_simulation(0.033);
        if (spring_z_ <= 0.0 || sprites_[mood_idx_].id != "blush") {
            std::cerr << "[self-test] FAIL: pat interaction did not trigger blush/spring\n";
            cairo_surface_destroy(mw.buffer);
            return 13;
        }
        on_button_press(mw, 20.0, 20.0, 3); // right-click cycles theme
        if (theme_idx_ != 1) {
            std::cerr << "[self-test] FAIL: theme toggle did not cycle\n";
            cairo_surface_destroy(mw.buffer);
            return 14;
        }
        if (mw.tux_box[2] < 50 || mw.arch_box[2] < 50 || mw.lelouch_box[2] < 50) {
            std::cerr << "[self-test] FAIL: Tux / Arch / Lelouch ASCII boxes missing\n";
            cairo_surface_destroy(mw.buffer);
            return 14;
        }
        on_button_press(mw, mw.lelouch_box[0] + mw.lelouch_box[2] * 0.5, mw.lelouch_box[1] + mw.lelouch_box[3] * 0.5, 1);
        if (lelouch_geass_left_ <= 0.0) {
            std::cerr << "[self-test] FAIL: Lelouch click did not trigger Geass pulse\n";
            cairo_surface_destroy(mw.buffer);
            return 14;
        }
        on_button_press(mw, mw.tux_box[0] + mw.tux_box[2] * 0.3, mw.tux_box[1] + mw.tux_box[3] * 0.5, 1);
        step_simulation(0.033);
        if (tux_z_ <= 0.0) {
            std::cerr << "[self-test] FAIL: Tux click did not trigger bounce\n";
            cairo_surface_destroy(mw.buffer);
            return 14;
        }
        theme_idx_ = 0; // Restore Bliss x Arch for preview snapshot
        on_button_press(mw, mw.width - 22.0, 80.0, 1); // Tiny gear button opens menu
        if (!menu_open_) {
            std::cerr << "[self-test] FAIL: gear button did not toggle settings menu\n";
            cairo_surface_destroy(mw.buffer);
            return 14;
        }
        on_button_press(mw, mw.width - 22.0, 80.0, 1); // Gear closes menu cleanly
        render_monitor(mw);

        const uint32_t* px = reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(mw.buffer));
        int stride_px = cairo_image_surface_get_stride(mw.buffer) / 4;
        uint32_t base_col = px[0];
        int non_bg = 0;
        for (int y = 0; y < mw.height; ++y) {
            for (int x = 0; x < mw.width; ++x) {
                if (px[y * stride_px + x] != base_col) non_bg++;
            }
        }
        cairo_surface_write_to_png(mw.buffer, "/tmp/arch_ascii_wallpaper_preview.png");
        cairo_surface_destroy(mw.buffer);
        if (non_bg < 5000) {
            std::cerr << "[self-test] FAIL: rendered ASCII wallpaper has too few drawn pixels (" << non_bg << ")\n";
            return 15;
        }

        std::cout << "ASCII WALLPAPER SELF-TEST PASSED (6 jn_nya_cat moods, 5 themes, 4 glyph atlases, interactive hitboxes + spring, rendered_px="
                  << non_bg << ")\n";
        return 0;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool self_test = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        }
    }
    AsciiWallpaperApp app(self_test);
    return app.run(argc, argv);
}
