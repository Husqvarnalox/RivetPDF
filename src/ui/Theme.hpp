// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/UiTypes.hpp"

#include <cstdint>
#include <optional>

namespace rivet::ui {

// Design tokens of the Rivet shell (ADR-0033). Call sites never use raw
// colors: they read a semantic token from the current theme at paint time,
// so a theme switch (light/dark, accent change) is a repaint.
//
// Two palettes (Light, Dark) are computed from neutral greys plus the
// platform accent. Every text token has a contrast ratio of at least 4.5:1
// against the backgrounds it is drawn on (unit-tested); textDisabled is
// exempt (inactive controls, as in WCAG 1.4.3).
struct Palette {
    // Surfaces.
    Color windowBackground;    // content panes (Markdown, source editor, lists)
    Color chromeBackground;    // tab strip, toolbar, sidebar, status bar
    Color chromeSeparator;     // hairlines between chrome areas
    Color chromeActiveTab;     // the active tab (joins the toolbar)
    Color canvasBackground;    // behind PDF pages
    Color pageBackground;      // PDF "paper" (pages stay as authored: white)
    Color pageBorder;          // 1 px page outline
    Color pageShadow;          // soft page shadow
    Color presentationBackground;
    Color panelBackground;     // floating panels (find bar, prompts, sheets)
    Color popoverBackground;   // menus and popovers
    Color popoverBorder;
    Color tooltipBackground;
    Color tooltipText;
    Color scrim;               // dims content (crop tool)
    Color hudBackground;       // dark heads-up bars over the page
    Color hudText;

    // Controls.
    Color controlFill;         // resting fill (segmented track, push button)
    Color controlFillHover;
    Color controlFillPressed;
    Color controlBorder;
    Color controlSelectedFill; // selected segment / thumb of a segmented control
    Color fieldBackground;     // text fields
    Color fieldBorder;
    Color scrollTrack;
    Color scrollThumb;
    Color scrollThumbActive;
    Color listSelection;       // selected row in sidebars (unfocused-neutral)

    // Text.
    Color textPrimary;
    Color textSecondary;
    Color textDisabled;

    // Accent (platform accent color).
    Color accent;              // icons, borders, carets of the active state
    Color accentText;          // accent-colored text legible on chrome/window
    Color accentSubtle;        // selection backgrounds, active-tool tint
    Color accentFill;          // primary button fill (onAccent text on it)
    Color onAccent;            // text/icons on accentFill
    Color focusRing;

    // Status.
    Color danger;
    Color success;
    Color warning;

    // Content highlights (translucent, valid over pages in both themes).
    Color searchHighlight;
    Color searchHighlightCurrent;
    Color selectionHighlight;  // focused text selection
    Color selectionInactive;   // unfocused text selection
    Color linkText;
    Color linkHover;
    Color codeBackground;
    Color inlineCodeBackground;

    // Semantic document overlays (ADR-0033 §6): drawn over white pages, so
    // they keep one hue in both themes.
    Color comparisonDeleted;
    Color comparisonDeletedSelected;
    Color comparisonDeletedPage;
    Color comparisonInserted;
    Color comparisonInsertedSelected;
    Color comparisonInsertedPage;
    Color formFieldHighlight;
    Color formFieldHover;
    Color formFieldEditedBorder;
    Color formOptionHighlight;
    Color redactionMark;
    Color editorPaper;         // inline editors on the page (paper white)
    Color editorInk;           // their text
    Color editorPaperBorder;
    Color objectReadOnly;      // outline of content that cannot be edited
};

// Spacing scale, radii, control heights and stroke widths (ADR-0033 §1).
namespace metrics {
inline constexpr double kSpace4 = 4.0;
inline constexpr double kSpace8 = 8.0;
inline constexpr double kSpace12 = 12.0;
inline constexpr double kSpace16 = 16.0;
inline constexpr double kSpace24 = 24.0;

inline constexpr double kRadiusControl = 6.0;
inline constexpr double kRadiusCard = 8.0;
inline constexpr double kRadiusSmall = 4.0;

inline constexpr double kControlHeight = 28.0;      // toolbar controls
inline constexpr double kSmallControlHeight = 22.0; // context bar / sidebar header
inline constexpr double kIconSize = 20.0;           // icon grid
inline constexpr double kIconStroke = 1.5;
inline constexpr double kHairline = 1.0;

inline constexpr double kTabStripHeight = 32.0;
inline constexpr double kToolbarHeight = 44.0;
inline constexpr double kContextBarHeight = 36.0;
inline constexpr double kStatusBarHeight = 24.0;
inline constexpr double kSidebarWidth = 220.0;
inline constexpr double kSidebarMinWidth = 160.0;
inline constexpr double kSidebarMaxWidth = 420.0;
inline constexpr double kPageGap = 16.0;
inline constexpr double kPageShadowRadius = 3.0;

inline constexpr double kHoverFadeSeconds = 0.12; // ADR-0033 §7
inline constexpr double kTooltipDelaySeconds = 0.6;
} // namespace metrics

// Font roles. Font weight and size only: the family is the platform's UI
// font (and its fixed-pitch font for monospace).
namespace fonts {
inline constexpr Font kBody{13.0, Font::Weight::Regular};
inline constexpr Font kBodyStrong{13.0, Font::Weight::Semibold};
inline constexpr Font kSmall{11.0, Font::Weight::Regular};
inline constexpr Font kSmallStrong{11.0, Font::Weight::Semibold};
inline constexpr Font kStatus{11.5, Font::Weight::Regular};
inline constexpr Font kTitle{22.0, Font::Weight::Semibold};   // welcome view
inline constexpr Font kHeadline{15.0, Font::Weight::Semibold}; // panel titles
// Markdown heading sizes (H1..H6) relative to a 16 px body.
inline constexpr double kHeadingScale[6] = {2.0, 1.5, 1.25, 1.1, 1.0, 0.9};
} // namespace fonts

struct Theme {
    bool dark = false;
    Palette colors;

    // The platform's default accent (macOS system blue) when none is given.
    static Color defaultAccent(bool dark);
    // Builds a palette from neutral greys and `accent` (nullopt = default).
    static Theme make(bool dark, std::optional<Color> accent = std::nullopt);
    static Theme light() { return make(false); }
    static Theme darkTheme() { return make(true); }
};

// The process-wide current theme. Main thread only: widgets read it while
// painting; the shell replaces it when the platform appearance changes and
// repaints. Starts as Theme::light().
const Theme& currentTheme();
void setCurrentTheme(const Theme& theme);
// Incremented by every setCurrentTheme(): views that cache derived colors
// compare it to refresh lazily.
std::uint64_t themeGeneration();
// Shorthand for currentTheme().colors.
inline const Palette& colors() { return currentTheme().colors; }

// Color math used by the palettes and the contrast tests.
Color withAlpha(const Color& color, double alpha);
// `top` composited over an opaque `bottom` (source-over).
Color composite(const Color& top, const Color& bottom);
// Linear mix: t = 0 -> a, t = 1 -> b (alpha mixed too).
Color mix(const Color& a, const Color& b, double t);
// WCAG 2.x relative luminance of an opaque color (alpha ignored).
double relativeLuminance(const Color& color);
// WCAG contrast ratio of `foreground` (composited over `background` when
// translucent) against an opaque `background`; 1..21.
double contrastRatio(const Color& foreground, const Color& background);
// `color` moved toward black or white (whichever raises contrast) until it
// reaches `ratio` against `background` (or the extreme).
Color ensureContrast(const Color& color, const Color& background, double ratio);

} // namespace rivet::ui
