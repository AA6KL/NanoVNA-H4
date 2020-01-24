/*
 * Copyright (c) 2014-2020, TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ch.h"
#include "hal.h"
#include "chprintf.h"
#include "nanovna.h"
#include <stdlib.h>
#include <string.h>

#ifdef NANOVNA_F303
#include "adc_F303.h"
#endif

uistat_t uistat = {
 digit: 6,
 current_trace: 0,
 lever_mode: LM_MARKER,
 marker_delta: FALSE,
 marker_smith_format: MS_RLC
};



#define NO_EVENT          0
#define EVT_BUTTON_SINGLE_CLICK    0x01
#define EVT_BUTTON_DOUBLE_CLICK    0x02
#define EVT_BUTTON_DOWN_LONG       0x04
#define EVT_UP                     0x10
#define EVT_DOWN                   0x20
#define EVT_REPEAT                 0x40

#define BUTTON_DOWN_LONG_TICKS     5000  /* 500ms */
#define BUTTON_DOUBLE_TICKS        5000  /* 500ms */
#define BUTTON_REPEAT_TICKS        1000  /* 100ms */
#define BUTTON_DEBOUNCE_TICKS       100  /*  10ms */

/* lever switch assignment */
// Need to adjust board.h file to algn with the code here!
#define BIT_UP1   3
#define BIT_PUSH  2
#define BIT_DOWN1 1

#define READ_PORT() palReadPort(GPIOA)
#define BUTTON_MASK (0x0f)

static volatile uint16_t last_button = 0;
static volatile systime_t last_button_down_ticks;
static volatile systime_t last_button_repeat_ticks;
static volatile int8_t inhibit_until_release = FALSE;

uint8_t operation_requested = OP_NONE;

int8_t previous_marker = -1;

enum {
  UI_NORMAL, UI_MENU, UI_NUMERIC, UI_KEYPAD
};

enum {
  KM_START, KM_STOP, KM_CENTER, KM_SPAN, KM_CW, KM_SCALE, KM_REFPOS, KM_EDELAY, KM_VELOCITY_FACTOR, KM_SCALEDELAY,KM_BRIGHTNESS
};

static uint8_t ui_mode = UI_NORMAL;
static uint8_t keypad_mode;
static int8_t selection = 0;

typedef void (*menuaction_cb_t)(int item);

typedef struct menuitem_t menuitem_t;

struct menuitem_t {
  uint8_t type;
  char* label;
  union {
    const menuaction_cb_t pFunc;
    const menuitem_t* pMenu;
  };
};

// type of menu item 
enum {
    MT_NONE,
    MT_BLANK,
    MT_SUBMENU,
    MT_CALLBACK,
    MT_CANCEL,
//    MT_CLOSE
};

#define MENUITEM_MENU(text, pmenu) { .type=MT_SUBMENU, .label=text, .pMenu=pmenu }
#define MENUITEM_FUNC(text, pfunc) { .type=MT_CALLBACK, .label=text, .pFunc=pfunc }
//#define MENUITEM_CLOSE { .type=MT_CLOSE, .label="CLOSE", .pMenu=NULL }
#define MENUITEM_BACK { .type=MT_CANCEL, .label=S_LARROW" BACK", .pMenu=NULL }
#define MENUITEM_END { .type=MT_NONE, .label=NULL, .pMenu=NULL } /* sentinel */


static volatile int8_t last_touch_status = FALSE;
static volatile int16_t last_touch_x;
static volatile int16_t last_touch_y;
//int16_t touch_cal[4] = { 1000, 1000, 10*16, 12*16 };
//int16_t touch_cal[4] = { 620, 600, 130, 180 };
#define EVT_TOUCH_NONE 0
#define EVT_TOUCH_DOWN 1
#define EVT_TOUCH_PRESSED 2
#define EVT_TOUCH_RELEASED 3

int awd_count;
//int touch_x, touch_y;

#define NUMINPUT_LEN 10

#define KP_CONTINUE 0
#define KP_DONE 1
#define KP_CANCEL 2

static char kp_buf[11];
static int8_t kp_index = 0;


static void ui_mode_normal(void);
static void ui_mode_menu(void);
static void ui_mode_numeric(int _keypad_mode);
static void ui_mode_keypad(int _keypad_mode);
static void draw_menu(void);
static void leave_ui_mode(void);
static void erase_menu_buttons(void);
static void ui_process_keypad(void);
//static void ui_process_numeric(void);

static void menu_push_submenu(const menuitem_t *submenu);
static void menu_move_back(void);
static void menu_calop_cb(int item);
static void menu_caldone_cb(int item);
static void menu_save_cb(int item);
static void menu_cal2_cb(int item);
static void menu_trace_cb(int item);
static void menu_format2_cb(int item);
static void menu_format_cb(int item);
static void menu_scale_cb(int item);
static void menu_channel_cb(int item);
static void menu_transform_window_cb(int item);
static void menu_transform_cb(int item);
static void menu_stimulus_cb(int item);
static void menu_marker_sel_cb(int item);
static void menu_marker_op_cb(int item);
static void menu_marker_smith_cb(int item);
static void menu_marker_search_cb(int item);
static void menu_recall_cb(int item);
static void menu_dfu_cb(int item);
static void menu_config_cb(int item);

// ===[MENU DEFINITION]=========================================================
static const menuitem_t menu_calop[] = {
  MENUITEM_FUNC("OPEN",     menu_calop_cb),
  MENUITEM_FUNC("SHORT",    menu_calop_cb),
  MENUITEM_FUNC("LOAD",     menu_calop_cb),
  MENUITEM_FUNC("ISOLN",    menu_calop_cb),
  MENUITEM_FUNC("THRU",     menu_calop_cb),
  MENUITEM_FUNC("DONE",     menu_caldone_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_save[] = {
  MENUITEM_FUNC("SAVE 0",   menu_save_cb),
  MENUITEM_FUNC("SAVE 1",   menu_save_cb),
  MENUITEM_FUNC("SAVE 2",   menu_save_cb),
  MENUITEM_FUNC("SAVE 3",   menu_save_cb),
 #if !defined(ANTENNA_ANALYZER)
  MENUITEM_FUNC("SAVE 4",   menu_save_cb),
  #endif
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_cal[] = {
  MENUITEM_MENU("CALIBRATE",    menu_calop),
  MENUITEM_MENU("SAVE",         menu_save),
  MENUITEM_FUNC("RESET",        menu_cal2_cb),
  MENUITEM_FUNC("CORRECTION",   menu_cal2_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_trace[] = {
  MENUITEM_FUNC("TRACE 0",      menu_trace_cb),
  MENUITEM_FUNC("TRACE 1",      menu_trace_cb),
//   #if !defined(ANTENNA_ANALYZER)
  MENUITEM_FUNC("TRACE 2",      menu_trace_cb),
  MENUITEM_FUNC("TRACE 3",      menu_trace_cb),
//    #endif
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_format2[] = {
  MENUITEM_FUNC("POLAR",        menu_format2_cb),
  MENUITEM_FUNC("LINEAR",       menu_format2_cb),
  MENUITEM_FUNC("REAL",         menu_format2_cb),
  MENUITEM_FUNC("IMAG",         menu_format2_cb),
  MENUITEM_FUNC("RESISTANCE",   menu_format2_cb),
  MENUITEM_FUNC("REACTANCE",    menu_format2_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_format[] = {
  MENUITEM_FUNC("LOGMAG",       menu_format_cb),
  MENUITEM_FUNC("PHASE",        menu_format_cb),
  MENUITEM_FUNC("DELAY",        menu_format_cb),
  MENUITEM_FUNC("SMITH",        menu_format_cb),
  MENUITEM_FUNC("SWR",          menu_format_cb),
  MENUITEM_MENU(S_RARROW" MORE", menu_format2),  
  //MENUITEM_FUNC("LINEAR",     menu_format_cb),
  //MENUITEM_FUNC("SWR",        menu_format_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_scale[] = {
  MENUITEM_FUNC("SCALE/DIV",                menu_scale_cb),
  MENUITEM_FUNC("\2REFERENCE\0POSITION",    menu_scale_cb),
  MENUITEM_FUNC("\2ELECTRICAL\0DELAY",      menu_scale_cb),
  MENUITEM_BACK,
  MENUITEM_END
};


static const menuitem_t menu_channel[] = {
  MENUITEM_FUNC("\2CH0\0REFLECT",   menu_channel_cb),
  MENUITEM_FUNC("\2CH1\0THROUGH",   menu_channel_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_transform_window[] = {
  MENUITEM_FUNC("MINIMUM",      menu_transform_window_cb),
  MENUITEM_FUNC("NORMAL",       menu_transform_window_cb),
  MENUITEM_FUNC("MAXIMUM",      menu_transform_window_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_transform[] = {
  MENUITEM_FUNC("\2TRANSFORM\0ON",      menu_transform_cb),
  MENUITEM_FUNC("\2LOW PASS\0IMPULSE",  menu_transform_cb),
  MENUITEM_FUNC("\2LOW PASS\0STEP",     menu_transform_cb),
  MENUITEM_FUNC("BANDPASS",             menu_transform_cb),
  MENUITEM_MENU("WINDOW",               menu_transform_window),
  MENUITEM_FUNC("\2VELOCITY\0FACTOR",   menu_transform_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_display[] = {
  MENUITEM_MENU("TRACE", menu_trace),
  MENUITEM_MENU("FORMAT", menu_format),
  MENUITEM_MENU("SCALE", menu_scale),
  MENUITEM_MENU("CHANNEL", menu_channel),
  MENUITEM_MENU("TRANSFORM", menu_transform),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_stimulus[] = {
  MENUITEM_FUNC("START",            menu_stimulus_cb),
  MENUITEM_FUNC("STOP",             menu_stimulus_cb),
  MENUITEM_FUNC("CENTER",           menu_stimulus_cb),
  MENUITEM_FUNC("SPAN",             menu_stimulus_cb),
  MENUITEM_FUNC("CW FREQ",          menu_stimulus_cb),
  MENUITEM_FUNC("\2PAUSE\0SWEEP",   menu_stimulus_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_marker_sel[] = {
  MENUITEM_FUNC("MARKER 1",     menu_marker_sel_cb),
  MENUITEM_FUNC("MARKER 2",     menu_marker_sel_cb),
  MENUITEM_FUNC("MARKER 3",     menu_marker_sel_cb),
  MENUITEM_FUNC("MARKER 4",     menu_marker_sel_cb),
  MENUITEM_FUNC("ALL OFF",      menu_marker_sel_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

const menuitem_t menu_marker_ops[] = {
  MENUITEM_FUNC( S_RARROW"START", menu_marker_op_cb ),
  MENUITEM_FUNC( S_RARROW"STOP", menu_marker_op_cb ),
  MENUITEM_FUNC( S_RARROW"CENTER", menu_marker_op_cb ),
  MENUITEM_FUNC( S_RARROW"SPAN", menu_marker_op_cb ),
//  MENUITEM_FUNC( S_RARROW"EDELAY", menu_marker_op_cb ),
  MENUITEM_BACK,
  MENUITEM_END
};

const menuitem_t menu_marker_search[] = {
  //MENUITEM_FUNC( "OFF", menu_marker_search_cb  ),
  MENUITEM_FUNC( "MAXIMUM", menu_marker_search_cb  ),
  MENUITEM_FUNC( "MINIMUM", menu_marker_search_cb ),
  MENUITEM_FUNC( "\2SEARCH\0" S_LARROW" LEFT", menu_marker_search_cb ),
  MENUITEM_FUNC( "\2SEARCH\0" S_RARROW" RIGHT", menu_marker_search_cb ),
  //MENUITEM_FUNC( "TRACKING", menu_marker_search_cb  ),
  MENUITEM_BACK,
  MENUITEM_END
};

const menuitem_t menu_marker_smith[] = {
  MENUITEM_FUNC("LIN", menu_marker_smith_cb),
  MENUITEM_FUNC("LOG", menu_marker_smith_cb),
  MENUITEM_FUNC( "Re+Im", menu_marker_smith_cb),
  MENUITEM_FUNC("R+Xj", menu_marker_smith_cb),
  MENUITEM_FUNC("R+L/C", menu_marker_smith_cb),
  MENUITEM_BACK,
  MENUITEM_END
};


static const menuitem_t menu_marker[] = {
  MENUITEM_MENU( "\2SELECT\0MARKER", menu_marker_sel),
  MENUITEM_MENU( "SEARCH", menu_marker_search),
  MENUITEM_MENU( "OPERATIONS", menu_marker_ops),
  MENUITEM_MENU("\2SMITH\0VALUE", menu_marker_smith),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_recall[] = {
  MENUITEM_FUNC("RECALL 0",         menu_recall_cb),
  MENUITEM_FUNC("RECALL 1",         menu_recall_cb),
  MENUITEM_FUNC("RECALL 2",         menu_recall_cb),
  MENUITEM_FUNC("RECALL 3",         menu_recall_cb),
   #if !defined(ANTENNA_ANALYZER)
  MENUITEM_FUNC("RECALL 4",         menu_recall_cb),
   #endif
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_dfu[] = {
  MENUITEM_FUNC("\2RESET AND\0ENTER DFU", menu_dfu_cb),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_config[] = {
  MENUITEM_FUNC("TOUCH CAL",    menu_config_cb),
  MENUITEM_FUNC("TOUCH TEST",   menu_config_cb),
  MENUITEM_FUNC("SAVE",         menu_config_cb),
  MENUITEM_FUNC("VERSION",      menu_config_cb),
  MENUITEM_FUNC("BRIGHTNESS",   menu_config_cb),
//  MENUITEM_MENU(S_RARROW"DFU",  menu_dfu),
  MENUITEM_BACK,
  MENUITEM_END
};

static const menuitem_t menu_top[] = {
  MENUITEM_MENU("DISPLAY",   menu_display),
  MENUITEM_MENU("MARKER",    menu_marker),
  MENUITEM_MENU("STIMULUS",  menu_stimulus),
  MENUITEM_MENU("CAL",       menu_cal),
  MENUITEM_MENU("RECALL",    menu_recall),
  MENUITEM_MENU("CONFIG",    menu_config),
 // MENUITEM_CLOSE,
  MENUITEM_END
};



#define MENU_STACK_DEPTH_MAX 4
static uint8_t menu_current_level = 0;
static const menuitem_t *menu_stack[MENU_STACK_DEPTH_MAX] = {
  menu_top, NULL, NULL, NULL
};

// ===[/MENU DEFINITION]========================================================



static int btn_check(void)
{
    int cur_button = READ_PORT() & BUTTON_MASK;
    int changed = last_button ^ cur_button;
    int status = 0;
    uint32_t ticks = chVTGetSystemTime();
    if (changed & (1<<BIT_PUSH)) {
      if (chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DEBOUNCE_TICKS) {
        if (cur_button & (1<<BIT_PUSH)) {
          // button released
          status |= EVT_BUTTON_SINGLE_CLICK;
          if (inhibit_until_release) {
            status = 0;
            inhibit_until_release = FALSE;
          }
        }
        last_button_down_ticks = ticks;
      }
    }
    
    if (changed & (1<<BIT_UP1)) {
      if ((cur_button & (1<<BIT_UP1))
          && (chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DEBOUNCE_TICKS)) {
        status |= EVT_UP;
      }
      last_button_down_ticks = ticks;
    }
    if (changed & (1<<BIT_DOWN1)) {
      if ((cur_button & (1<<BIT_DOWN1))
          && (chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DEBOUNCE_TICKS)) {
        status |= EVT_DOWN;
      }
      last_button_down_ticks = ticks;
    }
    last_button = cur_button;
    
    return status;
}

static int btn_wait_release(void)
{
  while (TRUE) {
    int cur_button = READ_PORT() & BUTTON_MASK;
    int changed = last_button ^ cur_button;
    uint32_t ticks = chVTGetSystemTime();
    int status = 0;

    if (!inhibit_until_release) {
      if ((cur_button & (1<<BIT_PUSH))
          && (chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DOWN_LONG_TICKS)) {
        inhibit_until_release = TRUE;
        return EVT_BUTTON_DOWN_LONG;
      }
      if ((changed & (1<<BIT_PUSH))
          && (chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DOWN_LONG_TICKS)) {
        return EVT_BUTTON_SINGLE_CLICK;
      }
    }

    if (changed) {
      // finished
      last_button = cur_button;
      last_button_down_ticks = ticks;
      inhibit_until_release = FALSE;
      return 0;
    }

    if ((chTimeDiffX(last_button_down_ticks, ticks) >= BUTTON_DOWN_LONG_TICKS)
        && (chTimeDiffX(last_button_repeat_ticks, ticks) >= BUTTON_REPEAT_TICKS)) {
      if (cur_button & (1<<BIT_DOWN1)) {
        status |= EVT_DOWN | EVT_REPEAT;
      }
      if (cur_button & (1<<BIT_UP1)) {
        status |= EVT_UP | EVT_REPEAT;
      }
      last_button_repeat_ticks = ticks;
      return status;
    }
  }
}

static int touch_measure_y(void)
{
  int v;
  // open Y line
  palSetLineMode(LINE_YN, PAL_MODE_INPUT_PULLDOWN );
  palSetLineMode(LINE_YP, PAL_MODE_INPUT_PULLDOWN );
  // drive low to high on X line
  palSetLineMode(LINE_XN, PAL_MODE_OUTPUT_PUSHPULL );
  palClearLine(LINE_XN);
  palSetLineMode(LINE_XP, PAL_MODE_OUTPUT_PUSHPULL );
  palSetLine(LINE_XP);

  chThdSleepMilliseconds(2);
  v = adc_single_read(ADC1, ADC_CHSELR_CHSEL7);
  //chThdSleepMilliseconds(2);
  //v += adc_single_read(ADC1, ADC_CHSELR_CHSEL7);
  return v;
}

static int touch_measure_x(void)
{
  int v;
  // open X line
  palSetLineMode(LINE_XN, PAL_MODE_INPUT_PULLDOWN );
  palSetLineMode(LINE_XP, PAL_MODE_INPUT_PULLDOWN );
  // drive low to high on Y line
  palSetLineMode(LINE_YN, PAL_MODE_OUTPUT_PUSHPULL );
  palSetLine(LINE_YN);
  palSetLineMode(LINE_YP, PAL_MODE_OUTPUT_PUSHPULL );
  palClearLine(LINE_YP);

  chThdSleepMilliseconds(2);
  v = adc_single_read(ADC1, ADC_CHSELR_CHSEL6);
  //chThdSleepMilliseconds(2);
  //v += adc_single_read(ADC1, ADC_CHSELR_CHSEL6);
  return v;
}

static void touch_prepare_sense(void)
{
  // open Y line
  palSetLineMode(LINE_YN, PAL_MODE_INPUT_PULLDOWN );
  palSetLineMode(LINE_YP, PAL_MODE_INPUT_PULLDOWN );
  // force X line high
  palSetLineMode(LINE_XN, PAL_MODE_OUTPUT_PUSHPULL );
  palSetLine(LINE_XN);
  palSetLineMode(LINE_XP, PAL_MODE_OUTPUT_PUSHPULL );
  palSetLine(LINE_XP);
  chThdSleepMilliseconds(2);  // This is needed to ensure enough time for wire to settle.
}

void touch_start_watchdog(void)
{
  touch_prepare_sense();
  adc_start_analog_watchdogd(ADC1, ADC_CHSELR_CHSEL7);
}

static int touch_status(void)
{
  touch_prepare_sense();
  return adc_single_read(ADC1, ADC_CHSELR_CHSEL7) > TOUCH_THRESHOLD;
}

static int touch_check(void)
{
  int stat = touch_status();
  if (stat) {
    chThdSleepMilliseconds(2);
    int x = touch_measure_x();
    int y = touch_measure_y();
    if (touch_status()) {
      last_touch_x = x;
      last_touch_y = y;
    }
    touch_prepare_sense();
  }

  if (stat != last_touch_status) {
    last_touch_status = stat;
    if (stat) {
      return EVT_TOUCH_PRESSED;
    } else {
      return EVT_TOUCH_RELEASED;
    }
  } else {
    if (stat) 
      return EVT_TOUCH_DOWN;
    else
      return EVT_TOUCH_NONE;
  }
}

static void touch_wait_release(void)
{
  int status;
  /* wait touch release */
  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
}

void touch_cal_exec(void)
{
  int status;
  int x1, x2, y1, y2;
  
  adc_stop(ADC1);

  ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
  ili9341_line(0, 0, 0, 32, 0xffff);
  ili9341_line(0, 0, 32, 0, 0xffff);
  #if !defined(ST7796S)
  ili9341_drawstring_5x7("TOUCH UPPER LEFT", 10, 10, 0xffff, 0x0000);
  #else
  ili9341_drawstring_7x13("TOUCH UPPER LEFT", 10, 10, 0xffff, 0x0000);
  #endif
  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
  x1 = last_touch_x;
  y1 = last_touch_y;

  ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
  ili9341_line(LCD_WIDTH-1, LCD_HEIGHT-1, LCD_WIDTH-1, LCD_HEIGHT-32, 0xffff);
  ili9341_line(LCD_WIDTH-1, LCD_HEIGHT-1, LCD_WIDTH-32, LCD_HEIGHT-1, 0xffff);
  #if !defined(ST7796S)
  ili9341_drawstring_5x7("TOUCH LOWER RIGHT", 230, 220, 0xffff, 0x0000);
  #else
  ili9341_drawstring_7x13("TOUCH LOWER RIGHT", 350, 300, 0xffff, 0x0000);
  #endif
  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
  x2 = last_touch_x;
  y2 = last_touch_y;

  config.touch_cal[0] = x1;
  config.touch_cal[1] = y1;
  config.touch_cal[2] = (x2 - x1) * 16 / LCD_WIDTH;
  config.touch_cal[3] = (y2 - y1) * 16 / LCD_HEIGHT;

  //redraw_all();
  touch_start_watchdog();
}

void touch_draw_test(void)
{
  int status;
  int x0, y0;
  int x1, y1;
  
  adc_stop(ADC1);

  ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
     #if !defined(ST7796S)
  ili9341_drawstring_5x7("TOUCH TEST: DRAG PANEL", OFFSETX, 233, 0xffff, 0x0000);
 #else
ili9341_drawstring_7x13("TOUCH TEST: DRAG PANEL", OFFSETX, 307, 0xffff, 0x0000);
 #endif
  do {
    status = touch_check();
  } while(status != EVT_TOUCH_PRESSED);
  touch_position(&x0, &y0);

  do {
    status = touch_check();
    touch_position(&x1, &y1);
    ili9341_line(x0, y0, x1, y1, 0xffff);
    x0 = x1;
    y0 = y1;
    chThdSleepMilliseconds(50);
  } while(status != EVT_TOUCH_RELEASED);

  touch_start_watchdog();
}


void touch_position(int *x, int *y)
{
  *x = (last_touch_x - config.touch_cal[0]) * 16 / config.touch_cal[2];
  *y = (last_touch_y - config.touch_cal[1]) * 16 / config.touch_cal[3];
}


void
show_version(void)
{
  

  adc_stop(ADC1);
  show_logo();

  while (true) {
    if (touch_check() == EVT_TOUCH_PRESSED)
      break;
    if (btn_check() & EVT_BUTTON_SINGLE_CLICK)
      break;
  }

  touch_start_watchdog();
}

void
show_logo(void)
{

#if !defined(ST7796S)
  int x = 15, y = 30;
  ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
  ili9341_drawstring_size(BOARD_NAME, x+60, y, RGBHEX(0x0000FF), 0x0000, 4);
  y += 25;

  ili9341_drawstring_size("NANOVNA.COM", x+100, y += 10, 0xffff, 0x0000, 2);
  ili9341_drawstring_5x7("https://github.com/hugen79/NanoVNA-H", x, y += 20, 0xffff, 0x0000);
  ili9341_drawstring_5x7("Based on edy555 design", x, y += 10, 0xffff, 0x0000);
  ili9341_drawstring_5x7("2016-2020 Copyright @edy555", x, y += 10, 0xffff, 0x0000);
  ili9341_drawstring_5x7("Licensed under GPL. See: https://github.com/ttrftech/NanoVNA", x, y += 10, 0xffff, 0x0000);
  ili9341_drawstring_5x7("Version: " VERSION, x, y += 10, 0xffff, 0x0000);
  ili9341_drawstring_5x7("Build Time: " __DATE__ " - " __TIME__, x, y += 10, 0xffff, 0x0000);
//  y += 5;
//  ili9341_drawstring_5x7("Kernel: " CH_KERNEL_VERSION, x, y += 10, 0xffff, 0x0000);
//  ili9341_drawstring_5x7("Architecture: " PORT_ARCHITECTURE_NAME " Core Variant: " PORT_CORE_VARIANT_NAME, x, y += 10, 0xffff, 0x0000);
//  ili9341_drawstring_5x7("Port Info: " PORT_INFO, x, y += 10, 0xffff, 0x0000);
//  ili9341_drawstring_5x7("Platform: " PLATFORM_NAME, x, y += 10, 0xffff, 0x0000);

#else

  int x = 20, y = 30;
    ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
    ili9341_drawstring_size(BOARD_NAME, x+74, y, RGBHEX(0x0000FF), 0x0000, 4);
      y += 35;

      ili9341_drawstring_size("NANOVNA.COM", x+150, y += 15, 0xffff, 0x0000, 2);
      ili9341_drawstring_7x13("https://github.com/hugen79/NanoVNA-H", x, y += 30, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Based on edy555 design, the MCU and LCD were ported by AA6KL.", x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("2016-2020 Copyright @edy555", x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Licensed under GPL. See: https://github.com/ttrftech/NanoVNA", x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Version: " VERSION, x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Build Time: " __DATE__ " - " __TIME__, x, y += 15, 0xffff, 0x0000);

      y += 10;
      ili9341_drawstring_7x13("Kernel: " CH_KERNEL_VERSION, x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Architecture: " PORT_ARCHITECTURE_NAME " Core Variant: " PORT_CORE_VARIANT_NAME, x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Port Info: " PORT_INFO, x, y += 15, 0xffff, 0x0000);
      ili9341_drawstring_7x13("Platform: " PLATFORM_NAME, x, y += 15, 0xffff, 0x0000);
#endif
}


#if 1
// Not available for F303
void enter_dfu(void)
{
  adc_stop(ADC1);

  int x = 5, y = 5;

  // leave a last message 
  ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
#if !defined(ST7796S)
  ili9341_drawstring_5x7("DFU: Device Firmware Update Mode", x, y += LINE_SPACE, 0xffff, 0x0000);
  ili9341_drawstring_5x7("To exit DFU mode, please reset device yourself.", x, y += LINE_SPACE, 0xffff, 0x0000);
#else
  ili9341_drawstring_7x13("DFU: Device Firmware Update Mode", x, y += LINE_SPACE, 0xffff, 0x0000);
   ili9341_drawstring_7x13("To exit DFU mode, please reset device yourself.", x, y += LINE_SPACE, 0xffff, 0x0000);
#endif

  // see __early_init in ./NANOVNA_STM32_F072/board.c
  *((unsigned long *)BOOT_FROM_SYTEM_MEMORY_MAGIC_ADDRESS) = BOOT_FROM_SYTEM_MEMORY_MAGIC;
  NVIC_SystemReset();
}
#endif


static void menu_calop_cb(int item)
{
  switch (item) {
  case 0: // OPEN
    cal_collect(CAL_OPEN);
    break;
  case 1: // SHORT
    cal_collect(CAL_SHORT);
    break;
  case 2: // LOAD
    cal_collect(CAL_LOAD);
    break;
  case 3: // ISOLN
    cal_collect(CAL_ISOLN);
    break;
  case 4: // THRU
    cal_collect(CAL_THRU);
    break;
  }
  selection = item+1;
  draw_cal_status();
  draw_menu();
}

static void menu_caldone_cb(int item)
{
  (void) item;
  cal_done();
  draw_cal_status();
  menu_move_back();
  menu_push_submenu(menu_save);
}

static void menu_cal2_cb(int item)
{
  switch (item) {
  case 2: // RESET
    cal_status = 0;
    break;
  case 3: // CORRECTION
    // toggle applying correction
    if (cal_status)
      cal_status ^= CALSTAT_APPLY;
    draw_menu();
    break;
  }
  draw_cal_status();
  //menu_move_back();
}

static void menu_recall_cb(int item)
{
  // item == RECALL number
  if (item < 0 || item >= 5)
    return;
  if (caldata_recall(item) == 0) {
    menu_move_back();
    ui_mode_normal();
    update_grid();
    draw_cal_status();
  }
}

static void menu_config_cb(int item)
{
  int status;
  switch (item) {
  case 0: // TOUCH CAL
      touch_cal_exec();
      redraw_frame();
      request_to_redraw_grid();
      draw_menu();
      break;
  case 1: // TOUCH TEST
      touch_draw_test();
      redraw_frame();
      request_to_redraw_grid();
      draw_menu();
      break;
  case 2: // SAVE
      config_save();
      menu_move_back();
      ui_mode_normal();
      break;
  case 3: // VERSION
      show_version();
      redraw_frame();
      request_to_redraw_grid();
      draw_menu();
      break;
  case 4: // BRIGHTNESS
      status = btn_wait_release();
          if (status & EVT_BUTTON_DOWN_LONG) {
           ui_mode_numeric(KM_BRIGHTNESS);
            //ui_process_numeric();
          } else {
            ui_mode_keypad(KM_BRIGHTNESS);
            ui_process_keypad();
          }

  }
}

// Not available for F303
#if 1
static void menu_dfu_cb(int item)
{
  switch (item) {
  case 0:
      enter_dfu();
  }
}
#endif

static void menu_save_cb(int item)
{
  // item == RECALL number
  if (item < 0 || item >= 5)
    return;
  if (caldata_save(item) == 0) {
    menu_move_back();
    ui_mode_normal();
    draw_cal_status();
  }
}

static void choose_active_trace(void)
{
  int i;
  if (trace[uistat.current_trace].enabled)
    // do nothing
    return;
  for (i = 0; i < TRACE_COUNT ; i++)
    if (trace[i].enabled) {
      uistat.current_trace = i;
      return;
    }
}

static void menu_trace_cb(int item)
{
  // item == TRACE number
  if (item < 0 || item >= TRACE_COUNT )
    return;
  if (trace[item].enabled) {
    if (item == uistat.current_trace) {
      // disable if active trace is selected
      trace[item].enabled = FALSE;
      choose_active_trace();
    } else {
      // make active selected trace
      uistat.current_trace = item;
    }
  } else {
    trace[item].enabled = TRUE;
    uistat.current_trace = item;
  }
  request_to_redraw_grid();
  draw_menu();
}

static void menu_format_cb(int item)
{
  switch (item) {
  case 0: // LOGMAG
    set_trace_type(uistat.current_trace, TRC_LOGMAG);
    break;
  case 1: // PHASE
    set_trace_type(uistat.current_trace, TRC_PHASE);
    break;
  case 2: // DELAY
    set_trace_type(uistat.current_trace, TRC_DELAY);
    break;
  case 3: // SMITH
    set_trace_type(uistat.current_trace, TRC_SMITH);
    break;
  case 4: // SWR
    set_trace_type(uistat.current_trace, TRC_SWR);
    break;
  }

  request_to_redraw_grid();
  ui_mode_normal();
  //redraw_all();
}

static void menu_format2_cb(int item)
{
  switch (item) {
  case 0: // POLAR
    set_trace_type(uistat.current_trace, TRC_POLAR);
    break;
  case 1: // LINEAR
    set_trace_type(uistat.current_trace, TRC_LINEAR);
    break;
  case 2: // REAL
    set_trace_type(uistat.current_trace, TRC_REAL);
    break;
  case 3: // IMAG
    set_trace_type(uistat.current_trace, TRC_IMAG);
    break;
  case 4: // RESISTANCE
    set_trace_type(uistat.current_trace, TRC_R);
    break;
  case 5: // REACTANCE
    set_trace_type(uistat.current_trace, TRC_X);
    break;
  }

  request_to_redraw_grid();
  ui_mode_normal();
}

static void menu_channel_cb(int item)
{
  if (item < 0 || item >= 2)
    return;
  set_trace_channel(uistat.current_trace, item);
  menu_move_back();
  ui_mode_normal();
}

static void menu_transform_window_cb(int item)
{
  // TODO
  switch (item) {
  case 0: // MININUM
      domain_mode = (domain_mode & ~TD_WINDOW) | TD_WINDOW_MINIMUM;
      ui_mode_normal();
      break;
  case 1: // NORMAL
      domain_mode = (domain_mode & ~TD_WINDOW) | TD_WINDOW_NORMAL;
      ui_mode_normal();
      break;
  case 2: // MAXUMUM
      domain_mode = (domain_mode & ~TD_WINDOW) | TD_WINDOW_MAXIMUM;
      ui_mode_normal();
      break;
  }
}

static void menu_transform_cb(int item)
{
  int status;
  switch (item) {
  case 0: // 2TRANSFORM 0ON
      if ((domain_mode & DOMAIN_MODE) == DOMAIN_TIME) {
          domain_mode = (domain_mode & ~DOMAIN_MODE) | DOMAIN_FREQ;
      } else {
          domain_mode = (domain_mode & ~DOMAIN_MODE) | DOMAIN_TIME;
      }
      draw_frequencies();
      ui_mode_normal();
      break;
  case 1: // 2LOW PASS 0IMPULSE
      domain_mode = (domain_mode & ~TD_FUNC) | TD_FUNC_LOWPASS_IMPULSE;
      ui_mode_normal();
      break;
  case 2: // 2LOW PASS 0STEP
      domain_mode = (domain_mode & ~TD_FUNC) | TD_FUNC_LOWPASS_STEP;
      ui_mode_normal();
      break;
  case 3: // BANDPASS
      domain_mode = (domain_mode & ~TD_FUNC) | TD_FUNC_BANDPASS;
      ui_mode_normal();
      break;
  case 5: // 2VELOCITY 0FACTOR
      status = btn_wait_release();
      if (status & EVT_BUTTON_DOWN_LONG) {
        ui_mode_numeric(KM_VELOCITY_FACTOR);
//        ui_process_numeric();
      } else {
        ui_mode_keypad(KM_VELOCITY_FACTOR);
        ui_process_keypad();
      }
      break;
  }
}

static void choose_active_marker(void)
{
  int i;
  for (i = 0; i < MARKER_COUNT; i++)
    if (markers[i].enabled) {
      active_marker = i;
      return;
    }
  active_marker = -1;
}

static void menu_scale_cb(int item)
{
  int status;
  int km = KM_SCALE + item;
  if (km == KM_SCALE && trace[uistat.current_trace].type == TRC_DELAY) {
    km = KM_SCALEDELAY;
  }
  status = btn_wait_release();
  if (status & EVT_BUTTON_DOWN_LONG) {
    ui_mode_numeric(km);
//    ui_process_numeric();
  } else {
    ui_mode_keypad(km);
    ui_process_keypad();
  }
}

static void menu_stimulus_cb(int item)
{
  int status;
  switch (item) {
  case 0: /* START */
  case 1: /* STOP */
  case 2: /* CENTER */
  case 3: /* SPAN */
  case 4: /* CW */
    status = btn_wait_release();
    if (status & EVT_BUTTON_DOWN_LONG) {
      ui_mode_numeric(item);
//      ui_process_numeric();
    } else {
      ui_mode_keypad(item);
      ui_process_keypad();
    }
    break;
  case 5: /* PAUSE */
    toggle_sweep();
    //menu_move_back();
    //ui_mode_normal();
    draw_menu();
    break;
  }
}


static int32_t get_marker_frequency(int marker)
{
  if (marker < 0 || marker >= MARKER_COUNT)
    return -1;
  if (!markers[marker].enabled)
    return -1;
  return frequencies[markers[marker].index];
}

static void
menu_marker_op_cb(int item)
{
  int32_t freq = get_marker_frequency(active_marker);
  if (freq < 0)
    return; // no active marker

  switch (item) {
  case 0: /* MARKER->START */
    set_sweep_frequency(ST_START, freq);
    break;
  case 1: /* MARKER->STOP */
    set_sweep_frequency(ST_STOP, freq);
    break;
  case 2: /* MARKER->CENTER */
    set_sweep_frequency(ST_CENTER, freq);
    break;
  case 3: /* MARKERS->SPAN */
    {
      if (previous_marker == -1 || active_marker == previous_marker) {
        // if only 1 marker is active, keep center freq and make span the marker comes to the edge
        int32_t center = get_sweep_frequency(ST_CENTER);
        int32_t span = center - freq;
        if (span < 0) span = -span;
        set_sweep_frequency(ST_SPAN, span * 2);
      } else {
        // if 2 or more marker active, set start and stop freq to each marker
        int32_t freq2 = get_marker_frequency(previous_marker);
        if (freq2 < 0)
          return;
        if (freq > freq2) {
          freq2 = freq;
          freq = get_marker_frequency(previous_marker);
        }
        set_sweep_frequency(ST_START, freq);
        set_sweep_frequency(ST_STOP, freq2);
      }
    }
    break;
#if 0
  case 4: /* MARKERS->EDELAY */
    {
      if (uistat.current_trace == -1)
        break;
      float (*array)[2] = measured[trace[uistat.current_trace].channel];
      float v =group_delay(coeff, freq, point_count, i);
      set_electrical_delay(electrical_delay + (v / 1e-12));
    }
    break;
#endif
  }
  ui_mode_normal();
  draw_cal_status();
  //redraw_all();
}

static void
menu_marker_search_cb(int item)
{
  int i;
  if (active_marker == -1)
    return;

  switch (item) {
  case 0: /* maximum */
  case 1: /* minimum */
    i = marker_search(item);
    if (i != -1)
      markers[active_marker].index = i;
    draw_menu();
    break;
  case 2: /* search Left */
    i = marker_search_left(markers[active_marker].index);
    if (i != -1)
      markers[active_marker].index = i;
    draw_menu();
    break;
  case 3: /* search right */
    i = marker_search_right(markers[active_marker].index);
    if (i != -1)
      markers[active_marker].index = i;
    draw_menu();
    break;
  }
  redraw_marker(active_marker, TRUE);
  uistat.lever_mode = LM_SEARCH;
}

static void
menu_marker_smith_cb(int item)
{
  uistat.marker_smith_format = item;
  redraw_marker(active_marker, TRUE);
  draw_menu();
}

static void active_marker_select(int item)
{
  if (item == -1) {
    active_marker = previous_marker;
    previous_marker = -1;
    if (active_marker == -1) {
      choose_active_marker();
    }
  } else {
    if (previous_marker != active_marker)
      previous_marker = active_marker;
    active_marker = item;
  }
}

static void menu_marker_sel_cb(int item)
{
  if (item >= 0 && item < 4 && item < MARKER_COUNT) {
    if (markers[item].enabled) {
      if (item == active_marker) {
        // disable if active trace is selected
        markers[item].enabled = FALSE;
        active_marker_select(-1);
      } else {
        active_marker_select(item);
      }
    } else {
      markers[item].enabled = TRUE;
      active_marker_select(item);
    }
  } else if (item == 4) { /* all off */
      markers[0].enabled = FALSE;
      markers[1].enabled = FALSE;
      markers[2].enabled = FALSE;
      markers[3].enabled = FALSE;
      previous_marker = -1;
      active_marker = -1;      
  }
  redraw_marker(active_marker, TRUE);
  draw_menu();
}

static void ensure_selection(void)
{
  const menuitem_t *menu = menu_stack[menu_current_level];
  int i;
  for (i = 0; menu[i].type != MT_NONE; i++)
    ;
  if (selection >= i)
    selection = i-1;
}

static void menu_move_back(void)
{
  if (menu_current_level == 0)
    return;
  menu_current_level--;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}

static void menu_push_submenu(const menuitem_t *submenu)
{
  if (menu_current_level < MENU_STACK_DEPTH_MAX-1)
    menu_current_level++;
  menu_stack[menu_current_level] = submenu;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}

/*
static void menu_move_top(void)
{
  if (menu_current_level == 0)
    return;
  menu_current_level = 0;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}
*/

static void menu_invoke(int item)
{
  const menuitem_t *menu = menu_stack[menu_current_level];
  menu = &menu[item];

  switch (menu->type) {
  case MT_NONE:
  case MT_BLANK:
//  case MT_CLOSE:
    ui_mode_normal();
    break;

  case MT_CANCEL:
    menu_move_back();
    break;

  case MT_CALLBACK: {
    menuaction_cb_t cb = menu->pFunc;
    if (cb == NULL)
      return;
    (*cb)(item);
    break;
  }

  case MT_SUBMENU:
    menu_push_submenu(menu->pMenu);
    break;
  }
}
#if !defined(ST7796S)

#define KP_X(x) (48*(x) + 2 + (LCD_WIDTH-64-192))
#define KP_Y(y) (48*(y) + 2)
#else
#define KP_X(x) (64*(x) + 2 + (LCD_WIDTH-96-256))
#define KP_Y(y) (64*(y) + 2)
#endif


#define KP_PERIOD 10
#define KP_MINUS 11
#define KP_X1 12
#define KP_K 13
#define KP_M 14
#define KP_G 15
#define KP_BS 16
#define KP_INF 17
#define KP_DB 18
#define KP_PLUSMINUS 19
#define KP_KEYPAD 20
#define KP_N 21
#define KP_P 22

typedef struct {
  uint16_t x, y;
  int8_t c;
} keypads_t;

static const keypads_t *keypads;
static uint8_t keypads_last_index;

static const keypads_t keypads_freq[] = {
  { KP_X(1), KP_Y(3), KP_PERIOD },
  { KP_X(0), KP_Y(3), 0 },
  { KP_X(0), KP_Y(2), 1 },
  { KP_X(1), KP_Y(2), 2 },
  { KP_X(2), KP_Y(2), 3 },
  { KP_X(0), KP_Y(1), 4 },
  { KP_X(1), KP_Y(1), 5 },
  { KP_X(2), KP_Y(1), 6 },
  { KP_X(0), KP_Y(0), 7 },
  { KP_X(1), KP_Y(0), 8 },
  { KP_X(2), KP_Y(0), 9 },
  { KP_X(3), KP_Y(0), KP_G },
  { KP_X(3), KP_Y(1), KP_M },
  { KP_X(3), KP_Y(2), KP_K },
  { KP_X(3), KP_Y(3), KP_X1 },
  { KP_X(2), KP_Y(3), KP_BS },
  { 0, 0, -1 }
};

static const keypads_t keypads_scale[] = {
  { KP_X(1), KP_Y(3), KP_PERIOD },
  { KP_X(0), KP_Y(3), 0 },
  { KP_X(0), KP_Y(2), 1 },
  { KP_X(1), KP_Y(2), 2 },
  { KP_X(2), KP_Y(2), 3 },
  { KP_X(0), KP_Y(1), 4 },
  { KP_X(1), KP_Y(1), 5 },
  { KP_X(2), KP_Y(1), 6 },
  { KP_X(0), KP_Y(0), 7 },
  { KP_X(1), KP_Y(0), 8 },
  { KP_X(2), KP_Y(0), 9 },
  { KP_X(3), KP_Y(3), KP_X1 },
  { KP_X(2), KP_Y(3), KP_BS },
  { 0, 0, -1 }
};

static const keypads_t keypads_time[] = {
  { KP_X(1), KP_Y(3), KP_PERIOD },
  { KP_X(0), KP_Y(3), 0 },
  { KP_X(0), KP_Y(2), 1 },
  { KP_X(1), KP_Y(2), 2 },
  { KP_X(2), KP_Y(2), 3 },
  { KP_X(0), KP_Y(1), 4 },
  { KP_X(1), KP_Y(1), 5 },
  { KP_X(2), KP_Y(1), 6 },
  { KP_X(0), KP_Y(0), 7 },
  { KP_X(1), KP_Y(0), 8 },
  { KP_X(2), KP_Y(0), 9 },
  { KP_X(3), KP_Y(1), KP_N },
  { KP_X(3), KP_Y(2), KP_P },
  { KP_X(3), KP_Y(3), KP_MINUS },  
  { KP_X(2), KP_Y(3), KP_BS },
  { 0, 0, -1 }
};

static const keypads_t * const keypads_mode_tbl[] = {
  keypads_freq, // start
  keypads_freq, // stop
  keypads_freq, // center
  keypads_freq, // span
  keypads_freq, // cw freq
  keypads_scale, // scale
  keypads_scale, // refpos
  keypads_time, // electrical delay
  keypads_scale, // velocity factor
  keypads_time, // scale of delay
  keypads_scale // scale of brightness
};

static const char * const keypad_mode_label[] = {
  "START", "STOP", "CENTER", "SPAN", "CW FREQ", "SCALE", "REFPOS", "EDELAY", "VELOCITY%", "DELAY", "BRIGHTNESS"
};

static void draw_keypad(void)
{
  int i = 0;
  while (keypads[i].x) {
    uint16_t bg = config.menu_normal_color;
    if (i == selection)
      bg = config.menu_active_color;
#if !defined(ST7796S)
    ili9341_fill(keypads[i].x, keypads[i].y, 44, 44, bg);
    ili9341_drawfont(keypads[i].c, &NF20x22, keypads[i].x+12, keypads[i].y+11, 0x0000, bg);
#else
    ili9341_fill(keypads[i].x, keypads[i].y, 60, 60, bg);
    ili9341_drawfont(keypads[i].c, &NF20x22, keypads[i].x+20, keypads[i].y+19, 0x0000, bg);
#endif
    i++;
  }
}

static void draw_numeric_area_frame(void)
{
  ili9341_fill(0, LCD_HEIGHT*0.9, LCD_WIDTH, 32, 0xffff);
#if !defined(ST7796S)
  ili9341_drawstring_5x7(keypad_mode_label[keypad_mode], 10, 220, 0x0000, 0xffff);
#else
  ili9341_drawstring_7x13(keypad_mode_label[keypad_mode], 10, 296, 0x0000, 0xffff);
#endif
  //ili9341_drawfont(KP_KEYPAD, &NF20x22, LCD_WIDTH-20, LCD_HEIGHT*0.9, 0x0000, 0xffff);
}

static void draw_numeric_input(const char *buf)
{
  int i = 0;
  int x = 10 * FONT_WIDTH + 14;
  int focused = FALSE;
  const uint16_t xsim[] = { 0, 0, 8, 0, 0, 8, 0, 0, 0, 0 };
  for (i = 0; i < 10 && buf[i]; i++) {
    uint16_t fg = 0x0000;
    uint16_t bg = 0xffff;
    int c = buf[i];
    if (c == '.')
      c = KP_PERIOD;
    else if (c == '-')
      c = KP_MINUS;
    else if (c >= '0' && c <= '9')
      c = c - '0';
    else
      c = -1;

    if (uistat.digit == 8-i) {
      fg = RGBHEX(0xf7131f);
      focused = TRUE;
      if (uistat.digit_mode)
        bg = 0x0000;
    }

    if (c >= 0)
      ili9341_drawfont(c, &NF20x22, x, LCD_HEIGHT*0.9+4, fg, bg);
    else if (focused)
      ili9341_drawfont(0, &NF20x22, x, LCD_HEIGHT*0.9+4, fg, bg);
    else
      ili9341_fill(x, LCD_HEIGHT*0.9+4, 20, 24, bg);
      
    x += 20;
    if (xsim[i] > 0) {
      //ili9341_fill(x, LCD_HEIGHT*0.9+4, xsim[i], 20, bg);
      x += xsim[i];
    }
  }
  if (i < 10) {
      ili9341_fill(x, LCD_HEIGHT*0.9+4, 20*(10-i), 24, 0xffff);
  }
}

static int menu_is_multiline(const char *label, const char **l1, const char **l2)
{
  if (label[0] != '\2')
    return FALSE;

  *l1 = &label[1];
  *l2 = &label[1] + strlen(&label[1]) + 1;
  return TRUE;
}

static void menu_item_modify_attribute(
    const menuitem_t *menu, int item, uint16_t *fg, uint16_t *bg)
{
  if (menu == menu_trace && item < TRACE_COUNT  && item < MARKER_COUNT) {
    if (trace[item].enabled)
      *bg = config.trace_color[item];
  } else if (menu == menu_marker_sel && item < 4&& item < MARKER_COUNT) {
    if (markers[item].enabled) {
      *bg = 0x0000;
      *fg = 0xffff;
    }   
  } else if (menu == menu_calop) {
    if ((item == 0 && (cal_status & CALSTAT_OPEN))
        || (item == 1 && (cal_status & CALSTAT_SHORT))
        || (item == 2 && (cal_status & CALSTAT_LOAD))
        || (item == 3 && (cal_status & CALSTAT_ISOLN))
        || (item == 4 && (cal_status & CALSTAT_THRU))) {
      domain_mode = (domain_mode & ~DOMAIN_MODE) | DOMAIN_FREQ;
      *bg = 0x0000;
      *fg = 0xffff;
    }
  } else if (menu == menu_stimulus) {
    if (item == 5 /* PAUSE */ && !sweep_enabled) {
      *bg = 0x0000;
      *fg = 0xffff;
    }
  } else if (menu == menu_cal) {
    if (item == 3 /* CORRECTION */ && (cal_status & CALSTAT_APPLY)) {
      *bg = 0x0000;
      *fg = 0xffff;
    }
  } else if (menu == menu_transform) {
      if ((item == 0 && (domain_mode & DOMAIN_MODE) == DOMAIN_TIME)
       || (item == 1 && (domain_mode & TD_FUNC) == TD_FUNC_LOWPASS_IMPULSE)
       || (item == 2 && (domain_mode & TD_FUNC) == TD_FUNC_LOWPASS_STEP)
       || (item == 3 && (domain_mode & TD_FUNC) == TD_FUNC_BANDPASS)
       ) {
        *bg = 0x0000;
        *fg = 0xffff;
      }
  } else if (menu == menu_transform_window) {
      if ((item == 0 && (domain_mode & TD_WINDOW) == TD_WINDOW_MINIMUM)
       || (item == 1 && (domain_mode & TD_WINDOW) == TD_WINDOW_NORMAL)
       || (item == 2 && (domain_mode & TD_WINDOW) == TD_WINDOW_MAXIMUM)
       ) {
        *bg = 0x0000;
        *fg = 0xffff;
      }
  }
}

static void draw_menu_buttons(const menuitem_t *menu)
{
  int i = 0;
  for (i = 0; i < 7; i++) {
    const char *l1, *l2;
    if (menu[i].type == MT_NONE)
      break;
    if (menu[i].type == MT_BLANK) 
      continue;
#if !defined(ST7796S)
    int y = 32*i;
#else
    int y = 42*i;
#endif

    uint16_t bg = config.menu_normal_color;
    uint16_t fg = 0x0000;
    // focus only in MENU mode but not in KEYPAD mode
    if (ui_mode == UI_MENU && i == selection)
      bg = config.menu_active_color;
#if !defined(ST7796S)
    ili9341_fill(LCD_WIDTH-60, y, 60, 30, bg);
    
    menu_item_modify_attribute(menu, i, &fg, &bg);
    if (menu_is_multiline(menu[i].label, &l1, &l2)) {
      ili9341_drawstring_5x7(l1, LCD_WIDTH-54, y+8, fg, bg);
      ili9341_drawstring_5x7(l2, LCD_WIDTH-54, y+15, fg, bg);
    } else {
      ili9341_drawstring_5x7(menu[i].label, LCD_WIDTH-54, y+12, fg, bg);
    }

#else
    ili9341_fill(LCD_WIDTH-90, y, 90, 40, bg);

        menu_item_modify_attribute(menu, i, &fg, &bg);
        if (menu_is_multiline(menu[i].label, &l1, &l2)) {
          ili9341_drawstring_7x13(l1, LCD_WIDTH-80, y+7, fg, bg);
          ili9341_drawstring_7x13(l2, LCD_WIDTH-80, y+20, fg, bg);
        } else {
          ili9341_drawstring_7x13(menu[i].label, LCD_WIDTH-80, y+14, fg, bg);
        }
#endif
  }
}

static void menu_select_touch(int i)
{
  selection = i;
  draw_menu();
  touch_wait_release();
  selection = -1;
  menu_invoke(i);
}

static void menu_apply_touch(void)
{
  int touch_x, touch_y;
  const menuitem_t *menu = menu_stack[menu_current_level];
  int i;

  touch_position(&touch_x, &touch_y);
  for (i = 0; i < 7; i++) {
    if (menu[i].type == MT_NONE)
      break;
    if (menu[i].type == MT_BLANK) 
      continue;
#if !defined(ST7796S)
    int y = 32*i;
    if (y-2 < touch_y && touch_y < y+30+2
        && LCD_WIDTH-60 < touch_x) {
    #else
      int y = 42*i;
  if (y-2 < touch_y && touch_y < y+40+2
    && LCD_WIDTH-90 < touch_x) {
#endif
      menu_select_touch(i);
      return;
    }
  }

  touch_wait_release();
  ui_mode_normal();
}

static void draw_menu(void)
{
  draw_menu_buttons(menu_stack[menu_current_level]);
}

static void erase_menu_buttons(void)
{
  uint16_t bg = 0;
  #if !defined(ST7796S)
  ili9341_fill(LCD_WIDTH-60, 0, 60, 32*7, bg);
  #else
   ili9341_fill(LCD_WIDTH-90, 0, 90, 42*7, bg);
#endif

}

void
erase_numeric_input(void)
{
  uint16_t bg = 0;
  ili9341_fill(0, LCD_HEIGHT-32, LCD_WIDTH, 32, bg);
}

static void leave_ui_mode(void)
{
  if (ui_mode == UI_MENU) {
    request_to_draw_cells_behind_menu();
    erase_menu_buttons();
  } else if (ui_mode == UI_NUMERIC) {
    request_to_draw_cells_behind_numeric_input();
    erase_numeric_input();
    draw_frequencies();
  }
}

static void fetch_numeric_target(void)
{
  switch (keypad_mode) {
  case KM_START:
    uistat.value = get_sweep_frequency(ST_START);
    break;
  case KM_STOP:
    uistat.value = get_sweep_frequency(ST_STOP);
    break;
  case KM_CENTER:
    uistat.value = get_sweep_frequency(ST_CENTER);
    break;
  case KM_SPAN:
    uistat.value = get_sweep_frequency(ST_SPAN);
    break;
  case KM_CW:
    uistat.value = get_sweep_frequency(ST_CW);
    break;
  case KM_SCALE:
    uistat.value = get_trace_scale(uistat.current_trace) * 1000;
    break;
  case KM_REFPOS:
    uistat.value = get_trace_refpos(uistat.current_trace) * 1000;
    break;
  case KM_EDELAY:
    uistat.value = get_electrical_delay();
    break;
  case KM_VELOCITY_FACTOR:
    uistat.value = velocity_factor;
    break;
  case KM_SCALEDELAY:
    uistat.value = get_trace_scale(uistat.current_trace) * 1e12;
    break;
  case KM_BRIGHTNESS:
      uistat.value = config.dac_value;
      break;
  }
  
  {
    uint32_t x = uistat.value;
    int n = 0;
    for (; x >= 10 && n < 9; n++)
      x /= 10;
    uistat.digit = n;
  }
  uistat.previous_value = uistat.value;
}

#if 0
static void set_numeric_value(void)
{
  switch (keypad_mode) {
  case KM_START:
    set_sweep_frequency(ST_START, uistat.value);
    break;
  case KM_STOP:
    set_sweep_frequency(ST_STOP, uistat.value);
    break;
  case KM_CENTER:
    set_sweep_frequency(ST_CENTER, uistat.value);
    break;
  case KM_SPAN:
    set_sweep_frequency(ST_SPAN, uistat.value);
    break;
  case KM_CW:
    set_sweep_frequency(ST_CW, uistat.value);
    break;
  case KM_SCALE:
    set_trace_scale(uistat.current_trace, uistat.value / 1000.0);
    break;
  case KM_REFPOS:
    set_trace_refpos(uistat.current_trace, uistat.value / 1000.0);
    break;
  case KM_EDELAY:
    set_electrical_delay(uistat.value);
    break;
  case KM_VELOCITY_FACTOR:
    velocity_factor = uistat.value;
    break;
  case KM_BRIGHTNESS:
    uistat.value = (uistat.value < 800) ? 800: uistat.value;
    uistat.value = (uistat.value > 3300) ? 3300: uistat.value;
    config.dac_value = uistat.value;
    dacPutChannelX(&DACD2, 0, uistat.value);
    break
  }
}

#endif

static void draw_numeric_area(void)
{
  char buf[10];
  chsnprintf(buf, sizeof buf, "%9d", uistat.value);
  draw_numeric_input(buf);
}


static void ui_mode_menu(void)
{
  if (ui_mode == UI_MENU) 
    return;

  ui_mode = UI_MENU;
  /* narrowen plotting area */
  #if !defined(ST7796S)
  area_width = AREA_WIDTH_NORMAL - (64-8);
  #else
   area_width = AREA_WIDTH_NORMAL - (90-7);
  #endif
  area_height = HEIGHT;
  ensure_selection();
  draw_menu();
}

static void ui_mode_numeric(int _keypad_mode)
{
  if (ui_mode == UI_NUMERIC) 
    return;

  leave_ui_mode();
  
  // keypads array
  keypad_mode = _keypad_mode;
  ui_mode = UI_NUMERIC;
  area_width = AREA_WIDTH_NORMAL;
  area_height = LCD_HEIGHT-32;//HEIGHT - 32;

  draw_numeric_area_frame();
  fetch_numeric_target();
  draw_numeric_area();
}

static void ui_mode_keypad(int _keypad_mode)
{
  if (ui_mode == UI_KEYPAD) 
    return;

  // keypads array
  keypad_mode = _keypad_mode;
  keypads = keypads_mode_tbl[_keypad_mode];
  int i;
  for (i = 0; keypads[i+1].c >= 0; i++)
    ;
  keypads_last_index = i;

  ui_mode = UI_KEYPAD;
  area_width = AREA_WIDTH_NORMAL - (64-8);
  area_height = HEIGHT - 32;
  draw_menu();
  draw_keypad();
  draw_numeric_area_frame();
  fetch_numeric_target();
  draw_numeric_area();
  //draw_numeric_input("");
}

static void ui_mode_normal(void)
{
  if (ui_mode == UI_NORMAL) 
    return;

  area_width = AREA_WIDTH_NORMAL;
  area_height = HEIGHT;
  leave_ui_mode();
  ui_mode = UI_NORMAL;
}

static void ui_process_normal(void)
{
  int status = btn_check();
  if (status != 0) {
    if (status & EVT_BUTTON_SINGLE_CLICK) {
      ui_mode_menu();
    } else {
      do {
        if (active_marker >= 0 && markers[active_marker].enabled) {
          if ((status & EVT_DOWN) && markers[active_marker].index > 0) {
            markers[active_marker].index--;
            markers[active_marker].frequency = frequencies[markers[active_marker].index];
            redraw_marker(active_marker, FALSE);
          }
          if ((status & EVT_UP) && markers[active_marker].index < (POINT_COUNT-1)) {
            markers[active_marker].index++;
            markers[active_marker].frequency = frequencies[markers[active_marker].index];
            redraw_marker(active_marker, FALSE);
          }
        }
        status = btn_wait_release();
      } while (status != 0);
      if (active_marker >= 0)
        redraw_marker(active_marker, TRUE);
    }
  }
}

static void ui_process_menu(void)
{
  int status = btn_check();
  if (status != 0) {
    if (status & EVT_BUTTON_SINGLE_CLICK) {
      menu_invoke(selection);
    } else {
      do {
        if (status & EVT_UP){
            if (menu_stack[menu_current_level][selection+1].type == MT_NONE) {
              ui_mode_normal();
              return;
            }
            selection++;
            draw_menu();
        }
        else if (status & EVT_DOWN){
            if (selection == 0){
              ui_mode_normal();
              return;}
            selection--;
            draw_menu();
        }
        status = btn_wait_release();
      } while (status != 0);
    }
  }
}

static int keypad_click(int key) 
{
  int c = keypads[key].c;
  if ((c >= KP_X1 && c <= KP_G) || c == KP_N || c == KP_P) {
    int32_t scale = 1;
    if (c >= KP_X1 && c <= KP_G) {
      int n = c - KP_X1;
      while (n-- > 0)
        scale *= 1000;
    } else if (c == KP_N) {
      scale *= 1000;
    }
    /* numeric input done */
    double value = my_atof(kp_buf) * (double)scale;
    switch (keypad_mode) {
    case KM_START:
      set_sweep_frequency(ST_START, (int32_t)value);
      break;
    case KM_STOP:
      set_sweep_frequency(ST_STOP, (int32_t)value);
      break;
    case KM_CENTER:
      set_sweep_frequency(ST_CENTER, (int32_t)value);
      break;
    case KM_SPAN:
      set_sweep_frequency(ST_SPAN, (int32_t)value);
      break;
    case KM_CW:
      set_sweep_frequency(ST_CW, (int32_t)value);
      break;
    case KM_SCALE:
      set_trace_scale(uistat.current_trace, value);
      break;
    case KM_REFPOS:
      set_trace_refpos(uistat.current_trace, value);
      break;
    case KM_EDELAY:
      set_electrical_delay(value); // pico seconds
      break;
    case KM_VELOCITY_FACTOR:
      velocity_factor = (uint8_t)value;
      break;
    case KM_SCALEDELAY:
      set_trace_scale(uistat.current_trace, value * 1e-12); // pico second
      break;
    case KM_BRIGHTNESS:
      value = (value < 800) ? 800: value;
      value = (value > 3300) ? 3300: value;
      config.dac_value = value;
      dacPutChannelX(&DACD2, 0, value);
      break;
    }

    return KP_DONE;
  } else if (c <= 9 && kp_index < NUMINPUT_LEN)
    kp_buf[kp_index++] = '0' + c;
  else if (c == KP_PERIOD && kp_index < NUMINPUT_LEN) {
    // check period in former input
    int j;
    for (j = 0; j < kp_index && kp_buf[j] != '.'; j++)
      ;
    // append period if there are no period
    if (kp_index == j)
      kp_buf[kp_index++] = '.';
  } else if (c == KP_MINUS) {
    if (kp_index == 0)
      kp_buf[kp_index++] = '-';
  } else if (c == KP_BS) {
    if (kp_index == 0) {
      return KP_CANCEL;
    }
    --kp_index;
  }
  kp_buf[kp_index] = '\0';
  draw_numeric_input(kp_buf);
  return KP_CONTINUE;
}

static int keypad_apply_touch(void)
{
  int touch_x, touch_y;
  int i = 0;

  touch_position(&touch_x, &touch_y);

  while (keypads[i].x) {
    if (keypads[i].x-2 < touch_x && touch_x < keypads[i].x+44+2
        && keypads[i].y-2 < touch_y && touch_y < keypads[i].y+44+2) {
      // draw focus
      selection = i;
      draw_keypad();
      touch_wait_release();
      // erase focus
      selection = -1;
      draw_keypad();
      return i;
    }
    i++;
  }
  if (touch_y > 48 * 4) {
    // exit keypad mode
    return -2;
  }
  return -1;
}

static void numeric_apply_touch(void)
{
  int touch_x, touch_y;
  touch_position(&touch_x, &touch_y);

  if (touch_x < 64) {
    ui_mode_normal();
    return;
  }
  if (touch_x > 64+9*20+8+8) {
    ui_mode_keypad(keypad_mode);
    ui_process_keypad();
    return;
  }

  if (touch_y > LCD_HEIGHT-40) {
    int n = 9 - (touch_x - 64) / 20;
    uistat.digit = n;
    uistat.digit_mode = TRUE;
  } else {
    int step, n;
    if (touch_y < 100) {
      step = 1;
    } else {
      step = -1;
    }

    for (n = uistat.digit; n > 0; n--)
      step *= 10;
    uistat.value += step;
  }
  draw_numeric_area();
  
  touch_wait_release();
  uistat.digit_mode = FALSE;
  draw_numeric_area();
  
  return;
}

#if 0
static void ui_process_numeric(void)
{
  int status = btn_check();

  if (status != 0) {
    if (status == EVT_BUTTON_SINGLE_CLICK) {
      status = btn_wait_release();
      if (uistat.digit_mode) {
        if (status & (EVT_BUTTON_SINGLE_CLICK | EVT_BUTTON_DOWN_LONG)) {
          uistat.digit_mode = FALSE;
          draw_numeric_area();
        }
      } else {
        if (status & EVT_BUTTON_DOWN_LONG) {
          uistat.digit_mode = TRUE;
          draw_numeric_area();
        } else if (status & EVT_BUTTON_SINGLE_CLICK) {
          set_numeric_value();
          ui_mode_normal();
        }
      }
    } else {
      do {
        if (uistat.digit_mode) {
          if (status & EVT_DOWN) {
            if (uistat.digit < 8) {
              uistat.digit++;
              draw_numeric_area();
            } else {
              goto exit;
            }
          }
          if (status & EVT_UP) {
            if (uistat.digit > 0) {
              uistat.digit--;
              draw_numeric_area();
            } else {
              goto exit;
            }
          }
        } else {
          int32_t step = 1;
          int n;
          for (n = uistat.digit; n > 0; n--)
            step *= 10;
          if (status & EVT_DOWN) {
            uistat.value += step;
            draw_numeric_area();
          }
          if (status & EVT_UP) {
            uistat.value -= step;
            draw_numeric_area();
          }
        }
        status = btn_wait_release();
      } while (status != 0);
    }
  }

  return;

 exit:
  // cancel operation
  ui_mode_normal();
}
#endif


static void ui_process_keypad(void)
{
  int status;
  adc_stop(ADC1);

  kp_index = 0;
  while (TRUE) {
    status = btn_check();
    if (status & (EVT_UP|EVT_DOWN)) {
      int s = status;
      do {
        if (s & EVT_UP) {
          selection--;
          if (selection < 0)
            selection = keypads_last_index;
          draw_keypad();
        }
        if (s & EVT_DOWN) {
          selection++;
          if (keypads[selection].c < 0) {
            // reaches to tail
            selection = 0;
          }
          draw_keypad();
        }
        s = btn_wait_release();
      } while (s != 0);
    }

    if (status == EVT_BUTTON_SINGLE_CLICK) {
      if (keypad_click(selection))
        /* exit loop on done or cancel */
        break; 
    }

    status = touch_check();
    if (status == EVT_TOUCH_PRESSED) {
      int key = keypad_apply_touch();
      if (key >= 0 && keypad_click(key))
        /* exit loop on done or cancel */
        break;
      else if (key == -2) {
        //xxx;
 //       return;
      }
    }
  }

  redraw_frame();
  request_to_redraw_grid();
  ui_mode_normal();
  //redraw_all();
  touch_start_watchdog();
}

static void ui_process_lever(void)
{
  switch (ui_mode) {
  case UI_NORMAL:
    ui_process_normal();
    break;    
  case UI_MENU:
    ui_process_menu();
    break;    
//  case UI_NUMERIC:
//    ui_process_numeric();
//    break;
  case UI_KEYPAD:
    ui_process_keypad();
    break;    
  }
}


static void drag_marker(int t, int m)
{
  int status;
  /* wait touch release */
  do {
    int touch_x, touch_y;
    int index;
    touch_position(&touch_x, &touch_y);
    touch_x -= OFFSETX;
    touch_y -= OFFSETY;
    index = search_nearest_index(touch_x, touch_y, t);
    if (index >= 0) {
      markers[m].index = index;
      markers[m].frequency = frequencies[index];
      redraw_marker(m, TRUE);
    }

    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
}

static int sq_distance(int x0, int y0)
{
  return x0*x0 + y0*y0;
}

static int touch_pickup_marker(void)
{
  int touch_x, touch_y;
  int m, t;
  touch_position(&touch_x, &touch_y);
  touch_x -= OFFSETX;
  touch_y -= OFFSETY;

  for (m = 0; m < MARKER_COUNT; m++) {
    if (!markers[m].enabled)
      continue;

    for (t = 0; t < TRACE_COUNT; t++) {
      int x, y;
      if (!trace[t].enabled)
        continue;

      marker_position(m, t, &x, &y);

      if (sq_distance(x - touch_x, y - touch_y) < 400) {
        if (active_marker != m) {
          previous_marker = active_marker;
          active_marker = m;
          redraw_marker(active_marker, TRUE);
        }
        // select trace
        uistat.current_trace = t;
        
        // drag marker until release
        drag_marker(t, m);
        return TRUE;
      }
    }
  }

  return FALSE;
}


static void ui_process_touch(void)
{
  awd_count++;
  adc_stop(ADC1);

  int status = touch_check();
  if (status == EVT_TOUCH_PRESSED || status == EVT_TOUCH_DOWN) {
    switch (ui_mode) {
    case UI_NORMAL:

      if (touch_pickup_marker()) {
        break;
      }
      
      touch_wait_release();

      // switch menu mode
      selection = -1;
      ui_mode_menu();
      break;

    case UI_MENU:
      menu_apply_touch();
      break;

    case UI_NUMERIC:
      numeric_apply_touch();
      break;
    }
  }
  touch_start_watchdog();
}

void ui_process(void)
{
  switch (operation_requested) {
  case OP_LEVER:
    ui_process_lever();
    break;
  case OP_TOUCH:
    ui_process_touch();
    break;
  }
  operation_requested = OP_NONE;
}

#if defined (PAL_USE_CALLBACKS)
// Use new pal API snce EXTI has been deprecated.
palcallback_t switch_cb (void *arg) {
  (void)arg;

  chSysLockFromISR();
  operation_requested = OP_LEVER;
  //cur_button = READ_PORT() & BUTTON_MASK;
  chSysUnlockFromISR();
  return(NULL);
}
#else
/* Triggered when the button is pressed or released. The LED4 is set to ON.*/
static void extcb1(EXTDriver *extp, expchannel_t channel) {
  (void)extp;
  (void)channel;
  operation_requested = OP_LEVER;
  //cur_button = READ_PORT() & BUTTON_MASK;
}

static const EXTConfig extcfg = {
  {
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_RISING_EDGE | EXT_CH_MODE_AUTOSTART | EXT_MODE_GPIOA, extcb1},
    {EXT_CH_MODE_RISING_EDGE | EXT_CH_MODE_AUTOSTART | EXT_MODE_GPIOA, extcb1},
    {EXT_CH_MODE_RISING_EDGE | EXT_CH_MODE_AUTOSTART | EXT_MODE_GPIOA, extcb1},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL},
    {EXT_CH_MODE_DISABLED, NULL}
  }
};
#endif
static const GPTConfig gpt3cfg = {
  1000,    /* 1kHz timer clock.*/
  NULL,   /* Timer callback.*/
  0x0020, /* CR2:MMS=02 to output TRGO */
  0
};

void test_touch(int *x, int *y)
{
  adc_stop(ADC1);

  *x = touch_measure_x();
  *y = touch_measure_y();

  touch_start_watchdog();
}

void handle_touch_interrupt(void)
{
  operation_requested = OP_TOUCH;
}

void ui_init()
{
  adc_init();

#if defined (PAL_USE_CALLBACKS)
  /* Events initialization and registration.*/
  //chEvtObjectInit(&button_pressed_event);
  //chEvtRegister(&button_pressed_event, &el0, 0);
  /* Enabling events on rising edge of the button lines.*/
  palEnableLineEvent(LINE_UP, PAL_EVENT_MODE_RISING_EDGE);
  palEnableLineEvent(LINE_DOWN, PAL_EVENT_MODE_RISING_EDGE);
  palEnableLineEvent(LINE_PUSH,   PAL_EVENT_MODE_RISING_EDGE);
  /* Assigning a callback. */
  palSetLineCallback(LINE_UP, switch_cb, NULL);
  palSetLineCallback(LINE_DOWN, switch_cb, NULL);
  palSetLineCallback(LINE_PUSH,   switch_cb, NULL);
#else
  /*
   * Activates the EXT driver 1.
   */
  extStart(&EXTD1, &extcfg);
#endif
  
#if 1
  gptStart(&GPTD3, &gpt3cfg);
  gptPolledDelay(&GPTD3, 10); /* Small delay.*/

  gptStartContinuous(&GPTD3, 10);
#endif

  touch_start_watchdog();
}
