#!/usr/bin/env python3
"""Read-only source inventory generator for the CarLife+ / LIVI reference snapshots
and our two Input modules (Input/WirelessCarLifePlus, Input/WirelessCarPlay).

WHAT THIS IS
    Evidence collection only. It enumerates declarations found by the explicit
    patterns listed in BOUNDARY, with the exact repository-relative source file and
    1-based line for every row.

WHAT THIS IS NOT
    A statement that any enumerated identifier is integrated. A name that also
    appears in our tree is recorded as a *text occurrence* (see "occurrence" rows)
    and is explicitly NOT evidence of runtime wiring. Numeric message-ID equality
    between the upstream Kotlin table and our service_types.h is recorded as a
    value match, not as behavioural equivalence.

DETERMINISM
    stdlib only, UTF-8, repository-relative POSIX paths, all lists sorted.
    Re-running against unchanged sources reproduces byte-identical JSON/Markdown.

USAGE
    python inventory.py               # write inventory.json / inventory.md / gaps.json / gaps.md
    python inventory.py --help
    python inventory.py --out-dir DIR # override output directory (default: script dir)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
DEFAULT_OUT_DIR = SCRIPT_PATH.parent
REPO_ROOT = SCRIPT_PATH.parents[2]

# ---------------------------------------------------------------------------
# Extraction boundary (emitted verbatim into the artifacts)
# ---------------------------------------------------------------------------

BOUNDARY = {
    "generator": "Input/API_AUDIT/generate_inventory.py",
    "repo_root": ".",
    "read_only": True,
    "notes": [
        "Text matching is NEVER evidence of full integration.",
        "Numeric equality of a CarLife message ID is a value match, not proof of behavioural equivalence.",
        "Missing and log-only CarLife dispatch entries are kept visible (see dispatch section).",
        "LIVI dongle-transport commands are NOT assumed to be native CarPlay commands; each",
        "preload channel is annotated with the main-process module that registers it.",
        "Parser completeness is claimed only for the enumerated files and patterns below.",
    ],
    "skip_dirs": [".git", "node_modules", "target", "build", "out", "dist", ".venv"],
    "patterns": {
        "carlife.upstream.const": "ServiceTypes.kt: ^\\s*const val NAME = (0xHEX|DEC)$",
        "carlife.upstream.fun": "ServiceTypes.kt: \\bfun NAME(",
        "carlife.local.const": "service_types.h: ^\\s*constexpr TYPE NAME = VALUE;  (namespace stack tracked)",
        "carlife.local.keycode_name_map": "keycode_map.cpp: text == \"NAME\" -> keycode::K",
        "carlife.dispatch": "session.cpp: case msg::NAME: with body classification",
        "carlife.vehicle_lib.api": "LibSource/include/*.h: declaration lines ending in ');'",
        "carlife.vehicle_lib.struct": "LibSource/include/*.h: typedef struct ... } S_NAME;",
        "carlife.vehicle_lib.type": "LibSource/include/*.h: typedef/class/enum lines",
        "carlife.feature_key": "CarlifeConfUtil.java: KEY_* = \"NAME\"",
        "apollo.sdk.api": "carlife-sdk/src/main/java/com/baidu/carlife/sdk/** (path must not contain /internal/)",
        "livi.preload.api": "src/preload/index.ts: property keys at indent 2 or 4 inside api/appApi",
        "livi.input_command": "src/main/shared/types/InputCommand.ts: enum InputCommand entries",
        "livi.native.rust": "native/livi-helperd/crates/**/*.rs: pub enum/struct/const/fn",
        "livi.transport.iface": "src/main/services/projection/transport/*.ts: exported members + TransportArbiter methods",
        "occurrence": "literal substring scan of Input/ and Core/ for each upstream name (text evidence only)",
    },
}

SOURCES = {
    "carlife_upstream_service_types_kt": (
        "Reference/apollo-DuerOS/CarLife-Android-Vehicle-V2.0/carlife-sdk/src/main/java/"
        "com/baidu/carlife/sdk/internal/protocol/ServiceTypes.kt"
    ),
    "carlife_local_service_types_h": "Input/WirelessCarLifePlus/include/carlife/service_types.h",
    "carlife_local_keycode_map_cpp": "Input/WirelessCarLifePlus/src/keycode_map.cpp",
    "carlife_local_session_cpp": "Input/WirelessCarLifePlus/src/session.cpp",
    "carlife_vehicle_lib_include_dir": (
        "Reference/carlife-vehicle-lib/CarLife-Vehicle-Lib/LibSource/include"
    ),
    "carlife_feature_config_java": (
        "Reference/carlife-vehicle-lib/CarLife-Android-Vehicle/src/com/baidu/carlifevehicle/util/"
        "CarlifeConfUtil.java"
    ),
    "apollo_sdk_root": (
        "Reference/apollo-DuerOS/CarLife-Android-Vehicle-V2.0/carlife-sdk/src/main/java/com/baidu/carlife/sdk"
    ),
    "livi_preload_ts": "Reference/LIVI/src/preload/index.ts",
    "livi_input_command_ts": "Reference/LIVI/src/main/shared/types/InputCommand.ts",
    "livi_native_crates": [
        "Reference/LIVI/native/livi-helperd/crates/livi-runtime/src",
        "Reference/LIVI/native/livi-helperd/crates/iap2-csm/src",
        "Reference/LIVI/native/livi-helperd/crates/iap2-link/src",
    ],
    "livi_native_other_crates_dir": "Reference/LIVI/native/livi-helperd/crates",
    "livi_transport_dir": "Reference/LIVI/src/main/services/projection/transport",
    "livi_ipc_registration_dirs": [
        "Reference/LIVI/src/main",
    ],
    "livi_ipc_registration_exclude_dirs": ["__tests__"],
    "our_symbol_scan_roots": ["Input", "Core"],
}

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

SKIP_DIRS = set(BOUNDARY["skip_dirs"])

_DEC = re.compile(r"^-?\d+$")
_HEX = re.compile(r"^(-?)0[xX]([0-9A-Fa-f]+)$")


def _ns_short(stack) -> str:
    """Namespace path with the file's root namespace (e.g. 'carlife') removed."""
    parts = list(stack)
    if parts and parts[0] == "carlife":
        parts = parts[1:]
    return ".".join(parts)


def to_int(text: str):
    """Return the integer value of a C/Kotlin integer literal, else None."""
    text = text.strip()
    m = _HEX.match(text)
    if m:
        value = int(m.group(2), 16)
        return -value if m.group(1) else value
    if _DEC.match(text):
        return int(text, 10)
    return None


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def walk_files(root: Path, suffixes=None, exclude_dirs=()):
    """Deterministic file walk that prunes SKIP_DIRS and hidden directories."""
    if root.is_file():
        if suffixes is None or root.suffix in suffixes:
            yield root
        return
    excluded = set(exclude_dirs)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(
            d for d in dirnames if d not in SKIP_DIRS and d not in excluded and not d.startswith(".")
        )
        for name in sorted(filenames):
            p = Path(dirpath) / name
            if suffixes is not None and p.suffix not in suffixes:
                continue
            yield p


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def read_lines(path: Path):
    return read_text(path).splitlines()


def row(category, source, line, name, kind, **detail):
    return {
        "category": category,
        "source": rel(source) if isinstance(source, Path) else source,
        "line": int(line),
        "name": name,
        "kind": kind,
        "detail": detail,
    }


def sort_rows(rows):
    return sorted(rows, key=lambda r: (r["category"], r["source"], r["line"], r["name"]))


# ---------------------------------------------------------------------------
# Category A: upstream CarLife ServiceTypes.kt constants
# ---------------------------------------------------------------------------

_KT_CONST = re.compile(
    r"^\s*const\s+val\s+([A-Za-z_][A-Za-z0-9_]*)"
    r"(?:\s*:\s*[A-Za-z0-9_<>.?]+)?\s*=\s*(-?0[xX][0-9A-Fa-f]+|-?\d+)\s*"
    r"(?://\s*(.*))?$"
)
_KT_CONST_ANY = re.compile(r"\bconst\s+val\b")
_KT_FUN = re.compile(r"\bfun\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


# Constants declared directly in namespace `msg` that are value domains rather than
# CarLife message ids (module ids from Constants.kt, protocol-version match flags).
_AUX_LOCAL_NAMES = {
    "MODULE_PHONE",
    "MODULE_NAVI",
    "MODULE_MUSIC",
    "MODULE_VR",
    "MODULE_CONNECT",
    "MODULE_MIC",
    "MODULE_CRUISE",
    "MODULE_CRUISE_FOLLOW",
    "PROTOCOL_VERSION_MATCH",
    "PROTOCOL_VERSION_NOT_MATCH",
}


def _kt_group(name: str) -> str:
    # These MSG_* names describe payload value domains, not wire service IDs.
    if (name.startswith(("MSG_VEHICLE_", "MSG_PERMISSION_", "MSG_CMD_LAUNCH_MODE_"))
            or name in {"MSG_CHANNEL", "MSG_CMD_PROTOCOL_VERSION", "MSG_READ_MEDIA"}):
        return "payload_value"
    if name.startswith("MSG_"):
        return "message"
    if name.startswith("KEYCODE_"):
        return "keycode"
    if name.startswith("GEAR_"):
        return "gear"
    if name.startswith("PROTOCOL_VERSION"):
        return "protocol"
    return "other"


def collect_kt_constants(path: Path):
    rows, unhandled, funcs = [], [], []
    lines = read_lines(path)
    total_const = 0
    for idx, line in enumerate(lines, start=1):
        if _KT_CONST_ANY.search(line):
            total_const += 1
        m = _KT_CONST.match(line)
        if m:
            name, raw, comment = m.group(1), m.group(2), (m.group(3) or "").strip()
            group = _kt_group(name)
            rows.append(
                row(
                    "carlife.upstream.const",
                    path,
                    idx,
                    name,
                    group,
                    value=to_int(raw),
                    value_literal=raw,
                    upstream_group=group,
                    comment=comment,
                )
            )
        elif _KT_CONST_ANY.search(line):
            unhandled.append(
                {"source": rel(path), "line": idx, "name": line.strip(), "reason": "const val not matched by pattern"}
            )
        fm = _KT_FUN.search(line)
        if fm:
            funcs.append(row("carlife.upstream.fun", path, idx, fm.group(1), "function", signature=line.strip()))
    meta = {
        "const_val_lines_total": total_const,
        "const_val_rows_extracted": len(rows),
        "const_val_complete": total_const == len(rows),
    }
    return rows, funcs, unhandled, meta


# ---------------------------------------------------------------------------
# Category B: our CarLife service_types.h constants (namespace-tracked)
# ---------------------------------------------------------------------------

_H_NS_OPEN = re.compile(r"^namespace\s+([A-Za-z_][A-Za-z0-9_:]*)\s*\{")
_H_NS_ALIAS = re.compile(r"^namespace\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_:]*)\s*;")
_H_CONSTEXPR = re.compile(
    r"^\s*constexpr\s+(const\s+char\s*\*|char\s+const\s*\*|uint32_t|uint8_t|int32_t|int)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;]+?)\s*;(.*)$"
)
_H_CONSTEXPR_ANY = re.compile(r"\bconstexpr\b")
_H_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_H_CLOSE = re.compile(r"^\s*\}(?:\s*//.*)?\s*$")


def collect_local_service_types(path: Path):
    rows, unhandled, ns_aliases = [], [], []
    lines = read_lines(path)
    ns_stack: list[str] = []
    defined: dict[str, dict] = {}
    total_constexpr = 0

    for idx, line in enumerate(lines, start=1):
        if _H_CONSTEXPR_ANY.search(line):
            total_constexpr += 1

        ns = _H_NS_OPEN.match(line)
        if ns:
            ns_stack.append(ns.group(1))
            continue
        alias = _H_NS_ALIAS.match(line)
        if alias:
            ns_aliases.append(
                row(
                    "carlife.local.namespace_alias",
                    path,
                    idx,
                    alias.group(1),
                    "namespace_alias",
                    target=alias.group(2),
                )
            )
            continue

        m = _H_CONSTEXPR.match(line)
        if m:
            ctype, name, raw, tail = m.group(1), m.group(2), m.group(3).strip(), m.group(4)
            comment = ""
            cm = re.search(r"//\s*(.*)$", tail)
            if cm:
                comment = cm.group(1).strip()
            ns_path = _ns_short(ns_stack)
            value = None
            alias_target = None
            if ctype in ("const char *", "char const *") or "*" in ctype:
                sm = re.match(r'^"(.*)"$', raw)
                value = sm.group(1) if sm else raw
            elif raw and _H_IDENT.match(raw) and raw in defined:
                alias_target = raw
                value = defined[raw]["value"]
            else:
                value = to_int(raw)
            entry = {
                "namespace": ns_path,
                "value": value,
                "value_literal": raw,
                "alias_of": alias_target,
                "comment": comment,
            }
            defined[name] = entry
            rows.append(row("carlife.local.const", path, idx, name, "constexpr", **entry))
            continue

        if _H_CONSTEXPR_ANY.search(line):
            unhandled.append(
                {"source": rel(path), "line": idx, "name": line.strip(), "reason": "constexpr not matched"}
            )

        if _H_CLOSE.match(line) and ns_stack:
            ns_stack.pop()

    meta = {
        "constexpr_lines_total": total_constexpr,
        "constexpr_rows_extracted": len(rows),
        "constexpr_complete": total_constexpr == len(rows),
    }
    return rows + ns_aliases, unhandled, meta


# ---------------------------------------------------------------------------
# Category C: our local keycode name map (keycode_map.cpp)
# ---------------------------------------------------------------------------

_KCM = re.compile(r'text\s*==\s*"([^"]*)"[^;]*?return\s+keycode::([A-Za-z0-9_]+)')


def collect_keycode_name_map(path: Path):
    rows = []
    for idx, line in enumerate(read_lines(path), start=1):
        for m in _KCM.finditer(line):
            rows.append(row("carlife.local.keycode_name_map", path, idx, m.group(1), "name_map", keycode=m.group(2)))
    return rows


# ---------------------------------------------------------------------------
# Category D: runtime dispatch in session.cpp
# ---------------------------------------------------------------------------

_MSG_CASE = re.compile(r"^(\s*)case\s+msg::([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$")
_CH_CASE = re.compile(r"^(\s*)case\s+([A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$")
_DEFAULT = re.compile(r"^(\s*)default\s*:\s*(.*)$")
_SWITCH = re.compile(r"^(\s*)switch\s*\((.+)\)\s*\{\s*$")
_ANY_LABEL = re.compile(r"^\s*(?:case\s|default\s*:)")
_HANDLER = re.compile(r"\b(handle[A-Za-z0-9_]*)\s*\(")


def _clean(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return text


def _strip_call(text: str, fname: str) -> str:
    """Remove every `fname(...)` invocation, balancing parentheses."""
    out = []
    i = 0
    while True:
        idx = text.find(fname + "(", i)
        if idx < 0:
            out.append(text[i:])
            break
        out.append(text[i:idx])
        depth = 0
        j = idx + len(fname)
        while j < len(text):
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        i = j + 1
    return "".join(out)


def collect_dispatch(path: Path):
    rows, unhandled = [], []
    lines = read_lines(path)
    # attribute every switch case to the nearest enclosing `switch (...)` by indentation
    switches: list[tuple[int, int, str]] = []  # (line, indent, expr)
    for idx, line in enumerate(lines, start=1):
        sm = _SWITCH.match(line)
        if sm:
            switches.append((idx, len(sm.group(1)), sm.group(2).strip()))

    def owner(indent, line_no):
        best = None
        for ln, sind, expr in switches:
            if ln < line_no and sind < indent:
                best = (ln, expr)
        return best if best else (0, "")

    pending: list[tuple[int, str]] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = _MSG_CASE.match(line)
        if m:
            indent, name, rest = len(m.group(1)), m.group(2), m.group(3).strip()
            own_line, own_expr = owner(indent, i + 1)
            # A single source line may contain several fall-through labels.
            while (following := _MSG_CASE.match(rest)) is not None:
                pending.append((i + 1, name, indent, own_line, own_expr))
                name, rest = following.group(2), following.group(3).strip()
            next_label = i + 1
            while next_label < len(lines) and (not lines[next_label].strip() or lines[next_label].lstrip().startswith('//')):
                next_label += 1
            if not rest and next_label < len(lines) and _MSG_CASE.match(lines[next_label]):
                pending.append((i + 1, name, indent, own_line, own_expr))
                i += 1
                continue
            body_lines = [rest]
            j = i + 1
            if not re.match(r"(?:return|break)\b", rest):
                while j < len(lines) and len(body_lines) < 60:
                    if _ANY_LABEL.match(lines[j]):
                        break
                    stripped = lines[j].strip()
                    body_lines.append(stripped)
                    if re.match(r"(?:return|break)\b", stripped):
                        break
                    j += 1
            body = _clean(" ".join(body_lines))
            handler = _HANDLER.search(body)
            has_log = "log(" in body
            residual = _strip_call(body, "log") if has_log else body
            residual = re.sub(r"\b(?:return|break)\s*;", "", residual)
            residual = re.sub(r"[\s;{}\/]+", "", residual)
            residual = re.sub(r"//.*", "", residual)
            if handler:
                classification = "dispatch_handler"
            elif has_log and not residual:
                classification = "log_only"
            elif not has_log:
                classification = "inline_no_log"
            elif "host_->" in body or "->on" in body:
                classification = "log_plus_host_callback"
            elif ".store(" in body:
                classification = "log_plus_state_flag"
            else:
                classification = "log_plus_inline"
            detail = {
                "switch_line": own_line,
                "switch_expr": own_expr,
                "recv_direction": _recv_direction(body),
                "body_excerpt": body[:240],
            }
            if handler:
                detail["handler"] = handler.group(1)
            detail["host_callback"] = sorted(set(re.findall(r"host_->([A-Za-z0-9_]+)", body)))
            groups = list(pending)
            pending = []
            groups.append((i + 1, name, indent, own_line, own_expr))
            for ln, nm, ind, sl, se in groups:
                d = dict(detail)
                d["switch_line"] = sl
                d["switch_expr"] = se
                d["label_line"] = ln
                rows.append(row("carlife.dispatch", path, ln, nm, classification, **d))
            i = j if j > i else i + 1
            continue
        cm = _CH_CASE.match(line)
        if cm:
            rows.append(
                row(
                    "carlife.dispatch.channel_case",
                    path,
                    i + 1,
                    cm.group(2),
                    "channel_case",
                    switch_line=owner(len(cm.group(1)), i + 1)[0],
                )
            )
            i += 1
            continue
        dm = _DEFAULT.match(line)
        if dm:
            own_line, own_expr = owner(len(dm.group(1)), i + 1)
            body_lines = []
            j = i + 1
            while j < len(lines) and len(body_lines) < 60:
                if _ANY_LABEL.match(lines[j]):
                    break
                stripped = lines[j].strip()
                body_lines.append(stripped)
                if re.search(r"\b(?:return|break)\s*;", stripped):
                    break
                j += 1
            body = _clean(" ".join(body_lines))
            rows.append(
                row(
                    "carlife.dispatch.default_branch",
                    path,
                    i + 1,
                    "default",
                    "default_branch",
                    switch_line=own_line,
                    switch_expr=own_expr,
                    body_excerpt=body[:240],
                    host_callback=sorted(set(re.findall(r"host_->([A-Za-z0-9_]+)", body))),
                )
            )
        i += 1
    for ln, nm, _, _, _ in pending:
        unhandled.append(
            {
                "source": rel(path),
                "line": ln,
                "name": nm,
                "reason": "case label with no resolvable body (case group not closed)",
            }
        )
    meta = {
        "msg_cases": sum(1 for r in rows if r["category"] == "carlife.dispatch"),
        "channel_cases": sum(1 for r in rows if r["category"] == "carlife.dispatch.channel_case"),
        "switch_statements": len(switches),
    }
    return rows, unhandled, meta


def _recv_direction(body: str) -> str:
    if "->on" in body or "host_->" in body:
        return "host-inbound"
    if "send" in body:
        return "outbound-or-mixed"
    return "none-observed"


# ---------------------------------------------------------------------------
# Category E: carlife-vehicle-lib LibSource public headers
# ---------------------------------------------------------------------------

_LIB_DECL = re.compile(
    r"^\s*(?:virtual\s+|static\s+|explicit\s+|inline\s+)*"
    r"((?:[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?))\s+"
    r"(\*?\s*[A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{}]*)\)\s*(?:const\s*)?;\s*$"
)
_LIB_TYPEDEF_STRUCT = re.compile(r"^\s*typedef\s+struct\s+([A-Za-z_][A-Za-z0-9_]*)")
_LIB_TYPEDEF_ALIAS = re.compile(r"^\s*typedef\s+(?!struct\b)(.*?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;")
_LIB_STRUCT_CLOSE = re.compile(r"^\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")
_LIB_CLASS = re.compile(r"^\s*(class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)\s*[:{]")
_LIB_CTOR = re.compile(r"^\s*(?:virtual\s+|explicit\s+)*(~?[A-Z][A-Za-z0-9_]*)\s*\(([^;{})]*)\)\s*;\s*$")
_LIB_CB_FIELD = re.compile(
    r"^\s*(?:[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?)\s*\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*"
    r"\(([^;]*)\)\s*;\s*$"
)
_LIB_ENUM_OPEN = re.compile(r"^\s*enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{?")
_LIB_ACCESS = re.compile(r"^\s*(public|private|protected)\s*:")
_SKIP_DECL_PREFIX = ("#", "typedef", "using ", "return ", "//", "/*", "*", "}")


def collect_vehicle_lib(include_dir: Path):
    api_rows, struct_rows, other_rows, unhandled = [], [], [], []
    for path in walk_files(include_dir, suffixes={".h", ".hpp"}):
        lines = read_lines(path)
        pending_struct = None
        access = None
        cls = None
        seen_decl = 0
        for idx, line in enumerate(lines, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            am = _LIB_ACCESS.match(line)
            if am:
                access = am.group(1)
                continue
            cm = _LIB_CLASS.match(line)
            if cm:
                cls = cm.group(2)
                other_rows.append(
                    row("carlife.vehicle_lib.type", path, idx, cls, cm.group(1), access=access)
                )
                continue
            em = _LIB_ENUM_OPEN.match(line)
            if em:
                other_rows.append(row("carlife.vehicle_lib.type", path, idx, em.group(1), "enum"))
                continue
            tm = _LIB_TYPEDEF_STRUCT.match(line)
            if tm:
                pending_struct = (tm.group(1), idx)
                continue
            sm = _LIB_STRUCT_CLOSE.match(line)
            if sm and pending_struct is not None:
                struct_rows.append(
                    row(
                        "carlife.vehicle_lib.struct",
                        path,
                        pending_struct[1],
                        sm.group(1),
                        "typedef_struct",
                        tag=pending_struct[0],
                        close_line=idx,
                    )
                )
                pending_struct = None
                continue
            am2 = _LIB_TYPEDEF_ALIAS.match(line)
            if am2:
                other_rows.append(
                    row("carlife.vehicle_lib.type", path, idx, am2.group(2), "typedef", underlying=am2.group(1).strip())
                )
                continue
            if "(" in line and ");" in line and not stripped.startswith(_SKIP_DECL_PREFIX):
                dm = _LIB_DECL.match(line)
                if dm:
                    ret, name, params = dm.group(1).strip(), dm.group(2).strip(), dm.group(3).strip()
                    if name in ("if", "for", "while", "switch", "return"):
                        continue
                    seen_decl += 1
                    api_rows.append(
                        row(
                            "carlife.vehicle_lib.api",
                            path,
                            idx,
                            name,
                            "declaration",
                            returns=ret,
                            params=params,
                            class_scope=cls,
                            access=access,
                            is_static="static" in line,
                        )
                    )
                    continue
                cm2 = _LIB_CTOR.match(line)
                if cm2:
                    api_rows.append(
                        row(
                            "carlife.vehicle_lib.ctor_dtor",
                            path,
                            idx,
                            cm2.group(1),
                            "ctor_dtor",
                            params=cm2.group(2).strip(),
                            class_scope=cls,
                            access=access,
                        )
                    )
                    continue
                fm2 = _LIB_CB_FIELD.match(line)
                if fm2:
                    api_rows.append(
                        row(
                            "carlife.vehicle_lib.callback_field",
                            path,
                            idx,
                            fm2.group(1),
                            "callback_field",
                            signature=fm2.group(2).strip(),
                            class_scope=cls,
                            access=access,
                        )
                    )
                    continue
                if ";" in line and not stripped.startswith(("#", "typedef", "using")):
                    unhandled.append(
                        {"source": rel(path), "line": idx, "name": stripped[:160], "reason": "declaration-like line not matched"}
                    )
        unhandled_meta = seen_decl
    meta = {
        "files": [rel(p) for p in sorted(walk_files(include_dir, suffixes={".h", ".hpp"}))],
        "declarations_extracted": len(api_rows),
        "structs_extracted": len(struct_rows),
        "unhandled_declaration_like": len(unhandled),
    }
    return api_rows + struct_rows + other_rows, unhandled, meta


# ---------------------------------------------------------------------------
# Category F: FEATURE_CONFIG keys (upstream java) and our feature::k* keys
# ---------------------------------------------------------------------------

_JAVA_KEY = re.compile(r'^\s*public\s+static\s+final\s+String\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"([^"]*)"')


def collect_feature_config(path: Path):
    rows, unhandled = [], []
    total = 0
    for idx, line in enumerate(read_lines(path), start=1):
        if "static final String" in line:
            total += 1
        m = _JAVA_KEY.match(line)
        if m:
            const_name, key = m.group(1), m.group(2)
            group = "hardkey_name" if key.startswith("KEYCODE") else "feature_key"
            rows.append(
                row(
                    "carlife.upstream.feature_key",
                    path,
                    idx,
                    key,
                    group,
                    java_const=const_name,
                    key_group=group,
                )
            )
        elif "static final String" in line:
            unhandled.append({"source": rel(path), "line": idx, "name": line.strip(), "reason": "not matched"})
    meta = {"static_final_string_total": total, "extracted": len(rows)}
    return rows, unhandled, meta


# ---------------------------------------------------------------------------
# Category G: apollo SDK public API (internal/ excluded)
# ---------------------------------------------------------------------------

_KT_FUN_ANY = re.compile(r"\bfun\s+([A-Za-z_][A-Za-z0-9_]*)\s*[<(]")
_KT_VAL = re.compile(r"^\s*(?:override\s+)?(?:const\s+)?(?:val|var)\s+([A-Za-z_][A-Za-z0-9_]*)")
_KT_TYPE = re.compile(
    r"^\s*(?:public\s+|open\s+|abstract\s+|sealed\s+|data\s+|annotation\s+)*(interface|object|class|enum\s+class)\s+([A-Za-z_][A-Za-z0-9_]*)"
)
_JAVA_METHOD = re.compile(r"^\s*(?:public|protected)\s+(?:static\s+|final\s+|synchronized\s+|abstract\s+)*[\w<>\[\],\.\s]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def _kt_visibility(line: str) -> str:
    s = line.strip()
    for token in ("private", "protected", "internal"):
        if re.search(r"\b" + token + r"\b", s.split("fun ")[0] + "fun "):
            return token
    return "public-or-default"


def collect_apollo_sdk(sdk_root: Path):
    rows, unhandled = [], []
    files = [p for p in walk_files(sdk_root, suffixes={".kt", ".java"}) if "/internal/" not in rel(p)]
    for path in files:
        lines = read_lines(path)
        for idx, line in enumerate(lines, start=1):
            if line.lstrip().startswith(("//", "*", "/*")):
                continue
            tm = _KT_TYPE.match(line)
            if tm:
                rows.append(row("apollo.sdk.type", path, idx, tm.group(2), tm.group(1).replace(" ", "_")))
                continue
            fm = _KT_FUN_ANY.search(line)
            if fm:
                vis = _kt_visibility(line)
                rows.append(
                    row(
                        "apollo.sdk.api",
                        path,
                        idx,
                        fm.group(1),
                        "function",
                        visibility=vis,
                        signature=line.strip()[:200],
                    )
                )
                continue
            vm = _KT_VAL.match(line)
            if vm:
                vis = _kt_visibility(line)
                rows.append(
                    row(
                        "apollo.sdk.property",
                        path,
                        idx,
                        vm.group(1),
                        "property",
                        visibility=vis,
                        signature=line.strip()[:200],
                    )
                )
                continue
            if path.suffix == ".java":
                jm = _JAVA_METHOD.match(line)
                if jm and jm.group(1) not in ("if", "for", "while", "switch"):
                    vis = "public" if "public" in line else "protected"
                    rows.append(
                        row(
                            "apollo.sdk.api",
                            path,
                            idx,
                            jm.group(1),
                            "java_method",
                            visibility=vis,
                            signature=line.strip()[:200],
                        )
                    )
    meta = {
        "files_scanned": len(files),
        "extracted": len(rows),
        "excluded_path_segment": "/internal/",
        "non_public_members": sum(
            1 for r in rows if r["detail"].get("visibility") in ("private", "protected", "internal")
        ),
        "visibility_note": "visibility is derived from the declaration line; non-public members are kept visible",
    }
    return rows, unhandled, meta


# ---------------------------------------------------------------------------
# Category H: LIVI preload API + main-process channel registrations
# ---------------------------------------------------------------------------

_TS_PROP = re.compile(r"^(\s{2,4})([A-Za-z_$][\w$]*)\s*:\s*(.*)$")
_TS_OBJECT_START = re.compile(r"^const\s+([A-Za-z_$][\w$]*)\s*=\s*\{\s*$")
_TS_EXPOSE = re.compile(r"contextBridge\.exposeInMainWorld\(\s*'([^']+)'\s*,\s*([A-Za-z_$][\w$]*)\s*\)")
_TS_IPC_CALL = re.compile(r"ipcRenderer\.(invoke|send|on)\(\s*([^,)\n]+)")
_TS_CONST_STR = re.compile(r"const\s+([A-Za-z_$][\w$]*)\s*=\s*'([^']*)'")
_REG_HANDLE = re.compile(r"registerIpc(Handle|On)\(\s*(?:'([^']*)')?")
_REG_HANDLE_LITERAL = re.compile(r"'([^']+)'")


def _collect_channel_registrations(dirs, exclude_dirs=()):
    regs = {}
    for d in dirs:
        root = REPO_ROOT / d
        for path in walk_files(root, suffixes={".ts"}, exclude_dirs=exclude_dirs):
            lines = read_lines(path)
            for idx, line in enumerate(lines, start=1):
                m = _REG_HANDLE.search(line)
                if not m:
                    continue
                kind = m.group(1)
                channel = m.group(2)
                if not channel:
                    for probe in lines[idx : idx + 4]:
                        lit = _REG_HANDLE_LITERAL.search(probe)
                        if lit:
                            channel = lit.group(1)
                            break
                if not channel:
                    continue
                regs.setdefault(channel, []).append(
                    {
                        "file": rel(path),
                        "line": idx,
                        "module": path.stem,
                        "kind": "handle" if kind == "Handle" else "on",
                    }
                )
    for channel in regs:
        regs[channel].sort(key=lambda r: (r["file"], r["line"]))
    return regs


MODULE_DOMAIN = {
    "dongle": "dongle-vendor-protocol",
    "input": "input-routing",
    "transport": "transport-arbitration",
    "lifecycle": "session-lifecycle",
    "audio": "audio",
    "bluetooth": "bluetooth",
    "cluster": "cluster-display",
    "data": "media-nav-read",
    "app": "host-ui",
    "settings": "host-ui",
    "update": "host-ui",
    "utils": "host-ui",
}
# Modules not listed above keep the literal fallback `module:<file stem>` so that no
# transport protocol is invented for a channel whose owning module was not read.
UNKNOWN_DOMAIN_PREFIX = "module:"


def collect_livi_preload(path: Path, registrations):
    rows, unhandled = [], []
    lines = read_lines(path)
    # object blocks
    blocks = {}
    starts = []
    for idx, line in enumerate(lines, start=1):
        m = _TS_OBJECT_START.match(line)
        if m and m.group(1) in ("api", "appApi"):
            starts.append((idx, m.group(1)))
    expose = {}
    for line in lines:
        em = _TS_EXPOSE.search(line)
        if em:
            expose[em.group(2)] = em.group(1)
    for i, (start, varname) in enumerate(starts):
        end = starts[i + 1][0] - 1 if i + 1 < len(starts) else len(lines)
        blocks[varname] = (start, end)

    file_consts = {}
    for line in lines:
        for cm in _TS_CONST_STR.finditer(line):
            file_consts.setdefault(cm.group(1), []).append(cm.group(2))

    for varname in sorted(blocks):
        start, end = blocks[varname]
        global_name = expose.get(varname, varname)
        stack: list[tuple[int, str]] = []
        for idx in range(start, end):
            m = _TS_PROP.match(lines[idx])
            if not m:
                continue
            indent, key, rest = len(m.group(1)), m.group(2), m.group(3).strip()
            while stack and stack[-1][0] >= indent:
                stack.pop()
            if rest == "" or rest.startswith("{"):
                stack.append((indent, key))
                continue
            text = [rest]
            for probe in range(idx + 1, min(idx + 40, end)):
                nxt = lines[probe]
                if _TS_PROP.match(nxt):
                    break
                if re.match(r"^\s{0,2}\}\s*;?\s*$", nxt):
                    break
                text.append(nxt.strip())
            body = " ".join(text)
            # body-local string consts take precedence over file-wide ones, otherwise a
            # local variable such as `ch` would inherit every literal assigned to any
            # `ch` anywhere in the file.
            body_consts: dict[str, list[str]] = {}
            for cm in _TS_CONST_STR.finditer(body):
                body_consts.setdefault(cm.group(1), []).append(cm.group(2))
            channels, directions = [], set()
            unresolvable = []
            for cm in _TS_IPC_CALL.finditer(body):
                directions.add(cm.group(1))
                arg = cm.group(2).strip()
                lit = re.match(r"^'([^']*)'$", arg)
                if lit:
                    channels.append(lit.group(1))
                elif arg in body_consts:
                    channels.extend(body_consts[arg])
                elif arg in file_consts:
                    channels.extend(file_consts[arg])
                else:
                    unresolvable.append(arg)
            channels = sorted(set(channels))
            registrations_for = []
            domains = set()
            for ch in channels:
                for reg in registrations.get(ch, []):
                    registrations_for.append({"channel": ch, **reg})
                    domains.add(MODULE_DOMAIN.get(reg["module"], UNKNOWN_DOMAIN_PREFIX + reg["module"]))
            path_name = ".".join([k for _, k in stack] + [key])
            rows.append(
                row(
                    "livi.preload.api",
                    path,
                    idx + 1,
                    f"{global_name}.{path_name}",
                    "property",
                    exposed_global=global_name,
                    path=path_name,
                    channels=channels,
                    ipc_directions=sorted(directions),
                    main_modules=sorted({r["module"] for r in registrations_for}),
                    transport_domain=sorted(domains) if domains else None,
                    main_registrations=registrations_for,
                    source_scope="host/UI",
                    unresolvable_channel_args=sorted(set(unresolvable)),
                    is_input_surface=bool(domains & {"input-routing", "dongle-vendor-protocol"}),
                )
            )

    # channels used at module scope but not captured by a property row
    captured = set()
    for r in rows:
        captured.update(r["detail"]["channels"])
    for idx, line in enumerate(lines, start=1):
        for cm in _TS_IPC_CALL.finditer(line):
            arg = cm.group(2).strip()
            lit = re.match(r"^'([^']*)'$", arg)
            if lit and lit.group(1) not in captured:
                unhandled.append(
                    {
                        "source": rel(path),
                        "line": idx,
                        "name": lit.group(1),
                        "reason": "ipcRenderer channel used outside a captured api property (module-scope listener)",
                    }
                )
    return rows, unhandled


# ---------------------------------------------------------------------------
# Category I: LIVI InputCommand enum
# ---------------------------------------------------------------------------

_IC_OPEN = re.compile(r"export\s+enum\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")
_IC_ENTRY = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*'([^']*)'\s*,?\s*$")


def collect_input_command(path: Path):
    rows, unhandled = [], []
    lines = read_lines(path)
    in_enum = False
    enum_name = None
    for idx, line in enumerate(lines, start=1):
        if not in_enum:
            m = _IC_OPEN.search(line)
            if m:
                in_enum = True
                enum_name = m.group(1)
                rows.append(row("livi.input_command", path, idx, enum_name, "enum", literal=None))
            continue
        if line.strip().startswith("}"):
            in_enum = False
            continue
        em = _IC_ENTRY.match(line)
        if em:
            rows.append(
                row(
                    "livi.input_command",
                    path,
                    idx,
                    em.group(1),
                    "enum_variant",
                    literal=em.group(2),
                    enum=enum_name,
                )
            )
        elif line.strip():
            unhandled.append({"source": rel(path), "line": idx, "name": line.strip(), "reason": "enum member not matched"})
    return rows, unhandled, {"entries": sum(1 for r in rows if r["kind"] == "enum_variant")}


# ---------------------------------------------------------------------------
# Category J: LIVI native (Rust) framing/control declarations
# ---------------------------------------------------------------------------

_RS_ENUM_OPEN = re.compile(r"^\s*pub\s+enum\s+([A-Za-z_][A-Za-z0-9_]*)")
_RS_STRUCT = re.compile(r"^\s*pub\s+struct\s+([A-Za-z_][A-Za-z0-9_]*)")
_RS_CONST = re.compile(r"^\s*pub\s+const\s+([A-Z_][A-Z0-9_]*)\s*:\s*([^=]+?)\s*=\s*(.+?);")
_RS_FN = re.compile(r"^\s*pub\s+(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)")
_RS_IMPL = re.compile(r"^\s*impl(?:<[^>]*>)?\s+([A-Za-z_][A-Za-z0-9_]*)")
_RS_VARIANT = re.compile(r"^(\s{4})([A-Z][A-Za-z0-9_]*)\s*(?:[({,].*)?$")
_RS_METHOD = re.compile(r"^\s{4}pub\s+(?:async\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)")


def collect_livi_rust(dirs, other_crates_dir: Path):
    rows, unhandled = [], []
    target_files = []
    for d in dirs:
        target_files.extend(walk_files(REPO_ROOT / d, suffixes={".rs"}))
    target_files = sorted(target_files)
    target_set = {rel(p) for p in target_files}

    for path in target_files:
        lines = read_lines(path)
        impl_stack: list[str] = []
        enum_name = None
        enum_start = 0
        for idx, line in enumerate(lines, start=1):
            if enum_name:
                if line.strip().startswith("}"):
                    rows.append(row("livi.native.enum", path, enum_start, enum_name, "enum", crate=_crate_of(path)))
                    enum_name = None
                else:
                    vm = _RS_VARIANT.match(line)
                    if vm:
                        rows.append(
                            row(
                                "livi.native.enum_variant",
                                path,
                                idx,
                                f"{enum_name}::{vm.group(2)}",
                                "enum_variant",
                                enum=enum_name,
                                crate=_crate_of(path),
                                declaration=line.strip()[:160],
                            )
                        )
            em = _RS_ENUM_OPEN.match(line)
            if em and not enum_name:
                enum_name = em.group(1)
                enum_start = idx
                continue
            im = _RS_IMPL.match(line)
            if im:
                impl_stack.append(im.group(1))
                continue
            if line.strip().startswith("}"):
                if impl_stack:
                    impl_stack.pop()
                continue
            sm = _RS_STRUCT.match(line)
            if sm:
                rows.append(row("livi.native.struct", path, idx, sm.group(1), "struct", crate=_crate_of(path)))
                continue
            cm = _RS_CONST.match(line)
            if cm:
                rows.append(
                    row(
                        "livi.native.const",
                        path,
                        idx,
                        cm.group(1),
                        "const",
                        rust_type=cm.group(2).strip(),
                        value=cm.group(3).strip(),
                        crate=_crate_of(path),
                    )
                )
                continue
            fm = _RS_FN.match(line)
            if fm:
                rows.append(row("livi.native.fn", path, idx, fm.group(1), "function", crate=_crate_of(path)))
                continue
            mm = _RS_METHOD.match(line)
            if mm:
                prefix = "::".join(impl_stack[-1:]) if impl_stack else ""
                name = f"{prefix}::{mm.group(1)}" if prefix else mm.group(1)
                rows.append(row("livi.native.method", path, idx, name, "method", crate=_crate_of(path)))

    # every pub enum outside the framing/control crates stays visible
    for path in walk_files(other_crates_dir, suffixes={".rs"}):
        if rel(path) in target_set:
            continue
        lines = read_lines(path)
        enum_name = None
        enum_start = 0
        variants = []
        for idx, line in enumerate(lines, start=1):
            if enum_name:
                if line.strip().startswith("}"):
                    unhandled.append(
                        {
                            "source": rel(path),
                            "line": enum_start,
                            "name": enum_name,
                            "reason": "pub enum outside framing/control crates (not enumerated)",
                            "variants": variants,
                        }
                    )
                    enum_name = None
                    variants = []
                else:
                    vm = _RS_VARIANT.match(line)
                    if vm:
                        variants.append(vm.group(2))
                continue
            em = _RS_ENUM_OPEN.match(line)
            if em:
                enum_name = em.group(1)
                enum_start = idx
    return rows, unhandled


def _crate_of(path: Path) -> str:
    parts = rel(path).split("/")
    for i, part in enumerate(parts):
        if part == "crates" and i + 1 < len(parts):
            return parts[i + 1]
    return "native"


# ---------------------------------------------------------------------------
# Category K: LIVI projection transport interface
# ---------------------------------------------------------------------------

_TRA_EXPORT = re.compile(r"^export\s+(?:type|interface|const)\s+([A-Za-z_][A-Za-z0-9_]*)")
_TRA_CLASS = re.compile(r"^export\s+class\s+([A-Za-z_][A-Za-z0-9_]*)")
_TRA_METHOD = re.compile(r"^\s{2}(private\s+|protected\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*:\s*(.+?)\s*\{")
_TRA_FIELD = re.compile(r"^\s{2}(private\s+|protected\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([^=]+?)\s*=")


def collect_livi_transport(transport_dir: Path):
    rows, unhandled = [], []
    for path in walk_files(transport_dir, suffixes={".ts"}):
        lines = read_lines(path)
        cls = None
        for idx, line in enumerate(lines, start=1):
            cm = _TRA_CLASS.match(line)
            if cm:
                cls = cm.group(1)
                rows.append(row("livi.transport.type", path, idx, cls, "class"))
                continue
            em = _TRA_EXPORT.match(line)
            if em:
                rows.append(row("livi.transport.member", path, idx, em.group(1), "exported_declaration"))
                continue
            mm = _TRA_METHOD.match(line)
            if mm:
                vis = "private" if mm.group(1) else "public"
                rows.append(
                    row(
                        "livi.transport.method" if vis == "public" else "livi.transport.method_private",
                        path,
                        idx,
                        mm.group(2),
                        "method",
                        visibility=vis,
                        class_scope=cls,
                        params=mm.group(3).strip()[:200],
                        returns=mm.group(4).strip()[:120],
                    )
                )
                continue
            fm = _TRA_FIELD.match(line)
            if fm:
                vis = "private" if fm.group(1) else "public"
                rows.append(
                    row(
                        "livi.transport.field" if vis == "public" else "livi.transport.field_private",
                        path,
                        idx,
                        fm.group(2),
                        "field",
                        visibility=vis,
                        class_scope=cls,
                        declared_type=fm.group(3).strip()[:120],
                    )
                )
    return rows, unhandled


# ---------------------------------------------------------------------------
# Category L: our symbol occurrences (text evidence only)
# ---------------------------------------------------------------------------

_SCAN_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".hxx", ".py", ".md",
    ".kt", ".java", ".ts", ".tsx", ".rs", ".json", ".txt", ".sh", ".cmake", ".yaml", ".yml",
}


def collect_occurrences(names, roots):
    """Count literal substring occurrences of upstream names inside our tree."""
    name_to_rows: dict[str, list] = {n: [] for n in names}
    if not names:
        return {}
    plain_names = {n for n in names if not any(ch in n for ch in ".*+?^${}()|[]\\")}
    literal = sorted(plain_names, key=lambda s: (-len(s), s))
    pattern = re.compile("|".join(re.escape(n) for n in literal)) if literal else None
    for root_name in roots:
        root = REPO_ROOT / root_name
        for path in walk_files(root, suffixes=_SCAN_SUFFIXES):
            text = read_text(path)
            if pattern is None:
                break
            for idx, line in enumerate(text.splitlines(), start=1):
                for m in pattern.finditer(line):
                    token = m.group(0)
                    bucket = name_to_rows.get(token)
                    if bucket is None:
                        continue
                    if len(bucket) < 5:
                        bucket.append({"source": rel(path), "line": idx})
                    else:
                        bucket.append({"source": rel(path), "line": idx})
    return {n: name_to_rows[n] for n in sorted(name_to_rows)}


# ---------------------------------------------------------------------------
# Assembly
# ---------------------------------------------------------------------------

def collect_local_references(names, root_name, definition_file):
    """Where our own constants are referenced outside their definition header.

    This only shows text usage; it does not distinguish an outbound send path from an
    inbound handler, and it is never evidence of integration.
    """
    wanted = {n for n in names if n}
    if not wanted:
        return {}
    pattern = re.compile(
        r"\b(?:" + "|".join(re.escape(n) for n in sorted(wanted, key=lambda s: (-len(s), s))) + r")\b"
    )
    out: dict[str, list] = {n: [] for n in wanted}
    root = REPO_ROOT / root_name
    for path in walk_files(root, suffixes=_SCAN_SUFFIXES):
        if rel(path) == definition_file:
            continue
        for idx, line in enumerate(read_text(path).splitlines(), start=1):
            for m in pattern.finditer(line):
                out.setdefault(m.group(0), []).append({"source": rel(path), "line": idx})
    return {n: sorted(v, key=lambda x: (x["source"], x["line"])) for n, v in sorted(out.items())}


def collect_cp_driver_apis():
    """Supplement the UI/arbiter inventory with actual native CarPlay APIs.

    Explicit boundary: two-space class/interface method declarations and exported
    functions in driver/cp/**/*.ts and IPhoneDriver.ts, excluding private methods
    and test directories. Overloads remain individual declarations.
    """
    root = REPO_ROOT / 'Reference/LIVI/src/main/services/projection/driver'
    paths = [root / 'IPhoneDriver.ts'] + list(walk_files(root / 'cp', {'.ts'}))
    pattern = re.compile(r'^  (?:(?:public|async|static|override|abstract|get|set)\s+)*([A-Za-z_$][\w$]*)\??(?:<[^>]*>)?\s*\(')
    exported = re.compile(r'^export\s+(?:async\s+)?function\s+(\w+)')
    result = []
    for path in paths:
        if '__tests__' in path.parts or path.name.endswith('.test.ts'):
            continue
        for number, text in enumerate(read_lines(path), 1):
            match = pattern.match(text) or exported.match(text)
            if match and match[1] not in {'if', 'for', 'while', 'switch', 'catch', 'return', 'super'}:
                result.append(row('livi.cp.driver_api', path, number, match[1], 'method_or_function',
                                  signature=text.strip(), status='reference declaration; runtime parity not established'))
    return result


def build(out_dir: Path):
    R = lambda p: REPO_ROOT / p
    gaps: list[dict] = []

    kt_rows, kt_funcs, kt_unhandled, kt_meta = collect_kt_constants(
        R(SOURCES["carlife_upstream_service_types_kt"])
    )
    gaps.extend(kt_unhandled)

    h_rows, h_unhandled, h_meta = collect_local_service_types(
        R(SOURCES["carlife_local_service_types_h"])
    )
    gaps.extend(h_unhandled)

    kcm_rows = collect_keycode_name_map(R(SOURCES["carlife_local_keycode_map_cpp"]))

    dispatch_rows, dispatch_unhandled, dispatch_meta = collect_dispatch(
        R(SOURCES["carlife_local_session_cpp"])
    )
    gaps.extend(dispatch_unhandled)

    lib_rows, lib_unhandled, lib_meta = collect_vehicle_lib(
        R(SOURCES["carlife_vehicle_lib_include_dir"])
    )
    gaps.extend(lib_unhandled)

    feature_rows, feature_unhandled, feature_meta = collect_feature_config(
        R(SOURCES["carlife_feature_config_java"])
    )
    gaps.extend(feature_unhandled)

    sdk_rows, sdk_unhandled, sdk_meta = collect_apollo_sdk(R(SOURCES["apollo_sdk_root"]))
    gaps.extend(sdk_unhandled)

    registrations = _collect_channel_registrations(
        SOURCES["livi_ipc_registration_dirs"], SOURCES["livi_ipc_registration_exclude_dirs"]
    )
    preload_rows, preload_unhandled = collect_livi_preload(R(SOURCES["livi_preload_ts"]), registrations)
    gaps.extend(preload_unhandled)

    ic_rows, ic_unhandled, ic_meta = collect_input_command(R(SOURCES["livi_input_command_ts"]))
    gaps.extend(ic_unhandled)

    rust_rows, rust_unhandled = collect_livi_rust(
        SOURCES["livi_native_crates"], R(SOURCES["livi_native_other_crates_dir"])
    )
    gaps.extend(rust_unhandled)

    transport_rows, transport_unhandled = collect_livi_transport(R(SOURCES["livi_transport_dir"]))
    gaps.extend(transport_unhandled)

    all_rows = sort_rows(
        collect_cp_driver_apis()
        +
        kt_rows
        + kt_funcs
        + h_rows
        + kcm_rows
        + dispatch_rows
        + lib_rows
        + feature_rows
        + sdk_rows
        + preload_rows
        + ic_rows
        + rust_rows
        + transport_rows
    )

    # ---- comparisons -----------------------------------------------------
    upstream_msgs = [r for r in kt_rows if r["detail"].get("upstream_group") == "message"]
    local_msg_ns = [
        r
        for r in h_rows
        if r["category"] == "carlife.local.const"
        and r["detail"].get("namespace") == "msg"
        and isinstance(r["detail"].get("value"), int)
        and not r["name"] in _AUX_LOCAL_NAMES
    ]
    # MODULE_* / PROTOCOL_VERSION_MATCH* live in the same namespace but describe value
    # domains rather than CarLife message ids, so they are excluded from the id match.

    upstream_by_value: dict[int, list] = {}
    for r in upstream_msgs:
        upstream_by_value.setdefault(r["detail"]["value"], []).append(r)
    local_by_value: dict[int, list] = {}
    for r in local_msg_ns:
        local_by_value.setdefault(r["detail"]["value"], []).append(r)

    matched, upstream_only, local_only = [], [], []
    for value in sorted(set(upstream_by_value) | set(local_by_value)):
        ups = upstream_by_value.get(value, [])
        locs = local_by_value.get(value, [])
        payload = {
            "value": value,
            "value_hex": f"0x{value:08X}",
            "upstream": [{"name": r["name"], "source": r["source"], "line": r["line"]} for r in ups],
            "local": [
                {
                    "name": r["name"],
                    "source": r["source"],
                    "line": r["line"],
                    "alias_of": r["detail"].get("alias_of"),
                }
                for r in locs
            ],
        }
        if ups and locs:
            matched.append(payload)
        elif ups:
            upstream_only.append(payload)
        else:
            local_only.append(payload)

    missing_locally = [p for p in upstream_only]

    # keycode comparison
    upstream_keys = [r for r in kt_rows if r["detail"].get("upstream_group") == "keycode"]
    local_keys = [
        r
        for r in h_rows
        if r["category"] == "carlife.local.const"
        and r["detail"].get("namespace") in ("msg.keycode", "keycode")
        and isinstance(r["detail"].get("value"), int)
    ]
    up_key_by_value: dict[int, str] = {r["detail"]["value"]: r["name"] for r in upstream_keys}
    lo_key_by_value: dict[int, str] = {r["detail"]["value"]: r["name"] for r in local_keys}
    keycode_rows = []
    for value in sorted(set(up_key_by_value) | set(lo_key_by_value)):
        keycode_rows.append(
            {
                "value": value,
                "value_hex": f"0x{value:02X}",
                "upstream_name": up_key_by_value.get(value),
                "local_name": lo_key_by_value.get(value),
                "status": "match" if value in up_key_by_value and value in lo_key_by_value
                else ("upstream_only" if value in up_key_by_value else "local_only"),
            }
        )

    # feature key comparison (FEATURE_CONFIG negotiation keys only; KEYCODE_* names
    # from the same file are cross-checked against ServiceTypes.kt separately)
    upstream_feature = {r["name"] for r in feature_rows if r["detail"].get("key_group") == "feature_key"}
    upstream_java_keycode_names = {r["name"] for r in feature_rows if r["detail"].get("key_group") == "hardkey_name"}
    upstream_kt_keycode_names = {r["name"] for r in upstream_keys}
    keycode_name_crosscheck = {
        "java_only": sorted(upstream_java_keycode_names - upstream_kt_keycode_names),
        "kt_only": sorted(upstream_kt_keycode_names - upstream_java_keycode_names),
        "common": sorted(upstream_java_keycode_names & upstream_kt_keycode_names),
        "note": "both sides are upstream reference files; disagreements are upstream-internal, not ours",
    }
    local_feature = {
        r["detail"]["value"]
        for r in h_rows
        if r["category"] == "carlife.local.const"
        and r["detail"].get("namespace") in ("msg.feature", "feature")
        and isinstance(r["detail"].get("value"), str)
    }
    feature_rows_cmp = []
    for key in sorted(upstream_feature | local_feature):
        feature_rows_cmp.append(
            {
                "key": key,
                "upstream": key in upstream_feature,
                "local": key in local_feature,
                "status": "match" if key in upstream_feature and key in local_feature
                else ("upstream_only" if key in upstream_feature else "local_only"),
            }
        )

    # occurrence scan
    occurrence_names = sorted(
        {r["name"] for r in upstream_msgs}
        | {r["name"] for r in upstream_keys}
        | {r["name"] for r in ic_rows if r["kind"] == "enum_variant"}
        | local_feature
    )
    occurrences = collect_occurrences(occurrence_names, SOURCES["our_symbol_scan_roots"])
    occurrence_summary = []
    for name in occurrence_names:
        hits = occurrences.get(name, [])
        occurrence_summary.append(
            {
                "name": name,
                "occurrences_in_input_core": len(hits),
                "first_locations": hits[:5],
                "note": "literal text occurrence only; NOT evidence of integration",
            }
        )

    # where our own CarLife constants are referenced outside the definition header
    local_msg_names = sorted({r["name"] for r in local_msg_ns})
    local_refs = collect_local_references(
        local_msg_names, "Input/WirelessCarLifePlus", SOURCES["carlife_local_service_types_h"]
    )
    coverage_entries = _coverage(upstream_msgs, local_by_value, dispatch_rows)
    for entry in coverage_entries["entries"]:
        entry["local_reference_count"] = sum(len(local_refs.get(n, [])) for n in entry["local_constants"])
        entry["local_reference_locations"] = sorted(
            {loc["source"] for n in entry["local_constants"] for loc in local_refs.get(n, [])}
        )[:5]

    counts = {}
    for r in all_rows:
        counts[r["category"]] = counts.get(r["category"], 0) + 1
    counts = dict(sorted(counts.items()))

    inventory = {
        "boundary": BOUNDARY,
        "sources": SOURCES,
        "meta": {
            "service_types_kt": kt_meta,
            "service_types_h": h_meta,
            "session_cpp": dispatch_meta,
            "vehicle_lib": lib_meta,
            "feature_config_java": feature_meta,
            "apollo_sdk": sdk_meta,
            "livi_input_command": ic_meta,
            "livi_channel_registrations": len(registrations),
        },
        "counts": counts,
        "rows": all_rows,
        "comparisons": {
            "carlife_message_ids": {
                "rule": (
                    "match by numeric message id between upstream MSG_* (ServiceTypes.kt) and local "
                    "constants in namespace `msg` of Input/WirelessCarLifePlus/include/carlife/service_types.h; "
                    "local MODULE_* and PROTOCOL_VERSION_* are excluded because they are value domains, "
                    "not message ids. A numeric match is a value match, not proof of protocol equivalence."
                ),
                "matched": matched,
                "upstream_missing_locally": missing_locally,
                "local_only": local_only,
            },
            "carlife_keycodes": keycode_rows,
            "carlife_feature_keys": feature_rows_cmp,
            "carlife_upstream_keycode_names": keycode_name_crosscheck,
            "upstream_message_coverage": coverage_entries,
            "local_constant_references": {
                "root": "Input/WirelessCarLifePlus",
                "definition_file_excluded": SOURCES["carlife_local_service_types_h"],
                "entries": local_refs,
                "note": (
                    "text usage away from the definition header; inbound and outbound use are "
                    "not distinguished and this is not integration evidence"
                ),
            },
        },
        "dispatch_summary": {
            "by_classification": _tally(dispatch_rows, "kind"),
            "non_handler": [
                {
                    "name": r["name"],
                    "source": r["source"],
                    "line": r["line"],
                    "classification": r["kind"],
                    "label_line": r["detail"].get("label_line"),
                    "body_excerpt": r["detail"].get("body_excerpt"),
                }
                for r in dispatch_rows
                if r["kind"] != "dispatch_handler"
            ],
        },
        "livi_channel_domains": sorted(
            {d for v in MODULE_DOMAIN.values() for d in [v]} | {"dongle-vendor-protocol", "input-routing"}
        ),
        "occurrence_note": (
            "Text occurrences are recorded so that missing identifiers stay visible. "
            "A non-zero occurrence count MUST NOT be read as an implemented handler."
        ),
        "occurrence_summary": occurrence_summary,
        "gaps": sorted(gaps, key=lambda g: (g.get("source", ""), g.get("line", 0), g.get("name", ""))),
    }
    return inventory


def _tally(rows, key):
    out = {}
    for r in rows:
        out[r[key]] = out.get(r[key], 0) + 1
    return dict(sorted(out.items()))


def _coverage(upstream_msgs, local_by_value, dispatch_rows):
    """Per upstream MSG_* constant: does a local constant exist, and is there a
    switch case that dispatches it? Both are recorded, neither implies integration."""
    dispatched = {r["name"] for r in dispatch_rows}
    out = []
    for r in sorted(upstream_msgs, key=lambda x: (x["detail"]["value"], x["name"])):
        value = r["detail"]["value"]
        locals_ = local_by_value.get(value, [])
        local_names = sorted({x["name"] for x in locals_})
        hits = sorted(set(local_names) & dispatched)
        out.append(
            {
                "upstream_name": r["name"],
                "upstream_line": r["line"],
                "value": value,
                "value_hex": f"0x{value:08X}",
                "local_constants": local_names,
                "local_const_line": sorted({x["line"] for x in locals_}),
                "has_local_constant": bool(local_names),
                "dispatched_cases": hits,
                "has_switch_case": bool(hits),
            }
        )
    summary = {
        "upstream_message_constants": len(out),
        "with_local_constant": sum(1 for x in out if x["has_local_constant"]),
        "with_switch_case": sum(1 for x in out if x["has_switch_case"]),
        "with_local_constant_but_no_case": sum(
            1 for x in out if x["has_local_constant"] and not x["has_switch_case"]
        ),
        "no_local_constant": sum(1 for x in out if not x["has_local_constant"]),
    }
    return {"summary": summary, "entries": out}


# ---------------------------------------------------------------------------
# Markdown rendering
# ---------------------------------------------------------------------------

def render_markdown(inventory):
    L = []
    A = L.append
    A("# API inventory (CarLife+ / LIVI references vs our Input modules)")
    A("")
    A("Generated by `Input/API_AUDIT/generate_inventory.py`. Evidence collection only — see boundary notes.")
    A("")
    A("## Boundary")
    A("")
    for n in inventory["boundary"]["notes"]:
        A(f"- {n}")
    A("")
    A("Parsed sources:")
    A("")
    for k in sorted(inventory["sources"]):
        v = inventory["sources"][k]
        if isinstance(v, list):
            for item in v:
                A(f"- `{k}`: `{item}`")
        else:
            A(f"- `{k}`: `{v}`")
    A("")
    A("## Meta / parser completeness checks")
    A("")
    for k in sorted(inventory["meta"]):
        A(f"- `{k}`: `{json.dumps(inventory['meta'][k], ensure_ascii=False, sort_keys=True)}`")
    A("")
    A("## Counts by category")
    A("")
    A("| category | count |")
    A("| --- | --- |")
    for k, v in inventory["counts"].items():
        A(f"| `{k}` | {v} |")
    A("")
    A(f"Total rows: **{len(inventory['rows'])}**")
    A("")

    A("## Rows")
    A("")
    current = None
    for r in inventory["rows"]:
        if r["category"] != current:
            current = r["category"]
            A("")
            A(f"### `{current}`")
            A("")
            A("| source | line | name | kind | detail |")
            A("| --- | --- | --- | --- | --- |")
        detail = {k: v for k, v in r["detail"].items() if k not in ("main_registrations",)}
        A(
            f"| `{r['source']}` | {r['line']} | `{r['name']}` | {r['kind']} | "
            f"{json.dumps(detail, ensure_ascii=False, sort_keys=True)[:400]} |"
        )
    A("")

    c = inventory["comparisons"]
    A("## CarLife message-id comparison (upstream MSG_* vs local namespace `msg`)")
    A("")
    A("| value | hex | upstream | local |")
    A("| --- | --- | --- | --- |")
    for p in c["carlife_message_ids"]["matched"]:
        A(
            f"| {p['value']} | `{p['value_hex']}` | "
            + ", ".join(f"`{u['name']}`" for u in p["upstream"])
            + " | "
            + ", ".join(f"`{l['name']}`" for l in p["local"])
            + " |"
        )
    A("")
    A("### Upstream message ids with no local constant (visible gap)")
    A("")
    A("| value | hex | upstream |")
    A("| --- | --- | --- |")
    for p in c["carlife_message_ids"]["upstream_missing_locally"]:
        A(f"| {p['value']} | `{p['value_hex']}` | " + ", ".join(f"`{u['name']}`" for u in p["upstream"]) + " |")
    A("")
    A("### Local constants with no upstream numeric counterpart (aliases / extra)")
    A("")
    A("| value | hex | local | alias_of |")
    A("| --- | --- | --- | --- |")
    for p in c["carlife_message_ids"]["local_only"]:
        A(
            f"| {p['value']} | `{p['value_hex']}` | "
            + ", ".join(f"`{l['name']}`" for l in p["local"])
            + " | "
            + ", ".join(sorted({str(l['alias_of']) for l in p["local"]}))
            + " |"
        )
    A("")
    A("## CarLife keycode comparison")
    A("")
    A("| value | hex | upstream | local | status |")
    A("| --- | --- | --- | --- | --- |")
    for p in c["carlife_keycodes"]:
        A(f"| {p['value']} | `{p['value_hex']}` | `{p['upstream_name']}` | `{p['local_name']}` | {p['status']} |")
    A("")
    x = c["carlife_upstream_keycode_names"]
    A("Upstream-internal keycode-name disagreement (carlife-vehicle-lib java vs ServiceTypes.kt):")
    A("")
    A(f"- java only: {', '.join('`' + n + '`' for n in x['java_only']) or '(none)'}")
    A(f"- kt only: {', '.join('`' + n + '`' for n in x['kt_only']) or '(none)'}")
    A("")
    cov = c["upstream_message_coverage"]
    A("## Upstream message coverage (constant + dispatch case, NOT integration)")
    A("")
    for k, v in cov["summary"].items():
        A(f"- `{k}`: {v}")
    A("")
    A("| upstream | hex | local constant | local line | switch case | refs outside header |")
    A("| --- | --- | --- | --- | --- | --- |")
    for e in cov["entries"]:
        A(
            f"| `{e['upstream_name']}` | `{e['value_hex']}` | "
            f"{', '.join('`' + n + '`' for n in e['local_constants']) or '**(none)**'} | "
            f"{', '.join(str(l) for l in e['local_const_line']) or '-'} | "
            f"{', '.join('`' + n + '`' for n in e['dispatched_cases']) or '**(none)**'} | "
            f"{e.get('local_reference_count', 0)} |"
        )
    A("")
    A("## FEATURE_CONFIG key comparison")
    A("")
    A("| key | upstream (CarlifeConfUtil.java) | local (service_types.h feature::) | status |")
    A("| --- | --- | --- | --- |")
    for p in c["carlife_feature_keys"]:
        A(f"| `{p['key']}` | {p['upstream']} | {p['local']} | {p['status']} |")
    A("")

    d = inventory["dispatch_summary"]
    A("## session.cpp dispatch classification")
    A("")
    for k, v in d["by_classification"].items():
        A(f"- `{k}`: {v}")
    A("")
    A("Every `case msg::NAME` that is NOT a `dispatch_handler` (kept visible):")
    A("")
    A("| case | line | classification | body excerpt |")
    A("| --- | --- | --- | --- |")
    for r in d["non_handler"]:
        A(f"| `{r['name']}` | {r['line']} | {r['classification']} | `{str(r['body_excerpt'])[:200]}` |")
    A("")
    A("## LIVI channel → main-process module (transport domain)")
    A("")
    A("| exposed property | channels | domains |")
    A("| --- | --- | --- |")
    for r in inventory["rows"]:
        if r["category"] == "livi.preload.api" and r["detail"]["channels"]:
            A(
                f"| `{r['name']}` | {', '.join('`' + ch + '`' for ch in r['detail']['channels'])} | "
                f"{', '.join(r['detail']['transport_domain'] or ['(unregistered)'])} |"
            )
    A("")
    A("## Occurrence scan (text only — NOT integration evidence)")
    A("")
    A("| name | occurrences in Input/ + Core/ |")
    A("| --- | --- |")
    for r in inventory["occurrence_summary"]:
        A(f"| `{r['name']}` | {r['occurrences_in_input_core']} |")
    A("")
    A("## Unhandled / visible gaps")
    A("")
    A(f"Total: {len(inventory['gaps'])}")
    A("")
    A("| source | line | name | reason |")
    A("| --- | --- | --- | --- |")
    for g in inventory["gaps"]:
        A(f"| `{g.get('source','')}` | {g.get('line','')} | `{str(g.get('name',''))[:120]}` | {g.get('reason','')} |")
    A("")
    return "\n".join(L) + "\n"


def render_gaps_markdown(inventory):
    L = []
    A = L.append
    A("# Unhandled / visible gaps")
    A("")
    A("Declarations seen by the parser's broad probes but not captured by its extraction patterns,")
    A("plus parser-completeness counters. Nothing here is dropped silently.")
    A("")
    A("## Completeness counters")
    A("")
    for k in sorted(inventory["meta"]):
        A(f"- `{k}`: `{json.dumps(inventory['meta'][k], ensure_ascii=False, sort_keys=True)}`")
    A("")
    A("## Gap entries")
    A("")
    A("| source | line | name | reason |")
    A("| --- | --- | --- | --- |")
    for g in inventory["gaps"]:
        A(f"| `{g.get('source','')}` | {g.get('line','')} | `{str(g.get('name',''))[:160]}` | {g.get('reason','')} |")
    A("")
    return "\n".join(L) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate a deterministic read-only API inventory into Input/API_AUDIT."
    )
    ap.add_argument("--out-dir", default=str(DEFAULT_OUT_DIR), help="output directory (default: script directory)")
    ap.add_argument("--quiet", action="store_true", help="suppress the summary on stdout")
    args = ap.parse_args(argv)

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    inventory = build(out_dir)

    (out_dir / "inventory.json").write_text(
        json.dumps(inventory, ensure_ascii=False, indent=2, sort_keys=False) + "\n", encoding="utf-8", newline="\n"
    )
    (out_dir / "inventory.md").write_text(render_markdown(inventory), encoding="utf-8", newline="\n")
    (out_dir / "gaps.json").write_text(
        json.dumps(
            {
                "meta": inventory["meta"],
                "gaps": inventory["gaps"],
                "counts": inventory["counts"],
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    (out_dir / "gaps.md").write_text(render_gaps_markdown(inventory), encoding="utf-8", newline="\n")

    if not args.quiet:
        print(f"rows: {len(inventory['rows'])}")
        for k, v in inventory["counts"].items():
            print(f"  {k}: {v}")
        print(f"gaps: {len(inventory['gaps'])}")
        kt = inventory["meta"]["service_types_kt"]
        msg_rows = [
            r
            for r in inventory["rows"]
            if r["category"] == "carlife.upstream.const" and r["detail"].get("upstream_group") == "message"
        ]
        print(
            f"ServiceTypes.kt const val lines: {kt['const_val_lines_total']}"
            f" / extracted {kt['const_val_rows_extracted']} (complete={kt['const_val_complete']});"
            f" MSG_* subset {len(msg_rows)}"
        )
        print(f"message ids matched: {len(inventory['comparisons']['carlife_message_ids']['matched'])}")
        print(f"upstream ids missing locally: {len(inventory['comparisons']['carlife_message_ids']['upstream_missing_locally'])}")
        print(f"output: {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
