"""Regression checks for actual renderer boundary violations, not include counts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'renderer_audit', Path(__file__).with_name('Audit-RendererBoundaries.py'))
audit_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit_module)


class BoundaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'SARP/BasicRenderer'
        self.root.mkdir(parents=True)

    def put(self, relative, text='// fixture\n'):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path.resolve()

    def rules(self, *sources):
        issues, _ = audit_module.Audit(self.root).inspect(sources)
        return {issue[0] for issue in issues}

    def test_private_implementation_dependency_needs_no_exception(self):
        self.put('BasicRenderer/src/Feature/State.h')
        source = self.put('BasicRenderer/src/Feature/State.cpp', '#include "Feature/State.h"\n')
        self.assertEqual(self.rules(source), set())

    def test_cross_feature_private_dependency_is_not_automatically_a_violation(self):
        self.put('BasicRenderer/src/Materials/State.h')
        source = self.put('BasicRenderer/src/Scene/State.cpp', '#include "Materials/State.h"\n')
        self.assertEqual(self.rules(source), set())

    def test_external_relative_private_include_is_rejected(self):
        self.put('BasicRenderer/src/Feature/State.h')
        source = self.put('../apps/Host.cpp', '#include "../BasicRenderer/BasicRenderer/src/Feature/State.h"\n')
        self.assertEqual(self.rules(source), {'consumer-private'})

    def test_public_to_private_transitive_leak_is_rejected(self):
        self.put('BasicRenderer/src/Feature/State.h')
        self.put('BasicRenderer/include/BasicRenderer/Scene/Intermediate.h', '#include "Feature/State.h"\n')
        header = self.put('BasicRenderer/include/BasicRenderer/Renderer.h', '#include <BasicRenderer/Scene/Intermediate.h>\n')
        self.assertEqual(self.rules(header), {'exported-private'})

    def test_detail_may_support_layout_but_is_not_a_consumer_entry_point(self):
        self.put('BasicRenderer/include/BasicRenderer/Runtime/Detail/Layout.h')
        header = self.put('BasicRenderer/include/BasicRenderer/Renderer.h', '#include <BasicRenderer/Runtime/Detail/Layout.h>\n')
        source = self.put('../apps/Host.cpp', '#include <BasicRenderer/Renderer.h>\n')
        self.assertEqual(self.rules(header, source), set())
        source.write_text('#include <BasicRenderer/Runtime/Detail/Layout.h>\n')
        self.assertEqual(self.rules(header, source), {'consumer-detail'})

    def test_first_party_transitive_private_leak_is_rejected(self):
        self.put('OpenRenderGraph/src/Internal.h')
        self.put('OpenRenderGraph/include/ORG.h', '#include "../src/Internal.h"\n')
        header = self.put('BasicRenderer/include/BasicRenderer/Renderer.h', '#include <ORG.h>\n')
        self.assertEqual(self.rules(header), {'exported-private'})

    def test_demo_is_a_consumer_even_under_src(self):
        self.put('BasicRenderer/src/Feature/State.h')
        source = self.put('BasicRenderer/src/BasicRenderer.cpp', '#include "Feature/State.h"\n')
        self.assertEqual(self.rules(source), {'consumer-private'})

    def test_private_cross_package_include_is_rejected(self):
        self.put('OpenRenderGraph/src/Internal.h')
        source = self.put('BasicRenderer/src/Feature/State.h', '#include "../../../OpenRenderGraph/src/Internal.h"\n')
        self.assertEqual(self.rules(source), {'cross-package-private'})

    def test_comments_do_not_create_dependencies(self):
        self.put('BasicRenderer/src/Feature/State.h')
        source = self.put('../apps/Host.cpp', '/*\n#include "Feature/State.h"\n*/\n')
        self.assertEqual(self.rules(source), set())

    def test_generic_scheduler_cannot_acquire_a_feature_manager(self):
        self.put('BasicRenderer/src/Materials/Manager.h')
        source = self.put('BasicRenderer/src/Runtime/Scheduling/Worker.cpp', '#include "Materials/Manager.h"\n')
        self.assertEqual(self.rules(source), {'mechanism-feature-private'})

    def test_renderer_composition_may_connect_features(self):
        self.put('BasicRenderer/src/Materials/Manager.h')
        source = self.put('BasicRenderer/src/Runtime/Renderer/Renderer.cpp', '#include "Materials/Manager.h"\n')
        audit = audit_module.Audit(self.root)
        issues, counts = audit.inspect([source])
        self.assertEqual(issues, [])
        self.assertEqual(counts, {'renderer-composition': 1})


if __name__ == '__main__':
    unittest.main()
