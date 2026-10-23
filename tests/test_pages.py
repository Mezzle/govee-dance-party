"""Release packaging must never publish missing or mismatched firmware."""
import json
import tempfile
import unittest
from pathlib import Path
from tools.prepare_pages import BOARDS, prepare


class PagesTests(unittest.TestCase):
    def test_complete_release(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            assets = root / 'assets'
            assets.mkdir()
            names = [f'govee_party-{board}.bin' for board in BOARDS]
            names += ['govee_party-momentum.fap', 'govee_party-official.fap']
            for name in names:
                (assets / name).write_bytes(name.encode())
            output = root / 'site'
            prepare(assets, output, 'v0.2.0')
            for board, (_, chip) in BOARDS.items():
                manifest_path = output / 'firmware' / f'{board}.json'
                manifest = json.loads(manifest_path.read_text())
                self.assertEqual(manifest['version'], 'v0.2.0')
                self.assertEqual(manifest['builds'][0]['chipFamily'], chip)
                part = manifest['builds'][0]['parts'][0]
                self.assertEqual(part['offset'], 0)
                self.assertEqual((manifest_path.parent / part['path']).read_bytes(),
                                 (assets / f'govee_party-{board}.bin').read_bytes())
            self.assertTrue((output / 'index.html').is_file())
            (assets / names[-1]).write_bytes(b'')
            with self.assertRaises(ValueError):
                prepare(assets, root / 'broken-site', 'v0.2.0')
            self.assertFalse((root / 'broken-site').exists())


if __name__ == '__main__':
    unittest.main()
