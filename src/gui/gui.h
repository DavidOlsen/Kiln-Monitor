#ifndef GUI_H
#define GUI_H

#include <Arduino.h>

void gui_start();
void gui_run();

void statusScreen();
void configScreen();

void disp_top_bar();
void disp_error_msg(String title, String message1, String message2);
void disp_connecting();

void tftPrintCenterWidth(String text, int y);
void tftPrint(String text, int x, int y);
void readButtons();

void resetTFT();

#endif
