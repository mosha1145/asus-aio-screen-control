#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
asset_lib.py — 素材库管理与画面处理
===================================
素材库目录（工具自带）:
  assets/photos/   照片素材（处理后的 JPEG，长边 320；1:1 为 320x320）
  assets/videos/   视频素材（处理后的 MP4，长边 320；1:1 为 320x320）

添加素材时统一处理:
  - 照片: OpenCV 解码(支持中文路径) -> 长边320 -> JPEG 存储
  - 视频: ffmpeg 转码(优先 GPU 加速 NVENC/AMF/QSV, 回退 libx264) -> 长边320 -> H.264 CRF18 高质量

显示适配模式（发送前逐帧应用）:
  - contain (适应): 长边填满 320，短边黑边（等比）
  - cover   (填充): 短边填满 320，长边居中裁剪
  - stretch (拉伸): 直接拉伸到 320x320
"""
import os
import re
import shutil
import subprocess
import time
import uuid

import ctypes

import cv2
import numpy as np

SIZE = 320
JPEG_QUALITY = 90  # 素材库照片存储质量


def short_path(p):
    """转 Windows 8.3 短路径, 规避 ffmpeg 等 MinGW 程序对中文路径的解析问题"""
    _gsp = ctypes.windll.kernel32.GetShortPathNameW
    _gsp.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
    _gsp.restype = ctypes.c_uint32
    buf = ctypes.create_unicode_buffer(1024)
    n = _gsp(p, buf, 1024)
    return buf.value if n and n < 1024 else p

FIT_MODES = ('contain', 'cover', 'stretch')

# 视频质量档 -> 显示时 JPEG 质量（封顶 88，避免块数无限增加）
QUALITY_PRESETS = {
    'low': 40,
    'medium': 60,
    'high': 80,
}

# 动态质量策略: 播放时按实测帧率实时调节 JPEG 质量(块数)
#   fps  : 目标帧率(0=取 min(60, 源帧率));  low=满帧优先 / high=质量优先保底45
#   q0   : 起始质量(低档起步低, 画质差别明显)   qmin/qmax: 质量调节范围
QUALITY_TARGETS = {
    'low':    {'fps': 0,    'q0': 32, 'qmin': 20, 'qmax': 80},   # 满帧优先; 复杂场景可降很狠保60, 有余再升质
    'medium': {'fps': 0,    'q0': 55, 'qmin': 25, 'qmax': 85},   # 动态平衡: 帧率有余升质, 不足降质
    'high':   {'fps': 45,   'q0': 85, 'qmin': 28, 'qmax': 88},   # 画质优先; 跌破45fps才降质保帧率
}


def imread_unicode(path):
    """支持中文/Unicode 路径的图片读取 (cv2.imread 不支持中文路径)"""
    data = np.fromfile(path, dtype=np.uint8)
    if data.size == 0:
        return None
    return cv2.imdecode(data, cv2.IMREAD_COLOR)


def _ts_name(orig_name):
    """生成素材库文件名: 仅保留 ASCII 字母数字, 避免 cv2 无法处理中文文件名"""
    stem, ext = os.path.splitext(os.path.basename(orig_name))
    safe = ''.join(c for c in stem if c.isascii() and (c.isalnum() or c in '_-')) or 'asset'
    return f'{time.strftime("%Y%m%d_%H%M%S")}_{safe[:40]}{uuid.uuid4().hex[:6]}{ext.lower()}'


def find_ffmpeg():
    """定位自带 ffmpeg: 工具目录 tools/ffmpeg.exe, 其次系统 PATH"""
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.join(here, 'tools', 'ffmpeg.exe')
    if os.path.exists(cand):
        return cand
    return shutil.which('ffmpeg')


def detect_video_encoder(ffmpeg):
    """按显卡可用性选择编码器: NVENC > QSV > AMF > libx264"""
    try:
        out = subprocess.run([ffmpeg, '-hide_banner', '-encoders'],
                             capture_output=True, text=True, timeout=30).stdout or ''
        for enc in ('h264_nvenc', 'h264_qsv', 'h264_amf'):
            if enc in out:
                return enc
    except Exception:
        pass
    return 'libx264'


class AssetLib:
    """照片/视频素材库: 添加(处理入库存)、列出、删除、取路径"""

    def __init__(self, photos_dir, videos_dir):
        self.photos_dir = photos_dir
        self.videos_dir = videos_dir
        os.makedirs(photos_dir, exist_ok=True)
        os.makedirs(videos_dir, exist_ok=True)

    # ---------- 枚举 ----------
    # 照片支持格式: 添加时统一由 ffmpeg 转成 JPG 入库, 这里同时兼容旧素材
    PHOTO_EXTS = ('.jpg', '.jpeg', '.png', '.bmp', '.webp', '.tif', '.tiff')
    # 视频支持格式: 添加时统一由 ffmpeg 转成 MP4 入库 (gif 也走视频转 mp4)
    VIDEO_EXTS = ('.mp4', '.avi', '.mov', '.mkv', '.wmv', '.flv', '.webm',
                  '.m4v', '.ts', '.mpeg', '.mpg', '.3gp')

    def list_photos(self):
        out = []
        for name in sorted(os.listdir(self.photos_dir)):
            p = os.path.join(self.photos_dir, name)
            if os.path.isfile(p) and name.lower().endswith(self.PHOTO_EXTS):
                out.append(p)
        return out

    def list_videos(self):
        out = []
        for name in sorted(os.listdir(self.videos_dir)):
            p = os.path.join(self.videos_dir, name)
            if os.path.isfile(p) and name.lower().endswith(self.VIDEO_EXTS):
                out.append(p)
        return out

    # ---------- 添加: 照片 (ffmpeg, 与视频同一处理管线) ----------
    def add_photo(self, src_path):
        """照片 -> ffmpeg 短边320 -> 素材库 JPEG (与视频同管线: 短边320, 1:1->320x320, q:v2 高质量)。
        无 ffmpeg 时回退 OpenCV。支持中文路径。返回素材库路径或抛异常。"""
        img = imread_unicode(src_path)
        if img is None:
            raise RuntimeError(f'无法读取图片: {src_path}\n(格式不受支持或文件已损坏)')
        h, w = img.shape[:2]
        # 短边 320: 1:1 -> 320x320; 非1:1 短边固定320, 长边按比例 (各适配模式发送时只缩小不放大)
        if h == w:
            nh, nw = SIZE, SIZE
        elif w > h:
            nw, nh = int(round(w / h * SIZE)), SIZE
        else:
            nw, nh = SIZE, int(round(h / w * SIZE))
        nw, nh = max(2, nw), max(2, nh)
        cache_dir = os.path.join(os.path.dirname(self.photos_dir), 'cache')
        dst = os.path.join(self.photos_dir, _ts_name(src_path).rsplit('.', 1)[0] + '.jpg')
        ffmpeg = find_ffmpeg()
        if ffmpeg is None:
            # 回退: OpenCV 处理 (无 ffmpeg 时保持可用)
            if (nw, nh) != (w, h):
                interp = cv2.INTER_AREA if (nw * nh) < (w * h) else cv2.INTER_LINEAR
                img = cv2.resize(img, (nw, nh), interpolation=interp)
            ok, buf = cv2.imencode('.jpg', img, [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            if not ok:
                raise RuntimeError(f'图片编码失败: {src_path}')
            buf.tofile(dst)  # 字节写入, 兼容中文/Unicode 文件名
            return dst
        # ffmpeg 处理: 中文/Unicode 路径先复制到 ASCII 临时文件
        tmp = None
        src_for_ff = src_path
        if not all(ord(c) < 128 for c in src_path):
            os.makedirs(cache_dir, exist_ok=True)
            ext = os.path.splitext(src_path)[1].lower() or '.png'
            tmp = os.path.join(cache_dir, f'work_{uuid.uuid4().hex}{ext}')
            shutil.copy2(src_path, tmp)
            src_for_ff = tmp
        err_file = os.path.join(cache_dir, f'err_{uuid.uuid4().hex}.txt')
        try:
            # JPEG 高质量: -q:v 2 (0-31 越小越好, 2 接近肉眼无损); 与视频同管线 (scale lanczos)
            cmd = [ffmpeg, '-hide_banner', '-y', '-i', src_for_ff,
                   '-vf', f'scale={nw}:{nh}:flags=lanczos',
                   '-frames:v', '1', '-q:v', '2', dst]
            with open(err_file, 'wb') as ef:
                proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=ef, timeout=120)
            if proc.returncode != 0:
                tail = ''
                try:
                    tail = open(err_file, 'r', encoding='utf-8', errors='ignore').read()[-300:]
                except Exception:
                    pass
                raise RuntimeError(f'ffmpeg 图片处理失败: {src_path}\n{tail}')
        except subprocess.TimeoutExpired:
            raise RuntimeError(f'ffmpeg 图片处理超时: {src_path}')
        finally:
            if tmp and os.path.exists(tmp):
                try:
                    os.remove(tmp)
                except Exception:
                    pass
            try:
                if os.path.exists(err_file):
                    os.remove(err_file)
            except Exception:
                pass
        return dst

    # ---------- 添加: 视频 (ffmpeg, GPU 加速) ----------
    def _ffprobe_size_duration(self, ffmpeg_dir, src):
        """用 ffprobe 取分辨率/时长/帧率; 失败返回 (None, None, None, None)"""
        fp = os.path.join(ffmpeg_dir, 'ffprobe.exe')
        if not os.path.exists(fp):
            return None, None, None, None
        try:
            cmd = [fp, '-v', 'error', '-select_streams', 'v:0',
                   '-show_entries', 'stream=width,height,duration,avg_frame_rate',
                   '-of', 'default=noprint_wrappers=1', short_path(src)]
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
            w = h = d = fps = None
            for line in out.splitlines():
                k, _, v = line.partition('=')
                if k == 'width': w = int(v)
                elif k == 'height': h = int(v)
                elif k == 'duration': d = float(v)
                elif k == 'avg_frame_rate' and v not in ('0/0', 'N/A'):
                    try:
                        num, _, den = v.partition('/')
                        num = float(num)
                        den = float(den) if den else 1.0
                        fps = num / den if den > 0 else None
                    except Exception:
                        fps = None
            if w and h:
                return (w, h, d, fps)
            return None, None, None, None
        except Exception:
            return None, None, None, None

    def add_video(self, src_path, on_progress=None):
        """视频 -> ffmpeg 短边320 H.264 高质量 -> 素材库 MP4。支持中文路径。
        短边=320: 1:1压成320x320, 非1:1保留长边像素(如16:9->569x320),
        三种适配模式发送时只会缩小不会放大, 杜绝填充模式发糊。"""
        ffmpeg = find_ffmpeg()
        if ffmpeg is None:
            raise RuntimeError('未找到 ffmpeg，请将 ffmpeg.exe 放入 tools 目录')
        ffdir = os.path.dirname(ffmpeg)

        # 中文/Unicode 路径: ffmpeg(MinGW) 无法解析, 先复制到 ASCII 临时文件
        tmp = None
        src_for_ff = src_path
        size = self._ffprobe_size_duration(ffdir, src_path)
        if size[0] is None:
            cache_dir = os.path.join(os.path.dirname(self.photos_dir), 'cache')
            os.makedirs(cache_dir, exist_ok=True)
            tmp = os.path.join(cache_dir, f'work_{uuid.uuid4().hex}.mp4')
            try:
                shutil.copy2(src_path, tmp)
            except OSError as e:
                raise RuntimeError(f'无法复制源视频: {src_path} ({e})')
            src_for_ff = tmp
            size = self._ffprobe_size_duration(ffdir, src_for_ff)
        if size[0] is None:
            if tmp and os.path.exists(tmp):
                os.remove(tmp)
            raise RuntimeError(f'无法探测视频: {src_path}')
        w, h, duration, src_fps = size
        prog_file = err_file = None
        try:
            # 短边 320: 1:1 -> 320x320; 非1:1 短边固定320, 长边按比例 (偶数校正)
            if h == w:
                nw, nh = SIZE, SIZE
            elif w > h:
                nw, nh = max(2, int(round(w / h * SIZE))), SIZE
            else:
                nw, nh = SIZE, max(2, int(round(h / w * SIZE)))
            if nw % 2: nw -= 1
            if nh % 2: nh -= 1

            # 2. 选编码器 (NVENC/QSV/AMF/libx264)
            #    NVENC 对宽度 <160 的窄视频打不开 -> 窄视频直接 CPU
            enc = detect_video_encoder(ffmpeg)
            if enc != 'libx264' and min(nw, nh) < 160:
                enc = 'libx264'
            dst = os.path.join(self.videos_dir, _ts_name(src_path).rsplit('.', 1)[0] + '.mp4')

            # 3. 转码参数: CRF18 高质量; 硬件编码器失败(驱动/尺寸限制)时自动兜底 libx264
            #    进度写文件轮询(pipe 会块缓冲导致进度条卡), stderr 写文件(管道不消费会阻塞大文件转码)
            cache_dir = os.path.join(os.path.dirname(self.photos_dir), 'cache')
            os.makedirs(cache_dir, exist_ok=True)
            enc_tries = [enc, 'libx264'] if enc != 'libx264' else ['libx264']
            last_err = None
            prog_file = os.path.join(cache_dir, f'prog_{uuid.uuid4().hex}.txt')
            err_file = os.path.join(cache_dir, f'err_{uuid.uuid4().hex}.txt')
            for enc_try in enc_tries:
                preset = 'p7' if enc_try in ('h264_nvenc', 'h264_qsv', 'h264_amf') else 'slow'
                cmd = [ffmpeg, '-hide_banner', '-y', '-i', src_for_ff,
                       '-vf', f'scale={nw}:{nh}:flags=lanczos']
                if src_fps and src_fps > 60.5:
                    cmd += ['-r', '60']   # 高帧率视频一律压到 60fps
                # 画质优先: 各编码器用各自高质量参数(CRF/CQ/global_quality 15 ≈ 接近无损)
                if enc_try == 'h264_nvenc':
                    cmd += ['-c:v', enc_try, '-cq', '15', '-preset', preset,
                            '-b:v', '0']
                elif enc_try == 'h264_qsv':
                    cmd += ['-c:v', enc_try, '-global_quality', '15', '-preset', preset]
                elif enc_try == 'h264_amf':
                    cmd += ['-c:v', enc_try, '-qp', '15', '-rc', 'cqp', '-preset', preset]
                else:
                    cmd += ['-c:v', enc_try, '-crf', '15', '-preset', preset,
                            '-x264-params', 'qpmin=0:qpmax=51']
                cmd += ['-an', '-pix_fmt', 'yuv420p',
                        '-progress', prog_file, dst]
                with open(err_file, 'wb') as ef:
                    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=ef)
                    last_t = -1
                    while proc.poll() is None:
                        try:
                            with open(prog_file, 'r', encoding='utf-8', errors='ignore') as pf:
                                lines = pf.read().splitlines()
                            for ln in reversed(lines):
                                if ln.startswith('out_time_ms='):
                                    t = int(ln.split('=', 1)[1]) / 1_000_000
                                    if abs(t - last_t) >= 0.5 and duration and duration > 0 and on_progress:
                                        last_t = t
                                        on_progress(min(int(t), int(duration)), int(duration))
                                    break
                        except Exception:
                            pass
                        time.sleep(0.2)
                    rc = proc.wait(timeout=600)
                # 结束后补一次最终进度(快视频轮询循环可能未执行)
                if rc == 0 and duration and duration > 0 and on_progress:
                    try:
                        with open(prog_file, 'r', encoding='utf-8', errors='ignore') as pf:
                            lines = pf.read().splitlines()
                        for ln in reversed(lines):
                            if ln.startswith('out_time_ms='):
                                t = int(ln.split('=', 1)[1]) / 1_000_000
                                on_progress(min(int(t), int(duration)), int(duration))
                                break
                    except Exception:
                        pass
                if rc == 0:
                    break
                try:
                    last_err = open(err_file, 'r', encoding='utf-8', errors='ignore').read()[-400:]
                except Exception:
                    last_err = ''
            else:
                raise RuntimeError(f'ffmpeg 转码失败: {last_err}')
        except FileNotFoundError:
            raise RuntimeError(f'ffmpeg 不存在: {ffmpeg}')
        except subprocess.TimeoutExpired:
            proc.kill()
            raise RuntimeError('ffmpeg 转码超时')
        finally:
            if tmp and os.path.exists(tmp):
                os.remove(tmp)
            for f in (prog_file, err_file):
                try:
                    if os.path.exists(f):
                        os.remove(f)
                except Exception:
                    pass
        return dst

    # ---------- 兼容回退: 纯 OpenCV 转码 (无 ffmpeg 时) ----------
    def add_video_opencv_fallback(self, src_path, on_progress=None):
        cap = cv2.VideoCapture(short_path(src_path))
        if not cap.isOpened():
            raise RuntimeError(f'无法打开视频: {src_path}')
        try:
            fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
            total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
            ok, frame = cap.read()
            if not ok:
                raise RuntimeError('视频没有可读帧')
            h, w = frame.shape[:2]
            if h == w:
                nh, nw = SIZE, SIZE
            elif w > h:
                nw, nh = max(1, int(round(w / h * SIZE))), SIZE
            else:
                nw, nh = SIZE, max(1, int(round(h / w * SIZE)))
            dst = os.path.join(self.videos_dir, _ts_name(src_path))
            writer = None
            for fourcc, ext in [('mp4v', '.mp4'), ('MJPG', '.avi')]:
                dst = os.path.splitext(dst)[0] + ext
                w4 = cv2.VideoWriter_fourcc(*fourcc)
                writer = cv2.VideoWriter(dst, w4, fps, (nw, nh))
                if writer.isOpened():
                    break
                writer.release()
                writer = None
            if writer is None:
                raise RuntimeError('没有可用的视频编码器')
            count = 0
            while True:
                ok, frame = cap.read()
                if not ok:
                    break
                frame = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LANCZOS4)
                writer.write(frame)
                count += 1
                if on_progress and (count % 30 == 0 or count == total):
                    on_progress(count, total)
            writer.release()
            if count == 0:
                os.remove(dst)
                raise RuntimeError('视频处理结果为空')
            return dst
        finally:
            cap.release()


# ---------- 显示适配模式 ----------
ROT_K = {90: cv2.ROTATE_90_CLOCKWISE, 180: cv2.ROTATE_180, 270: cv2.ROTATE_90_COUNTERCLOCKWISE}


def rotate_frame(bgr, rotation):
    """整体旋转一帧(含已叠加内容)。rotation: 0/90/180/270"""
    rot = int(rotation or 0) % 360
    if rot and rot in ROT_K:
        return cv2.rotate(bgr, ROT_K[rot])
    return bgr


def fit_frame(bgr, mode, size=SIZE, rotation=0):
    """把任意 BGR 帧按旋转+适配模式生成 size x size BGR 帧（发送前调用）
    rotation: 0/90/180/270 (发送前先旋转, 适配在上) """
    if mode not in FIT_MODES:
        mode = 'contain'
    bgr = rotate_frame(bgr, rotation)
    h, w = bgr.shape[:2]
    if mode == 'stretch':
        return cv2.resize(bgr, (size, size), interpolation=cv2.INTER_LINEAR)

    if mode == 'contain':
        scale = size / max(h, w)
        nh, nw = max(1, int(round(h * scale))), max(1, int(round(w * scale)))
        resized = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_LINEAR)
        canvas = np.zeros((size, size, 3), dtype=np.uint8)
        x0, y0 = (size - nw) // 2, (size - nh) // 2
        canvas[y0:y0 + nh, x0:x0 + nw] = resized
        return canvas

    # cover: 短边填满, 长边裁剪
    scale = size / min(h, w)
    nh, nw = max(1, int(round(h * scale))), max(1, int(round(w * scale)))
    resized = cv2.resize(bgr, (nw, nh), interpolation=cv2.INTER_LINEAR)
    x0, y0 = (nw - size) // 2, (nh - size) // 2
    # .copy() 转连续数组: 切片视图(QImage/编码器)无法直接消费
    return resized[y0:y0 + size, x0:x0 + size].copy()


def bgr_to_jpeg(bgr, quality=60):
    """BGR -> JPEG 字节（BGR 直接编码，禁止先转 RGB，否则红蓝互换）"""
    ok, buf = cv2.imencode('.jpg', bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
    if not ok:
        raise RuntimeError('JPEG 编码失败')
    return buf.tobytes()
