#!/usr/bin/env python3
"""Publishing must never destroy the previous app on a failed rebuild."""
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('packager', Path(__file__).resolve().parents[1] / 'tools/package_macos_app.py')
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)

class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.old = self.make_app('Fable II.app', 'old')
        self.new = self.make_app('staging/Fable II.app', 'new')
        self.state = self.root / 'Application Support/saves/hero.bin'
        self.state.parent.mkdir(parents=True)
        self.state.write_bytes(b'player progress')

    def make_app(self, name, value):
        app = self.root / name
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.fable2recomp.metal'}))
        (app / 'Contents/version').write_text(value)
        return app

    def test_repeat_build_replaces_app_and_preserves_external_state(self):
        packager.publish(self.new, self.old, replace=True)
        self.assertEqual((self.old / 'Contents/version').read_text(), 'new')
        self.assertEqual(self.state.read_bytes(), b'player progress')

    def test_failed_publish_restores_previous_app(self):
        rename = Path.rename
        def fail(source, destination):
            if source == self.new:
                raise OSError('simulated publication failure')
            return rename(source, destination)
        with patch.object(Path, 'rename', fail):
            with self.assertRaises(OSError):
                packager.publish(self.new, self.old, replace=True)
        self.assertEqual((self.old / 'Contents/version').read_text(), 'old')

    def test_unrelated_app_is_preserved(self):
        (self.old / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier':'other.app'}))
        with self.assertRaises(ValueError):
            packager.publish(self.new, self.old, replace=True)
        self.assertEqual((self.old / 'Contents/version').read_text(), 'old')

    def test_symlink_is_not_followed(self):
        link = self.root / 'linked.app'
        link.symlink_to(self.old)
        with self.assertRaises(ValueError):
            packager.publish(self.new, link, replace=True)
        self.assertEqual((self.old / 'Contents/version').read_text(), 'old')

    def test_manual_packaging_needs_explicit_replace(self):
        with self.assertRaises(FileExistsError):
            packager.publish(self.new, self.old)
        self.assertEqual((self.old / 'Contents/version').read_text(), 'old')

class NoConverterTests(unittest.TestCase):
    """The release (no-converter) package: shipped notices drop the converter libraries, keep the MSC headers row, and
    the portable package check refuses a converter dylib (docs/no-converter.md)."""
    def test_notices_without_converter(self):
        full, lean = packager.notices_text(True), packager.notices_text(False)
        self.assertIn('libdxilconv', full)
        self.assertNotIn('libdxilconv', lean)
        self.assertNotIn('libmetalirconverter', lean)
        self.assertIn('Metal Shader Converter 3.1.1 runtime headers', lean)
        self.assertIn('Apache-2.0', lean)

    def test_portable_check_refuses_converter_dylib(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import fable2_portable as portable
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / 'Fable II.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            (app / 'Contents/Frameworks').mkdir()
            (app / 'Contents/Frameworks/libmetalirconverter.dylib').write_bytes(b'x')
            with self.assertRaisesRegex(RuntimeError, 'shader converter'):
                portable.check_package(app)
            portable.check_package(app, converter=True)  # --with-converter allows it

    def test_portable_check_refuses_personal_identifiers_in_binaries(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import fable2_portable as portable
        with tempfile.TemporaryDirectory() as temp:
            app = Path(temp) / 'Fable II.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            binary = app / 'Contents/MacOS/fable_2'
            binary.write_bytes(b'\0assert ./src/core/x.cpp\0')
            portable.check_package(app)
            # __FILE__ of a build without -ffile-prefix-map (spelled in pieces: tests/name_scrub_tests.py)
            binary.write_bytes(b'\0' + str(Path.home()).encode() + b'/src/core/x.cpp\0')
            with self.assertRaisesRegex(RuntimeError, 'absolute local path in Contents/MacOS/fable_2'):
                portable.check_package(app)
            binary.write_bytes(b'\0/Us' + b'ers/someone/x.cpp\0')
            with self.assertRaisesRegex(RuntimeError, 'personal identifier in Contents/MacOS/fable_2: home path'):
                portable.check_package(app)

if __name__ == '__main__':
    unittest.main()
