"""Sample synthetic packed colors using the production Metal format mapping."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'port/rex/graphics/metal/metal_texture_cache.cc').read_text()
source = source[source.index('MTL::PixelFormat MetalTextureCache::GetPixelFormatForKey'):]
case = source.split('case xenos::TextureFormat::k_1_5_5_5:', 1)[1].split('case ', 1)[0]
format_name = re.search(r'return MTL::(PixelFormat\w+);', case).group(1)
with tempfile.TemporaryDirectory() as directory:
    executable = str(Path(directory) / 'test')
    subprocess.run(['clang++', '-std=c++20', '-fobjc-arc',
                    '-DTEST_PIXEL_FORMAT=MTL' + format_name,
                    str(root / 'tests/packed_texture_format_test.mm'),
                    '-framework', 'Foundation', '-framework', 'Metal',
                    '-o', executable], check=True)
    subprocess.run([executable], check=True, timeout=30)
print('Packed texture format: RGB primaries, opaque white and transparent black passed on GPU.')
