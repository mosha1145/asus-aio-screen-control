#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tuf_gui.py — TUF 水冷屏控制 GUI (PySide6)
==========================================
功能:
  - 照片库 / 视频库（工具自带素材库目录 assets/photos, assets/videos）
  - 素材添加自动处理（长边320；1:1 压成 320x320）后入库
  - 显示适配模式: 适应(contain) / 填充(cover) / 拉伸(stretch)
  - 预览与水冷屏画面同步
  - 亮度 / 屏幕开关（配置自动记忆，下次启动恢复）
  - 控制命令不带亮度（恢复用配置亮度）
  - 图片帧率 debug: 可调低帧率测最低保持刷新率, 实时显示实际 fps
  - 视频质量档: 低/中/高（控制 JPEG 块数）
  - 素材实时增删

运行:  python tuf_gui.py
"""
import ctypes
import os
import re
import sys
import time
import winreg

os.environ.setdefault('QT_AUTO_SCREEN_SCALE_FACTOR', '1')

from PySide6.QtCore import Qt, QThread, Signal, QSize, QTimer, QMimeData
from PySide6.QtGui import QImage, QPixmap, QIcon, QAction
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
    QLabel, QPushButton, QSlider, QComboBox, QTabWidget, QListWidget,
    QListWidgetItem, QFileDialog, QMessageBox, QCheckBox,
    QFrame, QProgressBar, QSpinBox, QDoubleSpinBox, QGroupBox, QListView,
    QInputDialog, QMenu, QMenuBar,
    QLineEdit, QColorDialog, QDialog, QDialogButtonBox, QButtonGroup,
    QRadioButton, QStackedWidget,
)

# 照片发送质量: 接近无损 (视频仍走质量档)
PHOTO_JPEG_Q = 95

import cv2
import numpy as np

import tuf_hid
import asset_lib
import overlay
import theme
from config_store import ConfigStore

APP_DIR = os.path.dirname(os.path.abspath(__file__))
ASSET_DIR = os.path.join(APP_DIR, 'assets')
PHOTOS_DIR = os.path.join(ASSET_DIR, 'photos')
VIDEOS_DIR = os.path.join(ASSET_DIR, 'videos')
CONFIG_PATH = os.path.join(APP_DIR, 'config.json')


def _media_path_to_stored(p):
    """媒体路径 -> 配置存储值: 项目内(素材库)存相对 APP_DIR 的相对路径,
    整个软件目录移动后配置依然有效; 项目外的外部文件保留绝对路径"""
    if not p:
        return None
    if os.path.isabs(p):
        try:
            rel = os.path.relpath(p, APP_DIR)
            if not rel.startswith('..'):
                return rel.replace('\\', '/')
        except Exception:
            pass
    return p


def _media_path_from_stored(p):
    """配置存储值 -> 绝对路径: 相对路径拼回项目根, 旧配置的绝对路径原样返回"""
    if not p:
        return None
    if os.path.isabs(p):
        return p
    return os.path.normpath(os.path.join(APP_DIR, p))

FIT_LABELS = {'contain': '适应 · contain',
              'cover': '填充 · cover',
              'stretch': '拉伸 · stretch'}
QUALITY_LABELS = {'low': '低', 'medium': '中', 'high': '高'}



# ================= 图片流线程 =================
class PreviewLabel(QLabel):
    """预览标签: 接受硬件信息项拖放; 已放置项可在画面内拖动改位置"""
    dropped = Signal(str, float, float)
    drag_start = Signal(float, float)   # 按下 (x_frac, y_frac)
    drag_move = Signal(float, float)    # 拖动中
    drag_end = Signal()

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.setAcceptDrops(True)
        self._drag_idx = -1

    def dragEnterEvent(self, e):
        if e.mimeData().hasText():
            e.acceptProposedAction()

    def dropEvent(self, e):
        key = e.mimeData().text()
        pos = e.position()
        w = self.width() or 1
        h = self.height() or 1
        self.dropped.emit(key, pos.x() / w, pos.y() / h)
        e.acceptProposedAction()

    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._drag_idx = 0
            w = self.width() or 1
            h = self.height() or 1
            self.drag_start.emit(e.position().x() / w, e.position().y() / h)
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
        super().mousePressEvent(e)

    def mouseMoveEvent(self, e):
        if self._drag_idx >= 0:
            w = self.width() or 1
            h = self.height() or 1
            self.drag_move.emit(e.position().x() / w, e.position().y() / h)
        super().mouseMoveEvent(e)

    def mouseReleaseEvent(self, e):
        if self._drag_idx >= 0:
            self._drag_idx = -1
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self.drag_end.emit()
        super().mouseReleaseEvent(e)


class ScreenCaptureThread(QThread):
    """屏幕实时投射: 定时截屏 -> 适配 -> 叠加 -> 旋转 -> JPEG -> USB
    与视频同一套前馈画质控制: 目标 60fps, 跑不满时自动降块数, 有余量时升块数"""
    preview = Signal(object)
    fps_report = Signal(float)
    blocks_report = Signal(int)
    error = Signal(str)

    def __init__(self, cooler, get_mode, get_rotation, get_overlay, get_quality,
                 get_screen, parent=None, monitor=0, region=None):
        super().__init__(parent)
        self._cooler = cooler
        self._get_mode = get_mode
        self._get_rotation = get_rotation
        self._get_overlay = get_overlay
        self._get_quality = get_quality
        self._get_screen = get_screen
        self._fps = 60.0
        self._cur_q = 60
        self._ema_ms = 0.0
        self._q_cooldown = 0
        self._last_mode = None
        self._monitor = int(monitor or 0)
        self._region = region
        self._need_restart = False
        self._region_lock = False

    def set_region(self, mon_region):
        # 动态切换显示器/裁剪区域: 下一帧重启 dxcam 生效
        mon, region = mon_region
        if (int(mon or 0) != self._monitor) or (region != self._region):
            self._monitor = int(mon or 0)
            self._region = region
            self._need_restart = True

    def set_fps(self, f):
        self._fps = max(1.0, min(60.0, float(f)))

    def _adjust_quality(self, frame_ms):
        mode = self._get_quality()
        if mode != self._last_mode:
            self._last_mode = mode
            self._cur_q = asset_lib.QUALITY_TARGETS.get(
                mode, asset_lib.QUALITY_TARGETS['medium'])['q0']
            self._ema_ms = 0.0
            self._q_cooldown = 0
            return
        t = asset_lib.QUALITY_TARGETS.get(mode, asset_lib.QUALITY_TARGETS['medium'])
        target_fps = t['fps'] if t['fps'] else 60.0
        target_period = 1000.0 / target_fps
        q = self._cur_q
        if self._q_cooldown > 0:
            self._q_cooldown -= 1
        else:
            if frame_ms > target_period * 1.05:
                q = max(t['qmin'], q - 8)
                self._q_cooldown = 10
            elif frame_ms < target_period * 0.72:
                q = min(t['qmax'], q + 3)
        self._cur_q = q

    def _make_cam(self):
        import dxcam
        cam = dxcam.create(output_idx=self._monitor, output_color='BGR')
        if cam is None:
            return None
        region = None
        if isinstance(self._region, str) and self._region == 'square_center':
            info = dxcam.output_info(self._monitor)
            w = info['resolution'][0]; h = info['resolution'][1]
            side = min(w, h)
            x0 = (w - side) // 2; y0 = (h - side) // 2
            region = (x0, y0, x0 + side, y0 + side)
        elif isinstance(self._region, (tuple, list)) and len(self._region) == 4:
            x, y, w, h = self._region
            region = (x, y, x + w, y + h)
        cam.start(target_fps=int(self._fps), video_mode=True, region=region)
        return cam

    def run(self):
        cam = self._make_cam()
        if cam is None:
            self.error.emit('dxcam 初始化失败（显卡不支持 DXGI 或被独占）')
            return
        period = 1.0 / self._fps
        count = 0
        t0 = time.time()
        try:
            while not self.isInterruptionRequested():
                if self._need_restart:
                    self._need_restart = False
                    try:
                        cam.stop()
                    except Exception:
                        pass
                    cam = self._make_cam()
                    if cam is None:
                        self.error.emit('dxcam 切换显示器失败')
                        return
                    continue
                loop_t0 = time.time()
                arr = cam.get_latest_frame()   # BGR ndarray, video_mode 持续输出
                if arr is None or arr.size == 0:
                    time.sleep(0.005)
                    continue
                fitted = asset_lib.fit_frame(arr, self._get_mode())
                ov = self._get_overlay()
                if ov:
                    _hub, _pl, _cfg = ov
                    fitted = overlay.draw_overlay(fitted, _hub, _pl, _cfg)
                self.preview.emit(fitted)
                if self._get_screen():
                    send_frame = asset_lib.rotate_frame(fitted, self._get_rotation())
                    jpg = asset_lib.bgr_to_jpeg(send_frame, self._cur_q)
                    n, ok = self._cooler.send_jpeg_frame(jpg, block_delay=0.0)
                    if not ok:
                        self.error.emit('投射帧发送失败（设备未连接？）')
                        return
                    count += 1
                    self.blocks_report.emit(n)
                    dt = time.time() - loop_t0
                    if self._ema_ms <= 0:
                        self._ema_ms = dt * 1000.0
                    else:
                        self._ema_ms = 0.7 * self._ema_ms + 0.3 * dt * 1000.0
                    if count % 15 == 0:
                        elapsed = time.time() - t0
                        if elapsed > 0.5:
                            self.fps_report.emit(count / elapsed)
                            self._adjust_quality(self._ema_ms)
                            t0 = time.time()
                            count = 0
                dt = time.time() - loop_t0
                if dt < period:
                    time.sleep(period - dt)
        except Exception:
            import traceback
            traceback.print_exc()
        finally:
            try:
                cam.stop()
            except Exception:
                pass
            del cam


def _smtc_placeholder(title=''):
    """SMTC 无封面/无媒体时的占位帧 320x320 BGR"""
    frame = np.full((320, 320, 3), 16, np.uint8)
    cv2.putText(frame, 'NOW PLAYING', (30, 130),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (140, 190, 230), 1, cv2.LINE_AA)
    text = (title or 'NO MEDIA')[:22]
    cv2.putText(frame, text, (30, 170),
                cv2.FONT_HERSHEY_SIMPLEX, 0.75, (255, 255, 255), 1, cv2.LINE_AA)
    cv2.putText(frame, 'Windows SMTC', (30, 205),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (140, 150, 160), 1, cv2.LINE_AA)
    return frame


# 播放器类应用关键字 (SMTC 多媒体同时播放时优先捕获; 越靠前优先级越高)
PLAYER_HINTS = ('music', 'video', 'player', '网易云', 'netease', 'cloudmusic',
                'qq音乐', 'qqmusic', '酷狗', 'kugou', '酷我', 'kuwo', 'bilibili',
                'bili', 'youtube', 'spotify', 'itunes', 'foobar', 'potplayer',
                'vlc', 'mpv', 'windows media')


def _smtc_app_score(app_id):
    """播放器类应用打分(优先捕获); 返回 0=非播放器 到高分"""
    app = (app_id or '').lower()
    if not app:
        return 0
    score = 0
    for i, kw in enumerate(PLAYER_HINTS):
        if kw in app:
            score = max(score, 100 - i)
    return score


async def _fetch_smtc_media(smtc_cfg=None):
    """异步读取 Windows SMTC 媒体 (title, artist, thumb_bytes)
    支持多会话枚举 + 白名单/黑名单过滤 + 播放器优先
    smtc_cfg = {'mode': all|whitelist|blacklist, 'apps': [...]}"""
    import winsdk.windows.media.control as wmc
    import winsdk.windows.storage.streams as wss
    smtc_cfg = smtc_cfg or {}
    mode = smtc_cfg.get('mode', 'all')
    apps = [str(a).strip().lower() for a in smtc_cfg.get('apps', []) if str(a).strip()]

    def allowed(app_id):
        a = (app_id or '').lower()
        if mode == 'whitelist':
            return any(k in a for k in apps) if apps else True
        if mode == 'blacklist':
            return not (any(k in a for k in apps) if apps else False)
        return True

    mgr = await wmc.GlobalSystemMediaTransportControlsSessionManager.request_async()
    sessions = list(mgr.get_sessions()) if hasattr(mgr, 'get_sessions') else None
    if not sessions:
        sess = mgr.get_current_session()
        sessions = [sess] if sess else []
    # 过滤 + 播放器优先排序
    cand = [se for se in sessions if allowed(getattr(se, 'source_app_user_model_id', ''))]
    cand.sort(key=lambda se: _smtc_app_score(
        getattr(se, 'source_app_user_model_id', '')), reverse=True)
    for sess in cand:
        try:
            props = await sess.try_get_media_properties_async()
        except Exception:
            continue
        if props is None:
            continue
        title = str(props.title) if props.title else ''
        artist = str(props.artist) if props.artist else ''
        thumb = None
        try:
            if props.thumbnail is not None:
                stream = await props.thumbnail.open_read_async()
                reader = wss.DataReader(stream)
                await reader.load_async(stream.size)
                buf = bytearray(stream.size)
                reader.read_bytes(buf)
                thumb = bytes(buf)
        except Exception:
            thumb = None
        if title or artist or thumb:
            return title, artist, thumb
    return None, None, None


class SMTCCaptureThread(QThread):
    """媒体封面捕获: 轮询 Windows SMTC, 有媒体则发封面+标题, 无媒体发占位"""
    preview = Signal(object)
    error = Signal(str)

    def __init__(self, cooler, get_mode, get_rotation, get_overlay, get_screen, parent=None,
                 smtc_cfg=None):
        super().__init__(parent)
        self._cooler = cooler
        self._get_mode = get_mode
        self._get_rotation = get_rotation
        self._get_overlay = get_overlay
        self._get_screen = get_screen
        self._smtc_cfg = smtc_cfg or {}

    def run(self):
        import asyncio
        try:
            import winsdk  # noqa: F401
        except ImportError:
            self.error.emit('SMTC 需要 winsdk 库: pip install winsdk')
            return
        # 纯黑占位帧: SMTC 不可用/无媒体时维持黑屏, 不回自带动画
        black = np.zeros((320, 320, 3), dtype=np.uint8)
        _notified = False
        _last_key = None    # 已显示封面标识 (thumb 字节 or '__black__')
        _last_jpg = None    # 已显示帧的 JPEG, 用于未变化时的心跳维持
        _last_sig = None    # 显示参数签名(适配+旋转+覆盖层), 变化时即使封面未变也要重渲染
        import json
        while not self.isInterruptionRequested():
            frame = None
            key = None
            try:
                # wait_for 超时保护: winsdk 查询偶发卡死时最多阻塞2s, 保证线程可中断退出
                title, artist, thumb = asyncio.run(
                    asyncio.wait_for(_fetch_smtc_media(self._smtc_cfg), timeout=2.0))
                if thumb:
                    img = cv2.imdecode(np.frombuffer(thumb, np.uint8), cv2.IMREAD_COLOR)
                    if img is not None:
                        frame = img
                        key = thumb        # 封面字节作变化标识
                    else:
                        frame = black; key = b'__black__'
                else:
                    # 无媒体播放 -> 黑屏
                    frame = black; key = b'__black__'
            except Exception as e:
                # SMTC 读取异常 -> 黑屏 (仅提示一次, 避免刷屏)
                if not _notified:
                    self.error.emit(f'SMTC 不可用, 显示黑屏: {e}')
                    _notified = True
                frame = black; key = b'__black__'

            # 显示参数签名: 适配模式/旋转/覆盖层(类型+时钟样式+时钟样式参数+自定义文本)任一变化,
            # 即便封面未变也要重新叠加渲染, 保证"黑屏时编辑时钟/文本/选无不消失"实时生效
            ov = self._get_overlay()
            try:
                ov_sig = json.dumps(ov[2] if ov else None, sort_keys=True, ensure_ascii=False,
                                    default=str)
            except Exception:
                ov_sig = ''
            sig = f'{self._get_mode()}|{self._get_rotation()}|{ov_sig}'
            changed = (key != _last_key) or (sig != _last_sig)
            _last_key = key
            _last_sig = sig

            if changed:
                # 封面/显示参数变化: 重新适配叠加 -> 刷新预览 -> (屏幕开则)重编码发送
                fitted = asset_lib.fit_frame(frame, self._get_mode())
                if ov:
                    _hub, _pl, _cfg = ov
                    fitted = overlay.draw_overlay(fitted, _hub, _pl, _cfg)
                self.preview.emit(fitted)
                if self._get_screen():
                    send_frame = asset_lib.rotate_frame(fitted, self._get_rotation())
                    _last_jpg = asset_lib.bgr_to_jpeg(send_frame, PHOTO_JPEG_Q)
                    n, ok = self._cooler.send_jpeg_frame(_last_jpg, block_delay=0.0)
                    if not ok:
                        self.error.emit('SMTC 帧发送失败（设备未连接？）')
                        return
            else:
                # 封面未变: 仅重发已显示帧做心跳, 防固件约3秒超时回自带动画
                if self._get_screen() and _last_jpg is not None:
                    n, ok = self._cooler.send_jpeg_frame(_last_jpg, block_delay=0.0)
                    if not ok:
                        self.error.emit('SMTC 心跳发送失败（设备未连接？）')
                        return
            time.sleep(2.0)


class PhotoStreamThread(QThread):
    preview = Signal(object)   # BGR ndarray 320x320
    fps_report = Signal(float) # 实际发送帧率
    blocks_report = Signal(int)
    error = Signal(str)

    def __init__(self, cooler, photo_path, get_mode, get_rotation, get_overlay, get_fps,
                 get_screen, parent=None):
        super().__init__(parent)
        self._cooler = cooler
        self._photo_path = photo_path
        self._get_mode = get_mode
        self._get_rotation = get_rotation
        self._get_overlay = get_overlay
        self._get_fps = get_fps
        self._get_screen = get_screen

    def run(self):
        try:
            img = asset_lib.imread_unicode(self._photo_path)
            if img is None:
                self.error.emit(f'读取照片失败: {self._photo_path}')
                return
            count = 0
            t0 = time.time()
            while not self.isInterruptionRequested():
                loop_t0 = time.time()
                fitted = asset_lib.fit_frame(img, self._get_mode())
                ov = self._get_overlay()
                if ov:
                    _hub, _pl, _cfg = ov
                    fitted = overlay.draw_overlay(fitted, _hub, _pl, _cfg)
                # 预览: 不旋转 + 文本正读; 水冷: 整体旋转(画面+文本)后发送
                self.preview.emit(fitted)
                if self._get_screen():
                    # 屏幕开: 正常发送
                    send_frame = asset_lib.rotate_frame(fitted, self._get_rotation())
                    jpg = asset_lib.bgr_to_jpeg(send_frame, PHOTO_JPEG_Q)
                    n, ok = self._cooler.send_jpeg_frame(jpg, block_delay=0.001)
                    if not ok:
                        self.error.emit('图片帧发送失败（设备未连接？）')
                        return
                    count += 1
                    self.blocks_report.emit(n)
                    # 实际 fps 统计
                    if count % 5 == 0:
                        elapsed = time.time() - t0
                        if elapsed > 0.5:
                            self.fps_report.emit(count / elapsed)
                            t0 = time.time()
                            count = 0
                else:
                    # 屏幕关: 预览继续, USB 不再传任何数据
                    pass
                # 按目标帧率节流（可调低测最低刷新率）
                fps = max(0.1, float(self._get_fps()))
                remain = 1.0 / fps - (time.time() - loop_t0)
                if remain > 0:
                    time.sleep(remain)
        except Exception as e:
            self.error.emit(f'图片流异常: {e}')


# ================= 视频流线程 =================
class VideoStreamThread(QThread):
    preview = Signal(object)
    fps_report = Signal(float)
    blocks_report = Signal(int)
    info = Signal(str)
    error = Signal(str)

    video_finished = Signal()   # 列表循环模式: 播完一遍时通知 GUI 切下一个

    def __init__(self, cooler, video_path, get_mode, get_rotation, get_overlay, get_quality,
                 get_screen, parent=None, notify_finish=False):
        super().__init__(parent)
        self._cooler = cooler
        self._video_path = video_path
        self._get_mode = get_mode
        self._get_rotation = get_rotation
        self._get_overlay = get_overlay
        self._get_quality = get_quality
        self._get_screen = get_screen
        self._notify_finish = notify_finish
        self._finished_emitted = False
        self._cur_q = 60   # 动态 JPEG 质量(块数), 按实测帧率实时调节
        self._ema_ms = 0.0     # 单帧总耗时(读帧+处理+编码+发送) EMA
        self._q_cooldown = 0   # 降质后冷却帧数, 防振荡
        self._last_mode = None # 记录上次档位, 切档时重置

    def set_notify_finish(self, v):
        """热切换循环方式: 不重启线程, 画面不闪位置不跳 (list=播完通知切下一个)"""
        self._notify_finish = bool(v)
        self._finished_emitted = False

    def _adjust_quality(self, frame_ms, src_fps):
        # 前馈控制: 按单帧总耗时 EMA 调节 JPEG 质量(块数)
        #   frame_ms > 目标周期x1.05  -> 帧耗时超预算, 立即降质(减块数缩短传输)
        #   frame_ms < 目标周期x0.72  -> 明显有余量, 才升质(加块数提画质)
        # 这样输出帧率稳定在目标附近小幅波动, 不会大起大落
        mode = self._get_quality()
        # 切档时立即重置为该档起始质量, 并清空平滑/冷却状态
        if mode != self._last_mode:
            self._last_mode = mode
            self._cur_q = asset_lib.QUALITY_TARGETS.get(mode, asset_lib.QUALITY_TARGETS['medium'])['q0']
            self._ema_ms = 0.0
            self._q_cooldown = 0
            return
        t = asset_lib.QUALITY_TARGETS.get(mode, asset_lib.QUALITY_TARGETS['medium'])
        target_fps = t['fps'] if t['fps'] else min(60, src_fps or 60)
        target_period = 1000.0 / target_fps
        q = self._cur_q
        if self._q_cooldown > 0:
            self._q_cooldown -= 1
        else:
            if frame_ms > target_period * 1.05:
                # 超预算: 快速降质保帧率 (高画质档也同力度, 直到能稳住目标帧率)
                q = max(t['qmin'], q - 8)
                self._q_cooldown = 10
            elif frame_ms < target_period * 0.72:
                # 明显有余量: 慢慢升质
                q = min(t['qmax'], q + 3)
        self._cur_q = q

    def run(self):
        try:
            cap = cv2.VideoCapture(self._video_path)
            if not cap.isOpened():
                self.error.emit(f'无法打开视频: {self._video_path}')
                return
            try:
                src_fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
                w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
                h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
                self.info.emit(f'视频 {w}x{h} @ {src_fps:.1f}fps')
                if src_fps <= 0:
                    src_fps = 30.0
                period = 1.0 / src_fps
                timeline = time.time()   # 播放时间轴（真实速度基准）
                count = 0
                t0 = time.time()
                skipped = 0
                while not self.isInterruptionRequested():
                    # 追赶/丢帧: 处理发送慢导致落后时, 跳过帧维持播放速度
                    now = time.time()
                    while timeline + 2.0 * period < now:
                        ok_skip, _ = cap.read()
                        if not ok_skip:      # 到片尾
                            if self._notify_finish and not self._finished_emitted:
                                self._finished_emitted = True
                                self.video_finished.emit()
                                return
                            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                            timeline = time.time()
                            count = 0
                            t0 = time.time()
                            break
                        timeline += period
                        skipped += 1
                        now = time.time()

                    loop_t0 = time.time()
                    ok, frame = cap.read()
                    if not ok:
                        if self._notify_finish and not self._finished_emitted:
                            self._finished_emitted = True
                            self.video_finished.emit()
                            return
                        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                        timeline = time.time()
                        count = 0
                        t0 = time.time()
                        continue

                    quality = self._cur_q
                    fitted = asset_lib.fit_frame(frame, self._get_mode())
                    ov = self._get_overlay()
                    if ov:
                        _hub, _pl, _cfg = ov
                        fitted = overlay.draw_overlay(fitted, _hub, _pl, _cfg)
                    # 预览: 不旋转 + 文本正读; 水冷: 整体旋转(画面+文本)后发送
                    self.preview.emit(fitted)
                    if self._get_screen():
                        # 屏幕开: 正常发送
                        send_frame = asset_lib.rotate_frame(fitted, self._get_rotation())
                        jpg = asset_lib.bgr_to_jpeg(send_frame, quality)
                        n, ok = self._cooler.send_jpeg_frame(jpg, block_delay=0.0)
                        if not ok:
                            self.error.emit('视频帧发送失败（设备未连接？）')
                            return
                        count += 1
                        self.blocks_report.emit(n)
                        # 帧耗时(ms): 本帧 读帧+适配+叠加+旋转+编码+发送 总耗时
                        dt = time.time() - loop_t0
                        if self._ema_ms <= 0:
                            self._ema_ms = dt * 1000.0
                        else:
                            self._ema_ms = 0.7 * self._ema_ms + 0.3 * dt * 1000.0
                        if count % 15 == 0 or (time.time() - t0) > 0.6:
                            elapsed = time.time() - t0
                            if elapsed > 0.5:
                                measured = count / elapsed
                                self.fps_report.emit(measured)
                                # 前馈: 用帧耗时 EMA 调质量, 帧率自然稳定在目标附近
                                self._adjust_quality(self._ema_ms, src_fps)
                                t0 = time.time()
                                count = 0
                    else:
                        # 屏幕关: 预览继续, USB 不再传任何数据
                        pass

                    timeline += period
                    remain = timeline - time.time()
                    if remain > 0:
                        time.sleep(remain)
                    elif skipped:
                        skipped = 0
            finally:
                cap.release()
        except Exception as e:
            self.error.emit(f'视频流异常: {e}')


# ================= 素材添加线程 =================
class AddAssetThread(QThread):
    finished_ok = Signal(str, list)  # kind, [dst_path, ...]
    failed = Signal(str, str)        # kind, errmsg
    progress = Signal(int, int)      # 总进度(分母1000): 批量文件级 + 当前文件内部转码进度

    def __init__(self, lib, kind, src_paths, parent=None):
        super().__init__(parent)
        self._lib = lib
        self._kind = kind
        self._srcs = list(src_paths)

    def run(self):
        dsts = []
        total = len(self._srcs)
        for i, src in enumerate(self._srcs):
            try:
                if self._kind == 'photo':
                    dst = self._lib.add_photo(src)
                    self.progress.emit(i + 1, total)
                else:
                    # 视频/GIF: ffmpeg 短边320 H.264 高质量入库 (ffmpeg 缺失时直接报错)
                    # 内部转码进度按文件权重映射到总进度, 单个大视频转码时进度条实时走动
                    def _inner(c, t):
                        if t > 0:
                            cur = int(i * 1000 / total) + int(c / t * 1000 / total)
                            self.progress.emit(min(cur, int((i + 1) * 1000 / total)), 1000)
                    dst = self._lib.add_video(src, on_progress=_inner)
                    self.progress.emit(i + 1, total)
            except Exception as e:
                self.failed.emit(self._kind, f'{os.path.basename(src)}: {e}')
                return
            dsts.append(dst)
        self.finished_ok.emit(self._kind, dsts)


# ================= 设置对话框 =================
class SettingsDialog(QDialog):
    """设置: 常规(开机自启) / 电源操作 / 高级选项(照片发送频率)"""
    def __init__(self, cfg, parent=None):
        super().__init__(parent)
        self.setWindowTitle('设置')
        self.setMinimumWidth(430)
        self._cfg = cfg
        layout = QVBoxLayout(self)
        layout.setSpacing(10)

        # 常规
        gb1 = QGroupBox('常规')
        g1 = QVBoxLayout(gb1)
        self.chk_autostart = QCheckBox('开机自启 (登录 Windows 时自动运行本工具)')
        self.chk_autostart.setChecked(bool(cfg.get('auto_start', False)))
        g1.addWidget(self.chk_autostart)
        layout.addWidget(gb1)

        # 电源操作
        gb2 = QGroupBox('电源操作')
        g2 = QVBoxLayout(gb2)
        self.chk_power_main = QCheckBox('发送电源操作时执行 (勾选后以下行为生效)')
        self.chk_power_main.setChecked(bool(cfg.get('power_actions', True)))
        g2.addWidget(self.chk_power_main)
        self._power_checks = []
        for key, label in (
                ('power_close_app', '关闭程序时关闭水冷屏幕'),
                ('power_shutdown', '关闭电脑时关闭水冷屏幕'),
                ('power_logout', '注销时关闭水冷屏幕'),
                ('power_sleep', '睡眠时关闭水冷屏幕')):
            c = QCheckBox(label)
            c.setChecked(bool(cfg.get(key, False)))
            g2.addWidget(c)
            self._power_checks.append((key, c))
        self.chk_power_main.toggled.connect(self._update_power_enabled)
        self._update_power_enabled()
        layout.addWidget(gb2)

        # 高级选项
        gb3 = QGroupBox('高级选项')
        g3 = QVBoxLayout(gb3)
        row = QHBoxLayout()
        row.addWidget(QLabel('照片发送频率:'))
        self.spin_fps = QDoubleSpinBox()
        self.spin_fps.setRange(0.1, 30.0)
        self.spin_fps.setSingleStep(0.1)
        self.spin_fps.setDecimals(1)
        self.spin_fps.setValue(float(cfg.get('photo_fps', 10.0)))
        row.addWidget(self.spin_fps)
        row.addWidget(QLabel('Hz'))
        row.addStretch(1)
        g3.addLayout(row)
        hint = QLabel('发送频率越低, 固件超时回自带动画的风险越高; 推荐 5-30Hz')
        hint.setStyleSheet(f'font-size:11px; color:{theme.TOK["txt3"]};')
        g3.addWidget(hint)
        g3.addSpacing(6)
        r2 = QHBoxLayout()
        r2.addWidget(QLabel('SMTC 来源过滤:'))
        self.combo_smtc_mode = QComboBox()
        self.combo_smtc_mode.addItem('全部来源', 'all')
        self.combo_smtc_mode.addItem('仅白名单', 'whitelist')
        self.combo_smtc_mode.addItem('排除黑名单', 'blacklist')
        _m = cfg.get('smtc_filter_mode', 'all')
        self.combo_smtc_mode.setCurrentIndex(
            ['all', 'whitelist', 'blacklist'].index(_m) if _m in ('all', 'whitelist', 'blacklist') else 0)
        r2.addWidget(self.combo_smtc_mode)
        r2.addStretch(1)
        g3.addLayout(r2)
        r3 = QHBoxLayout()
        r3.addWidget(QLabel('应用名(逗号分隔):'))
        self.edit_smtc_apps = QLineEdit(', '.join(cfg.get('smtc_filter_apps', [])))
        r3.addWidget(self.edit_smtc_apps, 1)
        g3.addLayout(r3)
        hint2 = QLabel('多个媒体同时播放时优先捕获播放器类(music/video/player/网易云等)。\n白名单=仅列出的应用, 黑名单=排除列出的应用, 支持关键字匹配。')
        hint2.setStyleSheet(f'font-size:11px; color:{theme.TOK["txt3"]};')
        hint2.setWordWrap(True)
        g3.addWidget(hint2)
        layout.addWidget(gb3)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                QDialogButtonBox.StandardButton.Cancel)
        btns.accepted.connect(self.accept)
        btns.rejected.connect(self.reject)
        layout.addWidget(btns)

    def _update_power_enabled(self):
        en = self.chk_power_main.isChecked()
        for _, c in self._power_checks:
            c.setEnabled(en)

    def result_values(self):
        return {
            'auto_start': self.chk_autostart.isChecked(),
            'power_main': self.chk_power_main.isChecked(),
            'powers': [(k, c.isChecked()) for k, c in self._power_checks],
            'photo_fps': self.spin_fps.value(),
            'smtc_mode': self.combo_smtc_mode.currentData(),
            'smtc_apps': [a.strip() for a in self.edit_smtc_apps.text().split(',') if a.strip()],
        }


# ================= 主窗口 =================
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('TUF 水冷屏控制')
        self.resize(1320, 800)

        self.config = ConfigStore(CONFIG_PATH)
        self.lib = asset_lib.AssetLib(PHOTOS_DIR, VIDEOS_DIR)

        self.cooler = None
        self.photo_thread = None
        self.video_thread = None
        self.capture_thread = None
        self.add_thread = None
        self.current_kind = None  # 'photo' | 'video' | 'screen' | 'smtc' | None

        # 自定义文本覆盖层配置恢复
        self._custom_texts = []
        for it in self.config.get('custom_texts', []):
            self._custom_texts.append({
                'text': it.get('text', ''), 'x': float(it.get('x', 0.5)),
                'y': float(it.get('y', 0.5)),
                'color': tuple(it.get('color', [255, 255, 255])),
                'bg': tuple(it.get('bg', [0, 0, 0])),
                'alpha': float(it.get('alpha', 0.6)),
                'scale': float(it.get('scale', 0.6)),
                'thick': int(it.get('thick', 1)),
            })
        # 时钟纯文本样式 (text='__time__' 实时时间, 与自定义文本同逻辑可编辑样式/位置)
        _ct = self.config.get('clock_text') or {}
        self._clock_text = {
            'text': '__time__', 'x': float(_ct.get('x', 0.85)), 'y': float(_ct.get('y', 0.12)),
            'color': tuple(_ct.get('color', [255, 255, 255])),
            'bg': tuple(_ct.get('bg', [0, 0, 0])),
            'alpha': float(_ct.get('alpha', 0.55)),
            'scale': float(_ct.get('scale', 0.7)), 'thick': int(_ct.get('thick', 2)),
        }
        self._text_edit_target = None  # 当前编辑的文本条目 (clock_text 或 custom 某行)

        self.resize(1320, 800)
        self._build_ui()
        self._build_menu()
        self._connect_signals()
        self._refresh_lists()
        self._populate_monitors()

        # 打开设备 + 恢复配置
        self._init_device()

    # ---------- UI ----------
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # ===== 顶栏 =====
        titlebar = QWidget()
        tb = QHBoxLayout(titlebar)
        tb.setContentsMargins(14, 8, 14, 8)
        tb.setSpacing(10)
        brand = QLabel('TUF 水冷屏控制')
        brand.setStyleSheet(theme.brand())
        tb.addWidget(brand)
        tb.addStretch(1)
        self.lbl_status = QLabel('● 设备: 未连接')
        self.lbl_status.setStyleSheet(theme.status_chip(False))
        tb.addWidget(self.lbl_status)
        self.btn_settings = QPushButton('设置…')
        self.btn_settings.setFixedHeight(28)
        tb.addWidget(self.btn_settings)
        root.addWidget(titlebar)

        # ===== 主区 =====
        main = QWidget()
        ml = QHBoxLayout(main)
        ml.setContentsMargins(14, 4, 14, 14)
        ml.setSpacing(14)

        # ---------- 左: 预览 + 控制卡 ----------
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(10)

        sec_l = QLabel('实时预览')
        sec_l.setStyleSheet(theme.section_label())
        left_layout.addWidget(sec_l)

        # 预览帧
        self.dev_frame = QFrame()
        self.dev_frame.setFixedSize(330, 330)
        self.dev_frame.setStyleSheet(theme.preview_frame())
        dv = QVBoxLayout(self.dev_frame)
        dv.setContentsMargins(6, 6, 6, 6)
        self.lbl_preview = PreviewLabel('未在显示')
        self.lbl_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_preview.setFixedSize(316, 316)
        self.lbl_preview.setStyleSheet(theme.preview_canvas())
        dv.addWidget(self.lbl_preview, 0, Qt.AlignmentFlag.AlignCenter)
        left_layout.addWidget(self.dev_frame, 0, Qt.AlignmentFlag.AlignHCenter)

        # 控制卡
        ctrl = QFrame()
        ctrl.setStyleSheet(theme.card())
        cl = QGridLayout(ctrl)
        cl.setContentsMargins(14, 6, 14, 6)
        cl.setVerticalSpacing(2)
        cl.setColumnStretch(1, 1)
        cl.setHorizontalSpacing(10)

        k_style = theme.label_text()
        # 屏幕开关
        _lbl_sw = QLabel('屏幕开关'); _lbl_sw.setStyleSheet(k_style); cl.addWidget(_lbl_sw, 0, 0)
        sw_wrap = QWidget()
        sw = QHBoxLayout(sw_wrap)
        sw.setContentsMargins(0, 0, 0, 0)
        sw.setSpacing(9)
        self._screen_on = bool(self.config.get('screen_on', True))
        self.btn_screen = QCheckBox()
        self.btn_screen.setStyleSheet(theme.switch())
        self._update_screen_btn()
        sw.addWidget(self.btn_screen)
        self.lbl_screen_state = QLabel('已开启' if self._screen_on else '已关闭')
        self.lbl_screen_state.setStyleSheet(f'font-size:12px; color:{theme.TOK["txt2"]};')
        sw.addWidget(self.lbl_screen_state)
        sw.addStretch(1)
        cl.addWidget(sw_wrap, 0, 1, 1, 2)

        # 亮度
        _lbl_b = QLabel('亮度'); _lbl_b.setStyleSheet(k_style); cl.addWidget(_lbl_b, 1, 0)
        self.slider_brightness = QSlider(Qt.Orientation.Horizontal)
        self.slider_brightness.setRange(0, 100)
        self.slider_brightness.setValue(int(self.config.get('brightness', 50)))
        self.lbl_brightness_val = QLabel(f'{self.slider_brightness.value()}%')
        self.lbl_brightness_val.setStyleSheet(theme.val_hint('38px'))
        self.lbl_brightness_val.setAlignment(Qt.AlignmentFlag.AlignRight)
        cl.addWidget(self.slider_brightness, 1, 1)
        cl.addWidget(self.lbl_brightness_val, 1, 2)

        # 视频质量行 (仅视频/屏幕捕获时可见)
        self.row_quality = QWidget()
        rq = QHBoxLayout(self.row_quality)
        rq.setContentsMargins(0, 0, 0, 0)
        rq.setSpacing(8)
        self._lbl_q = QLabel('视频质量')
        self._lbl_q.setStyleSheet(k_style)
        rq.addWidget(self._lbl_q)
        self.combo_quality = QComboBox()
        for key, label in QUALITY_LABELS.items():
            self.combo_quality.addItem(label, key)
        qk = self.config.get('video_quality', 'medium')
        if qk not in QUALITY_LABELS:
            qk = 'medium'
        self.combo_quality.setCurrentIndex(list(QUALITY_LABELS.keys()).index(qk))
        self.lbl_blocks = QLabel('实时: -- fps')
        self.lbl_blocks.setStyleSheet(theme.accent_hint())
        rq.addWidget(self.combo_quality)
        rq.addWidget(self.lbl_blocks)
        rq.addStretch(1)
        cl.addWidget(self.row_quality, 2, 0, 1, 3)

        # 发送频率 (照片): 挪到设置-高级选项, 控件独立隐藏供逻辑使用
        self.slider_fps = QDoubleSpinBox(ctrl)
        self.slider_fps.setRange(0.1, 30.0)
        self.slider_fps.setSingleStep(0.1)
        self.slider_fps.setDecimals(1)
        self.slider_fps.setValue(float(self.config.get('photo_fps', 10.0)))
        self.slider_fps.setVisible(False)
        self.lbl_fps_real = QLabel('实时: -- fps', ctrl)
        self.lbl_fps_real.setStyleSheet(theme.accent_hint())
        self.lbl_fps_real.setVisible(False)

        # 图像适应
        _lbl_m = QLabel('图像适应'); _lbl_m.setStyleSheet(k_style); cl.addWidget(_lbl_m, 3, 0)
        self.combo_mode = QComboBox()
        for key, label in FIT_LABELS.items():
            self.combo_mode.addItem(label, key)
        self.combo_mode.setCurrentIndex(list(FIT_LABELS.keys()).index(
            self.config.get('fit_mode', 'contain')))
        cl.addWidget(self.combo_mode, 3, 1, 1, 2)

        # 画面旋转
        _lbl_r = QLabel('画面旋转'); _lbl_r.setStyleSheet(k_style); cl.addWidget(_lbl_r, 4, 0)
        self.combo_rotation = QComboBox()
        for deg, label in ((0, '0°'), (90, '90°'), (180, '180°'), (270, '270°')):
            self.combo_rotation.addItem(label, deg)
        self.combo_rotation.setCurrentIndex([0, 90, 180, 270].index(
            int(self.config.get('rotation', 0))))
        cl.addWidget(self.combo_rotation, 4, 1, 1, 2)

        ctrl.setMinimumWidth(300)
        left_layout.addWidget(ctrl)
        left_layout.addStretch(1)
        left_panel.setFixedWidth(400)
        ml.addWidget(left_panel)

        # ---------- 右: 显示内容 ----------
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(10)

        sec_r = QLabel('显示内容')
        sec_r.setStyleSheet(theme.section_label())
        right_layout.addWidget(sec_r)

        # 媒体库下拉: 照片 / 视频 / 捕获
        src_row = QHBoxLayout()
        src_row.setSpacing(8)
        lbl_src = QLabel('媒体库')
        lbl_src.setStyleSheet(k_style)
        src_row.addWidget(lbl_src)
        self.combo_src = QComboBox()
        self.combo_src.setMinimumWidth(150)
        self.combo_src.addItem('照片', 'photo')
        self.combo_src.addItem('视频', 'video')
        self.combo_src.addItem('捕获', 'capture')
        src_row.addWidget(self.combo_src)
        src_row.addStretch(1)
        right_layout.addLayout(src_row)

        # 源内容堆栈: [0]=照片  [1]=视频  [2]=捕获
        self.stack_src = QStackedWidget()

        # --- 照片子页 ---
        page_photo = QWidget()
        pp = QVBoxLayout(page_photo)
        pp.setContentsMargins(0, 0, 0, 0)
        pp.setSpacing(8)
        f_photo = QFrame()
        f_photo.setStyleSheet(theme.card())
        fp = QVBoxLayout(f_photo)
        fp.setContentsMargins(0, 0, 0, 10)
        fhead = QHBoxLayout()
        fhead.setContentsMargins(14, 9, 14, 9)
        ft = QLabel('照片素材库')
        ft.setStyleSheet(f'font-size:13px; font-weight:600; color:{theme.TOK["txt"]};')
        self.lbl_photo_count = QLabel('0 张')
        self.lbl_photo_count.setStyleSheet(f'font-size:11.5px; color:{theme.TOK["txt3"]};')
        fhead.addWidget(ft)
        fhead.addStretch(1)
        fhead.addWidget(self.lbl_photo_count)
        fp.addLayout(fhead)
        self.list_photos = QListWidget()
        self.list_photos.setViewMode(QListView.ViewMode.IconMode)
        self.list_photos.setIconSize(QSize(96, 96))
        self.list_photos.setGridSize(QSize(122, 126))
        self.list_photos.setWordWrap(True)
        self.list_photos.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_photos.setStyleSheet(theme.media_list())
        fp.addWidget(self.list_photos, 1)
        op_photo = QHBoxLayout()
        op_photo.setContentsMargins(14, 6, 14, 0)
        self.btn_add_photo = QPushButton('+ 添加照片')
        op_photo.addWidget(self.btn_add_photo)
        op_photo.addStretch(1)
        fp.addLayout(op_photo)
        pp.addWidget(f_photo, 1)
        self.stack_src.addWidget(page_photo)

        # --- 视频子页 ---
        page_video = QWidget()
        pv = QVBoxLayout(page_video)
        pv.setContentsMargins(0, 0, 0, 0)
        pv.setSpacing(8)
        self.row_playback = QWidget()
        rp = QHBoxLayout(self.row_playback)
        rp.setContentsMargins(0, 0, 0, 0)
        rp.setSpacing(8)
        lbl_pb = QLabel('播放模式')
        lbl_pb.setStyleSheet(k_style)
        rp.addWidget(lbl_pb)
        self.combo_playback = QComboBox()
        self.combo_playback.addItem('单曲循环', 'single')
        self.combo_playback.addItem('列表循环', 'list')
        self.combo_playback.setCurrentIndex(
            0 if self.config.get('playback_mode', 'single') == 'single' else 1)
        rp.addWidget(self.combo_playback)
        rp.addStretch(1)
        pv.addWidget(self.row_playback)
        f_video = QFrame()
        f_video.setStyleSheet(theme.card())
        fv = QVBoxLayout(f_video)
        fv.setContentsMargins(0, 0, 0, 10)
        fhead2 = QHBoxLayout()
        fhead2.setContentsMargins(14, 9, 14, 9)
        ft2 = QLabel('视频素材库')
        ft2.setStyleSheet(f'font-size:13px; font-weight:600; color:{theme.TOK["txt"]};')
        self.lbl_video_count = QLabel('0 条')
        self.lbl_video_count.setStyleSheet(f'font-size:11.5px; color:{theme.TOK["txt3"]};')
        fhead2.addWidget(ft2)
        fhead2.addStretch(1)
        fhead2.addWidget(self.lbl_video_count)
        fv.addLayout(fhead2)
        self.list_videos = QListWidget()
        self.list_videos.setViewMode(QListView.ViewMode.IconMode)
        self.list_videos.setIconSize(QSize(96, 96))
        self.list_videos.setGridSize(QSize(122, 126))
        self.list_videos.setWordWrap(True)
        self.list_videos.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_videos.setStyleSheet(theme.media_list())
        fv.addWidget(self.list_videos, 1)
        self.progress = QProgressBar()
        self.progress.setVisible(False)
        self.progress.setRange(0, 100)
        self.progress.setFixedHeight(14)
        fv.addWidget(self.progress)
        op_video = QHBoxLayout()
        op_video.setContentsMargins(14, 6, 14, 0)
        self.btn_add_video = QPushButton('+ 添加视频')
        op_video.addWidget(self.btn_add_video)
        op_video.addStretch(1)
        fv.addLayout(op_video)
        pv.addWidget(f_video, 1)
        self.stack_src.addWidget(page_video)

        # ================= 捕获页 (SMTC + 桌面 + 桌面设置分组框) =================
        page_capture = QWidget()
        pc = QVBoxLayout(page_capture)
        pc.setContentsMargins(0, 0, 0, 0)
        pc.setSpacing(10)
        self.cap_group = QButtonGroup(self)
        self.cap_group.setExclusive(True)
        self.btn_cap_smtc = QPushButton(
            '媒体封面捕获 · SMTC\n通过 Windows SMTC 自动获取正在播放的媒体封面与标题,\n实时同步到水冷屏。支持音乐/视频播放器。')
        self.btn_cap_screen = QPushButton(
            '屏幕实时投射\n以指定帧率截取电脑桌面画面, 实时镜像到水冷屏。\n适合游戏/桌面画面同步。')
        for b in (self.btn_cap_smtc, self.btn_cap_screen):
            b.setCheckable(True)
            b.setCursor(Qt.CursorShape.PointingHandCursor)
            b.setMinimumHeight(88)
            b.setStyleSheet(theme.capture_card())
            self.cap_group.addButton(b)
            pc.addWidget(b)
        self.lbl_capture_hint = QLabel('')
        self.lbl_capture_hint.setStyleSheet(f'font-size:11.5px; color:{theme.TOK["txt3"]};')
        self.lbl_capture_hint.setWordWrap(True)
        pc.addWidget(self.lbl_capture_hint)

        # 桌面投射设置分组框 (仅桌面捕获时显示)
        self.grp_screen_setup = QGroupBox('桌面投射设置')
        gss = QGridLayout(self.grp_screen_setup)
        gss.setContentsMargins(12, 16, 12, 10)
        gss.setHorizontalSpacing(8)
        gss.setVerticalSpacing(6)
        gss.addWidget(QLabel('显示器:'), 0, 0)
        self.combo_monitor = QComboBox()
        self.combo_monitor.setMinimumWidth(180)
        gss.addWidget(self.combo_monitor, 0, 1, 1, 2)
        self.grp_screen_setup.setVisible(False)
        pc.addWidget(self.grp_screen_setup)

        # SMTC 来源过滤分组框 (仅 SMTC 捕获时显示)
        self.grp_smtc_setup = QGroupBox('SMTC 来源过滤')
        gsm = QGridLayout(self.grp_smtc_setup)
        gsm.setContentsMargins(12, 16, 12, 10)
        gsm.setHorizontalSpacing(8)
        gsm.setVerticalSpacing(6)
        gsm.addWidget(QLabel('过滤模式:'), 0, 0)
        self.combo_smtc_mode = QComboBox()
        self.combo_smtc_mode.addItem('全部来源', 'all')
        self.combo_smtc_mode.addItem('仅白名单', 'whitelist')
        self.combo_smtc_mode.addItem('排除黑名单', 'blacklist')
        _sm = self.config.get('smtc_filter_mode', 'all')
        self.combo_smtc_mode.setCurrentIndex(
            ['all', 'whitelist', 'blacklist'].index(_sm) if _sm in ('all', 'whitelist', 'blacklist') else 0)
        gsm.addWidget(self.combo_smtc_mode, 0, 1, 1, 2)
        gsm.addWidget(QLabel('应用名(逗号分隔):'), 1, 0)
        self.edit_smtc_apps = QLineEdit(', '.join(self.config.get('smtc_filter_apps', [])))
        gsm.addWidget(self.edit_smtc_apps, 1, 1, 1, 2)
        hint_smtc = QLabel('多个媒体同时播放时优先捕获播放器类(music/video/player/网易云等)。\n白名单=仅列出的应用, 黑名单=排除列出的应用, 支持关键字匹配。')
        hint_smtc.setStyleSheet(f'font-size:11px; color:{theme.TOK["txt3"]};')
        hint_smtc.setWordWrap(True)
        gsm.addWidget(hint_smtc, 2, 0, 1, 3)
        self.grp_smtc_setup.setVisible(False)
        pc.addWidget(self.grp_smtc_setup)
        pc.addStretch(1)
        self.stack_src.addWidget(page_capture)

        right_layout.addWidget(self.stack_src, 1)

        # --- 自定义信息叠加 ---
        sec_ov = QLabel('自定义信息叠加')
        sec_ov.setStyleSheet(theme.section_label())
        right_layout.addWidget(sec_ov)
        ov_row = QHBoxLayout()
        ov_row.setSpacing(8)
        self.ov_group = QButtonGroup(self)
        self.ov_group.setExclusive(True)
        self.btn_ov_none = QPushButton('无\n不叠加')
        self.btn_ov_clock = QPushButton('时钟\n右上角实时')
        self.btn_ov_custom = QPushButton('自定义文本\n时间/文字/时钟')
        self._ov_btns = [self.btn_ov_none, self.btn_ov_clock, self.btn_ov_custom]
        for b in self._ov_btns:
            b.setCheckable(True)
            b.setCursor(Qt.CursorShape.PointingHandCursor)
            b.setMinimumHeight(56)
            b.setStyleSheet(theme.overlay_card())
            self.ov_group.addButton(b)
            ov_row.addWidget(b, 1)
        right_layout.addLayout(ov_row)

        # 文本叠加统一面板: 时钟纯文本 + 自定义文本 共用同一套编辑逻辑
        self.stk_text = QWidget()
        st = QVBoxLayout(self.stk_text)
        st.setContentsMargins(0, 0, 0, 0)
        st.setSpacing(8)

        # 时钟样式行 (仅时钟卡显示)
        self.row_clock_style = QWidget()
        rcs = QHBoxLayout(self.row_clock_style)
        rcs.setContentsMargins(0, 0, 0, 0)
        rcs.setSpacing(8)
        rcs.addWidget(QLabel('时钟样式'))
        self.combo_clock_style = QComboBox()
        self.combo_clock_style.addItem('纯文本时间', 'text')
        self.combo_clock_style.addItem('简约钟表', 'analog')
        cs = self.config.get('clock_style', 'text')
        self.combo_clock_style.setCurrentIndex(['text', 'analog'].index(cs))
        rcs.addWidget(self.combo_clock_style)
        rcs.addStretch(1)
        st.addWidget(self.row_clock_style)

        # 文本条目列表 (仅自定义文本卡显示多条)
        self.list_custom = QListWidget()
        self.list_custom.setFixedHeight(64)
        st.addWidget(self.list_custom)

        # 内容 + 增删行 (仅自定义文本卡显示)
        self.row_custom_edit = QWidget()
        rce = QHBoxLayout(self.row_custom_edit)
        rce.setContentsMargins(0, 0, 0, 0)
        rce.setSpacing(8)
        rce.addWidget(QLabel('内容:'))
        self.edit_custom_text = QLineEdit()
        rce.addWidget(self.edit_custom_text, 1)
        self.btn_add_custom = QPushButton('+ 添加')
        self.btn_del_custom = QPushButton('删除')
        rce.addWidget(self.btn_add_custom)
        rce.addWidget(self.btn_del_custom)
        st.addWidget(self.row_custom_edit)

        # 文本样式编辑组 (时钟纯文本 + 自定义文本共用)
        self.grp_text_edit = QWidget()
        gte = QGridLayout(self.grp_text_edit)
        gte.setContentsMargins(0, 0, 0, 0)
        gte.setHorizontalSpacing(8)
        gte.setVerticalSpacing(6)
        gte.addWidget(QLabel('X:'), 0, 0)
        self.spin_custom_x = QDoubleSpinBox(); self.spin_custom_x.setRange(0.0, 1.0)
        self.spin_custom_x.setSingleStep(0.05); self.spin_custom_x.setDecimals(2)
        gte.addWidget(self.spin_custom_x, 0, 1)
        gte.addWidget(QLabel('Y:'), 0, 2)
        self.spin_custom_y = QDoubleSpinBox(); self.spin_custom_y.setRange(0.0, 1.0)
        self.spin_custom_y.setSingleStep(0.05); self.spin_custom_y.setDecimals(2)
        gte.addWidget(self.spin_custom_y, 0, 3)
        gte.addWidget(QLabel('透明:'), 0, 4)
        self.slider_custom_alpha = QSlider(Qt.Orientation.Horizontal)
        self.slider_custom_alpha.setRange(0, 100); self.slider_custom_alpha.setValue(60)
        gte.addWidget(self.slider_custom_alpha, 0, 5)
        gte.addWidget(QLabel('颜色:'), 1, 0)
        self.btn_custom_color = QPushButton(); self.btn_custom_color.setFixedSize(30, 22)
        gte.addWidget(self.btn_custom_color, 1, 1)
        gte.addWidget(QLabel('底色:'), 1, 2)
        self.btn_custom_bg = QPushButton(); self.btn_custom_bg.setFixedSize(30, 22)
        gte.addWidget(self.btn_custom_bg, 1, 3)
        gte.addWidget(QLabel('字号:'), 1, 4)
        self.spin_custom_scale = QDoubleSpinBox()
        self.spin_custom_scale.setRange(0.2, 2.0); self.spin_custom_scale.setSingleStep(0.1)
        self.spin_custom_scale.setDecimals(1); self.spin_custom_scale.setValue(0.6)
        gte.addWidget(self.spin_custom_scale, 1, 5)
        gte.addWidget(QLabel('粗细:'), 2, 0)
        self.spin_custom_thick = QSpinBox(); self.spin_custom_thick.setRange(1, 5)
        self.spin_custom_thick.setValue(1)
        gte.addWidget(self.spin_custom_thick, 2, 1)
        lbl_drag = QLabel('提示: 在预览画面按住拖动可移动文本位置, 也可用上方坐标精确微调')
        lbl_drag.setStyleSheet(f'font-size:11px; color:{theme.TOK["txt3"]};')
        gte.addWidget(lbl_drag, 3, 0, 1, 6)
        st.addWidget(self.grp_text_edit)

        right_layout.addWidget(self.stk_text)

        # 隐藏的覆盖层选择控件: 逻辑保持原样, 由卡片驱动
        self.combo_overlay_type = QComboBox(self)
        self.combo_overlay_type.addItem('无', 'none')
        self.combo_overlay_type.addItem('时钟', 'clock')
        self.combo_overlay_type.addItem('自定义文本', 'custom')
        ot = self.config.get('overlay_type', 'none')
        if ot == 'hwinfo':  # 旧配置可能有 hwinfo, 已移除该功能
            ot = 'none'
        idx = ['none', 'clock', 'custom'].index(ot) if ot in (
            'none', 'clock', 'custom') else 0
        self.combo_overlay_type.setCurrentIndex(idx)
        self.combo_overlay_type.setVisible(False)

        self.lbl_overlay_hint = QLabel('')
        self.lbl_overlay_hint.setStyleSheet(f'font-size:11.5px; color:{theme.TOK["txt3"]};')
        self.lbl_overlay_hint.setWordWrap(True)
        right_layout.addWidget(self.lbl_overlay_hint)

        right_layout.addStretch(1)
        right_panel.setMinimumWidth(430)
        ml.addWidget(right_panel, 1)

        root.addWidget(main, 1)

        # 初始互斥可见性: 由恢复逻辑/用户选择决定显示哪行
        self.row_quality.setVisible(False)
        # 初始选中: 照片
        self.combo_src.setCurrentIndex(0)
        self.stack_src.setCurrentIndex(0)
        self._current_src = 'photo'
        # 初始覆盖层卡片 + 子面板可见性
        self._sync_overlay_cards()
        self._on_overlay_type_changed()

    def _connect_signals(self):
        self.combo_mode.currentIndexChanged.connect(self._on_mode_changed)
        self.combo_rotation.currentIndexChanged.connect(self._on_rotation_changed)
        self.slider_brightness.valueChanged.connect(self._on_brightness_changed)
        self.btn_screen.toggled.connect(self._on_screen_btn)
        self.slider_fps.valueChanged.connect(self._on_fps_changed)
        self.combo_quality.currentIndexChanged.connect(self._on_quality_changed)
        self.combo_playback.currentIndexChanged.connect(self._on_playback_changed)
        self.btn_add_photo.clicked.connect(lambda: self._add_asset('photo'))
        self.btn_add_video.clicked.connect(lambda: self._add_asset('video'))
        self.list_photos.customContextMenuRequested.connect(
            lambda pos: self._media_context_menu(self.list_photos, pos))
        self.list_videos.customContextMenuRequested.connect(
            lambda pos: self._media_context_menu(self.list_videos, pos))
        self.combo_overlay_type.currentIndexChanged.connect(self._on_overlay_type_changed)
        self.combo_clock_style.currentIndexChanged.connect(self._on_clock_style_changed)
        self.list_photos.itemClicked.connect(lambda it: self._apply_asset('photo', it))
        self.list_photos.itemDoubleClicked.connect(lambda it: self._apply_asset('photo', it))
        self.list_videos.itemClicked.connect(lambda it: self._apply_asset('video', it))
        self.list_videos.itemDoubleClicked.connect(lambda it: self._apply_asset('video', it))
        self.btn_settings.clicked.connect(self._open_settings)
        self.combo_src.currentIndexChanged.connect(self._on_src_combo)
        self.btn_cap_smtc.clicked.connect(lambda: self._on_cap_clicked('smtc'))
        self.btn_cap_screen.clicked.connect(lambda: self._on_cap_clicked('screen'))
        self.combo_smtc_mode.currentIndexChanged.connect(self._on_smtc_filter_changed)
        self.edit_smtc_apps.editingFinished.connect(self._on_smtc_filter_changed)
        self.combo_monitor.currentIndexChanged.connect(self._on_monitor_changed)
        self.btn_ov_none.clicked.connect(lambda: self._set_overlay('none'))
        self.btn_ov_clock.clicked.connect(lambda: self._set_overlay('clock'))
        self.btn_ov_custom.clicked.connect(lambda: self._set_overlay('custom'))
        self.btn_add_custom.clicked.connect(self._add_custom_text)
        self.btn_del_custom.clicked.connect(self._del_custom_text)
        self.list_custom.currentRowChanged.connect(self._on_custom_row_changed)
        self.edit_custom_text.textChanged.connect(self._on_text_edited)
        self.spin_custom_x.valueChanged.connect(self._on_text_edited)
        self.spin_custom_y.valueChanged.connect(self._on_text_edited)
        self.slider_custom_alpha.valueChanged.connect(self._on_text_edited)
        self.spin_custom_scale.valueChanged.connect(self._on_text_edited)
        self.spin_custom_thick.valueChanged.connect(self._on_text_edited)
        self.btn_custom_color.clicked.connect(self._pick_text_color)
        self.btn_custom_bg.clicked.connect(self._pick_text_bg)
        # 预览画面按住拖动移动当前编辑文本位置 (时钟纯文本 + 自定义文本共用)
        self.lbl_preview.drag_start.connect(self._on_preview_drag_start)
        self.lbl_preview.drag_move.connect(self._on_preview_drag_move)
        self.lbl_preview.drag_end.connect(self._on_preview_drag_end)

    # ---------- 参数可见性 (照片/视频互斥) ----------
    def _update_param_visibility(self):
        is_video = self.current_kind == 'video'
        is_cap = self.current_kind in ('smtc', 'screen')
        # 视频: 显示质量选项卡 + 实时帧率; 捕获: 隐藏质量选项卡(USB 最高质量自动), 保留实时帧率; 照片: 全隐藏
        self.row_quality.setVisible(is_video or is_cap)
        self._lbl_q.setVisible(is_video)
        self.combo_quality.setVisible(is_video)
        self.lbl_blocks.setVisible(is_video or is_cap)

    # ---------- 自定义覆盖层 (时钟纯文本 + 自定义文本 统一) ----------
    def _on_overlay_type_changed(self):
        t = self.combo_overlay_type.currentData()
        self.config.set('overlay_type', t)
        if t == 'clock':
            self.stk_text.setVisible(True)
            self.row_clock_style.setVisible(True)
            self.list_custom.setVisible(False)
            self.row_custom_edit.setVisible(False)
            is_text = self.combo_clock_style.currentData() == 'text'
            self.grp_text_edit.setVisible(is_text)
            if is_text:
                self._load_text_editor(self._clock_text, is_clock=True)
            self.lbl_overlay_hint.setText('提示: 时钟纯文本样式/位置可编辑, 可在预览拖动; 简约钟表为图形样式')
        elif t == 'custom':
            self.stk_text.setVisible(True)
            self.row_clock_style.setVisible(False)
            self.list_custom.setVisible(True)
            self.row_custom_edit.setVisible(True)
            self.grp_text_edit.setVisible(True)
            if self.list_custom.count() == 0:
                self._add_custom_text()
            else:
                self._on_custom_row_changed(self.list_custom.currentRow())
            self.lbl_overlay_hint.setText('提示: 自定义文本可逐条添加, 样式/位置可编辑, 可在预览拖动')
        else:
            self.stk_text.setVisible(False)
            self.lbl_overlay_hint.setText('')

    def _on_clock_style_changed(self):
        self.config.set('clock_style', self.combo_clock_style.currentData())
        if self.combo_overlay_type.currentData() == 'clock':
            is_text = self.combo_clock_style.currentData() == 'text'
            self.grp_text_edit.setVisible(is_text)
            if is_text:
                self._load_text_editor(self._clock_text, is_clock=True)

    # 加载条目到编辑控件; is_clock=True 时内容固定为实时时间
    def _load_text_editor(self, it, is_clock=False):
        self._text_edit_target = it
        self.edit_custom_text.setEnabled(not is_clock)
        self.edit_custom_text.setText('实时时间 %H:%M:%S' if is_clock else str(it.get('text', '')))
        self.spin_custom_x.setValue(float(it['x']))
        self.spin_custom_y.setValue(float(it['y']))
        self.slider_custom_alpha.setValue(int(float(it.get('alpha', 0.6)) * 100))
        self.spin_custom_scale.setValue(float(it.get('scale', 0.6)))
        self.spin_custom_thick.setValue(int(it.get('thick', 1)))
        b, g, r = it['color']
        self.btn_custom_color.setStyleSheet(f'background: rgb({r},{g},{b});')
        bb, bg_, br = it['bg']
        self.btn_custom_bg.setStyleSheet(f'background: rgb({br},{bg_},{bb});')

    def _add_custom_text(self):
        self._custom_texts.append({'text': '自定义文本', 'x': 0.5, 'y': 0.5,
                                   'color': (255, 255, 255), 'bg': (0, 0, 0),
                                   'alpha': 0.6, 'scale': 0.6, 'thick': 1})
        self._refresh_custom_list()
        self.list_custom.setCurrentRow(len(self._custom_texts) - 1)
        self._sync_text_cfg()

    def _del_custom_text(self):
        row = self.list_custom.currentRow()
        if row < 0 or row >= len(self._custom_texts):
            return
        del self._custom_texts[row]
        self._refresh_custom_list()
        self._sync_text_cfg()

    def _refresh_custom_list(self):
        self.list_custom.clear()
        for i, it in enumerate(self._custom_texts):
            self.list_custom.addItem(f'{i + 1}. {it["text"]}')

    def _on_custom_row_changed(self, row):
        if row < 0 or row >= len(self._custom_texts):
            return
        self._load_text_editor(self._custom_texts[row], is_clock=False)

    def _on_text_edited(self):
        it = self._text_edit_target
        if it is None:
            return
        if self.edit_custom_text.isEnabled():
            it['text'] = self.edit_custom_text.text()
        it['x'] = self.spin_custom_x.value()
        it['y'] = self.spin_custom_y.value()
        it['alpha'] = self.slider_custom_alpha.value() / 100.0
        it['scale'] = self.spin_custom_scale.value()
        it['thick'] = self.spin_custom_thick.value()
        self._sync_text_cfg()

    def _pick_text_color(self):
        it = self._text_edit_target
        if it is None:
            return
        c = QColorDialog.getColor()
        if c.isValid():
            it['color'] = (c.blue(), c.green(), c.red())  # RGB -> BGR
            b, g, r = it['color']
            self.btn_custom_color.setStyleSheet(f'background: rgb({r},{g},{b});')
            self._sync_text_cfg()

    def _pick_text_bg(self):
        it = self._text_edit_target
        if it is None:
            return
        c = QColorDialog.getColor()
        if c.isValid():
            it['bg'] = (c.blue(), c.green(), c.red())  # RGB -> BGR
            bb, bg_, br = it['bg']
            self.btn_custom_bg.setStyleSheet(f'background: rgb({br},{bg_},{bb});')
            self._sync_text_cfg()

    def _sync_text_cfg(self):
        self.config.set('custom_texts', [
            {**t, 'color': list(t['color']), 'bg': list(t['bg'])} for t in self._custom_texts])
        self.config.set('clock_text', {
            **{k: self._clock_text[k] for k in ('x', 'y', 'alpha', 'scale', 'thick')},
            'color': list(self._clock_text['color']), 'bg': list(self._clock_text['bg']),
        })

    # 预览画面按住拖动: 移动当前编辑文本条目位置
    def _on_preview_drag_start(self, fx, fy):
        if self._text_edit_target is not None:
            self._text_edit_target['x'] = fx
            self._text_edit_target['y'] = fy

    def _on_preview_drag_move(self, fx, fy):
        if self._text_edit_target is not None:
            self._text_edit_target['x'] = fx
            self._text_edit_target['y'] = fy
            self.spin_custom_x.setValue(fx)
            self.spin_custom_y.setValue(fy)
            self._sync_text_cfg()

    def _on_preview_drag_end(self):
        if self._text_edit_target is not None:
            self._sync_text_cfg()

    def _get_overlay(self):
        # 覆盖层配置: 返回 (None, [], cfg) —— hub/placements 为旧 HWInfo 参数, 已弃用, 保留占位
        otype = self.combo_overlay_type.currentData()
        if otype not in ('clock', 'custom'):
            return None
        cfg = {
            'overlay_type': otype,
            'clock_style': self.combo_clock_style.currentData(),
            'clock_text': self._clock_text,
            'custom_texts': self._custom_texts,
        }
        return (None, [], cfg)

    # ---------- 设置菜单 ----------
    def _build_menu(self):
        menubar = self.menuBar()
        m = menubar.addMenu('设置')
        act_settings = QAction('设置…', self)
        act_settings.triggered.connect(self._open_settings)
        m.addAction(act_settings)

    def _open_settings(self):
        dlg = SettingsDialog(self.config, self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        v = dlg.result_values()
        self.config.set('auto_start', bool(v['auto_start']))
        self.config.set('power_actions', bool(v['power_main']))
        for k, val in v['powers']:
            self.config.set(k, bool(val))
        self.config.set('photo_fps', float(v['photo_fps']))
        self.slider_fps.setValue(float(v['photo_fps']))
        self.config.set('smtc_filter_mode', v['smtc_mode'])
        self.config.set('smtc_filter_apps', v['smtc_apps'])
        self._apply_autostart(bool(v['auto_start']))

    def _apply_autostart(self, checked):
        try:
            run_key = r'Software\Microsoft\Windows\CurrentVersion\Run'
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, run_key, 0,
                                winreg.KEY_SET_VALUE) as k:
                if checked:
                    exe = sys.executable
                    pyw = os.path.join(os.path.dirname(exe), 'pythonw.exe')
                    if not os.path.exists(pyw):
                        pyw = exe
                    cmd = f'"{pyw}" "{os.path.abspath(__file__)}"'
                    winreg.SetValueEx(k, 'TUFScreenControl', 0, winreg.REG_SZ, cmd)
                else:
                    try:
                        winreg.DeleteValue(k, 'TUFScreenControl')
                    except FileNotFoundError:
                        pass
        except Exception as e:
            print(f'设置开机自启失败: {e}', file=sys.stderr)

    # ---------- 电源操作时关闭水冷屏 ----------
    # ======================================================================
    # 【电源状态识别 - 已知问题 / 供后续 AI 参考，逻辑暂不修改】
    #
    # 当前实现依赖 Qt 窗口 nativeEvent 捕获系统消息:
    #   - WM_QUERYENDSESSION / WM_ENDSESSION (0x0011 / 0x0016)  -> 关机 / 注销
    #   - WM_POWERBROADCAST + PBT_APMSUSPEND (0x0218 / 0x0004)  -> 睡眠
    # 命中对应选项(power_logout/power_shutdown/power_sleep)且 power_actions 开启时,
    # 调用 cooler.set_screen(False) 关闭水冷屏(命令 0x80000110, 0x01=关 0x00=开)。
    #
    # 【可靠性缺口 - 实测某些情况识别不到电源状态, 导致"关屏"不触发】
    #   1) 系统关机/注销流程中, 窗口可能已被销毁, 收不到 nativeEvent;
    #   2) 部分睡眠路径(混合睡眠/超时睡眠)或特定显卡驱动组合下,
    #      WM_POWERBROADCAST 不一定送达本窗口;
    #   3) 注销时 ENDSESSION_LOGOFF 标志位可能未设置或存在时序竞争;
    #   4) 程序以托盘/后台运行时, 消息泵受限也可能漏事件。
    # 因此"关闭电脑/注销/睡眠时关闭水冷屏"在某些机器上可能失效——这是已知限制。
    #
    # 【后续改进方向】(迁移 C++/WinUI 时建议实现, 而非继续在 PySide 打补丁)
    #   - 系统电源通知: RegisterSuspendResumeNotification(GUID_SLEEP / GUID_HIBERNATE)
    #   - 会话通知: WTSRegisterSessionNotification(锁屏/注销/会话切换)
    #   - 兜底: 用 Windows 任务计划程序在 关机/注销 事件触发关屏脚本/命令
    #   - 关屏命令统一走 tuf_hid.TufCooler.set_screen(False)
    # ======================================================================
    def _maybe_power_off(self, which):
        if not self.config.get('power_actions', True):
            return
        if self.config.get(which, False) and self.cooler:
            try:
                self.cooler.set_screen(False)
            except Exception:
                pass

    def closeEvent(self, e):
        # 关闭程序时关闭水冷屏幕
        self._maybe_power_off('power_close_app')
        super().closeEvent(e)

    def nativeEvent(self, eventType, message):
        try:
            WM_QUERYENDSESSION, WM_ENDSESSION = 0x0011, 0x0016
            WM_POWERBROADCAST, PBT_APMSUSPEND = 0x0218, 0x0004
            ENDSESSION_LOGOFF = 0x80000000
            msg = ctypes.wintypes.MSG.from_address(int(message))
            if msg.message in (WM_QUERYENDSESSION, WM_ENDSESSION):
                if msg.lParam & ENDSESSION_LOGOFF:
                    self._maybe_power_off('power_logout')
                else:
                    self._maybe_power_off('power_shutdown')
                return True, 1
            if msg.message == WM_POWERBROADCAST and msg.wParam == PBT_APMSUSPEND:
                self._maybe_power_off('power_sleep')
                return True, 1
        except Exception:
            pass
        return super().nativeEvent(eventType, message)

    def _populate_monitors(self):
        # 枚举显示器填入下拉 (dxcam output_info); 失败时留空, 默认整屏
        self.combo_monitor.clear()
        try:
            import dxcam
            idx = 0
            while True:
                try:
                    info = dxcam.output_info(idx)
                except Exception:
                    break
                if not info:
                    break
                w, h = info['resolution']
                name = info.get('name', '') or ''
                self.combo_monitor.addItem(
                    f'显示器 {idx + 1}  ({w}x{h})' + (f'  {name}' if name else ''), idx)
                idx += 1
        except Exception:
            pass
        if self.combo_monitor.count() == 0:
            self.combo_monitor.addItem('默认显示器', 0)

    # ---------- 设备 ----------
    def _init_device(self):
        try:
            self.cooler = tuf_hid.TufCooler().open()
            self.lbl_status.setText('● 设备: 已连接 (VID_0B05&PID_1C7B)')
            self.lbl_status.setStyleSheet(theme.status_chip(True))

            # 启动恢复控制命令: 读取持久化配置(config.json), 复原软件关闭时的状态
            #   set_screen(config['screen_on'])      -> 0x80000110, 0x01=关 0x00=开
            #   set_brightness(config['brightness'])  -> 0x80000112, 0-100
            # (纯JPEG流直接驱动画面显示, 无需唤醒命令; 这里仅发控制命令恢复开关/亮度)
            self.cooler.set_screen(bool(self.config.get('screen_on', True)))
            self.cooler.set_brightness(int(self.config.get('brightness', 100)))

            # 恢复上次显示内容 (配置存相对路径, 读取时解析为绝对路径)
            dt = self.config.get('display_type')
            _pp = _media_path_from_stored(self.config.get('photo_path'))
            _vp = _media_path_from_stored(self.config.get('video_path'))
            if dt == 'photo' and _pp and os.path.exists(_pp):
                self.start_photo(_pp)
            elif dt == 'video' and _vp and os.path.exists(_vp):
                self.start_video(_vp)
            elif dt in ('screen', 'smtc'):
                self.start_capture(dt)
            else:
                # 上次的素材已不存在(被删除): 清空恢复引用, 不显示任何内容
                if dt == 'photo':
                    self.config.set('photo_path', None)
                elif dt == 'video':
                    self.config.set('video_path', None)
                self.config.set('display_type', None)
        except Exception as e:
            self.cooler = None
            self.lbl_status.setText(f'● 设备: 未连接 ({e})')
            self.lbl_status.setStyleSheet(theme.status_chip(False))

    # ---------- 素材库 ----------
    def _refresh_lists(self):
        self.list_photos.clear()
        for p in self.lib.list_photos():
            self._add_photo_item(p)
        self.list_videos.clear()
        for p in self.lib.list_videos():
            self._add_video_item(p)
        self.lbl_photo_count.setText(f'{self.list_photos.count()} 张')
        self.lbl_video_count.setText(f'{self.list_videos.count()} 条')

    def _add_photo_item(self, path):
        item = QListWidgetItem(os.path.basename(path))
        item.setData(Qt.ItemDataRole.UserRole, path)
        img = asset_lib.imread_unicode(path)
        if img is not None:
            h, w = img.shape[:2]
            scale = min(1.0, 96 / max(h, w))
            thumb = cv2.resize(img, (max(1, int(w*scale)), max(1, int(h*scale))))
            qimg = QImage(thumb.data, thumb.shape[1], thumb.shape[0],
                          thumb.shape[1]*3, QImage.Format.Format_BGR888).copy()
            item.setIcon(QIcon(QPixmap.fromImage(qimg)))
        self.list_photos.addItem(item)

    def _add_video_item(self, path):
        item = QListWidgetItem(os.path.basename(path))
        item.setData(Qt.ItemDataRole.UserRole, path)
        cap = cv2.VideoCapture(path)
        if cap.isOpened():
            ok, frame = cap.read()
            cap.release()
            if ok:
                h, w = frame.shape[:2]
                scale = min(1.0, 96 / max(h, w))
                thumb = cv2.resize(frame, (max(1, int(w*scale)), max(1, int(h*scale))))
                qimg = QImage(thumb.data, thumb.shape[1], thumb.shape[0],
                              thumb.shape[1]*3, QImage.Format.Format_BGR888).copy()
                item.setIcon(QIcon(QPixmap.fromImage(qimg)))
        self.list_videos.addItem(item)

    def _add_asset(self, kind):
        # 批量添加: 文件对话框多选, 全部经 ffmpeg 管线处理后入库
        if kind == 'photo':
            title = '添加照片 (可多选)'
            filt = ('图片文件 (*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff);;所有文件 (*.*)')
            start = PHOTOS_DIR
        else:
            title = '添加视频 (可多选)'
            filt = ('视频文件 (*.mp4 *.avi *.mov *.mkv *.gif *.wmv *.flv *.webm '
                    '*.m4v *.ts *.mpeg *.mpg *.3gp);;所有文件 (*.*)')
            start = VIDEOS_DIR
        paths, _ = QFileDialog.getOpenFileNames(self, title, start, filt)
        if not paths:
            return
        if self.add_thread and self.add_thread.isRunning():
            QMessageBox.information(self, '提示', '正在处理上一个素材, 请稍候')
            return
        self.progress.setVisible(True)
        self.progress.setValue(0)
        self.add_thread = AddAssetThread(self.lib, kind, paths, self)
        self.add_thread.finished_ok.connect(self._on_add_ok)
        self.add_thread.failed.connect(self._on_add_failed)
        self.add_thread.progress.connect(self._on_add_progress)
        self.add_thread.start()

    def _on_add_progress(self, cur, total):
        if total > 0:
            self.progress.setValue(int(cur / total * 100))

    def _on_add_ok(self, kind, dsts):
        self.progress.setVisible(False)
        self._refresh_lists()
        if not dsts:
            return
        # 自动选中第一个新素材并应用
        lst = self.list_photos if kind == 'photo' else self.list_videos
        for i in range(lst.count()):
            if lst.item(i).data(Qt.ItemDataRole.UserRole) == dsts[0]:
                lst.setCurrentRow(i)
                self._apply_asset(kind, lst.item(i))
                break

    def _on_add_failed(self, kind, errmsg):
        self.progress.setVisible(False)
        QMessageBox.warning(self, '添加失败', errmsg)

    def _delete_selected(self):
        lst = self.list_photos if self._current_src == 'photo' else self.list_videos
        item = lst.currentItem()
        if not item:
            QMessageBox.information(self, '提示', '请先选中要删除的素材')
            return
        path = item.data(Qt.ItemDataRole.UserRole)
        kind = 'photo' if lst is self.list_photos else 'video'
        if not path or not os.path.exists(path):
            return
        ret = QMessageBox.question(self, '确认删除',
                                   f'删除素材「{os.path.basename(path)}」?\n'
                                   f'（文件将从素材库移除, 不可恢复）')
        if ret != QMessageBox.StandardButton.Yes:
            return
        try:
            # 若正在显示该素材, 先停止播放(释放文件句柄)再删除, 否则文件被线程占用删不掉
            if kind == 'photo' and self.current_kind == 'photo' and \
                    _media_path_from_stored(self.config.get('photo_path')) == path:
                self.stop_display()
            if kind == 'video' and self.current_kind == 'video' and \
                    _media_path_from_stored(self.config.get('video_path')) == path:
                self.stop_display()
            os.remove(path)
        except Exception as e:
            QMessageBox.warning(self, '删除失败', str(e))
            return
        # 删除的是当前显示素材: 同步清空恢复引用
        if kind == 'photo' and _media_path_from_stored(self.config.get('photo_path')) == path:
            self.config.set('photo_path', None)
            self.config.set('display_type', None)
        if kind == 'video' and _media_path_from_stored(self.config.get('video_path')) == path:
            self.config.set('video_path', None)
            self.config.set('display_type', None)
        self._refresh_lists()

    # ---------- 显示控制 ----------
    def _apply_asset(self, kind, item):
        path = item.data(Qt.ItemDataRole.UserRole)
        if not path or not os.path.exists(path):
            return
        if kind == 'photo':
            self.start_photo(path)
        else:
            self.start_video(path)

    def start_photo(self, path):
        if not self.cooler:
            return
        self.stop_display()
        self.current_kind = 'photo'
        self._current_src = 'photo'
        self.combo_src.setCurrentIndex(0)
        self.stack_src.setCurrentIndex(0)
        self._update_param_visibility()
        self.config.set('display_type', 'photo')
        self.config.set('photo_path', _media_path_to_stored(path))
        self.config.set('video_path', None)
        self.lbl_preview.setText('加载中...')
        self.photo_thread = PhotoStreamThread(
            self.cooler, path, self._get_mode, self._get_rotation,
            self._get_overlay, self._get_fps, self._get_screen, self)
        self.photo_thread.preview.connect(self._show_preview)
        self.photo_thread.fps_report.connect(self._on_fps_report)
        self.photo_thread.blocks_report.connect(self._on_blocks)
        self.photo_thread.error.connect(self._on_stream_error)
        self.photo_thread.start()

    def start_video(self, path):
        if not self.cooler:
            return
        self.stop_display()
        self.current_kind = 'video'
        self._current_src = 'video'
        self.combo_src.setCurrentIndex(1)
        self.stack_src.setCurrentIndex(1)
        self._update_param_visibility()
        self.config.set('display_type', 'video')
        self.config.set('video_path', _media_path_to_stored(path))
        self.config.set('photo_path', None)
        self.lbl_preview.setText('加载中...')
        playback = self.config.get('playback_mode', 'single')
        self.video_thread = VideoStreamThread(
            self.cooler, path, self._get_mode, self._get_rotation,
            self._get_overlay, self._get_quality, self._get_screen,
            self, notify_finish=(playback == 'list'))
        self.video_thread.video_finished.connect(self._on_video_finished)
        self.video_thread.preview.connect(self._show_preview)
        self.video_thread.fps_report.connect(self._on_fps_report)
        self.video_thread.blocks_report.connect(self._on_blocks)
        self.video_thread.error.connect(self._on_stream_error)
        self.video_thread.start()

    def stop_display(self):
        for t in (self.photo_thread, self.video_thread, self.capture_thread):
            if t:
                try:
                    t.disconnect()   # 先断开所有信号, 防止残留线程的帧/报错污染新画面
                except Exception:
                    pass
                if t.isRunning():
                    t.requestInterruption()
                    t.wait(3000)
                t.deleteLater()
        self.photo_thread = self.video_thread = self.capture_thread = None
        self.current_kind = None
        self.lbl_preview.setText('未在显示')
        self.lbl_fps_real.setText('实时: -- fps')
        self.lbl_blocks.setText('实时: -- fps')
        if self.cooler:
            pass  # 停止发送后屏幕会回自带动画（固件特性）

    # ---------- 参数回调 ----------
    def _get_screen(self):
        return bool(self._screen_on)

    def _get_mode(self):
        return self.combo_mode.currentData()

    def _get_rotation(self):
        return int(self.combo_rotation.currentData() or 0)

    def _on_rotation_changed(self):
        self.config.set('rotation', self._get_rotation())
        # 不重启: 线程每帧动态读取旋转, 视频保持当前位置继续播

    def _on_playback_changed(self):
        self.config.set('playback_mode', self.combo_playback.currentData())
        # 循环方式热切换: 不重启视频线程, 画面不闪、播放位置不跳
        if self.current_kind == 'video' and self.video_thread:
            self.video_thread.set_notify_finish(
                self.combo_playback.currentData() == 'list')

    def _on_video_finished(self):
        # 列表循环: 播完切下一个
        if self.current_kind != 'video':
            return
        videos = self.lib.list_videos()
        if not videos:
            return
        cur = _media_path_from_stored(self.config.get('video_path'))
        idx = videos.index(cur) if cur in videos else -1
        nxt = videos[(idx + 1) % len(videos)] if idx >= 0 else videos[0]
        self.start_video(nxt)

    # ---------- 显示内容源切换 (图片/视频/捕获) ----------
    def _on_src_combo(self):
        src = self.combo_src.currentData()
        self._current_src = src
        self.stack_src.setCurrentIndex({'photo': 0, 'video': 1, 'capture': 2}[src])
        if src == 'capture':
            self.lbl_capture_hint.setText(
                '提示: 两种捕获互斥。选中即开始捕获并显示, 再次点击取消停止。')
        else:
            self.lbl_capture_hint.setText('')

    def _media_context_menu(self, lst, pos):
        # 媒体库右键菜单: 应用 / 重命名 / 删除
        item = lst.itemAt(pos)
        if item is None:
            return
        lst.setCurrentItem(item)
        kind = 'photo' if lst is self.list_photos else 'video'
        menu = QMenu(self)
        act_apply = menu.addAction('应用并显示')
        menu.addSeparator()
        act_rename = menu.addAction('重命名…')
        act_delete = menu.addAction('删除')
        act = menu.exec(lst.mapToGlobal(pos))
        if act == act_apply:
            self._apply_asset(kind, item)
        elif act == act_rename:
            self._rename_selected()
        elif act == act_delete:
            self._delete_selected()

    def _on_monitor_changed(self):
        # 显示器切换: 热切换捕获源显示器 (始终整屏投射)
        self._refresh_capture_preview_params()

    def _on_smtc_filter_changed(self):
        self.config.set('smtc_filter_mode', self.combo_smtc_mode.currentData())
        self.config.set('smtc_filter_apps', [
            a.strip() for a in self.edit_smtc_apps.text().split(',') if a.strip()])

    def _refresh_capture_preview_params(self):
        # 让捕获线程下一帧读取最新显示器 (动态生效, 始终整屏投射)
        if self.current_kind == 'screen' and self.capture_thread:
            try:
                self.capture_thread.set_region((self.combo_monitor.currentIndex() or 0, None))
            except Exception:
                pass

    # ---------- 捕获模式 ----------
    def start_capture(self, kind):
        if not self.cooler:
            QMessageBox.warning(self, '提示', '设备未连接, 无法开始捕获')
            return
        self.stop_display()
        self.current_kind = kind
        # 同步媒体库下拉/堆栈/捕获卡片回显 (配置恢复时也能正确高亮)
        self._current_src = 'capture'
        self.combo_src.setCurrentIndex(2)
        self.stack_src.setCurrentIndex(2)
        self.btn_cap_smtc.setChecked(kind == 'smtc')
        self.grp_screen_setup.setVisible(kind == 'screen')
        self.grp_smtc_setup.setVisible(kind == 'smtc')
        self.btn_cap_screen.setChecked(kind == 'screen')
        self._update_param_visibility()
        self.config.set('display_type', kind)
        self.config.set('photo_path', None)
        self.config.set('video_path', None)
        self.lbl_preview.setText('捕获中...')
        if kind == 'smtc':
            smtc_cfg = {'mode': self.config.get('smtc_filter_mode', 'all'),
                        'apps': self.config.get('smtc_filter_apps', [])}
            self.capture_thread = SMTCCaptureThread(
                self.cooler, self._get_mode, self._get_rotation,
                self._get_overlay, self._get_screen, self, smtc_cfg=smtc_cfg)
        else:
            # 屏幕捕获: 无质量选项卡, 固定 'high' 档 -> 动态顶到 USB 总线能承受的最高质量/最大块数
            self.capture_thread = ScreenCaptureThread(
                self.cooler, self._get_mode, self._get_rotation,
                self._get_overlay, lambda: 'high', self._get_screen, self,
                monitor=self.combo_monitor.currentIndex() or 0,
                region=None)
        self.capture_thread.preview.connect(self._show_preview)
        self.capture_thread.error.connect(self._on_stream_error)
        if hasattr(self.capture_thread, 'fps_report'):
            self.capture_thread.fps_report.connect(self._on_fps_report)
            self.capture_thread.blocks_report.connect(self._on_blocks)
        self.capture_thread.start()

    def _on_cap_clicked(self, kind):
        if self.current_kind == kind:
            # 再点取消: 停止捕获
            self.stop_display()
            self.btn_cap_smtc.setChecked(False)
            self.btn_cap_screen.setChecked(False)
            return
        self.start_capture(kind)

    # ---------- 自定义叠加卡片 ----------
    def _sync_overlay_cards(self):
        t = self.config.get('overlay_type', 'none')
        idx = ['none', 'clock', 'custom'].index(t) if t in (
            'none', 'clock', 'custom') else 0
        # 同步隐藏的 combo_overlay_type, 使 _on_overlay_type_changed 读到与卡片一致的值
        self.combo_overlay_type.setCurrentIndex(idx)
        for i, b in enumerate(self._ov_btns):
            b.setChecked(i == idx)

    def _set_overlay(self, t):
        self.combo_overlay_type.setCurrentIndex(
            ['none', 'clock', 'custom'].index(t))

    def _rename_selected(self):
        lst = self.list_photos if self._current_src == 'photo' else self.list_videos
        item = lst.currentItem()
        if not item:
            return
        old_path = item.data(Qt.ItemDataRole.UserRole)
        old_name = os.path.basename(old_path)
        new_name, ok = QInputDialog.getText(self, '重命名素材', '新文件名:', text=old_name)
        if not ok or not new_name.strip():
            return
        new_name = re.sub(r'[\\/:*?"<>|]', '_', new_name.strip())
        stem, ext = os.path.splitext(old_path)
        if not new_name.lower().endswith(('.jpg', '.jpeg', '.png', '.bmp', '.mp4', '.avi', '.mov', '.mkv')):
            new_name = new_name + ext
        new_path = os.path.join(os.path.dirname(old_path), new_name)
        if os.path.exists(new_path):
            return
        try:
            os.rename(old_path, new_path)
        except OSError as e:
            return
        # 同步 config 引用
        if _media_path_from_stored(self.config.get('photo_path')) == old_path:
            self.config.set('photo_path', _media_path_to_stored(new_path))
        if _media_path_from_stored(self.config.get('video_path')) == old_path:
            self.config.set('video_path', _media_path_to_stored(new_path))
        self._refresh_lists()

    def _get_fps(self):
        return self.slider_fps.value()

    def _get_quality(self):
        return self.combo_quality.currentData()

    def _on_mode_changed(self):
        self.config.set('fit_mode', self._get_mode())
        # 不重启: 线程每帧动态读取参数, 下一帧即生效, 视频保持当前位置继续播

    def _on_brightness_changed(self, v):
        self.lbl_brightness_val.setText(f'{v}%')
        self.config.set('brightness', v)
        if self.cooler:
            try:
                self.cooler.set_brightness(v)
            except Exception as e:
                print(f'亮度设置失败: {e}', file=sys.stderr)

    def _update_screen_btn(self):
        self.btn_screen.setChecked(self._screen_on)

    def _on_screen_btn(self, on):
        self._screen_on = bool(on)
        self.config.set('screen_on', self._screen_on)
        self._update_screen_btn()
        self.lbl_screen_state.setText('已开启' if on else '已关闭')
        if self.cooler:
            try:
                self.cooler.set_screen(self._screen_on)
            except Exception as e:
                print(f'{e}', file=sys.stderr)

    def _on_fps_changed(self, v):
        self.config.set('photo_fps', v)

    def _on_quality_changed(self):
        self.config.set('video_quality', self._get_quality())

    # ---------- 线程回调 ----------
    def _show_preview(self, bgr):
        # 1:1 显示: 320x320 原生像素, 不做任何缩放
        bgr = np.ascontiguousarray(bgr)   # 保险: 非连续数组 QImage 会花屏/卡死
        h, w = bgr.shape[:2]
        qimg = QImage(bgr.data, w, h, w * 3, QImage.Format.Format_BGR888).copy()
        self.lbl_preview.setPixmap(QPixmap.fromImage(qimg))

    def _on_fps_report(self, fps):
        if self.current_kind in ('video', 'screen'):
            self.lbl_blocks.setText(f'实时: {fps:.1f} fps')
        else:
            self.lbl_fps_real.setText(f'实时: {fps:.1f} fps')

    def _on_blocks(self, n):
        pass  # 块数不再展示, 视频实时帧率由 fps_report 提供

    def _on_stream_error(self, msg):
        self.stop_display()
        QMessageBox.warning(self, '显示错误', msg)

    # ---------- 退出 ----------
    def closeEvent(self, event):
        self.stop_display()
        if self.cooler:
            self.cooler.close()
        self.config.save()
        event.accept()


def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(theme.app_qss())
    win = MainWindow()
    win.show()
    theme.apply_light_titlebar(win.winId())
    sys.exit(app.exec())


if __name__ == '__main__':
    main()


