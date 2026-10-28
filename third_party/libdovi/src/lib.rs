// SPDX-License-Identifier: GPL-3.0-or-later
// Links libdovi in: its C API (#[no_mangle] extern "C" in a private module of the dolby_vision crate,
// built with the "capi" feature) is exported from this static library.
extern crate dolby_vision;
