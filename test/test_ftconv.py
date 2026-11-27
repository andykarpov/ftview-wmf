#!/usr/bin/env python3
"""tools/ftconv.py: short lavfi clips (testsrc2 + sine) of several shapes and
frame rates converted with each sound profile; the result must play frame by
frame (the conditions of avi_probe), the frame size is the one of FTViewConvert,
and the embedded FTVIEWI1 table is checked against the definition of an entry
(the first idx1 chunk from which frame k*step and the sound from byte
k*step*rate/fps are in the stream), worked out chunk by chunk here. Needs
ffmpeg / ffprobe in PATH (skipped without them)."""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import ftconv  # noqa: E402

errors = 0


def check(cond, what):
    global errors
    if not cond:
        errors += 1
        print("FAIL:", what)


def make_source(path, w, h, rate, seconds=3, audio=True, sar=None):
    vf = "testsrc2=size=%dx%d:rate=%s:duration=%d" % (w, h, rate, seconds)
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", vf]
    if audio:
        cmd += ["-f", "lavfi", "-i", "sine=frequency=440:duration=%d:sample_rate=48000" % seconds, "-ac", "2"]
    if sar:
        cmd += ["-vf", "setsar=%s" % sar]
    cmd += ["-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"]
    if audio:
        cmd += ["-c:a", "aac"]
    subprocess.run(cmd + [path], check=True)


def chunks(idx):
    for e in range(0, len(idx) - 15, 16):
        yield idx[e:e + 4], struct.unpack_from("<I", idx, e + 8)[0], struct.unpack_from("<I", idx, e + 12)[0]


def embedded(path, avi):
    """The FTVIEWI1 chunk: (step, pcm fields, entries) or None."""
    with open(path, "rb") as f:
        at = avi.movi_end + (avi.movi_end & 1)
        f.seek(at)
        h = f.read(8 + 46)
        if h[:4] != b"JUNK" or h[8:16] != b"FTVIEWI1":
            return None
        size = struct.unpack_from("<I", h, 4)[0]
        f.seek(at + 8)
        body = f.read(size)
        nxt = f.read(4)
    step, count = struct.unpack_from("<HH", body, 8)
    fields = struct.unpack_from("<HIIIIIII", body, 12)
    entries = [struct.unpack_from("<III", body, 44 + 12 * k) for k in range(count)]
    return step, count, fields, entries, nxt, len(body)


def expected_entries(avi, idx, step, pcm_rate):
    count = (avi.total + step - 1) // step
    out = []
    j0 = 0
    ck = list(chunks(idx))
    base = avi.movi_start - 4 if ck and ck[0][1] < avi.movi_start else 0
    for k in range(count):
        frame = k * step
        sample = k * step * pcm_rate // avi.rate if pcm_rate else 0
        video = sum(1 for t, _, _ in ck[:j0] if t in (b"00dc", b"00db"))
        audio = sum(n for t, _, n in ck[:j0] if t == b"01wb") if pcm_rate else 0
        for j in range(j0, len(ck)):
            t, off, n = ck[j]
            if (t in (b"00dc", b"00db") and video >= frame) or \
               (pcm_rate and t == b"01wb" and audio + n > sample):
                out.append((base + off, video, audio))
                j0 = j
                break
            if t in (b"00dc", b"00db"):
                video += 1
            if pcm_rate and t == b"01wb":
                audio += n
        else:
            break
    return out


def run_case(tmp, name, w, h, rate, args, want_size, audio=True, sar=None, want_sink=None):
    src = os.path.join(tmp, name + ".mp4")
    out = os.path.join(tmp, name + ".avi")
    make_source(src, w, h, rate, audio=audio, sar=sar)
    with open(os.devnull, "w") as null:
        old = sys.stdout
        sys.stdout = null
        try:
            rc = ftconv.main(args + ["-o", out, src])
        finally:
            sys.stdout = old
    check(rc == 0, "%s: ftconv rc %d" % (name, rc))
    avi = ftconv.Avi(out)
    mode, _ = avi.verdict()
    check(mode == "frame", "%s: not frame by frame: %s" % (name, avi.problems))
    check((avi.width, avi.height) == want_size, "%s: size %dx%d, want %dx%d" % (
        name, avi.width, avi.height, want_size[0], want_size[1]))
    check(avi.width % 16 == 0 and avi.height % 16 == 0, "%s: sides not of 16" % name)
    fps = int(args[args.index("--fps") + 1]) if "--fps" in args else 24
    check(avi.scale == 1 and avi.rate == fps, "%s: rate %d/%d" % (name, avi.rate, avi.scale))
    if want_sink:
        check(list(avi.sinks) == want_sink, "%s: sinks %s, want %s" % (name, list(avi.sinks), want_sink))
    found = avi.idx1()
    check(found is not None, "%s: no idx1" % name)
    emb = embedded(out, avi)
    esp = "--audio" in args and args[args.index("--audio") + 1] == "esp"
    if esp:
        check(emb is None, "%s: a table in an ESP file" % name)
        return
    check(emb is not None, "%s: no FTVIEWI1 table" % name)
    if emb is None or found is None:
        return
    step, count, fields, entries, nxt, size = emb
    check(nxt == b"idx1", "%s: idx1 does not follow the table" % name)
    pcm_rate = avi.a_rate if avi.wfx else 0
    pcm_total = avi.a_len if avi.wfx else 0
    bytes_ = (avi.width * avi.height * 2 + 3) & ~3
    check(fields == (pcm_rate, avi.movi_start, avi.movi_end, avi.total, avi.rate, avi.scale, bytes_, pcm_total),
          "%s: fields %s" % (name, fields))
    check(step == avi.total // ftconv.AVI_INDEX_MAX + 1, "%s: step %d" % (name, step))
    check(size == 44 + 12 * count, "%s: chunk size %d" % (name, size))
    want = expected_entries(avi, found[1], step, pcm_rate)
    check(entries == want, "%s: entries differ (%d / %d)" % (name, len(entries), len(want)))
    # Each entry points at a chunk of the movi list (as index_seek checks; entry 0
    # is the first chunk at movi_start, which index_seek never takes: frame 0 is
    # the start of the file).
    check(entries[0][0] == avi.movi_start and
          all(avi.movi_start < e[0] < avi.movi_end and not e[0] & 1 for e in entries[1:]),
          "%s: an entry outside movi" % name)
    with open(out, "rb") as f:
        riff = struct.unpack("<4sI", f.read(8))
    check(riff[1] + 8 == os.path.getsize(out), "%s: RIFF size" % name)
    # The table is not embedded twice.
    check(ftconv.embed_table(out) == 0, "%s: embedded again" % name)


def sizes():
    s = ftconv.Source()
    cases = [((1920, 1080, 1.0), (512, 288)), ((640, 480, 1.0), (512, 384)),
             ((320, 240, 1.0), (320, 240)), ((720, 576, 64 / 45), (512, 288)),
             ((1920, 800, 1.0), (656, 272)), ((1920, 804, 1.0), (576, 240)), ((1080, 1920, 1.0), (288, 512))]
    for (w, h, sar), want in cases:
        s.width, s.height, s.aspect = w, h, w * sar / h
        got = ftconv.output_size(s, ftconv.MAX_AREA)
        check(got == want, "size of %dx%d sar %.3f: %s, want %s" % (w, h, sar, got, want))
    s.width, s.height, s.aspect = 1920, 1080, 16 / 9
    check(ftconv.output_size(ftconv.crop43(s), ftconv.MAX_AREA) == (512, 384), "crop 4:3 size")


def main():
    sizes()
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("test_ftconv: ffmpeg not found, conversion skipped")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            run_case(tmp, "wide", 1280, 720, "30000/1001", [], (512, 288), want_sink=["GS", "ESP32", "FT812"])
            run_case(tmp, "pal", 720, 576, "25", ["--fps", "25", "--crop43"], (512, 384), sar="16/15")
            run_case(tmp, "esp", 640, 360, "24", ["--audio", "esp"], (512, 288), want_sink=["ESP32"])
            run_case(tmp, "esp2", 640, 360, "24", ["--audio", "esp", "--stereo", "--rate", "44100"],
                     (512, 288), want_sink=["ESP32"])
            run_case(tmp, "mute", 400, 300, "24", [], (384, 288), audio=False)
            # The output is never the input (X.AVI -> X.avi is one file on FAT / NTFS /
            # APFS) and an existing output is kept without --force.
            up = os.path.join(tmp, "UP.AVI")
            shutil.copy(os.path.join(tmp, "wide.avi"), up)
            quiet = open(os.devnull, "w")
            old, sys.stdout = sys.stdout, quiet
            olderr, sys.stderr = sys.stderr, quiet
            try:
                rc1 = ftconv.main(["--no-normalize", up])
                rc2 = ftconv.main(["--no-normalize", up])
                rc3 = ftconv.main(["--no-normalize", "-f", up])
                rc4 = ftconv.main(["--no-normalize", "-o", up, up])
            finally:
                sys.stdout, sys.stderr = old, olderr
                quiet.close()
            check(rc1 == 0 and os.path.exists(os.path.join(tmp, "UP-ft.avi")), "same name: no UP-ft.avi")
            check(rc2 == 2 and rc3 == 0, "existing output: rc %d / %d with -f" % (rc2, rc3))
            check(rc4 == 2, "-o the input: rc %d" % rc4)
            # Silent 29.97 fps: avi_probe wants an integer rate only with sound.
            nt = os.path.join(tmp, "ntsc.avi")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            "testsrc2=size=320x240:rate=30000/1001:duration=2", "-c:v", "mjpeg", nt], check=True)
            avi = ftconv.Avi(nt)
            check(avi.verdict()[0] == "frame", "silent 29.97: %s" % avi.problems)
            # A long clip: step > 1 needs more than 5460 frames; 4 minutes of a tiny frame.
            src = os.path.join(tmp, "long.mp4")
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            "color=c=gray:size=64x48:rate=24:duration=240", "-f", "lavfi", "-i",
                            "sine=duration=240", "-c:v", "libx264", "-preset", "ultrafast",
                            "-c:a", "aac", src], check=True)
            out = os.path.join(tmp, "long.avi")
            with open(os.devnull, "w") as null:
                old, sys.stdout = sys.stdout, null
                try:
                    ftconv.main(["--no-normalize", "--bitrate", "100", "-o", out, src])
                finally:
                    sys.stdout = old
            avi = ftconv.Avi(out)
            avi.verdict()
            emb = embedded(out, avi)
            check(emb is not None and emb[0] == 2, "long: step %s" % (emb and emb[0]))
            if emb:
                check(emb[3] == expected_entries(avi, avi.idx1()[1], emb[0], avi.a_rate), "long: entries")
    print("test_ftconv: %d errors" % errors)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
