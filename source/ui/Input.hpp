#pragma once

#include <vpad/input.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>

class Input {
public:
    Input();
    void Update();

    bool  IsPressed(uint32_t button) const;
    bool  IsHeld(uint32_t button)    const;

    // Analog sticks — range [-1, +1]
    float GetRightStickX() const { return mRightStick.x; }
    float GetRightStickY() const { return mRightStick.y; }
    float GetLeftStickX()  const { return mLeftStick.x; }
    float GetLeftStickY()  const { return mLeftStick.y; }

    // GamePad touch — screen-space coordinates (0-1920, 0-1080)
    bool  IsTouchPressed()    const { return mTouchPressed; }
    bool  IsTouchJustPressed()  const { return  mTouchPressed && !mTouchWasPressed; }
    bool  IsTouchJustReleased() const { return !mTouchPressed &&  mTouchWasPressed; }
    int   GetTouchX()  const { return mTouchX; }
    int   GetTouchY()  const { return mTouchY; }
    int   GetTouchDragX() const { return mTouchDragX; }
    int   GetTouchDragY() const { return mTouchDragY; }
    int   GetTouchStartX() const { return mTouchStartX; }
    int   GetTouchStartY() const { return mTouchStartY; }

    // Wiimote IR pointer — screen-space coordinates (0-1920, 0-1080)
    bool  IsPointerActive()     const { return mPointerActive; }
    int   GetPointerX()    const { return mPointerX; }
    int   GetPointerY()    const { return mPointerY; }
    int   GetPointerDragX() const { return mPointerDragX; }
    int   GetPointerDragY() const { return mPointerDragY; }

    // Unified button constants
    static constexpr uint32_t BUTTON_A     = 1 << 0;
    static constexpr uint32_t BUTTON_B     = 1 << 1;
    static constexpr uint32_t BUTTON_X     = 1 << 2;
    static constexpr uint32_t BUTTON_Y     = 1 << 3;
    static constexpr uint32_t BUTTON_LEFT  = 1 << 4;
    static constexpr uint32_t BUTTON_RIGHT = 1 << 5;
    static constexpr uint32_t BUTTON_UP    = 1 << 6;
    static constexpr uint32_t BUTTON_DOWN  = 1 << 7;
    static constexpr uint32_t BUTTON_L     = 1 << 8;
    static constexpr uint32_t BUTTON_R     = 1 << 9;
    static constexpr uint32_t BUTTON_PLUS  = 1 << 10;
    static constexpr uint32_t BUTTON_MINUS = 1 << 11;
    static constexpr uint32_t BUTTON_ZL    = 1 << 12;
    static constexpr uint32_t BUTTON_ZR    = 1 << 13;

private:
    VPADStatus    vpadStatus;
    VPADReadError vpadError;
    KPADStatus    kpadStatus[4];

    uint32_t buttonsPressed;
    uint32_t buttonsHeld;

    struct Stick { float x = 0.f, y = 0.f; };
    Stick mLeftStick;
    Stick mRightStick;

    // Touch state (screen-space, scaled from 1280x720 to 1920x1080)
    bool mTouchPressed     = false;
    bool mTouchWasPressed  = false;
    int  mTouchX           = 0;
    int  mTouchY           = 0;
    int  mTouchPrevX       = 0;
    int  mTouchPrevY       = 0;
    int  mTouchDragX       = 0;
    int  mTouchDragY       = 0;
    int  mTouchStartX      = 0;
    int  mTouchStartY      = 0;

    // Wiimote pointer state (screen-space, projected from KPAD normalized coords)
    bool mPointerActive    = false;
    int  mPointerX         = 0;
    int  mPointerY         = 0;
    int  mPointerPrevX     = 0;
    int  mPointerPrevY     = 0;
    int  mPointerDragX     = 0;
    int  mPointerDragY     = 0;
    float mPointerSmoothX  = 0.f;
    float mPointerSmoothY  = 0.f;

    uint32_t MapVPADButtons(uint32_t vpadButtons);
    uint32_t MapProButtons(uint32_t proButtons);
    uint32_t MapWiimoteButtons(uint32_t wiimoteButtons);
};
