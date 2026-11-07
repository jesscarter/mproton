"""compatdb rules for a mach9 bundle: graphics backends and DLL overrides.

On a mach9 bundle the Direct3D implementation is not chosen by copying DLLs
into the prefix.  ``compatdb.so`` (loaded by ntdll in every process) reads
rules from ``WINE_COMPATDB`` and from the file ``WINE_COMPATDB_FILE`` names,
and prepends one tree per family to the process's builtin search path:

    family  APIs                tree values in mach9 today
    dxgi    D3D10, D3D11, D3D12 dxmt (D3D12 from vkd3d-proton), wined3d (no D3D12)
    d3d9    D3D9                mtld3d, wined3d

mproton writes its rules to a per-prefix file and leaves ``WINE_COMPATDB``
to the user (launch options), so a hand-written rule for one executable
still beats the launcher's ``exe=*`` rule (crate docs, "Folding").

The wire format (crate ``compatdb-core``, https://docs.mach9.app, "Format"):
one rule per line, ``;``-separated ``key=value`` fields, every rule has an
``exe``, list keys repeat once per entry, fields in ``Key::ALL`` order, and
``%``, ``;`` and ASCII control characters in a value written as ``%XX``.
The crate is Rust-only, so this module mirrors its ``Display`` output for
the keys mproton uses; ``tests/test_compatdb.py`` pins the canonical text.
"""

import os

import mproton_graphics as gfx
from mproton_runtime import DXGI, D3D9

ENV_VAR = "WINE_COMPATDB"
FILE_ENV_VAR = "WINE_COMPATDB_FILE"
PROF_SERVICE_VAR = "WINE_PROF_SERVICE"

# compatdb_core::inherited_conflicts(): a launcher removes these from the
# environment Wine starts with when it inherited them.
INHERITED_CONFLICTS = ("WINEDLLOVERRIDES", "WINE_LARGE_ADDRESS_AWARE", FILE_ENV_VAR, PROF_SERVICE_VAR)

# Key::ALL order, which Display writes fields in.
KEY_ORDER = ("exe", "company", "product", "original_filename", "dxgi", "d3d9", "wined3d",
             "opengl", "dpi_aware", "large_address_aware", "gcmouse", "force_nx", "metal_hud",
             "dll_overrides", "arguments_override", "arguments", "debug_channels", "env")
LIST_KEYS = ("dll_overrides", "arguments_override", "arguments", "debug_channels", "env")

DEFAULT = "default"

# Backend -> the tree each family gets.  A family a backend leaves out keeps
# what the built-in rules give it (dxgi=dxmt, d3d9=mtld3d).
#
# - dxvk and d3dmetal are not trees in upstream mach9: each needs its tree in
#   the bundle *and* a compatdb.so whose Dxgi/D3d9 enums know the value
#   (docs/graphics-dxvk.md, "Adding DXVK as a backend in mach9").  The tree
#   is checked here; an older compatdb.so ignores the value with a warning
#   and the process keeps the built-in tree.
# - D3D12 (vkd3d-proton) lives in the dxgi/dxmt tree (and dxgi/dxvk once it
#   exists): DXMT's dxgi.dll presents vkd3d-proton's swapchains.
BACKEND_TREES = {
    gfx.MACH9: {},
    gfx.DXMT: {DXGI: "dxmt"},
    gfx.WINED3D: {DXGI: "wined3d", D3D9: "wined3d"},
    gfx.DXVK: {DXGI: "dxvk", D3D9: "dxvk"},
    gfx.D3DMETAL: {DXGI: "d3dmetal"},
}

# Per-family escape hatches: any tree name, or "default" for the built-in choice.
FAMILY_ENV_VARS = ((DXGI, "MPROTON_DXGI"), (D3D9, "MPROTON_D3D9"))


def escape(value):
    """Escape a field value the way compatdb_core's Display does."""
    out = []
    for ch in value:
        o = ord(ch)
        if ch in "%;" or o < 0x20 or o == 0x7f:
            out.append("%%%02X" % o)
        else:
            out.append(ch)
    return "".join(out)


class Rule:
    """One record: ``exe`` plus settings.  Lists keep entry order."""

    def __init__(self, exe="*"):
        if not exe:
            raise ValueError("a rule needs an exe")
        self.fields = {"exe": exe}

    def set(self, key, value):
        assert key in KEY_ORDER and key not in LIST_KEYS, key
        self.fields[key] = value
        return self

    def add(self, key, value):
        assert key in LIST_KEYS, key
        self.fields.setdefault(key, []).append(value)
        return self

    def get(self, key, default=None):
        return self.fields.get(key, default)

    @property
    def sets_nothing(self):
        return len(self.fields) == 1

    def to_line(self):
        parts = []
        for key in KEY_ORDER:
            if key not in self.fields:
                continue
            value = self.fields[key]
            for v in (value if key in LIST_KEYS else [value]):
                parts.append(key + "=" + escape(v))
        return ";".join(parts)

    def __repr__(self):
        return "Rule(%s)" % self.to_line()


def table_text(rules):
    """Canonical table text: one line per rule, no newline after the last."""
    return "\n".join(r.to_line() for r in rules if not r.sets_nothing)


def parse_winedlloverrides(value, log=lambda msg: None):
    """Split a WINEDLLOVERRIDES value into dll_overrides entries."""
    entries = []
    for item in (value or "").split(";"):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            log("WINEDLLOVERRIDES entry %r has no '=', ignored" % item)
            continue
        entries.append(item)
    return entries


def dll_override_entries(dlloverrides, inherited=None, log=lambda msg: None):
    """The launcher's overrides dict (+ the user's WINEDLLOVERRIDES) as entries.

    Upstream appends its own overrides after the user's WINEDLLOVERRIDES; the
    same order is kept, and compatdb folds a list in order.
    """
    entries = parse_winedlloverrides(inherited, log)
    for dll, setting in dlloverrides.items():
        entries.append(dll + "=" + setting)
    return entries


def steam_app_id(env):
    """SteamAppId as compatdb reads it: a decimal 1..u32::MAX, else None."""
    value = env.get("SteamAppId", "")
    if not value.isdigit():
        return None
    n = int(value)
    return str(n) if 0 < n <= 0xffffffff else None


def choose_trees(backend, compat_config, env, bundle, log=lambda msg: None):
    """Family -> tree for this launch; families absent keep the built-in choice."""
    trees = dict(BACKEND_TREES.get(backend, {}))
    if "mtld3d" in compat_config:
        trees[D3D9] = "mtld3d"
    for family, var in FAMILY_ENV_VARS:
        value = env.get(var, "").strip().lower()
        if value:
            trees[family] = value

    for family in list(trees):
        impl = trees[family]
        if impl == DEFAULT:
            continue
        if not bundle.has_tree(family, impl):
            log("%s=%s requested but %s is missing; keeping the built-in %s tree" %
                (family, impl, bundle.tree_dir(family, impl), family))
            del trees[family]
    return trees


def build_rules(backend, compat_config, env, bundle, dlloverrides, log=lambda msg: None):
    """The launcher's rules for one launch.

    Returns ``(rules, trees)``.  ``rules[0]`` is the launch-wide ``exe=*``
    rule.  When the user picked a backend explicitly and Steam gave an app
    id, a second rule ``exe=steam:<id>`` repeats the tree choice: a ``*``
    rule does not beat a built-in rule that pins one program's tree, but a
    rule for the app id does.
    """
    trees = choose_trees(backend, compat_config, env, bundle, log)

    every = Rule("*")
    for family in (DXGI, D3D9):
        if family in trees:
            every.set(family, trees[family])

    if "forcelgadd" in compat_config:
        every.set("large_address_aware", "true")
    elif "noforcelgadd" in compat_config:
        every.set("large_address_aware", "false")

    if env.get("MPROTON_METAL_HUD", "0") not in ("", "0"):
        every.set("metal_hud", "true")

    for entry in dll_override_entries(dlloverrides, env.get("WINEDLLOVERRIDES"), log):
        every.add("dll_overrides", entry)

    rules = [every]

    appid = steam_app_id(env)
    explicit = backend != gfx.MACH9 or any(env.get(v) for _, v in FAMILY_ENV_VARS) \
        or "mtld3d" in compat_config
    if appid and explicit and trees:
        game = Rule("steam:" + appid)
        for family in (DXGI, D3D9):
            if family in trees:
                game.set(family, trees[family])
        rules.append(game)

    return rules, trees


def write_rules_file(path, text):
    """Write the table atomically (temp file in the same dir, then rename).

    A process that reads the file while it is being written would get a
    partial table without a warning (crate docs, "Launcher responsibilities").
    """
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, ".%s.%d.tmp" % (os.path.basename(path), os.getpid()))
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def apply_env(env, rules_file, log=lambda msg: None):
    """Point Wine at the rules file and drop the variables the rules replace.

    - WINE_COMPATDB_FILE: ours, replacing an inherited one.
    - WINEDLLOVERRIDES, WINE_LARGE_ADDRESS_AWARE: folded into the rules.
    - WINE_PROF_SERVICE: hands every process's task port to a profiler, so it
      is only set on purpose, through MPROTON_PROF_SERVICE.
    - WINE_COMPATDB: left alone; it is how a user adds rules by hand.
    """
    for var in INHERITED_CONFLICTS:
        env.pop(var, None)
    env[FILE_ENV_VAR] = os.path.abspath(rules_file)
    prof = env.get("MPROTON_PROF_SERVICE")
    if prof:
        env[PROF_SERVICE_VAR] = prof
    return env
