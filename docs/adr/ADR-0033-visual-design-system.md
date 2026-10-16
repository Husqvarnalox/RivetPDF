# ADR-0033: Visual design system (themes, icons, shell layout)

Status: Accepted

Date: 2026-10-16

## Context

The shell grew feature by feature with ~80 hardcoded colors across 36
files, text-only gray buttons, no dark mode, and per-platform differences in
chrome. The product direction (decided 2026-10-16) is **native-feeling and
calm**: follow each OS's conventions, neutral greys, the system accent color,
light and dark themes, the document as the focus.

## Decision

1. **Design tokens in `rivet_ui`.** `ui/Theme.hpp` defines semantic tokens,
   never raw colors at call sites: `windowBackground`, `chromeBackground`
   (toolbar/tab strip/sidebar), `chromeSeparator`, `canvasBackground` (behind
   pages), `pageShadow`, `controlFill` / `controlFillHover` /
   `controlFillPressed`, `controlBorder`, `textPrimary` / `textSecondary` /
   `textDisabled`, `accent` / `accentText` / `accentSubtle` (selection
   backgrounds), `focusRing`, `danger`, `success`, `warning`,
   `searchHighlight`, `selectionHighlight`, `linkText`, `codeBackground`,
   plus metrics: spacing scale (4/8/12/16/24), radius (control 6, card 8),
   control heights (toolbar 28, small 22), font roles (body 13, small 11,
   heading sizes for Markdown) and stroke widths. Two palettes (Light, Dark)
   computed from neutral greys plus the platform accent; contrast of text
   tokens ≥ 4.5:1 against their backgrounds (unit-tested).
2. **Appearance from the platform.** `platform::IAppearance` (nullable in
   `ShellServices`): `isDark()`, `accentColor()`, `onChange(callback)` —
   macOS: `NSApp.effectiveAppearance` + `NSColor.controlAccentColor` (KVO);
   Windows: `AppsUseLightTheme` registry value + `DwmGetColorizationColor`
   / UISettings accent, `WM_SETTINGCHANGE`; Linux: `RIVET_THEME` env, then
   the XSETTINGS `Net/ThemeName` containing "dark", else light; accent
   default blue. The shell applies the theme to the widget tree and repaints
   on change; window chrome follows (macOS appearance, Windows immersive dark
   title bar, Linux in-window menu bar).
3. **Icons as code.** `ui/Icons.hpp`: a portable vector icon set drawn with
   `ui::Path` on a 20x20 grid, 1.5 px rounded strokes, in `textPrimary` (or
   `accent` when active): open, save, print, sidebar, zoom in/out, fit width,
   fit page, actual size, search, previous/next, undo/redo, rotate left/right,
   select, hand, highlight, underline, strikeout, note, ink, rectangle,
   ellipse, line/arrow, stamp, sign, edit content, add text, crop, redact,
   form, compare, OCR, markdown rendered/source/split, close, chevrons, more.
   No image assets, crisp at any scale, same on all platforms.
4. **Controls.** Icon buttons (borderless; hover/pressed fills; tooltip with
   the command title and shortcut after 600 ms), segmented controls (modes:
   Rendered/Source/Split, sidebar Pages/Outline/Signatures, tool groups),
   a zoom control (− [percent popover] +), a search field with icon, focus
   rings for keyboard focus only. Disabled = `textDisabled`, never hidden.
5. **Shell layout.** Tab strip (title, dirty dot, close on hover, active tab
   joins the toolbar) → unified toolbar (left: sidebar toggle, open/save;
   center: zoom group, page navigation; right: tool groups as a segmented
   control: View | Annotate | Edit | Forms | Redact, then search, print,
   overflow) → context bar under the toolbar for the active tool's options
   (annotation colors/stroke, content properties, redaction apply) → content
   (sidebar 220 px resizable with segmented header; canvas with
   `canvasBackground`, pages with 1 px border + soft shadow, 16 px gap) →
   status bar (small, `textSecondary`). Empty workspace shows a calm welcome
   view: app name, "Open…" primary button, recent files (stored through
   `IAppDataLocation`, max 10, paths only), drag-and-drop hint.
6. **Markdown preview** uses theme tokens (text, links, code, quote bar,
   table rules) and follows dark mode; PDF pages stay as authored (white);
   comparison/highlight colors are translucent tokens valid in both themes.
7. **Motion** is minimal: 120 ms fades for hover/tooltip, no animated
   layout.

## Consequences

- One look across macOS, Windows and Linux, adapted by platform appearance.
- Every new widget must use tokens; a test greps that no `rgba(` literals
  remain outside `ui/Theme.cpp` and the icon file.

## Implementation notes (2026-10-17)

- **Tokens** live in `ui/Theme.{hpp,cpp}`: `ui::Palette` (one struct of
  colors per theme, built by `Theme::make(dark, accent)`), `ui::metrics`
  (spacing, radii, control/bar heights, sidebar 220 px [160..420], page gap
  16, fade 120 ms, tooltip delay 600 ms) and `ui::fonts` roles. The current
  theme is a main-thread global (`ui::currentTheme()`, `ui::colors()`,
  `themeGeneration()` for caches such as the Markdown palette). Accent-derived
  text/fill tokens are pushed to >= 4.5:1 by `ensureContrast`
  (`tests/ui/TestTheme.cpp`, also with unusual accents).
- **Appearance**: `platform::IAppearance` (`ShellServices::appearance`).
  `RIVET_THEME=light|dark` overrides everywhere. macOS observes
  `effectiveAppearance` (KVO) and system color changes; Windows reads
  `AppsUseLightTheme` + `DwmGetColorizationColor` and re-reads on
  `WM_SETTINGCHANGE`/`WM_THEMECHANGED`/`WM_DWMCOLORIZATIONCOLORCHANGED`.
  Linux reads `RIVET_THEME`, `GTK_THEME` and the XSETTINGS `Net/ThemeName`
  (pure parser in `LinuxXSettings`) **once at startup**: live theme changes
  are not tracked there (deviation). The Windows native `HMENU` menu bar is
  not themed (only the immersive dark title bar follows); the Linux in-window
  menu bar is a Rivet widget and follows the theme.
- **No raw colors**: `tests/ui/TestNoHardcodedColors.cpp` scans `src/ui` and
  `src/app` for `Color::rgba/gray/white/black` and `Color{...}` literals;
  only `ui/Theme.cpp` and `ui/Icons.cpp` are allowed.
- **Icons**: `ui/Icons.{hpp,cpp}` (enum `ui::Icon`, `drawIcon`), stroked
  `ui::Path`s on a 20x20 grid. **Controls**: `IconButton`, `SegmentedControl`,
  `ZoomControl` (percent opens a preset `PopupMenu`), `Tooltip` (delay through
  `ui::IDelayedRunner`; the shell runs it on the task scheduler fenced by an
  `AsyncScope` and delivers it through the main-thread dispatcher), focus rings
  only while keyboard-navigating; `Toolbar` groups (leading/center/trailing)
  with Tab/Shift-Tab/arrow navigation, entered by a Tab the content did not use.
- **Shell** (`ShellController`): tab strip -> toolbar -> one context bar
  (annotation / content-edit / forms / redaction for the tool mode, Markdown
  format icons) -> sidebar (segmented header, splitter) + canvas -> status
  bar. Tool modes are derived from the controllers' state
  (`ShellController::toolMode()`), so menu shortcuts and the segmented control
  stay in sync. The overflow "more" menu is generated from
  `app::buildMenuBar()`. The empty workspace shows `WelcomeView` with
  `RecentFiles` (`recent-files.txt` in the app-data directory; paths only,
  max 10, missing files greyed and not openable). Hover leaves are tracked at
  the root (`ShellRoot::onPointer`) and platform pointer-leave events.

