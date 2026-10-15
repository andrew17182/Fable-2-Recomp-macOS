#!/usr/bin/env python3
"""Incrementally build the checkpoint experiment using existing local caches.

Does not stage or install. This is a local development helper, not a clean build.
"""
from pathlib import Path
import os
import shlex,subprocess
root=Path(__file__).resolve().parents[1];build=root/'out/build/mac-arm64-release';old=os.getenv('FABLE2_OLD_BUILD_ROOT') or str(root)  # previous absolute checkout path baked into old build caches, if any
flags={}
for l in (build/'CMakeFiles/fable_2.dir/flags.make').read_text().splitlines():
 if ' = ' in l:
  k,v=l.split(' = ',1);flags[k]=shlex.split(v.replace(old,str(root)))
output=root/'out/validation/telemetry-build';output.mkdir(parents=True,exist_ok=True)
subprocess.run(['/usr/bin/clang++',*flags['CXX_DEFINES'],*flags['CXX_INCLUDES'],*flags['CXX_FLAGS'],'-c',str(root/'src/main.cpp'),'-o',str(output/'main.cpp.o')],check=True)
link=shlex.split((build/'CMakeFiles/fable_2.dir/link.txt').read_text().replace(old,str(root)))
link=[str(output/'main.cpp.o') if x=='CMakeFiles/fable_2.dir/src/main.cpp.o' else x for x in link]
link[link.index('-o')+1]=str(output/'fable_2')
subprocess.run(link,cwd=build,check=True)
print('Built',output/'fable_2')

from pathlib import Path
import shlex, subprocess
root=Path(__file__).resolve().parents[1];build=root/'out/metal-build/backend';out=root/'out/validation/screenshot-timestamps';out.mkdir(parents=True,exist_ok=True)
old=os.getenv('FABLE2_OLD_BUILD_ROOT') or str(root)  # previous absolute checkout path baked into old build caches, if any
vars=dict(line.split(' = ',1) for line in (build/'CMakeFiles/rexgpu-metal.dir/flags.make').read_text().replace(old,str(root)).splitlines() if ' = ' in line)
changes=[('OBJCXX','ui/metal/metal_test_control.mm'),('CXX','ui/metal/metal_provider.cc'),('OBJCXX','ui/metal/metal_presenter.mm'),('OBJCXX','ui/metal/metal_immediate_drawer.mm'),('CXX','graphics/metal/metal_command_processor.cc'),('CXX','graphics/metal/metal_graphics_system.cc')]
for lang,source in changes:
 name=Path(source).stem+'.o'
 cmd=['/usr/bin/clang++']+sum([shlex.split(vars[lang+'_'+v]) for v in ['DEFINES','INCLUDES','FLAGS']],[])+['-c',str(root/'backends/metal/port/rex'/source),'-o',str(out/name)]
 r=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'.build.log')).write_text(r.stdout+r.stderr);print(name,r.returncode,flush=True)
 if r.returncode:print(r.stderr[-3000:]);r.check_returncode()
stable=root/'out/validation/stable30-automation-20261001'
cmd=shlex.split((build/'CMakeFiles/rexgpu-metal.dir/link.txt').read_text().replace(old,str(root)))
replacements={Path(source).name+'.o':out/(Path(source).stem+'.o') for _,source in changes}
replacements.update({n+'.cc.o':stable/(n+'.o') for n in ['metal_pipeline_cache']})
cmd=[str(replacements[Path(arg).name]) if Path(arg).name in replacements else arg for arg in cmd]
for p in [out/'metal_test_control.o',stable/'metal_frame_log.o',stable/'metal_startup_wait.o']:cmd.insert(1,str(p))
cmd[cmd.index('-o')+1]=str(out/'librexgpu-metal.dylib')
r=subprocess.run(cmd,cwd=build,capture_output=True,text=True);(out/'link.log').write_text(r.stdout+r.stderr);print('link',r.returncode,r.stderr[-2500:],flush=True);r.check_returncode()
