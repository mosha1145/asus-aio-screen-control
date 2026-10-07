#pragma once

#include "MainWindow.g.h"
#include "MainWindow.xaml.g.hpp"

namespace winrt::AsusAioScreenControl::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow()
        {
            // 模板 MainWindow.xaml 仅含空 Grid；InitializeComponent 为空实现，
            // 调用它以满足模板实例化并保持与官方模板一致的加载语义。
            InitializeComponent();
        }

        int32_t MyProperty();
        void MyProperty(int32_t value);
    };
}

namespace winrt::AsusAioScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
