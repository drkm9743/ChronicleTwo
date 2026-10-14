#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

#include "gamepad.hpp"
#include "mainloop.hpp"
#include "mglib.hpp"
#include "platform/clock.hpp"
#include "platform/input_script.hpp"
#include "sce/threads.hpp"

void WaitVSync(int start, int frames);

namespace {

constexpr std::int64_t kPressFrame = 4;
constexpr std::int64_t kReleaseFrame = 5;

// Both pads are scripted so both are connected; an unscripted port reads as unplugged.
const std::string kPressFirstPort = "0 pad1\n0 pad2\n4 pad1 cross 255 128 128 128\n5 pad1\n";
const std::string kPressSecondPort = "0 pad1\n0 pad2\n4 pad2 circle 128 128 255 128\n5 pad2\n";

void InstallScript(const std::string &text) {
    InputScript script;
    std::string error;
    ASSERT_TRUE(InputScriptParse(text, script, error)) << error;
    InputScriptInstall(std::move(script));
}

void InitGamePad() {
    // Init spins on sceGsSyncV, which would otherwise sleep a field per call.
    ClockSetUnbounded(true);
    ClockReset();
    GamePad__2.Init();
}

void Tick(std::int64_t frame) {
    InputScriptApply(frame);
    GamePad__2.UpDate();
}

void WarmUp() {
    for (std::int64_t frame = 0; frame < kPressFrame; ++frame) {
        Tick(frame);
    }
}

} // namespace

TEST(PlatformPad, InitReachesAnalogReadyOnBothPorts) {
    InstallScript("0 pad1\n0 pad2\n");
    InitGamePad();
    WarmUp();

    EXPECT_EQ(GamePad__2.pad[0].phase, PAD_PHASE_READY);
    EXPECT_EQ(GamePad__2.pad[1].phase, PAD_PHASE_READY);
    EXPECT_EQ(GamePad__2.pad[0].pad_mode, PAD_TERMINAL_DUALSHOCK);
    EXPECT_EQ(GamePad__2.pad[1].pad_mode, PAD_TERMINAL_DUALSHOCK);
    EXPECT_EQ(GamePad__2.GetLX(), 0);
    EXPECT_EQ(GamePad__2.GetRX2(), 0);
    InputScriptInstall({});
}

TEST(PlatformPad, ScriptedPressReadsOnFirstPortAndReleases) {
    InstallScript(kPressFirstPort);
    InitGamePad();
    WarmUp();

    Tick(kPressFrame);
    EXPECT_TRUE(GamePad__2.On(PAD_CROSS));
    EXPECT_TRUE(GamePad__2.Down(PAD_CROSS));
    EXPECT_FALSE(GamePad__2.Up(PAD_CROSS));
    EXPECT_FALSE(GamePad__2.On(PAD_CIRCLE));
    EXPECT_EQ(GamePad__2.GetLX(), 128);

    Tick(kPressFrame + 1);
    EXPECT_FALSE(GamePad__2.On(PAD_CROSS));
    EXPECT_FALSE(GamePad__2.Down(PAD_CROSS));
    EXPECT_TRUE(GamePad__2.Up(PAD_CROSS));
    EXPECT_EQ(GamePad__2.GetLX(), 0);
    InputScriptInstall({});
}

// A second controller, or the same one seen twice through DSX or Steam Input, never plays.
TEST(PlatformPad, SecondPortStaysNeutral) {
    InstallScript(kPressSecondPort);
    InitGamePad();
    WarmUp();

    Tick(kPressFrame);
    EXPECT_FALSE(GamePad__2.On2(PAD_CIRCLE));
    EXPECT_FALSE(GamePad__2.Down2(PAD_CIRCLE));
    EXPECT_FALSE(GamePad__2.On(PAD_CIRCLE));
    EXPECT_EQ(GamePad__2.pad[1].button, 0);
    EXPECT_EQ(GamePad__2.GetRX2(), 0);
    EXPECT_EQ(GamePad__2.pad[1].phase, PAD_PHASE_READY);
    InputScriptInstall({});
}

TEST(PlatformPad, VibrationThreadStepsWithoutDeadlock) {
    InstallScript("0 pad1\n0 pad2\n");
    InitGamePad();
    KzThreadsSetMainPriority(GAMEPAD_THREAD_PRIORITY);

    CreateGamePadThread(&GamePad__2);
    mgSetRotateThread(GAMEPAD_THREAD_PRIORITY);
    WaitVSync(mgGetVSyncCount(), 3);
    mgSetRotateThread(-1);

    // Step accumulates up to the thread's last turn, which runs one tick before the final count.
    EXPECT_GT(GamePad__2.vibration_elapsed, 0);
    EXPECT_EQ(GamePad__2.vibration_elapsed, mgGetVSyncCount() - 1);
    InputScriptInstall({});
}
