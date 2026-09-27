"""Enforce renderer boundaries; private implementation includes are not debt.

The policy lists reviewed boundary exceptions. Inventory output is descriptive,
never an allowlist. SDK/unresolved includes are checked by compilation.
"""
from __future__ import annotations
import argparse
from collections import Counter, deque
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
INCLUDES = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
SUFFIXES = {'.h', '.hpp', '.cpp', '.inl'}
PACKAGES = ('BasicScene', 'BasicRHI', 'OpenRenderGraph', 'ORGModuleServices', 'BasicTelemetry')
EXCLUDED = {'build', 'out', 'ThirdParty', 'external', 'generated', '.git', '__pycache__'}

class Audit:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.include = self.root / 'BasicRenderer/include'
        self.private = self.root / 'BasicRenderer/src'
        self.tests = self.root / 'BasicRenderer/tests'
        self.demo = self.private / 'BasicRenderer.cpp'
        self.search_roots = [self.include, self.private, self.root / 'BasicRenderer/generated']
        self.search_roots += [self.root / p / 'include' for p in PACKAGES]
        self.package_private = [self.root / p / 'src' for p in PACKAGES]
        self.graph = {}

    def name(self, path):
        return path.relative_to(self.root.parent).as_posix()

    def category(self, path):
        if path.is_relative_to(self.private): return 'private'
        if path.is_relative_to(self.include / 'BasicRenderer/Runtime/Detail'): return 'implementation-detail'
        if path.is_relative_to(self.include / 'BasicRenderer'): return 'supported-public'
        if path.is_relative_to(self.include): return 'compatibility-or-vendor'
        return 'dependency'

    def internal(self, path):
        return ((path.is_relative_to(self.private) and path != self.demo)
                or path.is_relative_to(self.tests)
                or path.is_relative_to(self.root / 'BasicRenderer/validation'))

    def owner(self, path):
        if path.is_relative_to(self.private):
            parts = path.relative_to(self.private).parts
            return '/'.join(parts[:2] if len(parts) > 2 else parts[:1])
        if path.is_relative_to(self.include / 'BasicRenderer'):
            return 'API/' + path.relative_to(self.include / 'BasicRenderer').parts[0]
        return 'consumer-or-dependency'

    def relationship(self, source, target):
        if source.is_relative_to(self.tests): return 'internal-test'
        if not source.is_relative_to(self.private): return 'internal-validation'
        parts = source.relative_to(self.private).parts
        if self.owner(source) == self.owner(target): return 'within-subsystem'
        if parts[:2] == ('Runtime', 'Renderer'): return 'renderer-composition'
        if 'GraphIntegration' in parts: return 'feature-graph-integration'
        return 'cross-subsystem implementation'

    def sources(self):
        roots = [self.root / 'BasicRenderer']
        roots += [self.root / p for p in (*PACKAGES, 'Plugins', 'CLodCacheTool', 'BRNifly', 'UsdPlugins')]
        roots += [self.root.parent / p for p in ('src', 'apps', 'include', 'tests', 'benchmarks')]
        for directory in roots:
            for path in directory.rglob('*'):
                if path.suffix in SUFFIXES and not EXCLUDED.intersection(path.relative_to(directory).parts):
                    yield path.resolve()

    def dependencies(self, source):
        if source in self.graph: return self.graph[source]
        targets = set()
        text = source.read_text(encoding='utf-8-sig', errors='replace')
        text = re.sub(r'/\*.*?\*/|//[^\r\n]*', '', text, flags=re.DOTALL)
        for include in INCLUDES.findall(text):
            for directory in [source.parent, *self.search_roots]:
                target = (directory / include.replace('\\', '/')).resolve()
                if target.is_file():
                    targets.add(target)
                    break
        self.graph[source] = targets
        return targets

    def inspect(self, sources=None):
        issues, counts = set(), Counter()
        sources = set(self.sources() if sources is None else sources)
        for source in sorted(sources):
            for target in self.dependencies(source):
                kind = self.category(target)
                if kind == 'private' and not self.internal(source) and not source.is_relative_to(self.include):
                    issues.add(('consumer-private', self.name(source), self.name(target)))
                if kind == 'implementation-detail' and not self.internal(source) and not source.is_relative_to(self.include):
                    issues.add(('consumer-detail', self.name(source), self.name(target)))
                if source.is_relative_to(self.private) and any(target.is_relative_to(p) for p in self.package_private):
                    issues.add(('cross-package-private', self.name(source), self.name(target)))
                if self.owner(source) in {'Runtime/StateGraph', 'Runtime/Scheduling'} and kind == 'private':
                    parts = target.relative_to(self.private).parts
                    infrastructure = parts[0] in {'Runtime', 'Utilities'} or self.owner(target) == 'Diagnostics/Telemetry'
                    if not infrastructure or 'RenderPasses' in parts:
                        issues.add(('mechanism-feature-private', self.name(source), self.name(target)))
                if self.internal(source) and kind == 'private':
                    counts[self.relationship(source, target)] += 1
        # Check exported-header closure, including first-party dependency headers.
        queue = deque(p for p in sources if p.is_relative_to(self.include))
        visited = set()
        while queue:
            source = queue.popleft()
            if source in visited: continue
            visited.add(source)
            for target in self.dependencies(source):
                if target.is_relative_to(self.private) or any(target.is_relative_to(p) for p in self.package_private):
                    issues.add(('exported-private', self.name(source), self.name(target)))
                elif target.suffix in SUFFIXES and target.is_relative_to(self.root):
                    queue.append(target)
        return sorted(issues), dict(sorted(counts.items()))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--write-inventory', type=Path, help='Write categorized dependencies; never approve exceptions')
    args = parser.parse_args()
    audit = Audit(ROOT)
    issues, counts = audit.inspect()
    policy_path = ROOT / 'docs/renderer-boundary-policy.json'
    policy = json.loads(policy_path.read_text()) if policy_path.exists() else {'exceptions': []}
    exceptions = {}
    for entry in policy['exceptions']:
        if not entry.get('reason', '').strip(): parser.error('Each exception needs a specific reason')
        exceptions[(entry['rule'], entry['source'], entry['target'])] = entry['reason']
    failures = [issue for issue in issues if issue not in exceptions]
    stale = sorted(set(exceptions) - set(issues))
    for rule, source, target in failures:
        print(f'Boundary violation [{rule}]: {source} -> {target}', file=sys.stderr)
    for rule, source, target in stale:
        print(f'Resolved exception must be removed [{rule}]: {source} -> {target}', file=sys.stderr)
    if args.write_inventory:
        inventory = {audit.name(p): {'category': audit.category(p), 'owner': audit.owner(p),
                     'dependencies': [audit.name(t) for t in sorted(ts) if t.is_relative_to(ROOT.parent)]}
                     for p, ts in sorted(audit.graph.items())}
        args.write_inventory.write_text(json.dumps(inventory, indent=2) + '\n', encoding='utf-8')
    print(f'Renderer boundaries: {len(failures)} violations; {len(issues) - len(failures)} reviewed exceptions; {len(stale)} stale exceptions')
    print('Private implementation dependencies (informational): ' + json.dumps(counts))
    return int(bool(failures or stale))

if __name__ == '__main__':
    raise SystemExit(main())
