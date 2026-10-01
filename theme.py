# -*- coding: utf-8 -*-
"""
theme.py — 集中设计令牌 / 浅色主题 / 控件样式
=====================================================================
本文件是整个 UI 的【唯一配色与样式来源】。PySide 版统一强制浅色,
不跟随系统深色模式, 保证深浅不混杂。

迁移 C++ / WinUI 3 时, 每个令牌右侧注释给出了对应的 WinUI XAML 画刷,
直接在 App.xaml 建 ResourceDictionary 即可 1:1 复刻:
    <ResourceDictionary x:Key="TufTokens">
      <Color x:Key="AccentColor">#0F6CBD</Color> ...
    </ResourceDictionary>
不要在业务代码里再硬编码颜色, 新增样式一律加到这里。
=====================================================================
"""

# ============ 设计令牌 (与 HTML 设计稿 :root 完全一致) ============
# WinUI 列 = 对应的 XAML ThemeResource / 系统画刷名
TOK = {
    'bg':         '#f3f4f6',   # WinUI ApplicationPageBackgroundThemeBrush (Light)
    'surface':    '#ffffff',   # WinUI CardBackgroundFillColorDefaultBrush
    'surface2':   '#fafafb',   # WinUI ControlFillColorDefault
    'surface3':   '#f0f1f3',   # WinUI SubtleFillColorSecondary
    'line':       '#e4e6e9',   # WinUI CardStrokeColorDefaultBrush
    'line2':      '#d2d5da',   # WinUI ControlElevationBorderBrush
    'txt':        '#1b2126',   # WinUI TextFillColorPrimary
    'txt2':       '#5a646d',   # WinUI TextFillColorSecondary
    'txt3':       '#8a939c',   # WinUI TextFillColorTertiary
    'accent':     '#0f6cbd',   # WinUI AccentFillColorDefaultBrush / SystemAccentColor
    'accent_txt': '#0f6cbd',   # WinUI AccentTextFillColorPrimaryBrush
    'ok':         '#0f7b41',   # WinUI SystemFillColorSuccess
    'warn':       '#b75a00',   # WinUI SystemFillColorCaution
    'danger':     '#c0392b',   # WinUI SystemFillColorCritical / ErrorTextColor
    'accent_soft': 'rgba(15,108,189,0.10)',  # WinUI AccentFillColorSecondary 近似
}

FONT_FAMILY = "'Segoe UI Variable Text','Segoe UI','Noto Sans SC',system-ui,sans-serif"
R_CARD = '10px'     # 卡片圆角
R_CTRL = '7px'      # 控件圆角
R_CHIP = '13px'     # 芯片圆角


# ============ 组件样式 helper (供 _build_ui 直接调用) ============
def card(radius=R_CARD):
    """白色卡片 QFrame"""
    return (f'QFrame {{ background:{TOK["surface"]}; border:1px solid {TOK["line"]};'
            f' border-radius:{radius}; }}')


def section_label():
    """分区小标题 (灰色、字距)"""
    return f'font-size:11.5px; color:{TOK["txt3"]}; letter-spacing:1px;'


def brand():
    return f'font-size:14px; font-weight:600; color:{TOK["txt"]};'


def status_chip(connected=False):
    color = TOK['ok'] if connected else TOK['txt3']
    dot = color
    return (f'font-size:12px; color:{TOK["txt2"]}; padding:3px 11px;'
            f'border:1px solid {TOK["line2"]}; border-radius:{R_CHIP};'
            f' background:{TOK["surface2"]};')


def label_text():
    return f'font-size:12.5px; color:{TOK["txt"]};'


def val_hint(min_w=None):
    mw = f' min-width:{min_w};' if min_w else ''
    return f'font-size:12px; color:{TOK["txt3"]};{mw}'


def accent_hint():
    return f'color:{TOK["accent"]}; font-size:11.5px;'


def preview_frame():
    """预览外框"""
    return (f'QFrame {{ background:{TOK["surface"]}; border:1px solid {TOK["line2"]};'
            f' border-radius:14px; }}')


def preview_canvas():
    """预览画面区"""
    return (f'background:{TOK["surface3"]}; border:1px solid {TOK["line"]};'
            f' border-radius:9px; color:{TOK["txt3"]};')


def switch():
    """屏幕开关 pill (QCheckBox indicator)"""
    return (
        'QCheckBox { spacing:8px; }'
        f'QCheckBox::indicator {{ width:44px; height:22px; border-radius:11px;'
        f' border:1px solid {TOK["line2"]}; background:{TOK["line2"]}; }}'
        f'QCheckBox::indicator:checked {{ background:{TOK["accent"]};'
        f' border-color:{TOK["accent"]}; }}')


def seg_button():
    """分段按钮 (图片/视频/捕获模式)"""
    return (
        f'QPushButton {{ border:1px solid {TOK["line"]}; border-radius:{R_CTRL};'
        f' background:{TOK["surface"]}; color:{TOK["txt2"]}; font-size:12.5px;'
        ' padding:5px 16px; }'
        f'QPushButton:hover {{ background:{TOK["surface3"]}; }}'
        f'QPushButton:checked {{ background:{TOK["accent_soft"]};'
        f' color:{TOK["accent_txt"]}; font-weight:500; border-color:{TOK["accent"]}; }}')


def capture_card():
    """捕获模式卡片 (可选中的大卡)"""
    return (
        f'QPushButton {{ border:1px solid {TOK["line"]}; border-radius:{R_CARD};'
        f' background:{TOK["surface"]}; color:{TOK["txt2"]}; font-size:12.5px;'
        ' padding:14px; text-align:left; }'
        f'QPushButton:hover {{ background:{TOK["surface3"]}; }}'
        f'QPushButton:checked {{ background:{TOK["accent_soft"]};'
        f' color:{TOK["accent_txt"]}; border:1px solid {TOK["accent"]}; }}')


def overlay_card():
    """自定义叠加三卡片"""
    return capture_card()


def media_list():
    """素材网格列表"""
    return (
        f'QListWidget {{ border:none; background:{TOK["surface"]}; outline:0; }}'
        f'QListWidget::item {{ border:1px solid {TOK["line"]}; border-radius:8px;'
        ' margin:4px; padding:3px; }'
        f'QListWidget::item:selected {{ border-color:{TOK["accent"]};'
        f' background:{TOK["accent_soft"]}; }}')


def danger_button():
    return f'color:{TOK["danger"]};'


# ============ 全局 QSS: 基础控件统一浅色 (不跟随系统) ============
def app_qss():
    t = TOK
    return f"""
* {{ font-family:{FONT_FAMILY}; }}
QWidget {{ background:{t['bg']}; color:{t['txt']}; font-size:12.5px; }}
QDialog {{ background:{t['bg']}; }}
QToolTip {{ background:{t['surface']}; color:{t['txt']};
  border:1px solid {t['line2']}; border-radius:5px; padding:3px 6px; }}

/* 菜单栏 + 菜单 */
QMenuBar {{ background:{t['bg']}; border:none; }}
QMenuBar::item {{ background:transparent; padding:5px 10px; border-radius:5px; }}
QMenuBar::item:selected {{ background:{t['surface3']}; }}
QMenu {{ background:{t['surface']}; border:1px solid {t['line2']};
  border-radius:8px; padding:5px; }}
QMenu::item {{ padding:6px 18px; border-radius:5px; }}
QMenu::item:selected {{ background:{t['accent_soft']}; color:{t['accent_txt']}; }}

/* 常规按钮 */
QPushButton {{ border:1px solid {t['line2']}; border-radius:{R_CTRL};
  background:{t['surface2']}; color:{t['txt']}; padding:5px 14px; }}
QPushButton:hover {{ background:{t['surface3']}; }}
QPushButton:pressed {{ background:{t['line']}; }}
QPushButton:disabled {{ color:{t['txt3']}; background:{t['surface3']};
  border-color:{t['line']}; }}

/* 输入框 / 数字框 */
QLineEdit, QSpinBox, QDoubleSpinBox {{
  border:1px solid {t['line2']}; border-radius:{R_CTRL};
  background:{t['surface']}; color:{t['txt']}; padding:3px 8px; }}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {{
  border:1px solid {t['accent']}; }}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {{ width:0; border:none; }}

/* 下拉框 */
QComboBox {{ border:1px solid {t['line2']}; border-radius:{R_CTRL};
  background:{t['surface']}; color:{t['txt']}; padding:3px 8px; min-height:20px; }}
QComboBox:hover {{ border-color:{t['accent']}; }}
QComboBox:focus {{ border-color:{t['accent']}; }}
QComboBox::drop-down {{ border:none; width:20px; }}
QComboBox::down-arrow {{
  image:none; width:0; height:0;
  border-left:4px solid transparent; border-right:4px solid transparent;
  border-top:5px solid {t['txt2']}; margin-right:7px; }}
QComboBox QAbstractItemView {{ background:{t['surface']};
  border:1px solid {t['line2']}; border-radius:6px; outline:0;
  selection-background-color:{t['accent_soft']};
  selection-color:{t['accent_txt']}; }}

/* 分组框 */
QGroupBox {{ border:1px solid {t['line']}; border-radius:{R_CARD};
  margin:12px 0 8px 0; padding:10px; background:{t['surface']};
  font-weight:600; }}
QGroupBox::title {{ subcontrol-origin:margin; left:12px; padding:0 5px;
  color:{t['txt2']}; background:{t['surface']}; }}

/* 滑杆 */
QSlider::groove:Horizontal {{ height:4px; border-radius:2px;
  background:{t['line']}; }}
QSlider::sub-page:Horizontal {{ background:{t['accent']}; border-radius:2px; }}
QSlider::handle:Horizontal {{ width:16px; height:16px; margin:-7px 0;
  border-radius:8px; background:{t['surface']};
  border:1px solid {t['line2']}; }}
QSlider::handle:Horizontal:hover {{ border-color:{t['accent']}; }}

/* 列表/视图 */
QListView, QListWidget {{ background:{t['surface']}; border:1px solid {t['line']};
  border-radius:{R_CARD}; outline:0; }}
QListView::item, QListWidget::item {{ border-radius:6px; }}
QListView::item:selected, QListWidget::item:selected {{
  background:{t['accent_soft']}; color:{t['accent_txt']}; }}

/* 滚动条 */
QScrollBar:Vertical {{ background:transparent; width:11px; margin:2px; }}
QScrollBar::handle:Vertical {{ background:{t['line2']}; border-radius:4px;
  min-height:30px; }}
QScrollBar::handle:Vertical:hover {{ background:{t['txt3']}; }}
QScrollBar::add-line:Vertical, QScrollBar::sub-line:Vertical {{ height:0; }}
QScrollBar:Horizontal {{ background:transparent; height:11px; margin:2px; }}
QScrollBar::handle:Horizontal {{ background:{t['line2']}; border-radius:4px;
  min-width:30px; }}
QScrollBar::handle:Horizontal:hover {{ background:{t['txt3']}; }}
QScrollBar::add-line:Horizontal, QScrollBar::sub-line:Horizontal {{ width:0; }}

/* 进度条 */
QProgressBar {{ border:1px solid {t['line']}; border-radius:5px;
  background:{t['surface3']}; height:8px; text-align:center; color:{t['txt2']}; }}
QProgressBar::chunk {{ background:{t['accent']}; border-radius:4px; }}

/* 复选框 (普通, 非开关) */
QCheckBox {{ color:{t['txt']}; spacing:7px; background:transparent; }}
QCheckBox::indicator {{ width:16px; height:16px; border-radius:4px;
  border:1px solid {t['line2']}; background:{t['surface']}; }}
QCheckBox::indicator:hover {{ border-color:{t['accent']}; }}
QCheckBox::indicator:checked {{ background:{t['accent']};
  border-color:{t['accent']}; }}
"""


def apply_light_titlebar(hwnd):
    """让 Windows 原生标题栏也用浅色 (非客户区不跟随系统深色)。
    WinUI 迁移: 对应 AppWindow.TitleBar 的 ButtonBackgroundColor / 自定义标题栏。"""
    try:
        import ctypes
        val = ctypes.c_int(0)   # 0 = 浅色, 1 = 深色
        # DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            ctypes.c_void_p(int(hwnd)), 20, ctypes.byref(val),
            ctypes.sizeof(val))
    except Exception:
        pass
