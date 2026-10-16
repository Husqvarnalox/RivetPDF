// SPDX-License-Identifier: MPL-2.0
#pragma once

// Xlib, wrapped. Xlib.h #defines short identifiers (Status, None, Bool,
// Success, ...) that collide with Rivet names such as core::Status and
// core::ErrorCode::None. Include this header LAST in a .cpp file (after every
// Rivet and standard header) and never from another header; it removes the
// colliding macros and offers typed replacements.

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cairo-xlib.h>

namespace rivet::platform::x11 {
inline constexpr unsigned long kNone = 0UL; // Xlib's None
using XStatus = int;                         // Xlib's Status
} // namespace rivet::platform::x11

#undef Status
#undef None
#undef Bool
#undef Success
#undef Always
#undef Above
#undef Below
#undef Unsorted
#undef DestroyAll
