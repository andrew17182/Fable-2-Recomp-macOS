"""Test the production cache loader using the configured Metal build dependencies."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
flags = (root / 'out/metal-build/backend/CMakeFiles/rexgpu-metal.dir/flags.make').read_text()
includes = next(line.split(' = ', 1)[1] for line in flags.splitlines()
                if line.startswith('CXX_INCLUDES ='))
# The SDK the backend was configured against (its -isystem .../mac-arm64/include): the source-built
# out/sdk-src by default, the prebuilt out/sdk only when REXGLUE_SDK_ROOT pointed there.
sdk_include = next((Path(x) for x in shlex.split(includes) if x.endswith('/mac-arm64/include')), None)
sdk_lib = sdk_include.parent / 'lib' if sdk_include else root / 'out/sdk/mac-arm64/lib'
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    executable = temp / 'test'
    subprocess.run([
        'clang++', '-std=c++23', '-fno-char8_t', '-fblocks',
        '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_FMT_EXTERNAL', '-DREX_PLATFORM_MAC=1',
        '-include', str(root / 'backends/metal/compat.h'),
        *shlex.split(includes), str(Path(__file__).with_suffix('.cpp')),
        str(sdk_lib / 'librexruntime.dylib'), str(sdk_lib / 'libxxhash.a'),
        '-Wl,-rpath,' + str(sdk_lib), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable), str(temp / 'cache')], check=True, timeout=20)
print('Shader and pipeline cache records survive two production-loader reopens.')
