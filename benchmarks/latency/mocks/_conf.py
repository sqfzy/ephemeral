"""Minimal bench.conf parser (INI with optional global header).

bench.conf has the shape::

    # comment
    key_a = value
    key_b = value

    [section_name]
    key_c = value

Everything before the first ``[section]`` is treated as globals.
Each ``[section]`` is a plain key=value block.

This mirrors what the C++ ``bench::load_common_config`` + the
scenario-specific loader do, so a Python mock and the C++ client
read exactly the same file.
"""

from typing import Dict, Tuple


def load(path: str) -> Tuple[Dict[str, str], Dict[str, Dict[str, str]]]:
    """Return (globals, sections) parsed from a bench.conf-like file."""
    globals_: Dict[str, str] = {}
    sections: Dict[str, Dict[str, str]] = {}
    current: Dict[str, str] = globals_

    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                name = line[1:-1].strip()
                current = sections.setdefault(name, {})
                continue
            if "=" not in line:
                # Tolerate stray lines; the C++ side ignores them too.
                continue
            key, _, value = line.partition("=")
            # Strip inline comments conservatively (only full-line handled above).
            current[key.strip()] = value.strip()

    return globals_, sections


def get_scenario(path: str, name: str) -> Dict[str, str]:
    """Return merged globals+section for scenario ``name``.

    Section values override globals. Missing section -> just globals.
    """
    g, s = load(path)
    merged: Dict[str, str] = {}
    merged.update(g)
    merged.update(s.get(name, {}))
    return merged
