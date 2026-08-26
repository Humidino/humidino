#pragma once
#include <lvgl.h>

// Шрифты с поддержкой кириллицы, сгенерированные из системных шрифтов
// Arial/Arial Bold через lv_font_conv (встроенные битмап-шрифты Montserrat в
// LVGL кириллицу не содержат). Диапазон глифов: базовая латиница + знак
// градуса + А-я/Ёё.
// Перегенерировать так (запускать из src/fonts/):
//   npx lv_font_conv@1.5.3 --font <ttf> -o <out>.c --format lvgl --bpp 4 \
//     --size <px> --lv-font-name <name> -r 0x20-0x7E,0xB0,0x401,0x410-0x44F,0x451

LV_FONT_DECLARE(font_ru_14);
LV_FONT_DECLARE(font_ru_20);
LV_FONT_DECLARE(font_ru_28_bold);
