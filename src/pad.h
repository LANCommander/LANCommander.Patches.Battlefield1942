/* gamepad.c: SDL gamepads offered to the game as DirectInput 8 joysticks. */
#ifndef PAD_H
#define PAD_H
#define SDL_MAIN_HANDLED
#include <SDL.h>
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#define PAD_MAX 4                     /* SDL gamepads offered to the game as joysticks */

SDL_GameController *pad_controller(int slot);
#endif
