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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Test)

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
    net/Session.cpp
    net/Auth.cpp
    # Phase 22（②⑭）：按来源 IP 分桶的登录失败退避（注入时钟的纯逻辑类）。
    net/AuthBackoff.cpp
    net/HostServer.cpp
    net/FileReceiver.cpp
    net/FileSender.cpp
    net/ClipboardSync.cpp
    # Phase 13：控制端「一条连接」的容器（为 1→N 一控多铺路）。
    # 放 core（不依赖 QWidget）—— 与 net/HostServer 配对；也便于日后单独单测。
    net/ClientConnection.cpp
    host/ScreenCapturer.cpp
    host/ScreenStreamer.cpp
    host/InputInjector.cpp
    # 被控端宿主边界（监听 + 会话池 + 抓屏编码 + 注入 + 文件 + 令牌）。
    # 不依赖 QWidget / QClipboard，故进 core —— 也便于单独单测。
    host/HostController.cpp
    host/ColorConvert.cpp
    host/MfRuntime.cpp
    host/H264Encoder.cpp
    # Phase 19（⑨ 自适应码率）：积压→码率/帧率的策略纯函数
    host/AdaptiveBitrate.cpp
    host/TokenGate.cpp
    viewer/KeyMap.cpp
    viewer/H264Decoder.cpp
    viewer/DecoderWorker.cpp
    # Phase 11②：日志端口（内存缓冲 + 落盘）。纯逻辑、不依赖 QWidget，
    # 放 core 让 app 与测试都能直接链接，不必各自重复列出。
    app/LogPort.cpp
    # 会话启动参数的落盘（QSettings + DPAPI）。同上：不依赖 QWidget，
    # 放 core 才能被 test_sessionstore 直链 —— 落盘逻辑最需要单测，
    # 却又最容易被"只能从 UI 走一遍"挡在门外。
    app/SessionStore.cpp
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
    # 界面外观：深色 palette + 全局 QSS + 悬浮条内务样式（色值的唯一出处）
    app/Theme.cpp
    # 矢量图标（QPainterPath 按 24 格绘制）：不依赖 qrc / qsvgicon 插件，
    # 任意尺寸与 DPI 都按目标尺寸重画，配色随调用方传入。
    app/Glyph.cpp
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
# 测试
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_frame tests/test_frame.cpp)
target_link_libraries(test_frame PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_frame COMMAND test_frame)

add_executable(test_session tests/test_session.cpp)
target_link_libraries(test_session PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_session COMMAND test_session)

add_executable(test_auth tests/test_auth.cpp)
target_link_libraries(test_auth PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_auth COMMAND test_auth)

add_executable(test_handshake tests/test_handshake.cpp)
target_link_libraries(test_handshake PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_handshake COMMAND test_handshake)

add_executable(test_capture tests/test_capture.cpp)
target_link_libraries(test_capture PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_capture COMMAND test_capture)

add_executable(test_screenframe tests/test_screenframe.cpp)
target_link_libraries(test_screenframe PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_screenframe COMMAND test_screenframe)

add_executable(test_streamer tests/test_streamer.cpp)
target_link_libraries(test_streamer PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_streamer COMMAND test_streamer)

add_executable(test_inputevent tests/test_inputevent.cpp)
target_link_libraries(test_inputevent PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_inputevent COMMAND test_inputevent)

add_executable(test_injector tests/test_injector.cpp)
target_link_libraries(test_injector PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_injector COMMAND test_injector)

add_executable(test_keymap tests/test_keymap.cpp)
target_link_libraries(test_keymap PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_keymap COMMAND test_keymap)

# UI 组件单独编译进测试（不依赖整个 app）。
# ⚠️ 画面控件自己取主题色板（底色 / 空态文字 / 拖放高亮），所以**每个**单独
# 编译 ScreenRenderer.cpp 的测试目标都要一并链上 Theme.cpp —— 漏一个就是
# 「undefined reference to lanrdp::theme::colors()」，而报错点在链接阶段，
# 看起来像测试本身崩了。
set(LANRDP_RENDERER_SOURCES
    viewer/ScreenRenderer.cpp
    app/Theme.cpp
)

add_executable(test_renderer tests/test_renderer.cpp ${LANRDP_RENDERER_SOURCES})
target_link_libraries(test_renderer PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_renderer COMMAND test_renderer)

add_executable(test_inputmap tests/test_inputmap.cpp ${LANRDP_RENDERER_SOURCES})
target_link_libraries(test_inputmap PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_inputmap COMMAND test_inputmap)

# 拖拽接收（Phase 7④）：firstLocalFile 纯函数判定
add_executable(test_rendererdrop tests/test_rendererdrop.cpp ${LANRDP_RENDERER_SOURCES})
target_link_libraries(test_rendererdrop PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_rendererdrop COMMAND test_rendererdrop)

add_executable(test_h264frame tests/test_h264frame.cpp)
target_link_libraries(test_h264frame PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_h264frame COMMAND test_h264frame)

add_executable(test_colorconvert tests/test_colorconvert.cpp)
target_link_libraries(test_colorconvert PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_colorconvert COMMAND test_colorconvert)

add_executable(test_h264encoder tests/test_h264encoder.cpp)
target_link_libraries(test_h264encoder PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_h264encoder COMMAND test_h264encoder)

add_executable(test_h264decoder tests/test_h264decoder.cpp)
target_link_libraries(test_h264decoder PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_h264decoder COMMAND test_h264decoder)

# Phase 15：解码移出 UI 线程 —— 解码线程对象的契约（含「产帧线程 ≠ 调用线程」的取证）
add_executable(test_decoderworker tests/test_decoderworker.cpp)
target_link_libraries(test_decoderworker PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_decoderworker COMMAND test_decoderworker)

# Phase 15：ClientConnection 的 H.264 端到端回归（真连接 + 真编码器 + 真解码线程）
add_executable(test_clientconnection_h264 tests/test_clientconnection_h264.cpp)
target_link_libraries(test_clientconnection_h264 PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_clientconnection_h264 COMMAND test_clientconnection_h264)

# ClientConnection 的通用行为（与上面的 _h264 分工：那个测媒体链路，这个测连接本身）。
# 起于真机 bug：控制端连局域网 IP 被**系统代理**拦掉（"The proxy type is invalid"）。
add_executable(test_clientconnection tests/test_clientconnection.cpp)
target_link_libraries(test_clientconnection PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_clientconnection COMMAND test_clientconnection)

add_executable(test_filetransfer tests/test_filetransfer.cpp)
target_link_libraries(test_filetransfer PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_filetransfer COMMAND test_filetransfer)

add_executable(test_filereceiver tests/test_filereceiver.cpp)
target_link_libraries(test_filereceiver PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_filereceiver COMMAND test_filereceiver)

add_executable(test_filesender tests/test_filesender.cpp)
target_link_libraries(test_filesender PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_filesender COMMAND test_filesender)

add_executable(test_filetransfer_e2e tests/test_filetransfer_e2e.cpp)
target_link_libraries(test_filetransfer_e2e PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_filetransfer_e2e COMMAND test_filetransfer_e2e)

add_executable(test_tokengate tests/test_tokengate.cpp)
target_link_libraries(test_tokengate PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_tokengate COMMAND test_tokengate)

add_executable(test_hostserver tests/test_hostserver.cpp)
target_link_libraries(test_hostserver PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_hostserver COMMAND test_hostserver)

# Phase 31：被控端宿主边界（host/HostController）—— 监听 / 会话池快照 / 令牌 / 统计。
# 这是被控端逻辑从 MainWindow 下沉后的直接落点，从前只能靠 UI 用例间接覆盖。
add_executable(test_hostcontroller tests/test_hostcontroller.cpp)
target_link_libraries(test_hostcontroller PRIVATE lanrdp_core Qt6::Test)
if (WIN32)
    # 响应时长用例要判断环境：桌面物理尺寸（gdi32）与会话是否可交互（user32）
    target_link_libraries(test_hostcontroller PRIVATE gdi32 user32)
endif()
add_test(NAME test_hostcontroller COMMAND test_hostcontroller)

# Phase 9⑤ 剪贴板同步：载荷编解码 + 同步器（回环抑制 / 长度上限）
add_executable(test_clipboard tests/test_clipboard.cpp)
target_link_libraries(test_clipboard PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_clipboard COMMAND test_clipboard)

add_executable(test_clipboardsync tests/test_clipboardsync.cpp)
target_link_libraries(test_clipboardsync PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_clipboardsync COMMAND test_clipboardsync)

# UI 结构守护（Phase 6/11/17：悬浮操作条 + 右侧画面；三个对话框）
add_executable(test_mainwindow_ui tests/test_mainwindow_ui.cpp
    ${LANRDP_UI_SOURCES}
)
target_link_libraries(test_mainwindow_ui PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_mainwindow_ui COMMAND test_mainwindow_ui)

# Phase 11 / 任务 11.1：日志端口（落盘 + 环形缓冲），不依赖 QWidget
add_executable(test_logport tests/test_logport.cpp)
target_link_libraries(test_logport PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_logport COMMAND test_logport)

# 会话参数落盘（ini + DPAPI）。落盘逻辑最该有断言：它错的时候不报错，
# 只表现为「明明记住了，下次开程序又是空的」。
add_executable(test_sessionstore tests/test_sessionstore.cpp)
target_link_libraries(test_sessionstore PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_sessionstore COMMAND test_sessionstore)

# 命令行参数解析（app/CliArgs.cpp）。从前这批分支全在 main() 里，
# 而 main.cpp 带 main() 没法链进测试目标 ⇒ 未知参数 / 缺取值 / 数字非法 / 动作互斥
# 全都没有断言。⚠️ 不进 lanrdp_core：它属于应用层，core 不该知道 CLI 的形状。
add_executable(test_cliargs tests/test_cliargs.cpp app/CliArgs.cpp)
target_link_libraries(test_cliargs PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_cliargs COMMAND test_cliargs)

# Phase 18：多视窗网格 —— 布局纯函数（1×1 / 1×2 / 左大右两小 / 2×2）
# + 宿主行为（重排 / 占满 / 选中）。⛔ 不进 lanrdp_core（core 不依赖 Qt Widgets）。
add_executable(test_viewgrid tests/test_viewgrid.cpp
    viewer/ViewGrid.cpp
)
target_link_libraries(test_viewgrid PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_viewgrid COMMAND test_viewgrid)

# Phase 19：自适应码率策略纯函数（积压→码率/帧率，降快升慢 + 迟滞）
add_executable(test_adaptive tests/test_adaptive.cpp)
target_link_libraries(test_adaptive PRIVATE lanrdp_core Qt6::Test)
add_test(NAME test_adaptive COMMAND test_adaptive)

# 界面外观：深色 palette 的暗度 / 文字对比度 / Fusion 是否真的装上。
# ⛔ 不进 lanrdp_core（core 不依赖 Qt Widgets）。
# ⚠️ 它只链 Theme.cpp，不链整个 UI —— 测的是色板与样式表本身，不需要建窗口。
add_executable(test_theme tests/test_theme.cpp app/Theme.cpp app/Glyph.cpp)
target_link_libraries(test_theme PRIVATE lanrdp_core Qt6::Test Qt6::Widgets)
add_test(NAME test_theme COMMAND test_theme)

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

# Phase 20 / BUILD-01：需要 Qt 运行时环境的测试（= 上面 add_test 注册的**全部**）。
# ⚠️ 这个集合必须与 add_test 一一对应。从前靠人手同步，结果漏了
#    `test_clientconnection` —— 它在这台机器上「碰巧」能跑，在干净机器上会因
#    找不到 Qt DLL 而失败。下面用 get_property(TESTS) 做**结构性校验**，
#    以后漏一个就在 configure 阶段直接报错，而不是等到别人机器上才发现。
set(_lanrdp_qt_tests
    test_frame
    test_session
    test_auth
    test_handshake
    test_capture
    test_screenframe
    test_streamer
    test_inputevent
    test_injector
    test_keymap
    test_renderer
    test_inputmap
    test_rendererdrop
    test_h264frame
    test_colorconvert
    test_h264encoder
    test_h264decoder
    test_decoderworker
    test_clientconnection_h264
    test_clientconnection
    test_filetransfer
    test_filereceiver
    test_filesender
    test_filetransfer_e2e
    test_tokengate
    test_hostserver
    test_hostcontroller
    test_clipboard
    test_clipboardsync
    test_mainwindow_ui
    test_logport
    test_sessionstore
    test_cliargs
    test_viewgrid
    test_adaptive
    test_theme
)

get_property(_lanrdp_registered DIRECTORY PROPERTY TESTS)
set(_lanrdp_listed   ${_lanrdp_qt_tests})
set(_lanrdp_actual   ${_lanrdp_registered})
list(SORT _lanrdp_listed)
list(SORT _lanrdp_actual)
if(NOT "${_lanrdp_listed}" STREQUAL "${_lanrdp_actual}")
    message(FATAL_ERROR
        "add_test 注册的测试集合与 set_tests_properties 用的列表不一致 —— "
        "新增测试时请把名字同时加进 _lanrdp_qt_tests，否则它在干净机器上会因找不到 Qt DLL 而失败。\n"
        "  add_test 注册： ${_lanrdp_actual}\n"
        "  列表里写的：   ${_lanrdp_listed}")
endif()

set_tests_properties(
    ${_lanrdp_qt_tests}
    PROPERTIES
    ENVIRONMENT "PATH=${_qt_runtime_dir};$ENV{PATH};QT_PLUGIN_PATH=${_qt_plugin_dir};QT_QPA_PLATFORM=offscreen"
)
