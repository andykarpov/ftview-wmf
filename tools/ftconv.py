#!/usr/bin/env python3
"""ftconv: any video -> MJPEG AVI for the AVI player of FTVIEW.WMF (FT812).

Python 3.8+, standard library only; the transcoding is done by FFmpeg
(ffmpeg and ffprobe: --ffmpeg DIR, $FTCONV_FFMPEG, next to this program, or
PATH).

The video recipe is the VDAC2 mode of FTViewConvert (Wild Commander Improved,
Andrew Lazarev, MIT, LICENSE.WCI): MJPEG baseline 4:2:0 with the standard
Huffman tables and his quantisation matrix, ~2 Mbit/s, constant frame rate,
sides a multiple of 16, area up to 512x384; the loudness normalisation and the
seek table (the FTVIEWI1 JUNK chunk) are ported from it too. Added here: the
IMA ADPCM sound for the ESP32 (ZiFi32) and --check, which reads an AVI the way
avi_probe / index_build of the player do (src/avi_index.c) and tells how it
will play.

  ftconv.py film.mkv                 -> "film.avi" next to it (PCM 8 bit, GS)
  ftconv.py --audio esp film.mkv     -> IMA ADPCM for the ESP32
  ftconv.py --check film.avi         -> how the player takes a file
"""

import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys

VERSION = "1.0.0"

# The player (src/avi_index.c, src/avi_shared.h, src/gs_zx.c)
AVI_RESULT = 0xDFFFC          # two RGB565 frames below the decoder result word
PCM_BASE = 0xC0000            # the FT812 sound ring
AVI_INDEX = 0xD0000           # the seek table in RAM_G
AVI_INDEX_MAX = 5460
GS_INT_HZ = 37500             # the GS interrupt: PCM 8 to GS up to this rate

# The recipe (FTViewConvert, VDAC2)
MAX_AREA = 512 * 384          # 196608: frame by frame with any sound sink
MAX_W, MAX_H = 1024, 768
ASPECT_TOL, MIN_AREA_SHARE = 0.005, 0.6
INTRA_MATRIX = ("8,10,11,20,23,24,46,54,10,10,20,22,24,46,54,59,11,20,23,24,46,54,54,61,"
                "20,20,23,43,46,54,59,96,20,23,43,46,51,56,96,115,23,43,46,51,56,96,115,139,"
                "42,43,46,54,91,110,134,166,43,46,56,91,110,134,166,199")
CROP_FROM = 1.334
CROP_FILTER = "crop=w='if(gt(dar,1.334),ih*4/3/sar,iw)',"
TARGET_LUFS, MAX_LIMIT_DB = -12.0, 2.0
OPENDML_WARN = 1000 * 1024 * 1024   # ffmpeg goes OpenDML near 1 GB; idx1 covers the first RIFF


class Fail(Exception):
    pass


# --- FFmpeg -----------------------------------------------------------------

def find_tool(name, hint):
    exe = name + (".exe" if os.name == "nt" else "")
    dirs = []
    for d in (hint, os.environ.get("FTCONV_FFMPEG")):
        if d:
            if os.path.isfile(d):   # the path of ffmpeg itself
                d = os.path.dirname(d)
            dirs.append(d)
    here = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__))
    dirs.append(here)
    for d in dirs:
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    p = shutil.which(name)
    if p:
        return p
    raise Fail(name + " not found: install FFmpeg (see README) or pass --ffmpeg DIR")


def run(cmd, capture=True):
    return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=capture,
                          text=True, encoding="utf-8", errors="replace")


def ffmpeg_has_fps_mode(ffmpeg):
    out = run([ffmpeg, "-hide_banner", "-h", "long"]).stdout
    return "-fps_mode" in out


class Source:
    pass


def probe(ffprobe, path):
    r = run([ffprobe, "-v", "error", "-print_format", "json", "-show_streams", "-show_format", path])
    if r.returncode:
        raise Fail("ffprobe cannot read %s: %s" % (path, r.stderr.strip()))
    info = json.loads(r.stdout)
    streams = info.get("streams", [])
    video = [s for s in streams if s.get("codec_type") == "video"
             and not s.get("disposition", {}).get("attached_pic")]
    if not video:
        raise Fail("%s: no video stream" % path)
    v = video[0]
    s = Source()
    s.width, s.height = int(v["width"]), int(v["height"])
    sar = v.get("sample_aspect_ratio", "1:1")
    try:
        n, d = (int(x) for x in sar.split(":"))
        sar = n / d if n and d else 1.0
    except ValueError:
        sar = 1.0
    rot = 0
    for sd in v.get("side_data_list", []):
        if "rotation" in sd:
            rot = int(round(float(sd["rotation"])))
    if not rot and "rotate" in v.get("tags", {}):
        rot = int(v["tags"]["rotate"])
    s.rotation = rot % 360
    if s.rotation in (90, 270):     # ffmpeg turns the frame before -vf
        s.width, s.height, sar = s.height, s.width, 1 / sar
    s.aspect = s.width * sar / s.height
    s.fps = v.get("avg_frame_rate", "0/0")
    s.duration = float(info.get("format", {}).get("duration", 0) or 0)
    s.has_audio = any(x.get("codec_type") == "audio" for x in streams)
    return s


def output_size(s, max_area):
    """FTViewConvert OutputSizeVdac2: sides of 16, area <= max_area, not larger than
    the source; the largest within 0.5 % of the aspect among those of 60 % of the
    largest area, else the most exact."""
    cap_w = max(16, math.ceil(s.height * s.aspect / 16.0) * 16)
    cap_h = max(16, math.ceil(s.height / 16.0) * 16)

    def height(w):
        return int(round(w / s.aspect / 16.0)) * 16   # C# Math.Round: to even, as Python

    def valid(w):
        h = height(w)
        return 16 <= h <= MAX_H and w * h <= max_area and w <= cap_w and h <= cap_h

    widths = [w for w in range(16, MAX_W + 1, 16) if valid(w)]
    if not widths:
        return 16, 16
    max_a = max(w * height(w) for w in widths)
    tol = near = None
    tol_err, near_err = 0.0, float("inf")
    for w in widths:
        h = height(w)
        area = w * h
        if area < MIN_AREA_SHARE * max_a:
            continue
        err = abs(w / h - s.aspect) / s.aspect
        if err <= ASPECT_TOL and (tol is None or area > tol[0] * tol[1] or
                                  (area == tol[0] * tol[1] and err < tol_err)):
            tol, tol_err = (w, h), err
        if err < near_err or (err == near_err and area > near[0] * near[1]):
            near, near_err = (w, h), err
    return tol or near


def crop43(s):
    if s.aspect <= CROP_FROM:
        return s
    c = Source()
    c.__dict__.update(s.__dict__)
    c.width = int(round(s.width * (4.0 / 3) / s.aspect))
    c.aspect = 4.0 / 3
    return c


def audio_prepare(a):
    return "aformat=sample_fmts=flt:channel_layouts=%s,aresample=%d" % (
        "stereo" if a.stereo else "mono", a.rate)


def loudness_gain(ffmpeg, path, a):
    """FTViewConvert: the peak to 0 dB; a record quieter than -12 LUFS after that
    goes up to 2 dB more (the limiter takes the peaks). None: silence / failed."""
    cut = (["-ss", "%.3f" % a.start] if a.start else []) + (["-t", "%.3f" % a.length] if a.length else [])
    r = run([ffmpeg, "-hide_banner", "-nostdin", "-nostats"] + cut + ["-i", path, "-map", "0:a:0", "-vn",
             "-af", audio_prepare(a) + ",astats=measure_perchannel=none:measure_overall=Peak_level,"
             "ebur128=framelog=quiet", "-f", "null", "-"])
    if r.returncode:
        return None
    peak = re.findall(r"Peak level dB:\s*([-+]?[0-9.]+)", r.stderr)
    loud = re.findall(r"\bI:\s*([-+]?[0-9.]+) LUFS", r.stderr)
    if not peak:
        return None
    normalize = -float(peak[-1])
    if not loud:
        return normalize
    return max(normalize, min(TARGET_LUFS - float(loud[-1]), normalize + MAX_LIMIT_DB))


def ffmpeg_args(ffmpeg, src_path, out_path, size, a, gain, crop):
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-y", "-nostats", "-progress", "pipe:1"]
    if a.start:
        cmd += ["-ss", "%.3f" % a.start]
    if a.length:
        cmd += ["-t", "%.3f" % a.length]
    cmd += ["-i", src_path, "-map", "0:v:0"]
    if a.audio != "none":
        cmd += ["-map", "0:a:0"]
    # Only the two streams, video first: avi_probe wants video as stream 1, sound as 2.
    cmd += ["-sn", "-dn", "-map_metadata", "-1",
            "-vf", "fps=%d,%sscale=%d:%d:flags=lanczos,setsar=1,format=yuvj420p" % (
                a.fps, CROP_FILTER if crop else "", size[0], size[1])]
    # cfr: a gap of the source time stamps becomes a copy of the frame (every
    # video chunk a whole JPEG), not empty 00dc chunks.
    cmd += ["-fps_mode", "cfr"] if ffmpeg_has_fps_mode(ffmpeg) else ["-vsync", "cfr"]
    cmd += ["-avoid_negative_ts", "disabled",
            "-c:v", "mjpeg", "-huffman", "default", "-intra_matrix", INTRA_MATRIX,
            "-b:v", "%dk" % a.bitrate, "-maxrate", "%dk" % (a.bitrate * 13 // 10),
            "-bufsize", "%dk" % a.bitrate]
    if a.audio != "none":
        af = audio_prepare(a)
        if gain is not None:
            af += ",volume=%.4fdB,alimiter=limit=1:attack=50:release=1000:level=0:latency=1" % gain
        cmd += ["-af", af, "-ar", str(a.rate), "-ac", "2" if a.stereo else "1"]
        if a.audio == "gs":
            cmd += ["-c:a", "pcm_u8"]
        else:
            cmd += ["-c:a", "adpcm_ima_wav", "-block_size", "1024"]
    cmd += ["-f", "avi", out_path]
    return cmd


def transcode(cmd, duration):
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, encoding="utf-8", errors="replace")
    err = []
    import threading
    t = threading.Thread(target=lambda: err.extend(p.stderr))
    t.start()
    for line in p.stdout:
        if line.startswith("out_time_us=") and duration > 0 and sys.stderr.isatty():
            try:
                pct = min(100.0, int(line[12:]) / 1e4 / duration)
                sys.stderr.write("\r  %5.1f %%" % pct)
                sys.stderr.flush()
            except ValueError:
                pass
    p.wait()
    t.join()
    if sys.stderr.isatty():
        sys.stderr.write("\r" + " " * 12 + "\r")
    if p.returncode:
        raise Fail("ffmpeg failed:\n" + "".join(err[-20:]))


# --- AVI as the player reads it -------------------------------------------------

def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Avi:
    """The fields of avi_probe (src/avi_index.c) and the verdict."""

    def __init__(self, path):
        self.path = path
        self.size = os.path.getsize(path)
        with open(path, "rb") as f:
            self.head = f.read(min(self.size, 1 << 20))
        self.problems = []      # why not frame by frame
        self.notes = []
        self.parse()

    def parse(self):
        b = self.head
        self.ok_riff = len(b) >= 12 and b[:4] == b"RIFF" and b[8:12] == b"AVI "
        self.period = self.total = self.width = self.height = 0
        self.scale = self.rate = 0
        self.vcodec = None
        self.video_stream = self.audio_stream = 0
        self.wfx = None
        self.a_scale = self.a_rate = self.a_len = self.a_ss = 0
        self.movi_start = self.movi_end = 0
        if not self.ok_riff:
            return
        riff = u32(b, 4)
        self.riffs_more = riff + 8 < self.size
        ends = [min(riff + 8, self.size)]
        pos, stream, kind = 12, 0, 0
        while True:
            while len(ends) > 1 and pos == ends[-1]:
                ends.pop()
            if pos + 12 > len(b) or pos > ends[-1] or ends[-1] - pos < 8:
                self.problems.append("the header is broken or longer than 1 MB")
                return
            tag, size = b[pos:pos + 4], u32(b, pos + 4)
            pos += 8
            end = pos + size
            if tag == b"LIST":
                sub = b[pos:pos + 4]
                if sub == b"movi":
                    self.movi_start, self.movi_end = pos + 4, end
                    return
                if sub in (b"hdrl", b"strl"):
                    if sub == b"strl":
                        stream += 1
                        kind = 0
                    ends.append(end)
                    pos += 4
                    continue
            elif tag == b"avih" and size >= 40:
                self.period, self.total = u32(b, pos), u32(b, pos + 16)
                self.width, self.height = u32(b, pos + 32), u32(b, pos + 36)
            elif tag == b"strh" and size >= 28:
                if b[pos:pos + 4] == b"vids":
                    self.vcodec = b[pos + 4:pos + 8].decode("latin-1")
                    if self.vcodec.lower() == "mjpg":
                        self.video_stream = stream
                        self.scale, self.rate = u32(b, pos + 20), u32(b, pos + 24)
                elif b[pos:pos + 4] == b"auds" and size >= 48:
                    kind = 1
                    self.audio_stream = stream
                    self.a_scale, self.a_rate = u32(b, pos + 20), u32(b, pos + 24)
                    self.a_start, self.a_len = u32(b, pos + 28), u32(b, pos + 32)
                    self.a_ss = u32(b, pos + 44)
            elif tag == b"strf" and kind and size >= 16:
                self.wfx = struct.unpack_from("<HHIIHH", b, pos)
                kind = 0
            pos = end + (size & 1)

    def verdict(self):
        """(mode, sinks, seek): mode 'frame' (frame by frame, HUD, seek) or 'play'
        (CMD_PLAYVIDEO); sinks: where the sound goes in each case."""
        p = self.problems
        if not self.ok_riff:
            return "none", "not a RIFF AVI"
        if self.vcodec is None or self.vcodec.lower() != "mjpg":
            p.append("video codec %s, not MJPEG" % self.vcodec)
            return "none", "the FT812 decodes only MJPEG"
        if self.video_stream != 1:
            p.append("video is not the first stream")
        if not self.width or not self.height or self.width > 2047 or self.height > 2047:
            p.append("frame size %dx%d" % (self.width, self.height))
        px = self.width * self.height
        bytes_ = (px * 2 + 3) & ~3
        if not self.total:
            p.append("no frame count in avih")
        if not 1000 <= self.period <= 10000000:
            p.append("frame period %d us" % self.period)
        sound = self.wfx is not None
        # avi_probe wants an integer rate only with sound (the sound clock counts
        # samples a frame); a silent clip is paced by the period of avih.
        if sound and (self.scale != 1 or not 1 <= self.rate <= 1000):
            p.append("frame rate %d/%d is not an integer (use --fps)" % (self.rate, self.scale or 1))
        if bytes_ > AVI_RESULT // 2:
            p.append("frame %dx%d = %d points > 229375" % (self.width, self.height, px))
        self.sinks = {}
        if sound:
            tag, ch, rate, avg, align, bits = self.wfx
            self.audio_desc = "%s %d Hz %s" % (
                {1: "PCM %d bit" % bits, 0x11: "IMA ADPCM", 6: "A-law", 7: "u-law", 0x55: "MP3",
                 0xFF: "AAC"}.get(tag, "format 0x%04X" % tag), rate, {1: "mono", 2: "stereo"}.get(ch, "%d ch" % ch))
            if self.audio_stream != 2:
                p.append("the sound is not the second stream")
            if self.a_start:
                p.append("the sound stream starts at %d, not 0" % self.a_start)
            pcm_ok = tag == 1 and ch == 1 and bits == 8 and rate == self.a_rate and self.a_scale == 1
            esp_ok = tag in (1, 0x11, 6, 7) and ch <= 2
            byte_rate = self.a_rate * self.a_ss // self.a_scale if self.a_scale else 0
            gs = pcm_ok and self.a_rate <= 48000 and self.a_rate <= GS_INT_HZ
            esp = esp_ok and self.a_ss == align and 0 < byte_rate <= 0xFFFF
            ft = pcm_ok and self.a_rate <= 48000
            if gs:
                self.sinks["GS"] = (self.a_rate, self.a_len)
            if esp:
                self.sinks["ESP32"] = (byte_rate, self.a_len * self.a_ss)
            if ft:
                self.sinks["FT812"] = (self.a_rate, self.a_len)
                if bytes_ > PCM_BASE // 2:
                    self.notes.append("frame > 196608 points: the FT812 sound (no GS, no ESP32) "
                                      "goes to CMD_PLAYVIDEO")
            if self.sinks and not self.a_len:
                p.append("the sound stream has no length in strh")
            if not self.sinks:
                why = "this sound" if not esp_ok else "%d bytes/s > 65535" % byte_rate
                p.append("%s: neither GS / FT812 (PCM 8 bit mono) nor the ESP32 (%s)" % (self.audio_desc, why))
        if bytes_ > AVI_INDEX // 2:
            self.notes.append("frame > 212992 points: no seek")
        if self.movi_start > 0x10000:
            self.notes.append("header longer than 64 KB: no seek")
        if self.riffs_more:
            # ffmpeg past 1 GB: avih and idx1 cover the first RIFF only (checked:
            # 8347 of 9600 frames), so the player ends the film there.
            p.append("OpenDML file over 1 GB: the player sees only the first %d frames "
                     "(%.0f s); split it (--start / --length) or lower --bitrate"
                     % (self.total, self.total * self.period / 1e6))
        return ("play" if p else "frame"), None

    def idx1(self):
        """(offset of the idx1 chunk, its data) or None, as index_build looks for it."""
        with open(self.path, "rb") as f:
            at = self.movi_end + (self.movi_end & 1)
            for _ in range(8):
                if at + 16 > self.size:
                    return None
                f.seek(at)
                h = f.read(16)
                size = u32(h, 4)
                if h[:4] == b"idx1":
                    if size > self.size - at - 8:
                        return None
                    f.seek(at + 8)
                    return at, f.read(size)
                if h[:4] == b"JUNK" and h[8:16] == b"FTVIEWI1":
                    self.embedded = True
                at += 8 + size + (size & 1)
        return None


def seek_table(avi, pcm_rate, pcm_total, idx):
    """index_build / index_scan of the player (FTViewConvert SeekIndex): entry k is
    the first idx1 chunk from which both frame k*step and the sound from byte
    k*step*pcm_rate/fps are in the stream: its absolute offset and the video
    chunks and sound bytes before it."""
    step = avi.total // AVI_INDEX_MAX + 1
    count = (avi.total + step - 1) // step
    fps = avi.rate
    pcm_on = pcm_total if pcm_rate else 0
    s_step = s_rem = 0
    if pcm_on:
        s_step = step * pcm_rate
        s_rem = s_step % fps
        s_step //= fps
    frame = sample = video = audio = base = rem = 0
    out = []
    for e in range(0, len(idx) - 15, 16):
        if len(out) >= count:
            break
        off, ln = u32(idx, e + 8), u32(idx, e + 12)
        if video == 0 and audio == 0 and not out:
            base = avi.movi_start - 4 if off < avi.movi_start else 0
        is_v = idx[e:e + 2] == b"00" and idx[e + 2:e + 3] == b"d" and idx[e + 3:e + 4] in (b"c", b"b")
        is_a = bool(pcm_on) and idx[e:e + 4] == b"01wb"
        while len(out) < count and ((is_v and video >= frame) or (is_a and audio + ln > sample)):
            out.append((base + off, video, audio))
            frame += step
            sample += s_step
            rem += s_rem
            if rem >= fps:
                rem -= fps
                sample += 1
        if is_v:
            video += 1
        if is_a:
            audio += ln
    return step, out


def embed_table(path):
    """The FTVIEWI1 JUNK chunk before idx1 (PCM 8 bit mono or no sound only: the
    table of the ESP32 sound counts bytes and is not checked against the player
    here, so such a file seeks through idx1). Number of entries, 0 = not embedded."""
    avi = Avi(path)
    mode, _ = avi.verdict()
    if mode != "frame" or avi.movi_start > 0x10000 or avi.riffs_more:
        return 0
    bytes_ = (avi.width * avi.height * 2 + 3) & ~3
    if bytes_ > AVI_INDEX // 2:
        return 0
    if avi.wfx is not None:
        if "GS" not in avi.sinks and "FT812" not in avi.sinks:
            return 0
        pcm_rate, pcm_total = avi.a_rate, avi.a_len
    else:
        pcm_rate = pcm_total = 0
    found = avi.idx1()
    if not found or getattr(avi, "embedded", False):
        return 0
    at, idx = found
    step, table = seek_table(avi, pcm_rate, pcm_total, idx)
    if not table:
        return 0
    body = b"FTVIEWI1" + struct.pack("<HHHIIIIIII", step, len(table), pcm_rate, avi.movi_start,
                                     avi.movi_end, avi.total, avi.rate, avi.scale, bytes_, pcm_total)
    body += b"\0\0" + b"".join(struct.pack("<III", *t) for t in table)
    with open(path, "r+b") as f:
        f.seek(at)
        tail = f.read()
        f.seek(at)
        f.write(b"JUNK" + struct.pack("<I", len(body)) + body + tail)
        f.seek(4)
        f.write(struct.pack("<I", at + len(body) + len(tail)))
    return len(table)


def report(path, out=None):
    out = out or sys.stdout
    avi = Avi(path)
    mode, why = avi.verdict()
    w = out.write
    w("%s\n" % path)
    if mode == "none":
        w("  not playable: %s\n" % "; ".join(avi.problems or [why]))
        return 2
    fps = "%g" % (avi.rate / avi.scale) if avi.scale else "?"
    w("  video: MJPEG %dx%d, %s fps, %d frames\n" % (avi.width, avi.height, fps, avi.total))
    if avi.wfx is not None:
        w("  sound: %s -> %s\n" % (avi.audio_desc, ", ".join(
            {"GS": "General Sound", "ESP32": "ESP32 (ZiFi32)", "FT812": "FT812"}[k]
            for k in ("GS", "ESP32", "FT812") if k in avi.sinks) or "none"))
    else:
        w("  sound: none\n")
    if mode == "frame":
        w("  plays frame by frame (HUD, pause, seek)\n")
        found = avi.idx1()
        if getattr(avi, "embedded", False):
            w("  seek table: FTVIEWI1 (fast first seek)\n")
        elif found:
            w("  seek table: built from idx1 at the first seek\n")
        else:
            w("  seek: no idx1, no seek\n")
    else:
        w("  plays by CMD_PLAYVIDEO (no HUD, no seek): %s\n" % "; ".join(avi.problems))
    for n in avi.notes:
        w("  note: %s\n" % n)
    if avi.size >= OPENDML_WARN:
        w("  note: file of %d MB\n" % (avi.size >> 20))
    return 0 if mode == "frame" else 1


# --- main ---------------------------------------------------------------------

def same_file(a, b):
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def convert(src_path, a, ffmpeg, ffprobe):
    s = probe(ffprobe, src_path)
    if a.start:
        s.duration = max(0.0, s.duration - a.start)
    if a.length:
        s.duration = min(s.duration, a.length) if s.duration else a.length
    has_audio = s.has_audio and a.audio != "none"
    shape = crop43(s) if a.crop43 else s
    size = output_size(shape, MAX_AREA)
    if a.output and os.path.isdir(a.output):
        out = os.path.join(a.output, os.path.splitext(os.path.basename(src_path))[0] + ".avi")
    elif a.output:
        out = a.output
    else:
        out = os.path.splitext(src_path)[0] + ".avi"
    # 3P.AVI -> 3P.avi is the same file on Windows, macOS and FAT.
    if os.path.exists(out) and same_file(out, src_path) or \
            os.path.normcase(os.path.abspath(out)).lower() == os.path.normcase(os.path.abspath(src_path)).lower():
        if a.output and not os.path.isdir(a.output):
            raise Fail("%s: the output is the input" % out)
        out = os.path.splitext(out)[0] + "-ft.avi"
    if os.path.exists(out) and not a.force:
        raise Fail("%s exists (--force to overwrite)" % out)
    snd = "none"
    if has_audio:
        snd = "%s %d Hz %s" % ("PCM 8 bit" if a.audio == "gs" else "IMA ADPCM", a.rate,
                               "stereo" if a.stereo else "mono")
    print("%s: %dx%d %.3f:1 -> %s: %dx%d, %d fps, sound %s" % (
        src_path, s.width, s.height, s.aspect, out, size[0], size[1], a.fps, snd), flush=True)
    audio_bps = 0
    if has_audio:
        audio_bps = a.rate if a.audio == "gs" else a.rate * (2 if a.stereo else 1) // 2
    estimate = s.duration * (a.bitrate * 1000 / 8 * 1.1 + audio_bps)
    if estimate > OPENDML_WARN:
        part = OPENDML_WARN / (a.bitrate * 1000 / 8 * 1.1 + audio_bps)
        raise Fail("%s: about %d MB: over 1 GB the player sees only the first part; convert it "
                   "in parts of up to %d s (--start / --length) or lower --bitrate"
                   % (src_path, estimate / 2 ** 20, max(1, int(part))))
    gain = None
    if has_audio and a.normalize:
        gain = loudness_gain(ffmpeg, src_path, a)
    b = a.audio
    if not has_audio:
        a = argparse.Namespace(**vars(a))
        a.audio = "none"
    transcode(ffmpeg_args(ffmpeg, src_path, out, size, a, gain, a.crop43), s.duration)
    if b == "gs" or not has_audio:
        n = embed_table(out)
        if n:
            print("  seek table: %d entries" % n)
    return report(out)


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")   # file names in a console of another code page
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(prog="ftconv", description="Video -> MJPEG AVI for FTVIEW.WMF (FT812).")
    ap.add_argument("inputs", nargs="+", metavar="FILE")
    ap.add_argument("-o", "--output", help="output file (one input) or folder")
    ap.add_argument("--audio", choices=("gs", "esp", "none"), default="gs",
                    help="gs: PCM 8 bit mono (General Sound, else ESP32 or FT812; default); "
                         "esp: IMA ADPCM (ESP32 only); none: no sound")
    ap.add_argument("--rate", type=int, help="sound sample rate (default 32000; gs up to 37500)")
    ap.add_argument("--stereo", action="store_true", help="stereo sound (esp only)")
    ap.add_argument("--fps", type=int, default=24, help="frame rate, an integer (default 24)")
    ap.add_argument("--bitrate", type=int, default=2000, help="video kbit/s (default 2000)")
    ap.add_argument("--start", type=float, default=0, metavar="SEC", help="start at this second of the source")
    ap.add_argument("--length", type=float, default=0, metavar="SEC", help="convert this many seconds")
    ap.add_argument("-f", "--force", action="store_true", help="overwrite an existing output")
    ap.add_argument("--crop43", action="store_true", help="crop the sides of a wide frame to 4:3")
    ap.add_argument("--no-normalize", dest="normalize", action="store_false",
                    help="keep the sound level as it is")
    ap.add_argument("--ffmpeg", help="folder of ffmpeg / ffprobe (or the path of ffmpeg)")
    ap.add_argument("--check", action="store_true", help="only tell how the player takes these AVI files")
    ap.add_argument("--no-pause", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--version", action="version", version="ftconv " + VERSION)
    a = ap.parse_args(argv)
    rc = 0
    try:
        if a.check:
            for p in a.inputs:
                rc = max(rc, report(p))
            return rc
        if a.rate is None:
            a.rate = 32000
        if a.audio == "gs" and a.stereo:
            raise Fail("--stereo: PCM 8 bit for GS / FT812 is mono only")
        if a.audio == "gs" and not 4000 <= a.rate <= GS_INT_HZ:
            raise Fail("--rate: 4000..37500 for gs")
        if a.audio == "esp" and not 8000 <= a.rate <= 48000:
            raise Fail("--rate: 8000..48000 for esp")
        if not 1 <= a.fps <= 50:
            raise Fail("--fps: 1..50")
        if a.output and a.output.endswith(("/", os.sep)):
            os.makedirs(a.output, exist_ok=True)
        if a.output and len(a.inputs) > 1 and not os.path.isdir(a.output):
            raise Fail("-o must be a folder for several inputs")
        ffmpeg = find_tool("ffmpeg", a.ffmpeg)
        ffprobe = find_tool("ffprobe", a.ffmpeg)
        for p in a.inputs:
            rc = max(rc, convert(p, a, ffmpeg, ffprobe))
    except Fail as e:
        sys.stderr.write("ftconv: %s\n" % e)
        rc = 2
    except KeyboardInterrupt:
        rc = 130
    return rc


def own_console():
    """Windows: the console was opened for this program alone (a file dropped on
    the .exe, a double click), not a cmd / PowerShell window it runs in."""
    if os.name != "nt" or not sys.stdin or not sys.stdin.isatty():
        return False
    try:
        import ctypes
        ids = (ctypes.c_uint * 4)()
        # the PyInstaller boot loader and Python itself
        return ctypes.windll.kernel32.GetConsoleProcessList(ids, 4) <= 2
    except Exception:
        return False


if __name__ == "__main__":
    rc = main()
    # A Windows binary started by dropping a file on it: keep the window open.
    if getattr(sys, "frozen", False) and "--no-pause" not in sys.argv and own_console():
        try:
            input("Press Enter to close")
        except EOFError:
            pass
    sys.exit(rc)
