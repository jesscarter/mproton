#!/usr/bin/env python3
"""Check a Vulkan device (vulkaninfo text dump) against DXVK's device requirements.

Usage: check-features.py DXVK_SRC VULKANINFO_TXT [--all]

DXVK_SRC is a DXVK checkout. Its src/dxvk/dxvk_device_info.cpp is the source of truth:
getFeatureList() marks each feature or extension DXVK enables as required or optional, and
checkDeviceCompatibility() refuses a device that lacks a required one. This script reads that
list instead of copying it, so it follows the pinned DXVK version.

VULKANINFO_TXT is the plain-text output of `vulkaninfo` (not --summary, not --json), e.g.
results/m0-kosmickrisp-m2-vulkaninfo.txt.

It prints every required entry DXVK's own check would reject, plus the other hard checks in
checkDeviceCompatibility (Vulkan API version, graphics queue, push-constant size). With --all
it also lists the optional entries the device lacks. Exit status: 0 if DXVK would accept the
device, 1 if not.
"""
import re
import sys


def norm(name):
    return name.replace("_", "").lower()


def parse_dxvk(src):
    cpp = open(f"{src}/src/dxvk/dxvk_device_info.cpp").read()
    hdr = open(f"{src}/src/dxvk/dxvk_device_info.h").read()
    ext_names = {m[0]: m[1] for m in re.findall(
        r"VkExtensionProperties\s+(\w+)\s*=\s*vk::makeExtension\((VK_\w+?)_EXTENSION_NAME\)", hdr)}
    body = cpp[cpp.index("DxvkDeviceCapabilities::getFeatureList()"):]
    body = body[body.index("return {{"):body.index("}};")]
    entries = []  # (kind, struct-or-ext member, feature or None, required)
    for m in re.finditer(r"ENABLE_FEATURE\(([\w.]+),\s*(\w+),\s*(true|false)\)", body):
        entries.append(("core", m[1], m[2], m[3] == "true"))
    for m in re.finditer(r"ENABLE_EXT_FEATURE\((\w+),\s*(\w+),\s*(true|false)\)", body):
        entries.append(("ext", m[1], m[2], m[3] == "true"))
    for m in re.finditer(r"ENABLE_EXT\((\w+),\s*(true|false)\)", body):
        entries.append(("ext", m[1], None, m[2] == "true"))
    api = re.search(r"DxvkVulkanApiVersion\s*=\s*VK_API_VERSION_(\d+)_(\d+)",
                    open(f"{src}/src/dxvk/dxvk_instance.h").read())
    push = re.search(r"MaxTotalPushDataSize\s*=\s*(\d+)", open(f"{src}/src/dxvk/dxvk_limits.h").read())
    return entries, ext_names, (int(api[1]), int(api[2])), int(push[1])


def parse_vulkaninfo(path):
    """Return (extensions {normalized name: name}, features {section: {name: bool}}, props {name: str}, queue flags)."""
    exts, feats, props, queues = {}, {}, {}, []
    section = None
    in_dev_exts = False
    for line in open(path, encoding="utf-8", errors="replace"):
        s = line.rstrip("\n")
        if s.startswith("Device Extensions:"):
            in_dev_exts = True
            continue
        if in_dev_exts:
            m = re.match(r"\s+(VK_\w+)\s*:", s)
            if m:
                exts[norm(m[1])] = m[1]
                continue
            if s.strip() and not s.startswith("="):
                in_dev_exts = False
        if re.match(r"^Vk\w+:$", s):
            section = s[:-1]
            continue
        m = re.match(r"\s+(\w+)\s*=\s*(.*)$", s)
        if not m or not section:
            continue
        key, val = m[1], m[2].strip()
        if "Features" in section and val in ("true", "false"):
            feats.setdefault(section, {})[key] = val == "true"
        else:
            props.setdefault(key, val)
        if key == "queueFlags":
            queues.append(val)
    return exts, feats, props, queues


CORE_SECTIONS = {
    "core.features": "VkPhysicalDeviceFeatures",
    "vk11": "VkPhysicalDeviceVulkan11Features",
    "vk12": "VkPhysicalDeviceVulkan12Features",
    "vk13": "VkPhysicalDeviceVulkan13Features",
}


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    show_all = "--all" in argv
    entries, ext_names, api_req, push_req = parse_dxvk(argv[1])
    exts, feats, props, queues = parse_vulkaninfo(argv[2])
    any_feature = {}
    for sec, d in feats.items():
        for k, v in d.items():
            any_feature[k] = any_feature.get(k, False) or v

    missing_req, missing_opt = [], []
    for kind, owner, feature, required in entries:
        if kind == "core":
            sec = CORE_SECTIONS[owner]
            ok = feats.get(sec, {}).get(feature, False)
            label = f"{sec}.{feature}"
        else:
            macro = ext_names.get(owner, owner)
            ext = exts.get(norm(macro))
            label = (ext or macro) + (f" ({feature})" if feature else "")
            ok = ext is not None and (feature is None or any_feature.get(feature, False))
        if not ok:
            (missing_req if required else missing_opt).append(label)

    hard = []
    api = props.get("apiVersion", "0.0.0").split()[0].split(".")
    if (int(api[0]), int(api[1])) < api_req:
        hard.append(f"Vulkan {api_req[0]}.{api_req[1]} (device has {props.get('apiVersion')})")
    if not any("QUEUE_GRAPHICS_BIT" in q and "QUEUE_COMPUTE_BIT" in q for q in queues):
        hard.append("a graphics+compute queue")
    # Without VK_EXT_descriptor_heap in use, DXVK needs MaxTotalPushDataSize bytes of push constants.
    if int(props.get("maxPushConstantsSize", "0")) < push_req:
        hard.append(f"maxPushConstantsSize >= {push_req} (device has {props.get('maxPushConstantsSize')})")

    n_req = sum(1 for e in entries if e[3])
    print(f"device: {props.get('deviceName', '?')}, {props.get('driverName', '?')} {props.get('driverInfo', '')}")
    print(f"DXVK entries: {len(entries)} ({n_req} required); device extensions parsed: {len(exts)}")
    print(f"required, missing: {len(missing_req) + len(hard)}")
    for x in hard + missing_req:
        print(f"  MISSING {x}")
    if show_all:
        print(f"optional, missing: {len(missing_opt)}")
        for x in missing_opt:
            print(f"  absent  {x}")
    print("verdict:", "DXVK accepts this device" if not (hard or missing_req) else "DXVK rejects this device")
    return 0 if not (hard or missing_req) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
