cmake_minimum_required(VERSION 3.21)

project(lanrdp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 让编译器免费帮我们抓问题（形参遮蔽、符号/无符号比较、未使用变量…）。
# 先只开警告、不加 -Werror：项目里可能还有历史告警，一上来就 -Werror 会挡住构建。
# 将来清干净了再考虑收紧。
add_compile_options(-Wall -Wextra)

# Qt 自动化工具：moc / rcc / uic
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if (WIN32)
    # 让 windres 可用 —— exe 资源段的图标（任务栏 / 资源管理器）只能从这里来。
    enable_language(RC)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test LinguistTools)

# ---------------------------------------------------------------------------
# lanrdp_core：可测试的核心代码（协议 / 网络 / 抓屏 / 注入），与 UI 解耦
# ---------------------------------------------------------------------------
add_library(lanrdp_core STATIC
    protocol/Frame.cpp
    protocol/ScreenFrame.cpp
    protocol/H264Frame.cpp
    protocol/FileTransfer.cpp
    protocol/InputEvent.cpp
    protocol/Clipboard.cpp
    protocol/AudioChunk.cpp
    protocol/ScreenControl.cpp
    net/Session.cpp
    net/Auth.cpp
    # Phase 22（②⑭）：按来源 IP 分桶的登录失败退避（注入时钟的纯逻辑类）。
    net/AuthBackoff.cpp
    net/HostServer.cpp
    net/FileReceiver.cpp
    net/FileSendQueue.cpp
    net/FileSender.cpp
    net/ClipboardSync.cpp
    # 往返时间（SIO_TCP_INFO）：质量读数里唯一需要问操作系统的量。
    net/TcpRtt.cpp
    # 局域网自动发现（UDP 广播）：跟 TCP 协议版本无关的旁路通道。
    net/Discovery.cpp
    # Phase 13：控制端「一条连接」的容器（为 1→N 一控多铺路）。
    # 放 core（不依赖 QWidget）—— 与 net/HostServer 配对；也便于日后单独单测。
    net/ClientConnection.cpp
    host/ScreenCapturer.cpp
    host/ScreenStreamer.cpp
    host/InputInjector.cpp
    # 被控端宿主边界（监听 + 会话池 + 抓屏编码 + 注入 + 文件 + 令牌）。
    # 不依赖 QWidget / QClipboard，故进 core —— 也便于单独单测。
    host/HostController.cpp
    common/ColorConvert.cpp
    common/MfRuntime.cpp
    # 「连我」卡片的地址筛选（纯名字判断 + QNetworkInterface 枚举）
    common/LanAddresses.cpp
    # 界面语言：QTranslator 装卸 + 「跟随系统」的判定。只依赖 QtCore，
    # 放 core 让 test_i18n 能直接链它 —— 语言这种全局状态最该有断言。
    common/I18n.cpp
    host/H264Encoder.cpp
    # 音频：被控端扇出（目标管理 + 格式补发 + 逐目标背压丢块）
    host/AudioStreamer.cpp
    # 音频：被控端 WASAPI loopback 采集（自己的线程；静默期靠轮询兜底）
    host/AudioCapturer.cpp
    # Phase 19（⑨ 自适应码率）：积压→码率/帧率的策略纯函数
    host/AdaptiveBitrate.cpp
    # BGRA -> NV12 的加速（GPU VideoProcessor / CPU 直读）。
    # 4K 下实测「抓屏 + 色彩转换」占一拍 40 ms 中的 25 ms，主体是两次 33 MB 的
    # CPU 全量搬运；本类把它们整段省掉。
    host/Nv12Converter.cpp
    host/TokenGate.cpp
    viewer/KeyMap.cpp
    viewer/H264Decoder.cpp
    viewer/DecoderWorker.cpp
    # 音频：播放端抖动缓冲（纯策略，按字节深度决策，不读时钟）
    viewer/AudioJitterBuffer.cpp
    # 音频：控制端 WASAPI 共享渲染（自己的线程；设备失效重建）
    viewer/AudioPlayer.cpp
    # Phase 11②：日志端口（内存缓冲 + 落盘）。纯逻辑、不依赖 QWidget，
    # 放 core 让 app 与测试都能直接链接，不必各自重复列出。
    common/LogPort.cpp
    # 会话启动参数的落盘（QSettings + DPAPI）。同上：不依赖 QWidget，
    # 放 core 才能被 test_sessionstore 直链 —— 落盘逻辑最需要单测，
    # 却又最容易被"只能从 UI 走一遍"挡在门外。
    app/SessionStore.cpp
    # 开机自启（当前用户 Run 键）。只碰 QtCore，放 core 让 test_autostart 直链
    # —— 注册表写入失败时不会抛异常，只会「看起来记住了」。
    app/AutoStart.cpp
    # 窗口标题与任务栏角标的判据。纯函数、只依赖 QString，放 core 是为了能
    # 直接单测：「本机此刻是否正在被看」不允许靠肉眼看窗口去验证。
    app/WindowStatus.cpp
    # 质量读数与一句话诊断（纯函数）。判定门槛最该有断言 —— 它会误报，
    # 而误报的代价是用户从此无视这个读数。
    app/QualityReport.cpp
    # 单实例协调（QLocalServer 命名管道，fail-open）。只依赖 QtNetwork，
    # 放 core 让 test_singleinstance 直链。
    app/SingleInstance.cpp
)
target_include_directories(lanrdp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lanrdp_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
)
if (WIN32)
    # DXGI 桌面复制 / D3D11 / COM GUID / SendInput(user32)
    target_link_libraries(lanrdp_core PRIVATE dxgi d3d11 dxguid user32)
    # Media Foundation：H.264 编解码器 MFT（mfuuid 提供 CLSID/GUID 定义）
    target_link_libraries(lanrdp_core PRIVATE mfplat mfuuid mfreadwrite ole32 oleaut32)
    # DPAPI（CryptProtectData / CryptUnprotectData）：会话口令落盘前加密。
    target_link_libraries(lanrdp_core PRIVATE crypt32)
    # WASAPI（音频）：CLSID_MMDeviceEnumerator / IID_IAudioClient 等 GUID 符号
    # 在 MinGW 下由 libuuid 提供；KSDATAFORMAT_SUBTYPE_PCM / IEEE_FLOAT 在 libksguid
    # —— 两者漏一个都是链接期 undefined reference。
    target_link_libraries(lanrdp_core PRIVATE uuid ksguid)
endif()

# ---------------------------------------------------------------------------
# 应用（双角色：控制端 + 被控端同二进制）
# ---------------------------------------------------------------------------
# WIN32 = 声明为 Windows GUI 子系统（MinGW 下等价 -mwindows）。
# 不写的话 exe 会是「控制台子系统」，双击时 Windows 会先给它分配一个空的
# 控制台窗口（看起来像终端/PowerShell，其实里面什么都没有），要等关掉程序才消失。
# 改成 GUI 子系统后双击干净无黑窗；CLI 自测仍可用——从终端启动时 stdout/stderr
# 句柄会被继承，直接跑 / 管道 / 重定向到文件都能拿到输出（已实测）。
# UI 源（app/ + viewer/）—— 多个目标共用，改动只需一处，避免手工重复列举漂移。
# ⚠️ ScreenOverlayBar / Dialogs / ViewGrid / RemoteView 没有进 lanrdp_core
#    （core 不依赖 Qt Widgets），所以哪些目标用到 MainWindow，就得把它们一并列进来。
set(LANRDP_UI_SOURCES
    app/MainWindow.cpp
    # 全局热键（⑮ / 急停）：RegisterHotKey 挂在一个永不显示的窗口上，
    # WM_HOTKEY 由 QAbstractNativeEventFilter 收。必须进 UI 源 —— 它要 HWND。
    app/GlobalHotkey.cpp
    # 隐私遮挡的本机那一层：铺满虚拟桌面的纯黑置顶窗
    app/PrivacyCurtain.cpp
    # 任务栏按钮角标（ITaskbarList3::SetOverlayIcon）+ 接入时闪任务栏。
    # 依赖 QWidget 取 HWND / DPR，所以只能进 UI 源。
    app/TaskbarBadge.cpp
    # 通知区域常驻图标（QSystemTrayIcon）。只在「关闭窗口后继续共享」打开时
    # 才真的创建，关着的时候它只是个不碰平台的状态记录器。
    app/TrayController.cpp
    # 界面外观：深色 palette + 全局 QSS + 悬浮条内务样式（色值的唯一出处）
    ui/Theme.cpp
    # 矢量图标（QPainterPath 按 24 格绘制）：不依赖 qrc / qsvgicon 插件，
    # 任意尺寸与 DPI 都按目标尺寸重画，配色随调用方传入。
    ui/Glyph.cpp
    # Phase 17：三个启动/设置对话框（【开始监听…】/【新连接…】/【设置…】）
    app/Dialogs.cpp
    # 口令框（内嵌「显示 / 隐藏」开关），两个启动对话框共用
    app/PasswordEdit.cpp
    viewer/ScreenRenderer.cpp
    # Phase 17：画面顶部悬浮操作条（独立子控件）
    viewer/ScreenOverlayBar.cpp
    # Phase 18：多视窗网格宿主（1×1 / 1×2 / 左大右两小 / 2×2 + 占满）
    viewer/ViewGrid.cpp
    # Phase 18：一控多的「一格」封装（画面 + 条 + 条上控件 + 该格连接）
    viewer/RemoteView.cpp
    # 空态层：未选模式时盖在画面上的两个入口（连接对方 / 让对方连进来）
    viewer/EmptyState.cpp
    # 会话期间把 Alt 从本机菜单栏手里截下来（否则远端 Alt 卡住、本机键盘断流）
    viewer/ModifierKeyGuard.cpp
)

# exe 资源段（程序图标 + 版本信息）。.rc 由 CMake 生成 —— 版本号只有 project(VERSION) 一处出处。
# 图标必须传绝对路径：生成物落在构建目录里，相对路径会指向空处。
set(LANRDP_VERSION_RC "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(LANRDP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/app/lanrdp.ico")
configure_file(app/lanrdp.rc.in "${CMAKE_CURRENT_BINARY_DIR}/lanrdp.rc" @ONLY)

add_executable(lanrdp WIN32
    app/main.cpp
    # 命令行解析单独成对（tests/test_cliargs.cpp 直接链它）——
    # 留在 main.cpp 里就没有测试目标能覆盖到那批分支。
    app/CliArgs.cpp
    ${LANRDP_UI_SOURCES}
    # 应用图标 + 版本信息。⚠️ 两处都要：资源段管任务栏/资源管理器，qrc 管窗口标题栏。
    "${CMAKE_CURRENT_BINARY_DIR}/lanrdp.rc"
    app/resources.qrc
)
target_link_libraries(lanrdp PRIVATE
    lanrdp_core
    Qt6::Widgets
)

# ---------------------------------------------------------------------------
# 界面语言翻译
# ---------------------------------------------------------------------------
# **中文是源语言**：源码里 tr("…") 的字面量就是最终中文，中文档不装任何翻译器。
# 英文在 translations/lanrdp_en.ts 里维护，构建时由 lrelease 编成 .qm 并嵌进
# exe 资源（:/i18n/lanrdp_en.qm）—— dist/ 因此不必额外带 .qm 文件，也不会
# 出现「换了语言却没带翻译包」这种只在别人机器上复现的问题。
#
# ⚠️ 改了界面文案要更新 .ts，否则新条目在英文界面下会退化成中文：
#     cmake --build build --target update_translations
# 然后到 .ts 里补 <translation>，再正常构建（lrelease 是构建期自动跑的）。
qt_add_translations(lanrdp
    TS_FILES translations/lanrdp_en.ts
    # ⚠️ 头文件里的 inline 文案也必须显式列出：lupdate 只扫 target 的**源文件列表**，
    # 被 include 的 .h 不在列表里就整批漏抽（症状是英文界面下那几句仍是中文，
    # 而 .ts 里根本查不到）。ConnectionHints.h 就是唯一一处这类实现。
    SOURCES common/ConnectionHints.h
    # 删掉的界面文案不该赖在 .ts 里：留着 obsolete 条目会让「还剩几条没翻」
    # 这个数永远对不上（也体检不出真的漏译）。
    LUPDATE_OPTIONS -no-obsolete
    RESOURCE_PREFIX "/i18n"
)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()

# ---------------------------------------------------------------------------
# 测试注册的统一入口
# ---------------------------------------------------------------------------
# 每个测试目标都是「建 exe + 链 lanrdp_core / Qt6::Test + 注册 ctest」三件套。
# 从前 44 处各写一遍，新增一个测试要动两处（注册 + 末尾的 Qt 测试名单）。
# 这个函数把三件事收成一行，并顺手把名字记进全局属性 —— 末尾设 ENVIRONMENT 时
# 直接取用，不再需要手工维护第二份名单（那份曾漏过 test_clientconnection，
# 症状是「本机能跑、干净机器上找不到 Qt DLL」）。
#
# LABELS 只表达「跑它需要什么环境」，不表达重要程度：
#   unit  = 纯逻辑，任何机器都能跑
#   ui    = 需要 Qt Widgets（offscreen 平台即可，不需要真窗口）
#   net   = 起真实 TCP / 命名管道
#   gfx   = 需要真实桌面或显示器（DXGI 抓屏）
#   media = 需要 H.264 MFT 或声卡
function(lanrdp_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS;LABELS" ${ARGN})
    if (NOT T_SOURCES)
        message(FATAL_ERROR "lanrdp_add_test(${name}) 缺少 SOURCES")
    endif()
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE lanrdp_core Qt6::Test ${T_LIBS})
    add_test(NAME ${name} COMMAND ${name})
    if (T_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
    endif()
    set_property(GLOBAL APPEND PROPERTY LANRDP_QT_TESTS ${name})
endfunction()


lanrdp_add_test(test_frame
    SOURCES tests/test_frame.cpp
    LABELS "unit"
)

lanrdp_add_test(test_session
    SOURCES tests/test_session.cpp
    LABELS "net"
)

lanrdp_add_test(test_auth
    SOURCES tests/test_auth.cpp
    LABELS "net"
)

lanrdp_add_test(test_handshake
    SOURCES tests/test_handshake.cpp
    LABELS "net"
)

lanrdp_add_test(test_capture
    SOURCES tests/test_capture.cpp
    LABELS "gfx"
)

lanrdp_add_test(test_screenframe
    SOURCES tests/test_screenframe.cpp
    LABELS "unit"
)

lanrdp_add_test(test_streamer
    SOURCES tests/test_streamer.cpp
    LABELS "net;gfx"
)

lanrdp_add_test(test_inputevent
    SOURCES tests/test_inputevent.cpp
    LABELS "unit"
)

lanrdp_add_test(test_injector
    SOURCES tests/test_injector.cpp
    LABELS "unit"
)

lanrdp_add_test(test_keymap
    SOURCES tests/test_keymap.cpp
    LABELS "unit"
)

# UI 组件单独编译进测试（不依赖整个 app）。
# ⚠️ 画面控件自己取主题色板（底色 / 空态文字 / 拖放高亮），所以**每个**单独
# 编译 ScreenRenderer.cpp 的测试目标都要一并链上 Theme.cpp —— 漏一个就是
# 「undefined reference to lanrdp::theme::colors()」，而报错点在链接阶段，
# 看起来像测试本身崩了。
set(LANRDP_RENDERER_SOURCES
    viewer/ScreenRenderer.cpp
    viewer/ModifierKeyGuard.cpp
    ui/Theme.cpp
)

lanrdp_add_test(test_renderer
    SOURCES tests/test_renderer.cpp ${LANRDP_RENDERER_SOURCES}
    LIBS Qt6::Widgets
    LABELS "ui"
)

lanrdp_add_test(test_inputmap
    SOURCES tests/test_inputmap.cpp ${LANRDP_RENDERER_SOURCES}
    LIBS Qt6::Widgets
    LABELS "ui"
)

# 拖拽接收（Phase 7④）：firstLocalFile 纯函数判定
lanrdp_add_test(test_rendererdrop
    SOURCES tests/test_rendererdrop.cpp ${LANRDP_RENDERER_SOURCES}
    LIBS Qt6::Widgets
    LABELS "ui"
)

lanrdp_add_test(test_h264frame
    SOURCES tests/test_h264frame.cpp
    LABELS "unit"
)

lanrdp_add_test(test_colorconvert
    SOURCES tests/test_colorconvert.cpp
    LABELS "unit"
)

# BGRA -> NV12 加速器：CPU 参考实现与显卡路径（显卡不可用的机器上自动 skip）
lanrdp_add_test(test_nv12converter
    SOURCES tests/test_nv12converter.cpp
    LABELS "unit"
)

# 音频两帧的线上格式（AUDIO_CONFIG / AUDIO_DATA）与定长块装配器
lanrdp_add_test(test_audiochunk
    SOURCES tests/test_audiochunk.cpp
    LABELS "unit"
)

# 播放端抖动缓冲的策略（补静音 / 丢旧块，不依赖时钟）
lanrdp_add_test(test_audiojitter
    SOURCES tests/test_audiojitter.cpp
    LABELS "unit"
)

# 多屏两帧的线上格式（SCREEN_LIST / SCREEN_SELECT，协议 v5）
lanrdp_add_test(test_screencontrol
    SOURCES tests/test_screencontrol.cpp
    LABELS "unit"
)

# 窗口标题 / 任务栏角标的判据（纯函数：谁在看、显示成什么文案）
lanrdp_add_test(test_windowstatus
    SOURCES tests/test_windowstatus.cpp
    LABELS "unit"
)

# 通知区域图标：状态点 / 悬停提示 / 无托盘环境下的状态记录。
# 需要 Widgets（QSystemTrayIcon），所以把它的三个依赖源一并列进来。
lanrdp_add_test(test_traycontroller
    SOURCES tests/test_traycontroller.cpp app/TrayController.cpp ui/Glyph.cpp ui/Theme.cpp
    LIBS Qt6::Widgets
    LABELS "ui"
)

# 单实例协调：命名管道监听 / 新实例探测 / fail-open / 名字释放
lanrdp_add_test(test_singleinstance
    SOURCES tests/test_singleinstance.cpp
    LABELS "unit"
)

# 被控端音频扇出（真 loopback 连接：目标管理 / 格式补发 / 背压丢块）
lanrdp_add_test(test_audiostreamer
    SOURCES tests/test_audiostreamer.cpp
    LABELS "net;media"
)

# 音频的硬件往返（WASAPI loopback 采集 / 渲染）。没有音频设备的机器上 QSKIP。
# ⚠️ 故意**不打桩**：这一层要测的正是设备交互本身。
lanrdp_add_test(test_audiohw
    SOURCES tests/test_audiohw.cpp
    LABELS "media"
)

lanrdp_add_test(test_h264encoder
    SOURCES tests/test_h264encoder.cpp
    LABELS "media"
)

lanrdp_add_test(test_h264decoder
    SOURCES tests/test_h264decoder.cpp
    LABELS "media"
)

# Phase 15：解码移出 UI 线程 —— 解码线程对象的契约（含「产帧线程 ≠ 调用线程」的取证）
lanrdp_add_test(test_decoderworker
    SOURCES tests/test_decoderworker.cpp
    LABELS "media"
)

# Phase 15：ClientConnection 的 H.264 端到端回归（真连接 + 真编码器 + 真解码线程）
lanrdp_add_test(test_clientconnection_h264
    SOURCES tests/test_clientconnection_h264.cpp
    LABELS "net;media"
)

# ClientConnection 的通用行为（与上面的 _h264 分工：那个测媒体链路，这个测连接本身）。
# 起于真机 bug：控制端连局域网 IP 被**系统代理**拦掉（"The proxy type is invalid"）。
lanrdp_add_test(test_clientconnection
    SOURCES tests/test_clientconnection.cpp
    LABELS "net"
)

lanrdp_add_test(test_filetransfer
    SOURCES tests/test_filetransfer.cpp
    LABELS "unit"
)

lanrdp_add_test(test_filereceiver
    SOURCES tests/test_filereceiver.cpp
    LABELS "unit"
)

lanrdp_add_test(test_filesender
    SOURCES tests/test_filesender.cpp
    LABELS "net"
)

lanrdp_add_test(test_filetransfer_e2e
    SOURCES tests/test_filetransfer_e2e.cpp
    LABELS "net"
)

lanrdp_add_test(test_tokengate
    SOURCES tests/test_tokengate.cpp
    LABELS "unit"
)

lanrdp_add_test(test_hostserver
    SOURCES tests/test_hostserver.cpp
    LABELS "net"
)

# Phase 31：被控端宿主边界（host/HostController）—— 监听 / 会话池快照 / 令牌 / 统计。
# 这是被控端逻辑从 MainWindow 下沉后的直接落点，从前只能靠 UI 用例间接覆盖。
lanrdp_add_test(test_hostcontroller
    SOURCES tests/test_hostcontroller.cpp
    LABELS "net;gfx"
)
if (WIN32)
    # 响应时长用例要判断环境：桌面物理尺寸（gdi32）与会话是否可交互（user32）
    target_link_libraries(test_hostcontroller PRIVATE gdi32 user32)
endif()

# Phase 9⑤ 剪贴板同步：载荷编解码 + 同步器（回环抑制 / 长度上限）
lanrdp_add_test(test_clipboard
    SOURCES tests/test_clipboard.cpp
    LABELS "unit"
)

lanrdp_add_test(test_clipboardsync
    SOURCES tests/test_clipboardsync.cpp
    LABELS "unit"
)

# UI 结构守护（Phase 6/11/17：悬浮操作条 + 右侧画面；三个对话框）
lanrdp_add_test(test_i18n
    SOURCES tests/test_i18n.cpp
    LIBS Qt6::Widgets
    LABELS "ui"
)

# 英文翻译也要嵌进这个测试 exe：lanrdp 的那份资源只属于主程序，测试目标
# 拿不到。少了这一步，「英文界面」这条断言根本无从验证 —— 而它恰恰是
# 最容易在打包环节被漏掉的东西。
qt_add_resources(test_i18n "i18n"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/lanrdp_en.qm"
)
add_dependencies(test_i18n lanrdp)

# ⑮ 全局热键：解析 + 「被占用时静默降级」的注册策略。
# 真实 RegisterHotKey 在无桌面会话里必然失败，所以注册行为必须能注入假平台。
lanrdp_add_test(test_globalhotkey
    SOURCES tests/test_globalhotkey.cpp app/GlobalHotkey.cpp
    LIBS Qt6::Widgets
    LABELS "ui"
)

lanrdp_add_test(test_mainwindow_ui
    SOURCES tests/test_mainwindow_ui.cpp ${LANRDP_UI_SOURCES}
    LIBS Qt6::Widgets
    LABELS "net;ui"
)# 同 test_i18n：主程序那份 qm 资源不共享，这个 exe 也要自己带一份，
# 否则「切换语言后界面真的变了」这条断言在这里跑不出英文。
qt_add_resources(test_mainwindow_ui "i18n"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES "${CMAKE_CURRENT_BINARY_DIR}/lanrdp_en.qm"
)
add_dependencies(test_mainwindow_ui lanrdp)

# Phase 11 / 任务 11.1：日志端口（落盘 + 环形缓冲），不依赖 QWidget
lanrdp_add_test(test_logport
    SOURCES tests/test_logport.cpp
    LABELS "unit"
)

# 会话参数落盘（ini + DPAPI）。落盘逻辑最该有断言：它错的时候不报错，
# 只表现为「明明记住了，下次开程序又是空的」。
lanrdp_add_test(test_sessionstore
    SOURCES tests/test_sessionstore.cpp
    LABELS "unit"
)

# 开机自启（HKCU 的 Run 值）。键路径可注入 —— 用例写进
# `HKCU\Software\lanrdp-test\Run`，绝不碰用户真实的自启列表。
lanrdp_add_test(test_autostart
    SOURCES tests/test_autostart.cpp
    LABELS "unit"
)

# 命令行参数解析（app/CliArgs.cpp）。从前这批分支全在 main() 里，
# 而 main.cpp 带 main() 没法链进测试目标 ⇒ 未知参数 / 缺取值 / 数字非法 / 动作互斥
# 全都没有断言。⚠️ 不进 lanrdp_core：它属于应用层，core 不该知道 CLI 的形状。
lanrdp_add_test(test_cliargs
    SOURCES tests/test_cliargs.cpp app/CliArgs.cpp
    LABELS "unit"
)

# 口令规则与随机生成（PasswordPolicy.h 是唯一判定口，两个对话框与 --host 共用）
lanrdp_add_test(test_passwordpolicy
    SOURCES tests/test_passwordpolicy.cpp
    LABELS "unit"
)

# 控制端「连不上」的下一步建议（按 socket 错误分类，别让用户对着英文原文发愣）
lanrdp_add_test(test_connectionhints
    SOURCES tests/test_connectionhints.cpp
    LABELS "unit"
)

# 「连我」卡片列出的地址：虚拟网卡必须被排除（列出来只会让人选错）
lanrdp_add_test(test_lanaddresses
    SOURCES tests/test_lanaddresses.cpp common/LanAddresses.cpp
    LABELS "unit"
)

# 往返时间（SIO_TCP_INFO）。这条断言守的是「本机 API 能不能用」这个假设：
# 它静默失效时界面只会显示「往返 —」，看起来像没有数据，而不是像探针坏了。
lanrdp_add_test(test_tcprtt
    SOURCES tests/test_tcprtt.cpp
    LABELS "unit"
)

# 质量读数 / 一句话诊断：门槛与优先级（网络先于解码）、缺读数不报警、
# 静止画面不算「没画面」。
lanrdp_add_test(test_qualityreport
    SOURCES tests/test_qualityreport.cpp
    LABELS "unit"
)

# 局域网自动发现：报文编解码 + 两个实例互见 / 不认自己 / 过期下线。
# 只绑系统分配端口、目标指向 127.0.0.1，绝不往真实局域网里发包。
lanrdp_add_test(test_discovery
    SOURCES tests/test_discovery.cpp
    LABELS "net"
)

# Phase 18：多视窗网格 —— 布局纯函数（1×1 / 1×2 / 左大右两小 / 2×2）
# + 宿主行为（重排 / 占满 / 选中）。⛔ 不进 lanrdp_core（core 不依赖 Qt Widgets）。
lanrdp_add_test(test_viewgrid
    SOURCES tests/test_viewgrid.cpp viewer/ViewGrid.cpp
    LIBS Qt6::Widgets
    LABELS "ui"
)

# Phase 19：自适应码率策略纯函数（积压→码率/帧率，降快升慢 + 迟滞）
lanrdp_add_test(test_adaptive
    SOURCES tests/test_adaptive.cpp
    LABELS "unit"
)

# 界面外观：深色 palette 的暗度 / 文字对比度 / Fusion 是否真的装上。
# ⛔ 不进 lanrdp_core（core 不依赖 Qt Widgets）。
# ⚠️ 它只链 Theme.cpp，不链整个 UI —— 测的是色板与样式表本身，不需要建窗口。
lanrdp_add_test(test_theme
    SOURCES tests/test_theme.cpp ui/Theme.cpp ui/Glyph.cpp
    LIBS Qt6::Widgets
    LABELS "ui"
)

# ---------------------------------------------------------------------------
# 压测工具（**不是单测**）：一控多解码 + 渲染负载
# ---------------------------------------------------------------------------
# 刻意 **不注册 add_test** —— 它是 benchmark，结果依赖机器且耗时较长，
# 混进常规测试会拖慢全量跑，也会打乱"测试口径 = 可执行测试目标数"的计数。
# 跑法：
#   cmake --build build-release --target bench_decode_multi
#   build-release/bench_decode_multi.exe D:/path/to/out.txt
add_executable(bench_decode_multi tests/bench_decode_multi.cpp)
target_link_libraries(bench_decode_multi PRIVATE lanrdp_core)

# Phase 15 收益实测：用**生产路径**（DecoderWorker）跑 4 路，量 UI 线程占用。
# 同样刻意不注册 ctest（要跑几分钟，且是测量而非断言）。
add_executable(bench_decoderworker tests/bench_decoderworker.cpp)
target_link_libraries(bench_decoderworker PRIVATE lanrdp_core)

# ⑨ 自适应码率：⑨-0 可行性实验（MF 同步编码器能否运行中即时改码率）。
# 同样刻意不注册 ctest（测量而非断言）。跑法：
#   cmake --build build --target bench_adaptive_bitrate
#   build/bench_adaptive_bitrate.exe D:/path/to/out.txt
add_executable(bench_adaptive_bitrate tests/bench_adaptive_bitrate.cpp)
target_link_libraries(bench_adaptive_bitrate PRIVATE lanrdp_core)

# 让 ctest 运行时能找到 Qt 运行时 DLL / 平台插件，不依赖调用方 shell 的环境
get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
get_filename_component(_qt_runtime_dir "${_qt_core_dll}" DIRECTORY)
get_filename_component(_qt_prefix_dir "${_qt_runtime_dir}" DIRECTORY)
set(_qt_plugin_dir "${_qt_prefix_dir}/plugins")

# Phase 20 / BUILD-01：需要 Qt 运行时环境的测试（= 上面 lanrdp_add_test 注册的全部）。
# 名单由 lanrdp_add_test 自己累积，不再手写 —— 手写的那份曾漏过
# `test_clientconnection`，它在本机「碰巧」能跑、在干净机器上因找不到 Qt DLL 而失败。
# 下面仍做一次结构性校验：绕过 helper 直接写 add_test 的目标会落不进名单，
# 在 configure 阶段就报错，而不是等到别人机器上才发现。
get_property(_lanrdp_qt_tests GLOBAL PROPERTY LANRDP_QT_TESTS)

get_property(_lanrdp_registered DIRECTORY PROPERTY TESTS)
set(_lanrdp_listed   ${_lanrdp_qt_tests})
set(_lanrdp_actual   ${_lanrdp_registered})
list(SORT _lanrdp_listed)
list(SORT _lanrdp_actual)
if(NOT "${_lanrdp_listed}" STREQUAL "${_lanrdp_actual}")
    message(FATAL_ERROR
        "add_test 注册的测试集合与 lanrdp_add_test 累积的名单不一致 —— "
        "多半是有人绕过 lanrdp_add_test 直接写了 add_test；"
        "那个目标在干净机器上会因找不到 Qt DLL 而失败。\n"
        "  add_test 注册： ${_lanrdp_actual}\n"
        "  列表里写的：   ${_lanrdp_listed}")
endif()

set_tests_properties(
    ${_lanrdp_qt_tests}
    PROPERTIES
    ENVIRONMENT "PATH=${_qt_runtime_dir};$ENV{PATH};QT_PLUGIN_PATH=${_qt_plugin_dir};QT_QPA_PLATFORM=offscreen"
)
