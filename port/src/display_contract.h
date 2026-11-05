/* display_contract.h -- pure 60 Parsecs! display/aspect policy.
 *
 * The SDL/EGL facade owns the physical drawable.  Unity must see one coherent
 * size through EGL, ANativeWindow and the small Java display surface; otherwise
 * it keeps the Android 1280x720 logical window while presenting into a 4:3
 * drawable and produces the observed top/bottom bars.
 */

#ifndef SF_DISPLAY_CONTRACT_H
#define SF_DISPLAY_CONTRACT_H

enum sf_aspect_policy {
    SF_ASPECT_FILL = 0,
    SF_ASPECT_NATIVE = 1,
    /* Screen-adaptive: Android and present both use the measured drawable.
     * Content is the full frame (no 16:9 crop), matching portmaster_nfsmw. */
    SF_ASPECT_SCREEN = 2,
};

typedef struct {
    enum sf_aspect_policy policy;
    int drawable_width;
    int drawable_height;
    int android_width;
    int android_height;
    int lock_android_size;
    int identity;
    int requested_policy_valid;
} sf_display_contract;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} sf_display_rect;

/* Builds a deterministic contract without touching GL or SDL state.
 *
 * screen (default): Android reports the measured drawable and present is 1:1
 *                   — resolution adapts to the device panel (nfsmw-style).
 * fill:             Android reports the measured drawable; present expands a
 *                   centered 16:9 source to the drawable (legacy stretch).
 * native (rollback): preserve the historical 1280x720 Android surface.
 *
 * The 1280x720 case is an identity in either policy.  Invalid policy strings
 * fail soft to screen but are identified in the receipt so a typo is visible.
 */
sf_display_contract sf_display_contract_make(int drawable_width,
                                             int drawable_height,
                                             const char *requested_policy);

const char *sf_display_policy_name(enum sf_aspect_policy policy);

/* Returns 1 when a centered 16:9 source must be expanded to fill the
 * drawable, 0 when the drawable is already 16:9, and -1 for invalid input. */
int sf_display_fill_source_rect(int drawable_width, int drawable_height,
                                sf_display_rect *source);

/* Screen-adaptive source: the whole drawable is the content (always 0 on
 * success, -1 on invalid input). */
int sf_display_screen_source_rect(int drawable_width, int drawable_height,
                                  sf_display_rect *source);

#endif
