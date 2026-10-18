// ============================================================================
//  XamlGeneratedIncludes.cpp
// ----------------------------------------------------------------------------
//  作用：把 XAML 编译器生成的两个 .g.hpp 实现单元编译进本工程。
//
//  为什么需要这个文件：
//  命令行 MSBuild 下，XAML 代码生成会产出
//    $(IntDir)\App.xaml.g.hpp        -> AppT::InitializeComponent 实现 + wWinMain 入口
//    $(IntDir)\MainWindow.xaml.g.hpp -> MainWindowT::InitializeComponent / Connect /
//                                       GetBindingConnector 实现
//  但这两个 .hpp 不会自动加入 ClCompile，导致链接期报未解析的外部符号
//  （InitializeComponent / Connect / GetBindingConnector / WinMain）。
//
//  这里用一个独立编译单元把它们包含进来（独立 TU 避免与 MainWindow.cpp
//  等文件重复定义）。$(IntDir) 已在 IncludePath 中。
// ============================================================================

#include "pch.h"

#if __has_include("App.xaml.g.hpp")
#include "App.xaml.g.hpp"
#endif

#if __has_include("MainWindow.xaml.g.hpp")
#include "MainWindow.xaml.g.hpp"
#endif
