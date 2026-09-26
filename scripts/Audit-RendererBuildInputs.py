"""Check explicit renderer sources and standalone public-header smoke coverage."""

from pathlib import Path
import re
import sys


def main():
    root = Path(__file__).resolve().parents[1] / "BasicRenderer"
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    cmake = re.sub(r"(?m)^\s*#.*$", "", cmake)
    sources = set(re.findall(
        r'"(?:\$\{BASICRENDERER_SOURCE_ROOT\}/)?(src/[^"\n]+\.cpp)"', cmake))
    actual_sources = {p.relative_to(root).as_posix() for p in (root / "src").rglob("*.cpp")}
    headers = {p.relative_to(root / "include").as_posix()
               for p in (root / "include" / "BasicRenderer").rglob("*")
               if p.suffix in (".h", ".hpp")}
    units = {p.relative_to(root).as_posix(): p for p in (root / "tests" / "PublicHeaders").glob("*.cpp")}
    listed_units = set(re.findall(r'"(tests/PublicHeaders/[^"\n]+\.cpp)"', cmake))
    covered = set()
    errors = []
    for name, path in units.items():
        includes = re.findall(r'^\s*#include\s*[<"](BasicRenderer/[^">]+)[">]',
                              path.read_text(encoding="utf-8-sig"), re.MULTILINE)
        if len(includes) != 1:
            errors.append(f"Smoke unit must directly include one renderer header: {name}")
        covered.update(includes)
    for label, entries in (
        ("Source missing from explicit list", actual_sources - sources),
        ("Stale source entry", sources - actual_sources),
        ("Unlisted smoke unit", units.keys() - listed_units),
        ("Stale smoke unit", listed_units - units.keys()),
        ("Header without isolated smoke unit", headers - covered),
        ("Smoke unit references missing header", covered - headers),
    ):
        errors.extend(f"{label}: {entry}" for entry in sorted(entries))
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Renderer build inputs: {len(errors)} violations; "
          f"{len(actual_sources)} C++ sources; {len(headers)} headers; {len(units)} smoke units")
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
