#include "Input.hpp"
#include "Gfx.hpp"
#include <cstring>

Input::Input() : buttonsPressed(0), buttonsHeld(0) {
    memset(&vpadStatus, 0, sizeof(vpadStatus));
    memset(&kpadStatus, 0, sizeof(kpadStatus));
    WPADEnableURCC(1);

    // Enable IR pointer (DPD) on all Wiimote channels
    for (int ch = 0; ch < 4; ch++) {
        KPADEnableDPD((KPADChan)ch);
    }
}

static float ApplyDeadzone(float v, float dz = 0.15f) {
    if (v > dz)  return (v - dz) / (1.f - dz);
    if (v < -dz) return (v + dz) / (1.f - dz);
    return 0.f;
}

void Input::Update() {
    uint32_t pressed = 0;
    uint32_t held    = 0;
    mLeftStick  = {0.f, 0.f};
    mRightStick = {0.f, 0.f};

    // GamePad (VPAD)
    VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &vpadError);
    if (vpadError == VPAD_READ_SUCCESS) {
        pressed |= MapVPADButtons(vpadStatus.trigger);
        held    |= MapVPADButtons(vpadStatus.hold);
        mLeftStick.x  = ApplyDeadzone(vpadStatus.leftStick.x);
        mLeftStick.y  = ApplyDeadzone(vpadStatus.leftStick.y);
        mRightStick.x = ApplyDeadzone(vpadStatus.rightStick.x);
        mRightStick.y = ApplyDeadzone(vpadStatus.rightStick.y);

        // GamePad Touch input — calibrate and scale to screen coordinates
        VPADTouchData calibrated;
        VPADGetTPCalibratedPoint(VPAD_CHAN_0, &calibrated, &vpadStatus.tpFiltered1);

        mTouchWasPressed = mTouchPressed;
        mTouchPrevX = mTouchX;
        mTouchPrevY = mTouchY;

        if (calibrated.touched != 0 && calibrated.validity == VPAD_VALID) {
            mTouchPressed = true;
            // Scale from 1280x720 → 1920x1080
            mTouchX = (int)(calibrated.x * 1920 / 1280);
            mTouchY = (int)(calibrated.y * 1080 / 720);

            if (IsTouchJustPressed()) {
                mTouchStartX = mTouchX;
                mTouchStartY = mTouchY;
                mTouchDragX = 0;
                mTouchDragY = 0;
            } else {
                mTouchDragX = mTouchX - mTouchPrevX;
                mTouchDragY = mTouchY - mTouchPrevY;
            }
        } else {
            mTouchPressed = false;
            mTouchDragX = 0;
            mTouchDragY = 0;
        }
    }

    // Pro Controller / Wiimote (KPAD)
    mPointerPrevX = mPointerX;
    mPointerPrevY = mPointerY;
    bool foundPointer = false;

    for (int ch = 0; ch < 4; ch++) {
        int32_t count = KPADRead((KPADChan)ch, &kpadStatus[ch], 1);
        if (count <= 0) continue;
        KPADStatus& k = kpadStatus[ch];

        if (k.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            pressed |= MapProButtons(k.pro.trigger);
            held    |= MapProButtons(k.pro.hold);
        } else if (k.extensionType == WPAD_EXT_CORE) {
            // Plain Wiimote — read IR pointer
            pressed |= MapWiimoteButtons(k.trigger);
            held    |= MapWiimoteButtons(k.hold);

            if (k.posValid && !foundPointer) {
                // Apply a slight scale-up so the cursor can reach screen edges (needs further tweaking)
                constexpr float EDGE_SCALE = 1.15f;
                KPADVec2D normPos = { k.pos.x * EDGE_SCALE, k.pos.y * EDGE_SCALE };
                KPADRect  screen  = {{0.f, 0.f}, {(float)Gfx::SCREEN_WIDTH, (float)Gfx::SCREEN_HEIGHT}};
                KPADVec2D projected;
                KPADGetProjectionPos(&projected, &normPos, &screen, 1.0f);

                // Clamp to screen bounds
                if (projected.x < 0.f) projected.x = 0.f;
                if (projected.y < 0.f) projected.y = 0.f;
                if (projected.x > (float)Gfx::SCREEN_WIDTH)  projected.x = (float)Gfx::SCREEN_WIDTH;
                if (projected.y > (float)Gfx::SCREEN_HEIGHT) projected.y = (float)Gfx::SCREEN_HEIGHT;

                if (!mPointerActive) {
                    // First frame — snap to position
                    mPointerSmoothX = projected.x;
                    mPointerSmoothY = projected.y;
                } else {
                    // Exponential moving average to smooth jitter
                    constexpr float SMOOTH = 0.35f;
                    mPointerSmoothX = mPointerSmoothX + SMOOTH * (projected.x - mPointerSmoothX);
                    mPointerSmoothY = mPointerSmoothY + SMOOTH * (projected.y - mPointerSmoothY);
                }

                mPointerActive = true;
                mPointerX = (int)mPointerSmoothX;
                mPointerY = (int)mPointerSmoothY;
                foundPointer = true;
            }
        }
    }

    if (!foundPointer) {
        mPointerActive = false;
    }

    if (mPointerActive) {
        mPointerDragX = mPointerX - mPointerPrevX;
        mPointerDragY = mPointerY - mPointerPrevY;
    } else {
        mPointerDragX = 0;
        mPointerDragY = 0;
    }

    buttonsPressed = pressed;
    buttonsHeld    = held;
}

bool Input::IsPressed(uint32_t button) const { return (buttonsPressed & button) != 0; }
bool Input::IsHeld(uint32_t button)    const { return (buttonsHeld    & button) != 0; }

uint32_t Input::MapVPADButtons(uint32_t v) {
    uint32_t m = 0;
    if (v & VPAD_BUTTON_A)     m |= BUTTON_A;
    if (v & VPAD_BUTTON_B)     m |= BUTTON_B;
    if (v & VPAD_BUTTON_X)     m |= BUTTON_X;
    if (v & VPAD_BUTTON_Y)     m |= BUTTON_Y;
    if (v & VPAD_BUTTON_LEFT)  m |= BUTTON_LEFT;
    if (v & VPAD_BUTTON_RIGHT) m |= BUTTON_RIGHT;
    if (v & VPAD_BUTTON_UP)    m |= BUTTON_UP;
    if (v & VPAD_BUTTON_DOWN)  m |= BUTTON_DOWN;
    if (v & VPAD_BUTTON_L)     m |= BUTTON_L;
    if (v & VPAD_BUTTON_R)     m |= BUTTON_R;
    if (v & VPAD_BUTTON_PLUS)  m |= BUTTON_PLUS;
    if (v & VPAD_BUTTON_MINUS) m |= BUTTON_MINUS;
    if (v & VPAD_BUTTON_ZL)    m |= BUTTON_ZL;
    if (v & VPAD_BUTTON_ZR)    m |= BUTTON_ZR;
    return m;
}

uint32_t Input::MapProButtons(uint32_t p) {
    uint32_t m = 0;
    if (p & WPAD_PRO_BUTTON_A)     m |= BUTTON_A;
    if (p & WPAD_PRO_BUTTON_B)     m |= BUTTON_B;
    if (p & WPAD_PRO_BUTTON_X)     m |= BUTTON_X;
    if (p & WPAD_PRO_BUTTON_Y)     m |= BUTTON_Y;
    if (p & WPAD_PRO_BUTTON_LEFT)  m |= BUTTON_LEFT;
    if (p & WPAD_PRO_BUTTON_RIGHT) m |= BUTTON_RIGHT;
    if (p & WPAD_PRO_BUTTON_UP)    m |= BUTTON_UP;
    if (p & WPAD_PRO_BUTTON_DOWN)  m |= BUTTON_DOWN;
    if (p & WPAD_PRO_BUTTON_L)     m |= BUTTON_L;
    if (p & WPAD_PRO_BUTTON_R)     m |= BUTTON_R;
    if (p & WPAD_PRO_BUTTON_PLUS)  m |= BUTTON_PLUS;
    if (p & WPAD_PRO_BUTTON_MINUS) m |= BUTTON_MINUS;
    if (p & WPAD_PRO_TRIGGER_ZL)   m |= BUTTON_ZL;
    if (p & WPAD_PRO_TRIGGER_ZR)   m |= BUTTON_ZR;
    if (p & WPAD_PRO_STICK_L_EMULATION_UP)    m |= BUTTON_UP;
    if (p & WPAD_PRO_STICK_L_EMULATION_DOWN)  m |= BUTTON_DOWN;
    if (p & WPAD_PRO_STICK_L_EMULATION_LEFT)  m |= BUTTON_LEFT;
    if (p & WPAD_PRO_STICK_L_EMULATION_RIGHT) m |= BUTTON_RIGHT;
    return m;
}

uint32_t Input::MapWiimoteButtons(uint32_t w) {
    uint32_t m = 0;
    if (w & WPAD_BUTTON_A)     m |= BUTTON_A;
    if (w & WPAD_BUTTON_B)     m |= BUTTON_B;
    if (w & WPAD_BUTTON_1)     m |= BUTTON_X;
    if (w & WPAD_BUTTON_2)     m |= BUTTON_Y;
    if (w & WPAD_BUTTON_LEFT)  m |= BUTTON_LEFT;
    if (w & WPAD_BUTTON_RIGHT) m |= BUTTON_RIGHT;
    if (w & WPAD_BUTTON_UP)    m |= BUTTON_UP;
    if (w & WPAD_BUTTON_DOWN)  m |= BUTTON_DOWN;
    if (w & WPAD_BUTTON_PLUS)  m |= BUTTON_PLUS;
    if (w & WPAD_BUTTON_MINUS) m |= BUTTON_MINUS;
    return m;
}
