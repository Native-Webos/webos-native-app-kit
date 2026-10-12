# SPDX-License-Identifier: GPL-3.0-or-later
# Locale, catalogs with plural forms, number formatting. SDL-free; unit-tested on the host.
add_library(wnk_i18n STATIC src/i18n.cpp)
target_include_directories(wnk_i18n PUBLIC include)
target_include_directories(wnk_i18n SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
