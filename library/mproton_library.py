#!/usr/bin/env python3
"""mproton Library: a local web UI that turns testing Windows Steam games into clicks.

Python standard library only. Serves http://127.0.0.1:<port>/ and drives
launcher/mproton the same way docs/running.md does by hand. See library/README.md.
"""

import argparse
import concurrent.futures
import datetime
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
import zipfile
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
HOME = Path.home()

STATE_DIR = Path(os.environ.get("MPROTON_LIBRARY_HOME",
                                HOME / "Library/Application Support/mproton-library"))
STEAM_ROOT = Path(os.environ.get("MPROTON_STEAM_ROOT", HOME / "Library/Application Support/Steam"))
STEAM_MACOS = STEAM_ROOT / "Steam.AppBundle/Steam/Contents/MacOS"
DEV_CFG = STEAM_MACOS / "steam_dev.cfg"
CONTENT_DIR = STEAM_MACOS / "steamapps/content"
PREFIXES = Path(os.environ.get("MPROTON_PREFIXES", HOME / "mproton-prefixes"))
REPORTS = Path(os.environ.get("MPROTON_REPORTS", HOME / "mproton-reports"))
LOG_DIR = HOME  # the launcher writes ~/steam-<SteamGameId>.log (PROTON_LOG_DIR unset)

FORCE_LINE = "@sSteamCmdForcePlatformType windows"
FORCE_KEY = "@ssteamcmdforceplatformtype"

BACKENDS = {
    # name: (label, env vars). From launcher/README.md "Backends as compatdb rules".
    "default": ("Default (mach9 built-in: DXMT / vkd3d-proton / mtld3d)", {}),
    "dxmt": ("DXMT", {"PROTON_USE_DXMT": "1"}),
    "dxvk": ("DXVK", {"PROTON_USE_DXVK": "1"}),
    "wined3d": ("wined3d", {"PROTON_USE_WINED3D": "1"}),
}
# Inherited from the shell that started the library; never let these leak into a game.
SCRUB_ENV = ["PROTON_USE_DXMT", "PROTON_USE_DXVK", "PROTON_USE_WINED3D", "PROTON_USE_WINED3D11",
             "PROTON_USE_D3DMETAL", "PROTON_USE_MTLD3D", "MPROTON_DXGI", "MPROTON_D3D9",
             "WINEDEBUG", "WINEPREFIX", "STEAM_COMPAT_DATA_PATH", "SteamAppId", "SteamGameId",
             "PROTON_LOG", "PROTON_LOG_DIR"]

HELPER_EXE = re.compile(r"(crashhandler|crashreport|unins|setup|vcredist|vc_redist|dxsetup|"
                        r"dotnet|directx|ue4prereq|ueprereq|easyanticheat|battleye|launcherhelper)",
                        re.I)

_lock = threading.RLock()


# ---------------------------------------------------------------- utilities

def now_iso():
    return datetime.datetime.now().isoformat(timespec="seconds")


def read_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json_atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def parse_vdf(text):
    """Parse Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf) into nested dicts."""
    tokens = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])|(//[^\n]*)', text)
    root, stack, key = {}, [], None
    cur = root
    for quoted, brace, comment in tokens:
        if comment:
            continue
        if brace == "{":
            new = {}
            if key is not None:
                cur[key] = new
            stack.append(cur)
            cur, key = new, None
        elif brace == "}":
            cur = stack.pop() if stack else root
            key = None
        else:
            s = quoted.replace('\\"', '"').replace("\\\\", "\\")
            if key is None:
                key = s
            else:
                cur[key] = s
                key = None
    return root


def parse_env_lines(text):
    """'KEY=VALUE' per line; blank lines and '#' comments ignored. Returns (dict, errors)."""
    env, errors = {}, []
    for n, line in enumerate((text or "").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        k, sep, v = line.partition("=")
        k = k.strip()
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k):
            errors.append(f"line {n}: expected KEY=VALUE, got {line!r}")
            continue
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
            v = v[1:-1]
        env[k] = v
    return env, errors


# ---------------------------------------------------------------- settings

SETTINGS_FILE = STATE_DIR / "settings.json"


def load_settings():
    s = read_json(SETTINGS_FILE, {})
    s.setdefault("paths", {})
    s.setdefault("games", {})
    s.setdefault("dev_cfg", {})
    return s


def save_settings(s):
    with _lock:
        write_json_atomic(SETTINGS_FILE, s)


def game_settings(s, appid):
    g = dict(s["games"].get(str(appid), {}))
    g.setdefault("exe", "")          # manual override, absolute path or relative to the game dir
    g.setdefault("args", "")
    g.setdefault("backend", "default")
    g.setdefault("winedebug", "")
    g.setdefault("env", "")
    return g


def _first_existing(candidates, check):
    for c in candidates:
        if c and check(Path(c).expanduser()):
            return str(Path(c).expanduser())
    return None


def resolve_paths(s=None):
    """Where the launcher, Proton's python modules and the mach9 bundle are.

    Order: settings.json "paths" > environment > this checkout > ~/mproton. A git worktree has
    launcher/ but not the untracked ref/ and work/ trees, hence the ~/mproton fallback.
    """
    s = s or load_settings()
    p = s.get("paths", {})
    roots = [REPO, HOME / "mproton"]
    launcher = _first_existing([p.get("launcher")] + [r / "launcher/mproton" for r in roots],
                               lambda x: x.is_file())
    proton_ref = _first_existing([p.get("proton_ref")] + [r / "ref/proton" for r in roots],
                                 lambda x: (x / "filelock.py").is_file() or x.is_dir())
    wine_dir = _first_existing([p.get("wine_dir"), os.environ.get("MPROTON_WINE_DIR")]
                               + [r / "work/mach9/dist/wine" for r in roots],
                               lambda x: (x / "bin/wine").exists())
    python = p.get("python") or sys.executable
    return {"launcher": launcher, "proton_ref": proton_ref, "wine_dir": wine_dir, "python": python,
            "prefixes": str(PREFIXES), "reports": str(REPORTS), "steam_root": str(STEAM_ROOT),
            "state_dir": str(STATE_DIR)}


# ---------------------------------------------------------------- steam launch config (steamcmd)

CACHE_DIR = STATE_DIR / "cache/steamcmd"
_fetch_failures = {}  # appid -> time of last failure (in memory, retried after 10 min)


def steamcmd_info(appid, refresh=False, offline=False):
    """api.steamcmd.net /v1/info/<appid> 'data.<appid>' object, cached on disk."""
    appid = str(appid)
    cache = CACHE_DIR / f"{appid}.json"
    if not refresh:
        cached = read_json(cache, None)
        if cached is not None:
            return cached.get("data")
    if offline or (not refresh and time.time() - _fetch_failures.get(appid, 0) < 600):
        return None
    url = f"https://api.steamcmd.net/v1/info/{appid}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "mproton-library/1"})
        with urllib.request.urlopen(req, timeout=15) as r:
            doc = json.load(r)
        data = doc.get("data", {}).get(appid)
        if doc.get("status") != "success" or not isinstance(data, dict):
            raise ValueError(f"unexpected reply: status={doc.get('status')!r}")
    except Exception as e:  # network down, rate limit, bad JSON
        _fetch_failures[appid] = time.time()
        log(f"steamcmd lookup for {appid} failed: {e}")
        return None
    write_json_atomic(cache, {"fetched": now_iso(), "url": url, "data": data})
    return data


def windows_launch_entries(info):
    """Windows entries of config.launch, best first: type default, 64-bit, no beta key."""
    launch = ((info or {}).get("config") or {}).get("launch") or {}
    out = []
    for key in sorted(launch, key=lambda k: int(k) if str(k).isdigit() else 999):
        e = launch[key]
        if not isinstance(e, dict):
            continue
        cfg = e.get("config") or {}
        oslist = cfg.get("oslist", "")
        exe = e.get("executable", "")
        if not exe.lower().endswith(".exe"):
            continue
        if oslist and "windows" not in oslist:
            continue
        rank = (0 if e.get("type", "default") in ("default", "") else 1,
                0 if cfg.get("osarch", "64") == "64" else 1,
                1 if cfg.get("betakey") else 0,
                int(key) if str(key).isdigit() else 999)
        out.append((rank, {"executable": exe.replace("\\", "/").lstrip("/"),
                           "arguments": e.get("arguments", ""),
                           "workingdir": (e.get("workingdir") or "").replace("\\", "/"),
                           "description": e.get("description", "")}))
    return [e for _, e in sorted(out, key=lambda x: x[0])]


def find_case_insensitive(base, rel):
    """base/rel, matching each path component case-insensitively (Steam paths are Windows paths)."""
    p = Path(base)
    for part in [x for x in rel.split("/") if x and x != "."]:
        try:
            names = [c.name for c in p.iterdir()]
        except OSError:
            return None
        # Use the on-disk spelling (APFS is usually case-insensitive, so exists() alone
        # would keep Steam's spelling and the log/UI would show a path that isn't on disk).
        name = part if part in names else next((n for n in names if n.lower() == part.lower()), None)
        if name is None:
            return None
        p = p / name
    return p if p.is_file() else None


def exe_candidates(dirs, depth=3):
    out = []
    for d in dirs:
        d = Path(d)
        for root, subdirs, files in os.walk(d):
            rel_depth = len(Path(root).relative_to(d).parts)
            if rel_depth >= depth:
                subdirs[:] = []
            subdirs[:] = [s for s in subdirs if not s.startswith(".")]
            for f in files:
                if f.lower().endswith(".exe"):
                    out.append(Path(root) / f)
    return out


def guess_exe(dirs):
    """Fallback when Steam's launch config is unavailable: shallowest non-helper .exe."""
    cands = [c for c in exe_candidates(dirs) if not HELPER_EXE.search(c.name)]
    if not cands:
        return None
    cands.sort(key=lambda c: (len(c.parts), "win32" in str(c).lower(), c.name.lower()))
    return cands[0]


# ---------------------------------------------------------------- game discovery

def library_folders():
    vdf = STEAM_ROOT / "steamapps/libraryfolders.vdf"
    folders = []
    try:
        data = parse_vdf(vdf.read_text(encoding="utf-8", errors="replace"))
        for v in (data.get("libraryfolders") or {}).values():
            if isinstance(v, dict) and v.get("path"):
                folders.append(Path(v["path"]))
    except OSError:
        pass
    if STEAM_ROOT not in folders:
        folders.insert(0, STEAM_ROOT)
    return folders


def find_icon(appid):
    base = STEAM_ROOT / "appcache/librarycache" / str(appid)
    if not base.is_dir():
        return None
    prefs = ["library_header.jpg", "header.jpg", "library_capsule.jpg", "library_600x900.jpg",
             "capsule_231x87.jpg", "logo.png"]
    found = {}
    for root, _dirs, files in os.walk(base):
        for f in files:
            found.setdefault(f, Path(root) / f)
    for name in prefs:
        if name in found:
            return found[name]
    return None


def resolve_launch(appid, dirs, gs, info):
    """-> dict(exe, source, args, cwd, note)."""
    override = (gs.get("exe") or "").strip()
    if override:
        p = Path(override).expanduser()
        if not p.is_absolute():
            p = next((Path(d) / override for d in dirs if (Path(d) / override).is_file()),
                     Path(dirs[0]) / override if dirs else p)
        return {"exe": str(p), "source": "override", "args": "", "cwd": str(p.parent),
                "ok": p.is_file(), "note": "" if p.is_file() else "override file not found"}
    entries = windows_launch_entries(info)
    for e in entries:
        for d in dirs:
            p = find_case_insensitive(d, e["executable"])
            if p:
                cwd = find_case_insensitive_dir(d, e["workingdir"]) if e["workingdir"] else p.parent
                return {"exe": str(p), "source": "steam", "args": e["arguments"],
                        "cwd": str(cwd or p.parent), "ok": True, "note": e["description"]}
    g = guess_exe(dirs)
    if g:
        note = ("Steam launch config not found on disk; guessed" if entries
                else "no Steam launch config (offline?); guessed")
        return {"exe": str(g), "source": "guess", "args": "", "cwd": str(g.parent), "ok": True,
                "note": note}
    return {"exe": None, "source": None, "args": "", "cwd": None, "ok": False,
            "note": "no .exe found"}


def find_case_insensitive_dir(base, rel):
    p = Path(base)
    for part in [x for x in rel.split("/") if x and x != "."]:
        try:
            p = next((c for c in p.iterdir() if c.name.lower() == part.lower() and c.is_dir()))
        except (OSError, StopIteration):
            return None
    return p


_scan_cache = {"t": 0, "games": None}
SCAN_TTL = 20  # seconds; Play, Save settings and Refresh rescan at once


def invalidate_scan():
    _scan_cache["games"] = None


def scan_games(refresh=False):
    """Discovery is cached for SCAN_TTL; run state and prefix existence are always live."""
    with _lock:
        cached = _scan_cache["games"]
        fresh = cached is not None and time.time() - _scan_cache["t"] < SCAN_TTL and not refresh
    if not fresh:
        cached = _scan_games(refresh)
        with _lock:
            _scan_cache.update(t=time.time(), games=cached)
    out = []
    for g in cached:
        g = dict(g, run=RUNS.public(g["appid"]),
                 prefix_exists=(PREFIXES / g["appid"] / "pfx").is_dir())
        out.append(g)
    return out


def _scan_games(refresh=False):
    s = load_settings()
    raw = []  # (appid, source, dirs, manifest_name)

    # (a) download_depot output: steamapps/content/app_<id>/depot_<id>
    if CONTENT_DIR.is_dir():
        for app in sorted(CONTENT_DIR.glob("app_*")):
            m = re.fullmatch(r"app_(\d+)", app.name)
            depots = sorted(d for d in app.glob("depot_*") if d.is_dir())
            if m and depots:
                raw.append((m.group(1), "download_depot", depots, None))

    # (b) Steam library folders; only installs that contain a .exe are candidates
    for lf in library_folders():
        for acf in sorted((lf / "steamapps").glob("appmanifest_*.acf")):
            try:
                st = parse_vdf(acf.read_text(encoding="utf-8", errors="replace")).get("AppState", {})
            except OSError:
                continue
            appid, installdir = st.get("appid"), st.get("installdir")
            if not appid or not installdir:
                continue
            d = lf / "steamapps/common" / installdir
            if d.is_dir() and exe_candidates([d]):
                raw.append((appid, "library", [d], st.get("name")))

    ids = sorted({r[0] for r in raw})
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
        infos = dict(zip(ids, ex.map(lambda a: steamcmd_info(a, refresh=refresh), ids)))

    games, seen = [], set()
    for appid, source, dirs, mname in raw:
        info = infos.get(appid)
        if source == "library":
            # A Windows build: Steam's Windows launch target exists on disk. Without launch
            # info (offline) fall back to "has a .exe" and mark it unverified.
            if info is not None:
                ok = any(find_case_insensitive(dirs[0], e["executable"])
                         for e in windows_launch_entries(info))
                if not ok:
                    continue
        key = (appid, source)
        if key in seen:
            continue
        seen.add(key)
        gs = game_settings(s, appid)
        launch = resolve_launch(appid, dirs, gs, info)
        name = ((info or {}).get("common") or {}).get("name") or mname or f"App {appid}"
        games.append({
            "appid": appid, "name": name, "source": source, "dirs": [str(d) for d in dirs],
            "launch": launch, "settings": gs, "has_icon": find_icon(appid) is not None,
            "steam_info": info is not None, "prefix": str(PREFIXES / appid),
            "prefix_exists": (PREFIXES / appid / "pfx").is_dir(),
            "verified_windows": info is not None or source == "download_depot",
            "run": RUNS.public(appid),
        })
    games.sort(key=lambda g: g["name"].lower())
    return games


def find_game(appid, refresh=False):
    for g in scan_games(refresh=refresh):
        if g["appid"] == str(appid):
            return g
    return None


# ---------------------------------------------------------------- running games

def log(msg):
    print(f"[{now_iso()}] {msg}", file=sys.stderr, flush=True)


def build_env(appid, gs, paths):
    env = {k: v for k, v in os.environ.items() if k not in SCRUB_ENV}
    env.update(BACKENDS.get(gs.get("backend"), BACKENDS["default"])[1])
    extra, _ = parse_env_lines(gs.get("env"))
    env.update(extra)
    if gs.get("winedebug", "").strip():
        env["WINEDEBUG"] = gs["winedebug"].strip()
    env.update({
        "PYTHONPATH": paths["proton_ref"],
        "MPROTON_WINE_DIR": paths["wine_dir"],
        "STEAM_COMPAT_DATA_PATH": str(PREFIXES / str(appid)),
        "SteamAppId": str(appid),
        "SteamGameId": str(appid),
        "PROTON_LOG": "1",
    })
    return env


class Runs:
    def __init__(self):
        self.runs = {}  # appid -> dict

    def public(self, appid):
        with _lock:
            r = self.runs.get(str(appid))
            if not r:
                return {"state": "idle"}
            return {k: v for k, v in r.items() if k not in ("proc",)}

    def any_running(self):
        with _lock:
            return [a for a, r in self.runs.items() if r["state"] in ("running", "stopping")]

    def start(self, game):
        appid = game["appid"]
        paths = resolve_paths()
        missing = [k for k in ("launcher", "proton_ref", "wine_dir") if not paths[k]]
        if missing:
            raise RuntimeError("not found: " + ", ".join(missing) + " (set them in Paths)")
        launch = game["launch"]
        if not launch["ok"]:
            raise RuntimeError(f"no launch exe: {launch['note']}")
        with _lock:
            cur = self.runs.get(appid)
            if cur and cur["state"] in ("running", "stopping"):
                raise RuntimeError("already running")
        gs = game["settings"]
        env = build_env(appid, gs, paths)
        args = []
        for a in (launch["args"], gs.get("args", "")):
            if a and a.strip():
                args += shlex.split(a)
        cmd = [paths["python"], paths["launcher"], "waitforexitandrun", launch["exe"]] + args
        PREFIXES.joinpath(appid).mkdir(parents=True, exist_ok=True)
        out_path = STATE_DIR / "runs" / f"{appid}.out"
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out = open(out_path, "w", encoding="utf-8")
        shown_env = {k: env[k] for k in sorted(env) if k not in os.environ or env[k] != os.environ.get(k)}
        out.write(f"# started {now_iso()} by mproton Library\n# cwd: {launch['cwd']}\n"
                  f"# cmd: {json.dumps(cmd)}\n# env (set by the library): {json.dumps(shown_env)}\n\n")
        out.flush()
        proc = subprocess.Popen(cmd, env=env, cwd=launch["cwd"], stdout=out, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True)
        out.close()
        rec = {"state": "running", "pid": proc.pid, "started": now_iso(), "ended": None,
               "exit_code": None, "cmd": cmd, "cwd": launch["cwd"], "env": shown_env,
               "settings": gs, "out": str(out_path), "proc": proc, "stopped_by_user": False}
        with _lock:
            self.runs[appid] = rec
        threading.Thread(target=self._wait, args=(appid, proc), daemon=True).start()
        log(f"play {appid}: pid {proc.pid}: {' '.join(cmd)}")
        return self.public(appid)

    def _wait(self, appid, proc):
        code = proc.wait()
        with _lock:
            r = self.runs.get(appid)
            if r and r.get("proc") is proc:
                r.update(state="exited", exit_code=code, ended=now_iso())
                r.pop("proc", None)
        log(f"exit {appid}: {code}")

    def stop(self, appid):
        appid = str(appid)
        paths = resolve_paths()
        prefix = PREFIXES / appid / "pfx"
        with _lock:
            r = self.runs.get(appid)
            if r and r["state"] == "running":
                r["state"] = "stopping"
                r["stopped_by_user"] = True
            proc = r.get("proc") if r else None
        res = {"wineserver_k": None}
        if paths["wine_dir"] and prefix.is_dir():
            ws = Path(paths["wine_dir"]) / "bin/wineserver"
            env = dict(os.environ, WINEPREFIX=str(prefix))
            try:
                cp = subprocess.run([str(ws), "-k"], env=env, capture_output=True, text=True, timeout=30)
                res["wineserver_k"] = cp.returncode
            except (OSError, subprocess.TimeoutExpired) as e:
                res["wineserver_k"] = str(e)
        if proc:
            def reap():
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(proc.pid, signal.SIGTERM)
                    except OSError:
                        pass
            threading.Thread(target=reap, daemon=True).start()
        log(f"stop {appid}: {res}")
        return res


RUNS = Runs()


# ---------------------------------------------------------------- logs and reports

def game_logs(game):
    out = []
    steam_log = LOG_DIR / f"steam-{game['appid']}.log"
    if steam_log.is_file():
        out.append(("launcher log (PROTON_LOG)", steam_log))
    run_out = STATE_DIR / "runs" / f"{game['appid']}.out"
    if run_out.is_file():
        out.append(("launcher console output", run_out))
    users = Path(game["prefix"]) / "pfx/drive_c/users"
    if users.is_dir():
        for user in sorted(users.iterdir()):
            # Unity: AppData/LocalLow/<company>/<product>/Player.log (and Player-prev.log)
            for p in sorted(list((user / "AppData/LocalLow").glob("*/*/Player*.log"))
                            + list((user / "AppData/LocalLow").glob("*/*/output_log.txt"))):
                out.append(("Unity " + p.name, p))
            # Unreal: AppData/Local/<Project>/Saved/Logs/*.log
            for p in sorted((user / "AppData/Local").glob("*/Saved/Logs/*.log")):
                out.append(("Unreal " + p.name, p))
    # Some games log next to the exe (Unreal also writes <game>/Saved/Logs when portable).
    for d in game["dirs"]:
        # Unity before 2018.3 wrote <game>_Data/output_log.txt instead of Player.log.
        for pat in ("*.log", "*_Data/output_log.txt", "*/Saved/Logs/*.log", "Saved/Logs/*.log"):
            for p in sorted(Path(d).glob(pat)):
                out.append(("game dir " + p.name, p))
    res = []
    for i, (label, p) in enumerate(out):
        try:
            st = p.stat()
        except OSError:
            continue
        res.append({"id": i, "label": label, "path": str(p), "size": st.st_size,
                    "mtime": datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds")})
    return res


def tail(path, max_bytes=400_000):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - max_bytes))
        data = f.read()
    text = data.decode("utf-8", errors="replace")
    if size > max_bytes:
        text = f"[... showing the last {max_bytes // 1000} kB of {size // 1000} kB ...]\n" + text.split("\n", 1)[-1]
    return text


def run_cmd(cmd, timeout=20):
    try:
        cp = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return cp.returncode, (cp.stdout + cp.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, str(e)


def save_report(game):
    REPORTS.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r"[^A-Za-z0-9]+", "-", game["name"]).strip("-")[:40] or "game"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    zpath = REPORTS / f"{game['appid']}-{slug}-{stamp}.zip"
    status = get_status()
    run = RUNS.public(game["appid"])
    paths = status["paths"]
    wine_version = None
    if paths["wine_dir"]:
        _, wine_version = run_cmd([str(Path(paths["wine_dir"]) / "bin/wine"), "--version"])
    _, sw = run_cmd(["sw_vers"])
    _, hw = run_cmd(["sysctl", "-n", "machdep.cpu.brand_string", "hw.memsize"])
    lines = [
        f"mproton Library report, {now_iso()}",
        "",
        f"Game: {game['name']} (appid {game['appid']}), from {game['source']}",
        f"Game dirs: {', '.join(game['dirs'])}",
        f"Launch exe: {game['launch']['exe']} (source: {game['launch']['source']}; {game['launch']['note']})",
        f"Steam launch arguments: {game['launch']['args']!r}",
        f"Prefix: {game['prefix']}",
        "",
        "Settings: " + json.dumps(game["settings"], indent=2),
        "",
        "Last run: " + json.dumps(run, indent=2),
        "",
        f"Bundle: {paths['wine_dir']}",
        f"wine --version: {wine_version}",
        "Bundle signature: " + json.dumps(status["bundle"].get("signature"), indent=2),
        f"Launcher: {paths['launcher']}  (repo HEAD {status['repo_head']})",
        f"Steam running: {status['steam']['running']}",
        f"Windows downloads (steam_dev.cfg): {status['dev_cfg']['enabled']}",
        "",
        sw, hw,
    ]
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("report.txt", "\n".join(str(x) for x in lines) + "\n")
        z.writestr("settings.json", json.dumps({"game": game["settings"], "launch": game["launch"],
                                                "paths": paths}, indent=2))
        for lg in game_logs(game):
            p = Path(lg["path"])
            if p.stat().st_size > 200_000_000:
                z.writestr("logs/" + p.name + ".SKIPPED.txt", f"{p} is {p.stat().st_size} bytes\n")
                continue
            arc = "logs/" + re.sub(r"[^A-Za-z0-9._-]+", "_", lg["label"]) + "__" + p.name
            z.write(p, arc)
        rules = Path(game["prefix"]) / "compatdb.rules"
        if rules.is_file():
            z.write(rules, "compatdb.rules")
        if paths["wine_dir"]:
            bi = Path(paths["wine_dir"]) / "share/mproton/BUILD-INFO"
            if bi.is_file():
                z.write(bi, "BUILD-INFO")
    log(f"report {game['appid']}: {zpath}")
    return str(zpath)


# ---------------------------------------------------------------- steam_dev.cfg toggle (Mechanism A)

def dev_cfg_state():
    exists = DEV_CFG.is_file()
    text = DEV_CFG.read_text(encoding="utf-8", errors="replace") if exists else ""
    lines = text.splitlines()
    enabled = any(l.strip().lower().split()[:1] == [FORCE_KEY] and "windows" in l.lower()
                  for l in lines if l.strip())
    other = [l for l in lines if l.strip() and not l.strip().lower().startswith(FORCE_KEY)]
    return {"path": str(DEV_CFG), "exists": exists, "enabled": enabled, "other_lines": len(other)}


def set_dev_cfg(enabled):
    """Add or remove exactly the one ConVar line; every other line is left as it was.

    - The file is created if missing, and deleted on 'off' only if the library created it and
      nothing else is in it: the file's mere existence makes Steam skip its self-update.
    - If the file already set the same ConVar to another value, that line is swapped out on
      'on' and put back on 'off'.
    - A timestamped copy of the previous file goes to <state>/steam_dev.cfg.backups/ first.
    """
    s = load_settings()
    d = s["dev_cfg"]
    with _lock:
        before = dev_cfg_state()
        exists = DEV_CFG.is_file()
        text = DEV_CFG.read_text(encoding="utf-8", errors="replace") if exists else ""
        lines = text.splitlines()
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        backup = STATE_DIR / "steam_dev.cfg.backups" / (stamp + (".cfg" if exists else ".absent"))
        backup.parent.mkdir(parents=True, exist_ok=True)
        if exists:
            shutil.copy2(DEV_CFG, backup)
        else:
            backup.write_text("steam_dev.cfg did not exist\n")
        is_ours = lambda l: l.strip().lower().startswith(FORCE_KEY)
        if enabled:
            if not exists:
                d["created_by_library"] = True
            foreign = [l for l in lines if is_ours(l) and l.strip() != FORCE_LINE]
            if foreign and not d.get("replaced_lines"):
                d["replaced_lines"] = foreign
            # Our line takes the place of the first line that set this ConVar, else goes last.
            new, placed = [], False
            for l in lines:
                if is_ours(l):
                    if not placed:
                        new.append(FORCE_LINE)
                        placed = True
                    continue
                new.append(l)
            if not placed:
                new.append(FORCE_LINE)
            DEV_CFG.parent.mkdir(parents=True, exist_ok=True)
            DEV_CFG.write_text("\n".join(new) + "\n", encoding="utf-8")
        elif exists:
            restore = d.pop("replaced_lines", [])
            kept = []
            for l in lines:
                if is_ours(l):
                    kept += restore  # put back what was there before, in the same place
                    restore = []
                    continue
                kept.append(l)
            if not any(l.strip() for l in kept) and d.get("created_by_library"):
                DEV_CFG.unlink()
            else:
                DEV_CFG.write_text(("\n".join(kept) + "\n") if kept else "", encoding="utf-8")
        if not DEV_CFG.exists():
            d["created_by_library"] = False
        # Remember what the running Steam started with, so the UI can tell whether a restart
        # is actually needed (turning it on and off again needs none).
        steam = steam_status()
        if steam["started"] and (d.get("baseline") or {}).get("steam_started") != steam["started"]:
            d["baseline"] = {"steam_started": steam["started"], "enabled": before["enabled"]}
        d["last_change"] = time.time()
        d["last_backup"] = str(backup)
        save_settings(s)
    log(f"steam_dev.cfg: windows downloads {'on' if enabled else 'off'} (backup {backup})")
    return dev_cfg_state()


# ---------------------------------------------------------------- status

_sig_cache = {}


def steam_status():
    code, out = run_cmd(["pgrep", "-x", "steam_osx"])
    pids = [int(x) for x in out.split()] if code == 0 else []
    started = None
    if pids:
        _, lstart = run_cmd(["ps", "-o", "lstart=", "-p", str(pids[0])])
        try:
            started = time.mktime(time.strptime(" ".join(lstart.split()), "%a %b %d %H:%M:%S %Y"))
        except ValueError:
            pass
    return {"running": bool(pids), "pid": pids[0] if pids else None, "started": started}


def bundle_status(wine_dir):
    if not wine_dir:
        return {"exists": False}
    root = Path(wine_dir)
    loader = (root / "bin/wine").resolve()
    app = next((p for p in loader.parents if p.suffix == ".app"), None)
    info = {"exists": (root / "bin/wine").exists()
            and (root / "lib/wine/aarch64-unix/compatdb.so").exists(),
            "path": str(root), "loader": str(loader)}
    bi = root / "share/mproton/BUILD-INFO"
    info["build_info"] = bi.read_text(errors="replace").strip() if bi.is_file() else None
    target = app or loader
    try:
        key = (str(target), target.stat().st_mtime)
    except OSError:
        key = None
    if key and key in _sig_cache:
        info["signature"] = _sig_cache[key]
    elif key:
        vcode, vout = run_cmd(["codesign", "--verify", "--strict", str(target)])
        _, dout = run_cmd(["codesign", "-dv", str(target)])
        fields = dict(l.split("=", 1) for l in dout.splitlines() if "=" in l)
        sig = {"valid": vcode == 0, "verify_output": vout,
               "identifier": fields.get("Identifier"),
               "team": fields.get("TeamIdentifier"),
               "adhoc": "Signature=adhoc" in dout or fields.get("TeamIdentifier") == "not set",
               "timestamp": fields.get("Timestamp")}
        _sig_cache[key] = sig
        info["signature"] = sig
    return info


def get_status():
    s = load_settings()
    paths = resolve_paths(s)
    steam = steam_status()
    dev = dev_cfg_state()
    base = s["dev_cfg"].get("baseline") or {}
    dev["restart_needed"] = bool(steam["running"] and steam["started"]
                                 and base.get("steam_started") == steam["started"]
                                 and base.get("enabled") != dev["enabled"])
    dev["last_backup"] = s["dev_cfg"].get("last_backup")
    _, head = run_cmd(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"])
    return {"steam": steam, "bundle": bundle_status(paths["wine_dir"]), "dev_cfg": dev,
            "paths": paths, "repo_head": head, "running": RUNS.any_running(),
            "backends": {k: v[0] for k, v in BACKENDS.items()}}


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "mproton-library/1"

    def log_message(self, fmt, *args):
        if os.environ.get("MPROTON_LIBRARY_VERBOSE"):
            super().log_message(fmt, *args)

    def _host_ok(self):
        # DNS-rebinding guard: only answer to the loopback names we bind to.
        host = (self.headers.get("Host") or "").rsplit(":", 1)[0]
        return host in ("127.0.0.1", "localhost", "[::1]")

    def _send(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _err(self, code, msg):
        self._send(code, {"error": msg})

    def do_GET(self):
        if not self._host_ok():
            return self._err(403, "bad host")
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        parts = [p for p in u.path.split("/") if p]
        try:
            if u.path in ("/", "/index.html"):
                return self._send(200, (HERE / "static/index.html").read_bytes(),
                                  "text/html; charset=utf-8",
                                  {"Content-Security-Policy": "default-src 'self'; img-src 'self' data:; "
                                   "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'"})
            if u.path == "/api/status":
                return self._send(200, get_status())
            if u.path == "/api/games":
                return self._send(200, scan_games())
            if len(parts) == 3 and parts[:2] == ["api", "icon"]:
                p = find_icon(parts[2])
                if not p:
                    return self._err(404, "no icon")
                ctype = "image/png" if p.suffix == ".png" else "image/jpeg"
                return self._send(200, p.read_bytes(), ctype, {"Cache-Control": "max-age=3600"})
            if len(parts) == 4 and parts[:2] == ["api", "game"] and parts[3] == "logs":
                g = find_game(parts[2])
                return self._send(200, game_logs(g) if g else [])
            if len(parts) == 4 and parts[:2] == ["api", "game"] and parts[3] == "log":
                g = find_game(parts[2])
                lid = int(q.get("id", ["-1"])[0])
                lg = next((x for x in game_logs(g) if x["id"] == lid), None) if g else None
                if not lg:
                    return self._err(404, "no such log")
                return self._send(200, {"path": lg["path"], "text": tail(lg["path"])})
        except Exception as e:
            log(f"GET {self.path}: {e!r}")
            return self._err(500, str(e))
        return self._err(404, "not found")

    def do_POST(self):
        if not self._host_ok():
            return self._err(403, "bad host")
        # CSRF guard: a custom header forces a CORS preflight, which this server never grants.
        if self.headers.get("X-Mproton") != "1":
            return self._err(403, "missing X-Mproton header")
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return self._err(400, "bad json")
        parts = [p for p in urllib.parse.urlparse(self.path).path.split("/") if p]
        try:
            if parts == ["api", "devcfg"]:
                return self._send(200, set_dev_cfg(bool(body.get("enabled"))))
            if parts == ["api", "paths"]:
                s = load_settings()
                invalidate_scan()
                for k in ("launcher", "proton_ref", "wine_dir", "python"):
                    if k in body:
                        v = (body[k] or "").strip()
                        if v:
                            s["paths"][k] = v
                        else:
                            s["paths"].pop(k, None)
                save_settings(s)
                return self._send(200, resolve_paths(s))
            if parts == ["api", "refresh"]:
                return self._send(200, scan_games(refresh=True))
            if parts == ["api", "quit"]:
                # Games keep running: they are in their own sessions and wineservers.
                self._send(200, {"quitting": True, "games_still_running": RUNS.any_running()})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            if parts == ["api", "open"]:
                return self._send(200, open_path(body))
            if len(parts) == 4 and parts[:2] == ["api", "game"]:
                appid, action = parts[2], parts[3]
                if not appid.isdigit():
                    return self._err(400, "bad appid")
                if action == "settings":
                    s = load_settings()
                    gs = game_settings(s, appid)
                    for k in ("exe", "args", "backend", "winedebug", "env"):
                        if k in body:
                            gs[k] = str(body[k])
                    if gs["backend"] not in BACKENDS:
                        return self._err(400, "unknown backend")
                    _, errors = parse_env_lines(gs["env"])
                    if errors:
                        return self._err(400, "; ".join(errors))
                    s["games"][appid] = gs
                    save_settings(s)
                    invalidate_scan()
                    return self._send(200, find_game(appid))
                if action == "stop":
                    return self._send(200, RUNS.stop(appid))
                if action == "play":
                    invalidate_scan()  # pick up an exe that appeared or moved since the last scan
                g = find_game(appid, refresh=(action == "refresh"))
                if not g:
                    return self._err(404, "unknown game")
                if action == "play":
                    return self._send(200, RUNS.start(g))
                if action == "report":
                    return self._send(200, {"zip": save_report(g)})
                if action == "refresh":
                    return self._send(200, g)
        except RuntimeError as e:
            return self._err(409, str(e))
        except Exception as e:
            log(f"POST {self.path}: {e!r}")
            return self._err(500, str(e))
        return self._err(404, "not found")


def open_path(body):
    """Open a known file or folder with macOS 'open'. Only paths the library itself lists."""
    kind = body.get("kind")
    appid = str(body.get("appid", ""))
    if kind == "reports":
        REPORTS.mkdir(parents=True, exist_ok=True)
        target, reveal = REPORTS, False
    elif kind == "devcfg":
        target, reveal = DEV_CFG.parent, False
    else:
        g = find_game(appid)
        if not g:
            raise RuntimeError("unknown game")
        if kind == "log":
            lg = next((x for x in game_logs(g) if x["id"] == int(body.get("id", -1))), None)
            if not lg:
                raise RuntimeError("no such log")
            target, reveal = Path(lg["path"]), False
        elif kind == "prefix":
            target, reveal = Path(g["prefix"]), False
        elif kind == "gamedir":
            target, reveal = Path(g["dirs"][0]), False
        elif kind == "zip":
            target = Path(body.get("path", ""))
            if target.parent != REPORTS or not target.is_file():
                raise RuntimeError("not a report")
            reveal = True
        else:
            raise RuntimeError("unknown kind")
    if not target.exists():
        raise RuntimeError(f"{target} does not exist yet")
    if target.is_file() and target.suffix == ".log":
        cmd = ["open", "-a", "Console", str(target)]
    elif target.is_file() and not reveal:
        cmd = ["open", "-t", str(target)]
    else:
        cmd = ["open"] + (["-R"] if reveal else []) + [str(target)]
    code, out = run_cmd(cmd)
    if code != 0 and cmd[1] == "-a":
        code, out = run_cmd(["open", "-t", str(target)])
    return {"opened": str(target), "code": code, "output": out}


def main():
    ap = argparse.ArgumentParser(description="mproton Library: local web UI for testing Windows games")
    ap.add_argument("--port", type=int, default=int(os.environ.get("MPROTON_LIBRARY_PORT", 8417)))
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser window")
    a = ap.parse_args()
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    except OSError as e:
        url = f"http://127.0.0.1:{a.port}/"
        log(f"port {a.port} busy ({e}); assuming the library is already running at {url}")
        if not a.no_browser:
            webbrowser.open(url)
        return 0
    url = f"http://127.0.0.1:{a.port}/"
    log(f"mproton Library at {url} (state in {STATE_DIR})")
    if not a.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
