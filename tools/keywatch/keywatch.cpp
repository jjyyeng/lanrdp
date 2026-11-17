// 被控端用的左右修饰键观测器：只调 Win32，不依赖 Qt / MinGW 运行时 DLL。
// 目的：控制端按住左/右修饰键时，在被控端上直接读出「到底是哪一侧按下了」。
// 判据：按下后松开，状态必须回到 0；停在 1 说明抬起丢了（扩展位没带回去）。
//
// 为什么需要它：左右 Ctrl 的虚拟键码都是 0x11，端到端只剩「Ctrl + 点击能加选」
// 这种弱证据 —— 左 Ctrl 能加选、右 Ctrl 塌成左 Ctrl 也一样能加选，肉眼等价。
// 判左右只能直接读被控端的 L/R 八项键态。
//
// 编译（静态链，只依赖 KERNEL32 / msvcrt / USER32，拷到任何 Windows 直接跑）：
//   g++ -O2 -static -s keywatch.cpp -o keywatch.exe -luser32
// 产物不打进 dist 之外的任何地方；验收时把它拷到被控端单独运行。
#include <windows.h>
#include <stdio.h>
namespace {

struct KeyDef
{
    int         vk;
    const char *name;
};

const KeyDef kKeys[] = {
    {VK_LCONTROL, "LCtrl"},  {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"},   {VK_RMENU, "RAlt"},
    {VK_LSHIFT, "LShift"},   {VK_RSHIFT, "RShift"},  {VK_LWIN, "LWin"},    {VK_RWIN, "RWin"},
};
const int kCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

bool isDown(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

} // namespace

int main()
{
    ::SetConsoleTitleA("lanrdp keywatch");

    ::printf("lanrdp keywatch -- shows which SIDE each modifier arrives on.\n");
    ::printf("1) Keep THIS window focused on the HOST machine.\n");
    ::printf("2) From the controller, press and release Left/Right Ctrl, Alt, Shift, Win.\n");
    ::printf("3) Expected: the matching side goes DOWN, then UP, and every digit is back to 0.\n");
    ::printf("   A digit that stays 1 after you let go = the key-up was lost (stuck key).\n\n");
    ::fflush(stdout);

    bool prev[kCount]  = {};
    bool seenDown      = false;
    unsigned long ticks = 0;

    for (;;) {
        bool now[kCount];
        bool changed = false;
        for (int i = 0; i < kCount; ++i) {
            now[i] = isDown(kKeys[i].vk);
            if (now[i] != prev[i])
                changed = true;
        }

        if (changed) {
            SYSTEMTIME st{};
            ::GetLocalTime(&st);
            ::printf("[%02u:%02u:%02u.%03u]", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

            bool anyDown = false;
            for (int i = 0; i < kCount; ++i) {
                if (now[i] != prev[i])
                    ::printf("  %s %s", kKeys[i].name, now[i] ? "DOWN" : "UP");
                anyDown = anyDown || now[i];
            }
            ::printf("   |");
            for (int i = 0; i < kCount; ++i)
                ::printf(" %s=%d", kKeys[i].name, now[i] ? 1 : 0);
            ::printf("\n");
            ::fflush(stdout);

            if (anyDown)
                seenDown = true;
            else if (seenDown) {
                ::printf("                     (all released -- no stuck key)\n");
                ::fflush(stdout);
                seenDown = false;
            }
        }

        for (int i = 0; i < kCount; ++i)
            prev[i] = now[i];

        ::Sleep(15);
        if (++ticks % 60000 == 0) {   // 约每 15 分钟提醒一次，确认程序还活着
            ::printf("... still watching\n");
            ::fflush(stdout);
        }
    }
}
