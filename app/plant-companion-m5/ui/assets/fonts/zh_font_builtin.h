/****************************************************************************
 * Flash-resident GB2312 level-1 fallback fonts.
 *
 * The generated binary has the same PLANTZH1 layout used by the former SD
 * files.  Keeping the arrays in XIP flash removes the SD-card dependency for
 * dynamic AI, diagnosis and voice text.
 ****************************************************************************/

#ifndef __PLANT_UI_ASSETS_FONTS_ZH_FONT_BUILTIN_H
#define __PLANT_UI_ASSETS_FONTS_ZH_FONT_BUILTIN_H

#define ZH_FONT_BUILTIN_12_SIZE 336314u
#define ZH_FONT_BUILTIN_16_SIZE 545389u
#define ZH_FONT_BUILTIN_19_SIZE 769885u

extern const unsigned char zh_font_builtin_12[];
extern const unsigned char zh_font_builtin_16[];
extern const unsigned char zh_font_builtin_19[];

#endif /* __PLANT_UI_ASSETS_FONTS_ZH_FONT_BUILTIN_H */
