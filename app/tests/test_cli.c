/* Modified by appcpy: percentage scaling for flex displays, key mapping, pausing video while minimized and secure displays. */
#include "common.h"

#include <assert.h>
#include <string.h>

#include "cli.h"
#include "options.h"

static void test_flag_version(void) {
    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
    };

    char *argv[] = {"scrcpy", "-v"};

    bool ok = scrcpy_parse_args(&args, 2, argv);
    assert(ok);
    assert(!args.help);
    assert(args.version);
}

static void test_flag_help(void) {
    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
    };

    char *argv[] = {"scrcpy", "-v"};

    bool ok = scrcpy_parse_args(&args, 2, argv);
    assert(ok);
    assert(!args.help);
    assert(args.version);
}

static void test_options(void) {
    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
    };

    char *argv[] = {
        "scrcpy",
        "--always-on-top",
        "--video-bit-rate", "5M",
        "--crop", "100:200:300:400",
        "--fullscreen",
        "--max-fps", "30",
        "--max-size", "1024",
        // "--no-control" is not compatible with "--turn-screen-off"
        // "--no-playback" is not compatible with "--fulscreen"
        "--port", "1234:1236",
        "--push-target", "/sdcard/Movies",
        "--record", "file",
        "--record-format", "mkv",
        "--serial", "0123456789abcdef",
        "--show-touches",
        "--turn-screen-off",
        "--prefer-text",
        "--window-title", "my device",
        "--window-x", "100",
        "--window-y", "-1",
        "--window-width", "600",
        "--window-height", "0",
        "--window-borderless",
    };

    bool ok = scrcpy_parse_args(&args, ARRAY_LEN(argv), argv);
    assert(ok);

    const struct scrcpy_options *opts = &args.opts;
    assert(opts->always_on_top);
    assert(opts->video_bit_rate == 5000000);
    assert(!strcmp(opts->crop, "100:200:300:400"));
    assert(opts->fullscreen);
    assert(!strcmp(opts->max_fps, "30"));
    assert(opts->max_size == 1024);
    assert(opts->port_range.first == 1234);
    assert(opts->port_range.last == 1236);
    assert(!strcmp(opts->push_target, "/sdcard/Movies"));
    assert(!strcmp(opts->record_filename, "file"));
    assert(opts->record_format == SC_RECORD_FORMAT_MKV);
    assert(!strcmp(opts->serial, "0123456789abcdef"));
    assert(opts->show_touches);
    assert(opts->turn_screen_off);
    assert(opts->key_inject_mode == SC_KEY_INJECT_MODE_TEXT);
    assert(!strcmp(opts->window_title, "my device"));
    assert(opts->window_x == 100);
    assert(opts->window_y == -1);
    assert(opts->window_width == 600);
    assert(opts->window_height == 0);
    assert(opts->window_borderless);
}

static void test_options2(void) {
    struct scrcpy_cli_args args = {
        .opts = scrcpy_options_default,
        .help = false,
        .version = false,
    };

    char *argv[] = {
        "scrcpy",
        "--no-control",
        "--no-playback",
        "--record", "file.mp4", // cannot enable --no-playback without recording
    };

    bool ok = scrcpy_parse_args(&args, ARRAY_LEN(argv), argv);
    assert(ok);

    const struct scrcpy_options *opts = &args.opts;
    assert(!opts->control);
    assert(!opts->video_playback);
    assert(!opts->audio_playback);
    assert(!strcmp(opts->record_filename, "file.mp4"));
    assert(opts->record_format == SC_RECORD_FORMAT_MP4);
}

static void test_parse_shortcut_mods(void) {
    uint8_t mods;
    bool ok;

    ok = sc_parse_shortcut_mods("lctrl", &mods);
    assert(ok);
    assert(mods == SC_SHORTCUT_MOD_LCTRL);

    ok = sc_parse_shortcut_mods("rctrl,lalt", &mods);
    assert(ok);
    assert(mods == (SC_SHORTCUT_MOD_RCTRL | SC_SHORTCUT_MOD_LALT));

    ok = sc_parse_shortcut_mods("lsuper,rsuper,lctrl", &mods);
    assert(ok);
    assert(mods == (SC_SHORTCUT_MOD_LSUPER
                  | SC_SHORTCUT_MOD_RSUPER
                  | SC_SHORTCUT_MOD_LCTRL));

    ok = sc_parse_shortcut_mods("", &mods);
    assert(!ok);

    ok = sc_parse_shortcut_mods("lctrl+", &mods);
    assert(!ok);

    ok = sc_parse_shortcut_mods("lctrl,", &mods);
    assert(!ok);
}

static void test_flex_scale(void) {
    struct scrcpy_cli_args args = {.opts = scrcpy_options_default};
    char *valid[] = {"scrcpy", "--new-display", "-x", "--flex-display-scale=50"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(valid), valid));
    assert(args.opts.flex_display_scale == 50);
    assert(args.opts.render_fit == SC_RENDER_FIT_LETTERBOX);

    args.opts = scrcpy_options_default;
    char *normal[] = {"scrcpy", "--new-display", "-x"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(normal), normal));
    assert(args.opts.flex_display_scale == 100);
    assert(args.opts.render_fit == SC_RENDER_FIT_UNSCALED);

    assert(!args.opts.flex_display_integer_scaling);
    const char *integer_scales[] = {"25", "33", "50", "75", "100"};
    for (unsigned i = 0; i < ARRAY_LEN(integer_scales); ++i) {
        args.opts = scrcpy_options_default;
        char *integer[] = {"scrcpy", "--new-display", "-x",
                          "--flex-display-integer-scaling",
                          "--flex-display-scale", (char *) integer_scales[i]};
        assert(scrcpy_parse_args(&args, ARRAY_LEN(integer), integer));
        assert(args.opts.flex_display_integer_scaling);
    }
    args.opts = scrcpy_options_default;
    char *integer_no_flex[] = {"scrcpy", "--flex-display-integer-scaling"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(integer_no_flex), integer_no_flex));

    char *bad_values[] = {"0", "24", "101", "50.5", "nan"};
    for (unsigned i = 0; i < ARRAY_LEN(bad_values); ++i) {
        args.opts = scrcpy_options_default;
        char *bad[] = {"scrcpy", "--new-display", "-x", "--flex-display-scale", bad_values[i]};
        assert(!scrcpy_parse_args(&args, ARRAY_LEN(bad), bad));
    }
    args.opts = scrcpy_options_default;
    char *no_flex[] = {"scrcpy", "--flex-display-scale=50"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(no_flex), no_flex));
}

static void test_secure_display_option(void) {
    struct scrcpy_cli_args args = {.opts = scrcpy_options_default};
    assert(!args.opts.secure_display);
    char *valid[] = {"scrcpy", "--new-display", "--secure-display"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(valid), valid));
    assert(args.opts.secure_display);

    args.opts = scrcpy_options_default;
    char *no_display[] = {"scrcpy", "--secure-display"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(no_display), no_display));

    args.opts = scrcpy_options_default;
    char *no_control[] = {"scrcpy", "--new-display", "-n",
                          "--secure-display"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(no_control), no_control));
    assert(args.opts.secure_display);
}

static void test_pause_video_when_minimized(void) {
    struct scrcpy_cli_args args = {.opts = scrcpy_options_default};
    assert(!args.opts.pause_video_when_minimized);
    char *valid[] = {"scrcpy", "--new-display", "--pause-video-when-minimized"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(valid), valid));
    assert(args.opts.pause_video_when_minimized);

    assert(!args.opts.freeze_app_when_minimized);

    args.opts = scrcpy_options_default;
    char *freeze[] = {"scrcpy", "--new-display", "--freeze-app-when-minimized"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(freeze), freeze));
    assert(args.opts.pause_video_when_minimized);
    assert(args.opts.freeze_app_when_minimized);

    args.opts = scrcpy_options_default;
    char *freeze_mirror[] = {"scrcpy", "--freeze-app-when-minimized"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(freeze_mirror), freeze_mirror));

    args.opts = scrcpy_options_default;
    char *mirror[] = {"scrcpy", "--pause-video-when-minimized"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(mirror), mirror));

    args.opts = scrcpy_options_default;
    char *no_control[] = {"scrcpy", "--new-display", "-n",
                          "--pause-video-when-minimized"};
    assert(!scrcpy_parse_args(&args, ARRAY_LEN(no_control), no_control));
}

static void test_keymap_option(void) {
    struct scrcpy_cli_args args = {.opts = scrcpy_options_default};
    assert(!args.opts.keymap);
    char *argv[] = {"scrcpy", "--keymap=C:/data/com.example.game.keymap"};
    assert(scrcpy_parse_args(&args, ARRAY_LEN(argv), argv));
    assert(!strcmp(args.opts.keymap, "C:/data/com.example.game.keymap"));
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    test_flag_version();
    test_flag_help();
    test_options();
    test_options2();
    test_parse_shortcut_mods();
    test_flex_scale();
    test_keymap_option();
    test_pause_video_when_minimized();
    test_secure_display_option();
    return 0;
}
