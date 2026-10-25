"""Port identity checks against the published v59 shader/ABI manifest."""
import hashlib
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / 'OptiScaler/shaders/dlssnr/superior/precompile'
MANIFEST = json.loads((ROOT / 'docs/superior-v59-shaders.json').read_text())

class SuperiorPort(unittest.TestCase):
    def test_v59_identity(self):
        for name, expected in MANIFEST['shaders'].items():
            with self.subTest(name=name):
                data = (SHADERS / name).read_bytes()
                if not name.endswith('.cso'):
                    data = data.replace(b'\r\n', b'\n')
                self.assertEqual(hashlib.sha256(data).hexdigest(), expected)

    def test_constant_abi(self):
        text = (ROOT / 'OptiScaler/shaders/dlssnr/superior/Constants.h').read_text()
        text = text[text.index('struct alignas(256) DlssNrConstants'):]
        text = re.sub(r'//[^\n]*', '', text[:text.index('};')])
        fields = re.findall(r'\b(uint32_t|float)\s+(\w+)(\[\d+\])?\s*;', text)
        self.assertEqual([list(f) for f in fields], MANIFEST['constant_fields'])
        self.assertEqual(sum(4 * (int(f[2][1:-1]) if f[2] else 1) for f in fields), 256)

    def test_embedded_bytecode(self):
        for path in SHADERS.glob('*.cso'):
            with self.subTest(name=path.name):
                text = path.with_suffix('.h').read_text()
                array = text[text.index('{') + 1:text.index('}')]
                self.assertEqual(bytes(int(x,16) for x in re.findall(r'0x([a-fA-F0-9]{2})',array)), path.read_bytes())

if __name__ == '__main__':
    unittest.main()
