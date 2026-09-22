#include "nook.h"
#include <math.h>
#include <stdio.h>

static gboolean valid_color(const char *value) {
    if (!value || strlen(value) != 7 || value[0] != '#') return FALSE;
    for (int i = 1; i < 7; i++) if (!g_ascii_isxdigit(value[i])) return FALSE;
    return TRUE;
}

static gboolean set_color(GKeyFile *file, const char *key, char **target, GError **error) {
    if (!g_key_file_has_key(file, "theme", key, NULL)) return TRUE;
    char *value = g_key_file_get_string(file, "theme", key, error);
    if (!value) return FALSE;
    if (!valid_color(value)) {
        g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "%s must be a #RRGGBB color", key);
        g_free(value);
        return FALSE;
    }
    g_free(*target);
    *target = value;
    return TRUE;
}

static gboolean set_opacity(GKeyFile *file, const char *key, double *target, GError **error) {
    if (!g_key_file_has_key(file, "theme", key, NULL)) return TRUE;
    double value = g_key_file_get_double(file, "theme", key, error);
    if (error && *error) return FALSE;
    if (value < 0 || value > 1 || !isfinite(value)) {
        g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "%s must be between 0 and 1", key);
        return FALSE;
    }
    *target = value;
    return TRUE;
}

NookTheme *nook_theme_load(const char *path, GError **error) {
    NookTheme *theme = g_new0(NookTheme, 1);
    theme->panel = g_strdup("#101010");
    theme->card = g_strdup("#191919");
    theme->card_hover = g_strdup("#272727");
    theme->border = g_strdup("#474747");
    theme->accent = g_strdup("#f5f5f5");
    theme->text = g_strdup("#f4f4f4");
    theme->muted = g_strdup("#b1b1b1");
    theme->panel_opacity = .80;
    theme->card_opacity = .42;
    theme->card_hover_opacity = .58;
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return theme;
    GKeyFile *file = g_key_file_new();
    gboolean ok = g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, error);
    if (ok) {
        const char *allowed[] = {"panel", "card", "card_hover", "border", "accent",
            "text", "muted", "panel_opacity", "card_opacity", "card_hover_opacity"};
        gsize count = 0;
        char **keys_in_file = g_key_file_has_group(file, "theme")
            ? g_key_file_get_keys(file, "theme", &count, error) : NULL;
        if (error && *error) ok = FALSE;
        for (gsize i = 0; ok && i < count; i++) {
            gboolean known = FALSE;
            for (guint j = 0; j < G_N_ELEMENTS(allowed); j++)
                if (!g_strcmp0(keys_in_file[i], allowed[j])) known = TRUE;
            if (!known) {
                g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                            "Unknown theme key: %s", keys_in_file[i]);
                ok = FALSE;
            }
        }
        g_strfreev(keys_in_file);
        const char *keys[] = {"panel", "card", "card_hover", "border", "accent", "text", "muted"};
        char **slots[] = {&theme->panel, &theme->card, &theme->card_hover,
                         &theme->border, &theme->accent, &theme->text, &theme->muted};
        for (guint i = 0; ok && i < G_N_ELEMENTS(keys); i++)
            ok = set_color(file, keys[i], slots[i], error);
        if (ok) ok = set_opacity(file, "panel_opacity", &theme->panel_opacity, error);
        if (ok) ok = set_opacity(file, "card_opacity", &theme->card_opacity, error);
        if (ok) ok = set_opacity(file, "card_hover_opacity", &theme->card_hover_opacity, error);
    }
    g_key_file_unref(file);
    if (ok) return theme;
    nook_theme_free(theme);
    return NULL;
}

void nook_theme_free(NookTheme *theme) {
    if (!theme) return;
    g_free(theme->panel);
    g_free(theme->card);
    g_free(theme->card_hover);
    g_free(theme->border);
    g_free(theme->accent);
    g_free(theme->text);
    g_free(theme->muted);
    g_free(theme);
}

static void add_rgba(GString *css, const char *name, const char *hex, double opacity) {
    guint r, g, b;
    char alpha[G_ASCII_DTOSTR_BUF_SIZE];
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    g_ascii_formatd(alpha, sizeof(alpha), "%.3f", opacity);
    g_string_append_printf(css, "@define-color nook_%s rgba(%u, %u, %u, %s);\n",
                           name, r, g, b, alpha);
}

char *nook_theme_css(const NookTheme *theme) {
    GString *css = g_string_new(NULL);
    add_rgba(css, "panel", theme->panel, theme->panel_opacity);
    add_rgba(css, "card", theme->card, theme->card_opacity);
    add_rgba(css, "card_hover", theme->card_hover, theme->card_hover_opacity);
    add_rgba(css, "accent_soft", theme->accent, .10);
    add_rgba(css, "accent_line", theme->accent, .12);
    add_rgba(css, "accent_border", theme->accent, .30);
    add_rgba(css, "accent_hover", theme->accent, .52);
    add_rgba(css, "accent_icon", theme->accent, .09);
    add_rgba(css, "accent_outline", theme->accent, .13);
    g_string_append_printf(css,
        "@define-color nook_border %s;\n@define-color nook_accent %s;\n"
        "@define-color nook_text %s;\n@define-color nook_muted %s;\n"
        "@define-color nook_preview #090909;\n",
        theme->border, theme->accent, theme->text, theme->muted);
    return g_string_free(css, FALSE);
}
