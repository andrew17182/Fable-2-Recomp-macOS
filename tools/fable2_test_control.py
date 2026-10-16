#!/usr/bin/env python3
"""App-owned test API. No OS accessibility or screen capture permissions required."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fable2_common as common  # noqa: E402  (main checkout vs agent slot paths, resource guards)

ROOT = common.ROOT
APP = ROOT / 'out/build/mac-arm64-metal-release/Fable II.app'
WINDOW_POSITION = 'fable2_window.toml'
KEYS = ('e', 'x', 'z', 'c', 'w', 'a', 's', 'd', 'q', 'tab', 'escape', 'return', 'up', 'down', 'left', 'right', 'm', '5', '6', '7', '8', 'f5', 'f6', 'f7', 'f9', 'share', 'view')  # share/view: controller buttons via a virtual pad

# Saves written by this checkout's sessions. In an agent slot this is the slot's own library; the main library
# in the main checkout (SHARED_SAVE_LIBRARY) is read-only there (local agent docs).
SAVE_LIBRARY = ROOT / 'out/save-library'
SHARED_SAVE_LIBRARY = common.MAIN_ROOT / 'out/save-library'


def library_dirs():
    return [SAVE_LIBRARY] + ([SHARED_SAVE_LIBRARY] if SHARED_SAVE_LIBRARY != SAVE_LIBRARY else [])


def library_entry(name):
    """Folder of library save `name`: this checkout's library first, then the main checkout's."""
    for folder in library_dirs():
        if (folder / name / 'save.json').is_file(): return folder / name
    return SAVE_LIBRARY / name
# Signed-in profile container (XUID B13EBABEBABEBABE, title 4D5307F1, content type 00000001).
SAVE_CONTAINER = Path('saves/B13EBABEBABEBABE/4D5307F1')
HERO_SLOTS = SAVE_CONTAINER / '00000001'
HERO_HEADERS = SAVE_CONTAINER / 'Headers/00000001'


def _xml_fields(path, names):
    import re
    try:
        text = Path(path).read_bytes()[4:].decode('utf-8', 'replace')  # 4-byte BE length prefix
    except FileNotFoundError:
        return {}
    fields = {}
    for name in names:
        match = re.search(r'<%s type="[^"]*">([^<]*)</%s>' % (name, name), text)
        if match: fields[name] = match.group(1).split(' ')[0]
    return fields


def describe_save(hero_dir, header):
    """Offline description of one HeroNNN save (header display name + chapter/hero XML)."""
    hero_dir, header = Path(hero_dir), Path(header)
    raw = header.read_bytes()
    display = raw[8:0x108].decode('utf-16-be', 'replace').split('\0')[0]  # UTF-16BE display name
    folder = raw[0x108:0x138].split(b'\0')[0].decode('ascii', 'replace')
    info = dict(folder=hero_dir.name, header_display_name=display, header_folder_name=folder,
                saved_mtime=time.strftime('%Y-%m-%d %H:%M:%S', time.localtime((hero_dir / 'mainsave.bin').stat().st_mtime)),
                mainsave_sha256=hashlib.sha256((hero_dir / 'mainsave.bin').read_bytes()).hexdigest())
    info.update(_xml_fields(hero_dir / 'chaptersave.bin', ('Chapter', 'LevelName', 'QuestName', 'RichPresenceType')))
    info.update(_xml_fields(hero_dir / 'herosave.bin', ('Money', 'Gender', 'Renown', 'Morality')))
    return info


def import_save(name, source, hero, note=None):
    """Copy one hero save (folder + header) from an app-state root or saves folder into the library."""
    source = Path(source).expanduser().resolve()
    if (source / 'saves').is_dir(): source = source / 'saves'
    container = source / SAVE_CONTAINER.relative_to('saves')
    hero_dir, header = container / '00000001' / hero, container / 'Headers/00000001' / (hero + '.header')
    if not hero_dir.is_dir() or not header.is_file(): raise ValueError(f'No {hero} save/header under {container}')
    entry = SAVE_LIBRARY / name
    if entry.exists(): raise ValueError(f'Library entry already exists: {entry}')
    entry.mkdir(parents=True)
    shutil.copytree(hero_dir, entry / hero)
    shutil.copy2(header, entry / header.name)
    meta = dict(name=name, source=str(hero_dir), imported=time.strftime('%Y-%m-%d %H:%M:%S'), **describe_save(hero_dir, header))
    if note: meta['note'] = note
    (entry / 'save.json').write_text(json.dumps(meta, indent=2) + '\n')
    return meta


def list_saves():
    entries, seen = [], set()
    for meta in sorted(m for folder in library_dirs() for m in folder.glob('*/save.json')):
        if meta.parent.name in seen: continue
        seen.add(meta.parent.name)
        data = json.loads(meta.read_text())
        entries.append({k: data.get(k) for k in ('name', 'folder', 'header_display_name', 'QuestName', 'LevelName', 'saved_mtime', 'note')})
    return entries


def apply_library_save(state, name):
    """Make library save `name` the only hero save in a session's copied state.

    Profile settings (saves/4D5307F1/profile) and achievements are kept; every other
    HeroNNN folder and header is removed so the Continue screen offers one card."""
    entry = library_entry(name)
    meta = json.loads((entry / 'save.json').read_text())
    hero = meta['folder']
    slots, headers = Path(state) / HERO_SLOTS, Path(state) / HERO_HEADERS
    for folder in (slots, headers):
        if folder.exists(): shutil.rmtree(folder)
        folder.mkdir(parents=True)
    shutil.copytree(entry / hero, slots / hero)
    shutil.copy2(entry / (hero + '.header'), headers / (hero + '.header'))
    return meta


def send(session, command):
    """Write one request without waiting; returns the reply path (poll with wait_reply)."""
    session = Path(session).resolve()
    folder = session / 'test-state/control'
    ident = str(uuid.uuid4())
    temporary = folder / (ident + '.tmp')
    temporary.write_text(command)
    temporary.rename(folder / (ident + '.req'))
    return folder / (ident + '.reply')


def wait_reply(reply, timeout=20, poll=.05):
    reply = Path(reply)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if reply.exists():
            response = reply.read_text()
            if not response.startswith('ok '): raise RuntimeError(response)
            return response[3:]
        time.sleep(poll)
    raise TimeoutError('Game did not reply; see the session log')


def request(session, command, timeout=20):
    session = Path(session).resolve()
    metadata = json.loads((session / 'session.json').read_text())
    reply = send(session, command)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if reply.exists():
            response = reply.read_text()
            if not response.startswith('ok '): raise RuntimeError(response)
            return response[3:]
        time.sleep(.05)
    raise TimeoutError('Game did not reply; see the session log')


def copy_latest_window_position(state, source):
    # Geometry is a desktop preference, independent from the copied game saves.
    # Native code saves every move atomically, even when shutdown later crashes.
    candidates = [Path(source) / WINDOW_POSITION,
                  Path.home() / 'Library/Application Support/Fable II Recomp' / WINDOW_POSITION]
    candidates.extend((ROOT / 'out/validation/gameplay-tests').glob('*/test-state/' + WINDOW_POSITION))
    available = []
    for candidate in candidates:
        try:
            if candidate.is_file():
                available.append((candidate.stat().st_mtime_ns, candidate))
        except FileNotFoundError:
            pass  # A session may have been removed between enumeration and stat.
    if not available:
        return None
    latest = max(available, key=lambda entry: entry[0])[1]
    shutil.copy2(latest, Path(state) / WINDOW_POSITION)
    return str(latest)


def launch(display_sync=True, record=False, replay=None, state_source=None, checkpoint=False, allow_build_mismatch=False, replay_timeout=60, save=None, background=True, game_args=None, cache_snapshot=None):
    if replay_timeout<=0:
        raise ValueError("Replay transition timeout must be positive")
    # Agent-driven tests: out/test-control-env.json may name a library save under
    # "save" (MCP launches cannot always pass one) plus FABLE2_* variables.
    _env_file = ROOT / 'out/test-control-env.json'
    if save is None and not replay and _env_file.is_file():
        save = json.loads(_env_file.read_text()).get('save') or None
    if save and not (library_entry(save) / 'save.json').is_file():
        raise ValueError(f'Unknown library save {save!r}; see `fable2_test_control.py saves`')
    if replay:
        from fable2_replay import load_recording
        recording = load_recording(replay)
        recording_folder = Path(replay).resolve().parent
        # Accept the native events file after closing without an explicit stop.
        if recording_folder.name == 'control' and recording_folder.parent.name == 'test-state':
            recording_folder = recording_folder.parent.parent
        metadata = json.loads((recording_folder / "session.json").read_text())
        state_source = recording_folder / "initial-state"
        if not state_source.is_dir(): raise ValueError("Recording has no initial save/settings snapshot")
        digest = hashlib.sha256((APP / "Contents/MacOS/fable_2").read_bytes()).hexdigest()
        if metadata.get("executable_sha256") != digest and not allow_build_mismatch:
            raise ValueError("Recording belongs to a different game build")
        if record: raise ValueError("Cannot record and replay in the same session")
    # Other instances (e.g. the player's playtest game) may run: this session uses its own state copy and
    # only ever addresses its own pid (control/pid.txt). Contention is recorded in session.json.
    # Agent shader cache (tools/fable2_agent_cache.py) for sessions from the live state; an explicit state source
    # (replay snapshot, fable2_cache_warm.py warming a player's cache) keeps its own cache.
    agent_cache_mode = None
    if not state_source:
        agent_cache_mode = cache_snapshot or (json.loads(_env_file.read_text()).get('FABLE2_AGENT_CACHE')
                                              if _env_file.is_file() else None) or os.environ.get('FABLE2_AGENT_CACHE') or 'auto'
    source = Path(state_source) if state_source else Path.home() / 'Library/Application Support/Fable II Recomp'
    common.ensure_free_space('a test launch')
    # Perf lock: hold perf.lock shared for the game's life (local agent docs); waits for a perf run. Then the
    # agent game limit (one of FABLE2_AGENT_GAMES game slots, and a 3x slot at 3x).
    scale_env = dict(os.environ)
    if _env_file.is_file():
        scale_env.update({k: str(v) for k, v in json.loads(_env_file.read_text()).items() if k.startswith('FABLE2_')})
    hold = common.test_game_hold('test_control launch', common.launch_scale(scale_env, source))
    try:
        return _launch_held(hold, display_sync, record, replay, state_source, checkpoint, allow_build_mismatch,
                            replay_timeout, save, background, source, game_args, agent_cache_mode)
    finally:
        hold.close()  # no-op once handed to the holder process


def _launch_held(hold, display_sync, record, replay, state_source, checkpoint, allow_build_mismatch, replay_timeout,
                 save, background, source, game_args=None, agent_cache_mode=None):
    import fable2_instances as instances
    common.wait_instance_room('test launch', lambda: len(instances.instances()))
    session = ROOT / 'out/validation/gameplay-tests' / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    state = session / 'test-state'
    control = state / 'control'
    control.mkdir(parents=True, mode=0o700)
    agent_cache_record = None
    for folder in ('saves', 'cache', 'metadata'):
        if folder == 'cache' and agent_cache_mode:
            import fable2_agent_cache as agent_cache
            agent_cache_record = agent_cache.seed(state / 'cache', APP, agent_cache_mode)
            continue
        if (source / folder).exists(): common.clone(source / folder, state / folder)  # APFS clone: ~0 disk until written
    for name in ('fable_2.toml', 'fable2_config.toml', 'fable2_patches.toml'):
        if (source / name).exists(): shutil.copy2(source / name, state / name)
    save_meta = apply_library_save(state, save) if save else None
    # Window placement: test windows remember their own (tools/fable2_test_windows.py), never the player's play window;
    # FABLE2_TEST_WINDOW_MEMORY=0 (environment or out/test-control-env.json) restores the latest-position copy.
    import fable2_test_windows as test_windows
    env_file = ROOT / 'out/test-control-env.json'
    env_file_data = json.loads(env_file.read_text()) if env_file.is_file() else {}
    test_window = None
    if test_windows.enabled(dict(os.environ, **{k: str(v) for k, v in env_file_data.items() if k.startswith('FABLE2_')})):
        test_window = test_windows.prepare(state, position_only=test_windows.fixed_size(
            [str(a) for a in game_args or []] + [str(a) for a in env_file_data.get('args', [])]))
        window_position_source = 'test-window %s (%s)' % (test_window['key'], test_window['source'])
    else:
        window_position_source = copy_latest_window_position(state, source)
    if record:
        shutil.copytree(state, session / 'initial-state', ignore=shutil.ignore_patterns('control'))
    origin_us = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000
    env = os.environ.copy()
    env.update(FABLE2_SESSION_DIR=str(control), FABLE2_SESSION_ORIGIN_US=str(origin_us))
    env.pop('FABLE2_RECORD_INPUT', None)
    env.pop('FABLE2_CHECKPOINT_DIR', None)
    if checkpoint: env['FABLE2_CHECKPOINT_DIR'] = str(control)
    if record: env['FABLE2_RECORD_INPUT'] = '1'
    # Optional extra FABLE2_*/REX_* settings for agent-driven tests: MCP launches cannot
    # pass environment variables, so read them from an ignored file when present.
    extra_env = {}
    extra_env_file = ROOT / 'out/test-control-env.json'
    if extra_env_file.is_file():
        extra_env = {k: str(v) for k, v in json.loads(extra_env_file.read_text()).items()
                     if k.startswith(('FABLE2_', 'REX_'))}
        env.update(extra_env)
    # Background test mode (default): test presses reach the game without focus
    # and no focus requests are made (src/input/test_background.h); the game is
    # launched through LaunchServices with `open -g` so it never activates.
    # --foreground restores the old behaviour (direct exec, focus on launch and
    # before each press).
    env.pop('FABLE2_TEST_BACKGROUND', None)
    if background: env['FABLE2_TEST_BACKGROUND'] = '1'
    # Test-only instance: physical controller/keyboard/mouse never reach it (local agent docs).
    env.update(FABLE2_PHYSICAL_INPUT='0', FABLE2_INSTANCE_ROLE='agent', FABLE2_INSTANCE_ID=session.name)
    # Window-title tag (src/core/fable2_window_title.h); a caller's FABLE2_WINDOW_TAG (warm-up) wins.
    env.setdefault('FABLE2_WINDOW_TAG', common.window_tag())
    env.update(FABLE2_APP_STATE_ROOT=str(state), FABLE2_GAME_DATA_ROOT=str(ROOT), FABLE2_TEST_CONTROL_DIR=str(control))
    common.force_test_intro_skip(env, ROOT)  # tests always skip the intros + fast title (FABLE2_TEST_INTRO=1 opts out)
    extra_args = [str(a) for a in game_args or []]  # e.g. --async_shader_compilation=false (cache warm-up)
    # REX_VSYNC=0 (environment or out/test-control-env.json; f2diag/dev-session "uncapped") turns vsync off. Before
    # 2026-10-14 this always passed --vsync=true, which outranked REX_VSYNC, so "uncapped" test-control runs were capped.
    vsync = env.get('REX_VSYNC', '1') != '0'
    game_args = ['--vsync=' + ('true' if vsync else 'false'),
                 '--metal_allow_tearing=' + ('false' if display_sync else 'true')] + extra_args
    if extra_env_file.is_file():
        game_args += [str(a) for a in json.loads(extra_env_file.read_text()).get('args', [])]
    # Every test launch: no "reopen windows?" alert after a crash (fable2_common.TEST_LAUNCH_APPKIT_ARGS).
    game_args += list(common.TEST_LAUNCH_APPKIT_ARGS)
    if background:
        game_args.append('--audio_mute=true')  # background test sessions are silent (SDK cvar)
        # A process exec'd from here is activated by macOS when its window opens
        # (SDL's no-activate hints are not enough). LaunchServices with -g keeps
        # it in the background, like fable2_dev_session.py. `open` passes only
        # the variables given with --env, so forward the game's settings.
        command = ['open', '-g', '-n']
        for key, value in env.items():
            if key.startswith(('FABLE2_', 'SDL_', 'MTL_', 'REX')):
                command += ['--env', f'{key}={value}']
        subprocess.run(command + [str(APP), '--args'] + game_args, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process = None
    else:
        process = subprocess.Popen([str(APP / 'Contents/MacOS/FableIILauncher')] + game_args, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    (session / 'session.json').write_text(json.dumps(dict(pid=process.pid if process else None, display_sync=display_sync,
        state=str(state), source_state=str(source), start_unix=time.time(),
        origin_us=origin_us, record=record, checkpoint=checkpoint,
        allow_build_mismatch=allow_build_mismatch, replay_transition_timeout=replay_timeout,
        window_position_source=window_position_source, test_window=test_window, background=background,
        library_save=save, library_save_folder=save_meta['folder'] if save_meta else None,
        extra_env=extra_env, agent_game_slots=hold.games, agent_game_wait_s=hold.games_wait,
        perf_lock_shared_wait_s=hold.wait, agent_cache=agent_cache_record,
        executable_sha256=hashlib.sha256((APP / "Contents/MacOS/fable_2").read_bytes()).hexdigest()), indent=2) + '\n')
    if replay:
        subprocess.Popen(["/usr/bin/python3", str(ROOT / "tools/fable2_replay.py"),
                          str(Path(replay).resolve()), str(session), "--timeout", str(replay_timeout)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 40
    while not (control / 'pid.txt').exists():
        if process and process.poll() is not None: raise RuntimeError('Game exited during launch; see ' + str(state / 'logs'))
        if time.monotonic() > deadline: raise TimeoutError('Test control did not start; see ' + str(state / 'logs'))
        time.sleep(.1)
    if not process:
        # LaunchServices launch: record the game's own pid (pid.txt is written atomically).
        metadata = json.loads((session / 'session.json').read_text())
        metadata['pid'] = int((control / 'pid.txt').read_text())
    else:
        metadata = json.loads((session / 'session.json').read_text())
    import fable2_instances as instances
    instances.register(metadata['pid'], 'agent', APP, session, True, env, state)
    if test_window: test_windows.update_claim(test_window['key'], metadata['pid'])
    hold.hand_off(metadata['pid'], session)
    metadata['perf_lock_shared_wait_s'] = hold.wait
    others = instances.contention(metadata['pid'])
    metadata.update(role='agent', input='test-only', concurrent_instances=others['others'], contention=others['text'] or None)
    (session / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
    if test_window: test_windows.spawn_watch(session)  # publishes the placement when the game exits
    if agent_cache_record and agent_cache_record.get('publish'):
        import fable2_agent_cache as agent_cache
        agent_cache.spawn_watch(session)  # grows the agent shader cache after a clean exit
    if not background: request(session, 'focus')
    return str(session)


def capture_with_metadata(session, screen=False):
    requested_us = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000
    ppm = Path(request(session, 'capture screen' if screen else 'capture'))
    received_us = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000
    sidecar = ppm.with_suffix('.capture.json')
    metadata = json.loads(sidecar.read_text()) if sidecar.exists() else {
        'schema': 1, 'clock': 'CLOCK_MONOTONIC',
        'native_timestamps_available': False,
    }
    png = ppm.with_suffix('.png')
    subprocess.run(['sips', '-s', 'format', 'png', str(ppm), '--out', str(png)],
                   check=True, stdout=subprocess.DEVNULL)
    ready_us = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000
    metadata.update(path=str(png), request_sent_monotonic_us=requested_us,
                    reply_received_monotonic_us=received_us,
                    image_ready_monotonic_us=ready_us)
    if 'capture_completed_monotonic_us' in metadata:
        metadata['native_timestamps_available'] = True
        metadata['capture_duration_us'] = metadata['capture_completed_monotonic_us'] - metadata['capture_started_monotonic_us']
        metadata['delivery_and_conversion_us'] = ready_us - metadata['capture_completed_monotonic_us']
        if metadata.get('frame_submitted_monotonic_us'):
            metadata['frame_age_at_image_ready_us'] = ready_us - metadata['frame_submitted_monotonic_us']
    png.with_suffix('.capture.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


class FrameMissed(RuntimeError):
    """capture_at: the requested guest frame had already been shown (.current = the frame seen)."""
    def __init__(self, message, current):
        super().__init__(message)
        self.current = current


def _capture_meta(ppm):
    metadata = {}
    try:
        metadata = json.loads(Path(ppm).with_suffix('.capture.json').read_text())
    except (OSError, ValueError):
        pass
    metadata['path'] = str(ppm)
    return metadata


def capture_at(session, frame, timeout=30.0, count=1, step=1):
    """Guest captures of exactly guest frames frame, frame+step, ... (count of them; test-control `capture at`,
    frame-matched parity, docs/readiness.md). The game reads each frame's output back as it is produced, so every
    arm of an A/B captures the same guest moments. Waits until the game reaches the frames (`timeout` is a guard).
    Returns a list of capture metadata dicts (path = the .ppm, frame_id, requested_frame). Raises FrameMissed when
    the first frame has already passed, TimeoutError when the frames do not all come within the guard (.captures
    holds what was captured), RuntimeError on an old build."""
    reply = send(session, 'capture at %d %d %d %d' % (int(frame), int(timeout * 1000), int(count), int(step)))
    try:
        paths = wait_reply(reply, timeout + 5, poll=.002).split()  # a fine poll: sweeps start on this reply
    except RuntimeError as error:
        words = str(error).split()
        if words[:2] == ['error', 'passed']:
            current = int(words[2])
            raise FrameMissed('frame %d already passed (game at %d)' % (frame, current), current)
        if words[:2] in (['error', 'timeout'], ['error', 'partial']):
            e = TimeoutError('frames %d+%dx%d: %s at game frame %s' % (frame, step, count, words[1], words[2]))
            e.captures = [_capture_meta(p) for p in words[3:]]
            raise e
        raise
    return [_capture_meta(p) for p in paths]


def capture(session, screen=False):
    # Retain the path-only CLI/API contract for existing scripts. screen=True: the presented drawable
    # including the ImGui overlay (turbo notice, menus), not just the guest frame.
    return capture_with_metadata(session, screen)['path']


def is_background(session):
    try:
        metadata = json.loads((Path(session) / 'session.json').read_text())
    except (OSError, ValueError):
        return False
    # Dev-launcher sessions record the environment they launched with.
    return bool(metadata.get('background') or metadata.get('env', {}).get('FABLE2_TEST_BACKGROUND') == '1')


def press(session, key, hold=150, wait=True):
    """Hold a key/button. The game runs each hold on its own thread, so captures and other commands
    proceed meanwhile; wait=False returns the reply path at once (wait_reply(path) collects it)."""
    if key not in KEYS or not 20 <= hold <= 10000: raise ValueError('Invalid key/hold')
    # Background sessions take test keys through the app's synthetic channel,
    # which works without focus. Otherwise the app input driver ignores keys
    # while unfocused, so restore focus through the app-owned endpoint first.
    if not is_background(session): request(session, 'focus')
    if not wait: return str(send(session, f'press {key} {hold}'))
    return request(session, f'press {key} {hold}', timeout=hold / 1000 + 20)


def key_down(session, key):
    if key not in KEYS: raise ValueError('Invalid key')
    return request(session, f'down {key}')


def key_up(session, key):
    if key not in KEYS: raise ValueError('Invalid key')
    return request(session, f'up {key}')


# Flight recorder (telemetry phase 1): dumps land in test-state/logs/trace/dumps/; read them with
# python3 tools/f2diag.py report <dump>.
def dump(session, reason=None):
    """Write a flight-recorder dump now; returns its folder."""
    return request(session, 'trace dump' + (f' {reason}' if reason else ''), timeout=20)


def mark(session, text):
    return request(session, f'trace mark {text}')


def tag(session, key, value):
    return request(session, f'trace tag {key}={value}')


def state(session):
    """Recorder state: frame id, fps, last gap, level loads, dumps, game-state tags."""
    return json.loads(request(session, 'state'))


def wait_for(session, key, op, value, timeout=60.0, poll=0.1):
    """Poll `state` until tags[key] (or a top-level field) <op> value, e.g. wait_for(s, 'loading', '==', 0).
    Returns the state that matched; raises TimeoutError."""
    ops = {'==': lambda a, b: a == b, '!=': lambda a, b: a != b, '>=': lambda a, b: a >= b,
           '<=': lambda a, b: a <= b, '>': lambda a, b: a > b, '<': lambda a, b: a < b}
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = state(session)
        current = last.get('tags', {}).get(key, last.get(key))
        try:
            if current is not None and ops[op](type(value)(current) if not isinstance(value, str) else str(current), value):
                return last
        except (TypeError, ValueError):
            pass
        time.sleep(poll)
    raise TimeoutError(f'{key} {op} {value} not reached in {timeout} s (last state {last})')


# ---------------------------------------------------------------- readiness (docs/readiness.md)

READY_ALL = ('loaded', 'world', 'steady', 'bg')


def ready_conditions(st):
    """Which readiness conditions a `state` reply meets: loaded (no level load, `loading` tag clear), world (the world
    is drawing), steady (frame pacing stable, no draw-path pipeline builds), bg (no background shader work)."""
    tags = st.get('tags', {})
    def num(key, default=None):
        try:
            return int(tags.get(key, default))
        except (TypeError, ValueError):
            return default
    return dict(loaded=num('loading', 1) == 0 and num('level_load', 1) == 0 and not st.get('in_level_load'),
                world=num('world', 0) == 1, steady=num('steady', 0) == 1, bg=num('bg_busy', 1) == 0)


def wait_ready(session, need=READY_ALL, timeout=None, poll=0.05, alive=None):
    """Wait until the game is ready to measure: every condition in `need` (READY_ALL by default) holds at once.
    There is no fixed wait: `timeout` (FABLE2_READY_TIMEOUT, default 60 s) is a guard, and the result says whether
    it fired (timed_out) and which conditions were still missing. Also returns when each condition first held
    (first_s, seconds from the call), the call-to-ready time (ready_s) and the last `ready` detail. `alive` (a
    callable) stops the wait early when the game has exited. A game without the readiness tags (an old build or
    FABLE2_TRACE=off) times out with missing ['unsupported'] after 2 s instead of waiting out the guard."""
    if timeout is None: timeout = float(os.environ.get('FABLE2_READY_TIMEOUT', 60))
    started = time.monotonic()
    first, last, conds = {}, None, {}
    while True:
        t = time.monotonic() - started
        try:
            last = state(session)
        except (RuntimeError, TimeoutError, ValueError) as error:
            last = dict(error=str(error))
        if 'tags' in last:
            if 'world' not in last['tags'] and t >= 2.0:
                return dict(ready=False, timed_out=True, missing=['unsupported'], ready_s=round(t, 3), need=list(need),
                            first_s=first, detail=last.get('ready'), frame=last.get('frame'))
            conds = ready_conditions(last)
            for k, ok in conds.items():
                if ok and k not in first: first[k] = round(t, 3)
            if all(conds.get(k) for k in need):
                return dict(ready=True, timed_out=False, missing=[], ready_s=round(t, 3), need=list(need),
                            first_s=first, detail=last.get('ready'), frame=last.get('frame'))
        if t >= timeout or (alive and not alive()):
            return dict(ready=False, timed_out=t >= timeout, missing=[k for k in need if not conds.get(k)],
                        ready_s=round(t, 3), need=list(need), first_s=first, detail=last.get('ready') if last else None,
                        frame=last.get('frame') if last else None, error=last.get('error') if last else None)
        time.sleep(poll)


def flush_frames(session, timeout_ms=2000):
    """Write the game's frame CSV now (test-control `flush`): returns {flushed_frame, flushed_us}, or None when the
    game has no flush command (old build) or frame logging is off."""
    try:
        return json.loads(request(session, 'flush %d' % timeout_ms, timeout=timeout_ms / 1000 + 5))
    except (RuntimeError, TimeoutError, ValueError):
        return None


def close(session):
    session = Path(session)
    pid = int((session / 'test-state/control/pid.txt').read_text())
    try:
        request(session, 'close', timeout=2)
    except TimeoutError:
        # Closing can exit before the endpoint writes its reply. Confirm the
        # process state below; missing acknowledgement alone is not failure.
        pass
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        result = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True)
        if (result.returncode == 1 and not result.stdout.strip() and not result.stderr.strip()) or (
                result.returncode == 0 and result.stdout.strip().startswith('Z')):
            import fable2_instances as instances
            instances.unregister(pid)
            return 'closed normally'
        if result.returncode != 0:
            raise RuntimeError('Could not verify game exit: ' + result.stderr.strip())
        time.sleep(.1)
    raise TimeoutError('Quit requested but process is still alive')


def summarize(session, last=120):
    paths = list((Path(session) / 'test-state/logs').glob('frames-*.csv'))
    path = max(paths, key=lambda p: p.stat().st_mtime)
    result = subprocess.run(['python3', str(ROOT / 'tools/summarize_frame_log.py'), str(path),
                             '--last', str(last), '--json'], capture_output=True, text=True, check=True)
    report = json.loads(result.stdout)
    (Path(session) / 'summary.json').write_text(result.stdout)
    return report


def telemetry(session):
    folder = Path(session) / 'test-state/control'
    path = folder / 'events.jsonl'
    events = []
    if path.exists():
        for line in path.read_text().splitlines():
            try: events.append(json.loads(line))
            except json.JSONDecodeError: pass  # A live final line may be incomplete.
    states = [e for e in events if e['type'] == 'state']
    status = folder / 'replay-status.json'
    return {'state': states[-1] if states else None,
            'transitions': states, 'events': len(events),
            'pad_changes': sum(e['type'] == 'pad' for e in events),
            'key_events': sum(e['type'] == 'key' for e in events),
            'recording_finished': any(e['type'] == 'recording_end' for e in events),
            'replay': json.loads(status.read_text()) if status.exists() else None}


def stop_recording(session):
    session = Path(session)
    metadata = json.loads((session / 'session.json').read_text())
    if not metadata.get('record'): raise ValueError('This session was not launched with recording enabled')
    folder = session / 'test-state/control'
    (folder / 'stop-recording').touch()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        status = telemetry(session)
        if status['recording_finished']:
            recording = session / 'recording.jsonl'
            # Freeze an immutable copy at the recording_end event. State telemetry
            # continues separately after stopping or while the game is closed.
            lines = []
            for line in (folder / 'events.jsonl').read_text().splitlines():
                event = json.loads(line)
                lines.append(line)
                if event['type'] == 'recording_end': break
            recording.write_text('\n'.join(lines) + '\n')
            return {'recording': str(recording), 'telemetry': status}
        time.sleep(.02)
    raise TimeoutError('Recorder did not acknowledge stop')


BUILD_LOG = ROOT / 'out/agent-build.log'
BUILD_STATE = ROOT / 'out/agent-build.json'
BUILD_EXIT = ROOT / 'out/agent-build.exit'


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def build_status(tail=20):
    """State of the last agent-started Metal build plus the end of its log."""
    if not BUILD_STATE.is_file():
        return {'state': 'none'}
    info = json.loads(BUILD_STATE.read_text())
    if BUILD_EXIT.is_file():
        code = int(BUILD_EXIT.read_text().strip() or -1)
        info['state'] = 'succeeded' if code == 0 else 'failed'
        info['exit_code'] = code
    else:
        info['state'] = 'running' if _pid_alive(info['pid']) else 'stopped (no exit code)'
    info['elapsed_seconds'] = round(time.time() - info['start_unix'], 1)
    lines = BUILD_LOG.read_text(errors='replace').splitlines() if BUILD_LOG.is_file() else []
    info['log_tail'] = lines[-max(0, int(tail)):]
    return info


def build():
    """Start the standard macOS Metal build (tools/build_macos_metal.py) in the background.

    Refuses while a build is running or a game from out/build is open (the build replaces that app);
    the frozen playtest copy (out/playtest) may keep running.
    Output: out/agent-build.log; poll with build_status().
    """
    if BUILD_STATE.is_file() and build_status(0)['state'] == 'running':
        raise RuntimeError('A build is already running; check build_status')
    import fable2_instances as instances
    from_build = [e for e in instances.instances() if str(e.get('exe') or '').startswith(str(ROOT / 'out/build') + '/')]
    if from_build:
        raise RuntimeError('Close the game(s) running from out/build before building (the build replaces the app): '
                           + ', '.join(instances.describe(e) for e in from_build))
    BUILD_EXIT.unlink(missing_ok=True)
    script = ('export FABLE2_BUILD_JOBS=12; python3 tools/build_macos_metal.py > %s 2>&1; echo $? > %s'
              % (shlex.quote(str(BUILD_LOG)), shlex.quote(str(BUILD_EXIT))))
    process = subprocess.Popen(['/bin/zsh', '-lc', script], cwd=ROOT, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    BUILD_STATE.write_text(json.dumps(dict(pid=process.pid, start_unix=time.time(),
                                           command='python3 tools/build_macos_metal.py', log=str(BUILD_LOG)), indent=2) + '\n')
    return {'state': 'started', 'pid': process.pid, 'log': str(BUILD_LOG)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    q = sub.add_parser('launch'); q.add_argument('--display-sync', choices=['on', 'off'], default='on')
    q.add_argument('--record', action='store_true')
    q.add_argument('--checkpoint', action='store_true')
    q.add_argument('--save', help='library save (out/save-library/<name>) to use as the only hero save')
    q.add_argument('--foreground', action='store_true',
                   help='old behaviour: focus the game window (takes the keyboard); default is background test mode')
    q = sub.add_parser('replay'); q.add_argument('recording', type=Path)
    q.add_argument('--checkpoint', action='store_true')
    q.add_argument('--allow-build-mismatch', action='store_true')
    q.add_argument('--save', help='library save (out/save-library/<name>) to use as the only hero save')
    q.add_argument('--foreground', action='store_true', help='focus the game window (default: background test mode)')
    sub.add_parser('saves', help='list save-library entries')
    q = sub.add_parser('import-save', help='copy one HeroNNN save into the save library')
    q.add_argument('name'); q.add_argument('--from', dest='source', type=Path, required=True,
                   help='app-state root or saves folder (read-only)')
    q.add_argument('--hero', required=True, help='folder name, e.g. Hero000'); q.add_argument('--note')
    for command in ('telemetry', 'stop-recording', 'cancel-replay'):
        q = sub.add_parser(command); q.add_argument('session', type=Path)
    for command in ('capture', 'close', 'ping', 'focus', 'summary', 'press', 'down', 'up', 'state', 'dump', 'mark', 'tag'):
        q = sub.add_parser(command); q.add_argument('session', type=Path)
        if command == 'press':
            q.add_argument('key', choices=KEYS); q.add_argument('--hold', type=int, default=150)
            q.add_argument('--no-wait', action='store_true', help='return at once (the hold runs in the game)')
        if command in ('down', 'up'): q.add_argument('key', choices=KEYS)
        if command == 'capture':
            q.add_argument('--screen', action='store_true', help='include the ImGui overlay')
            q.add_argument('--at-frame', type=int, help='guest capture of exactly this guest frame (waits for it)')
        if command == 'summary': q.add_argument('--last', type=float, default=120)
        if command == 'dump': q.add_argument('reason', nargs='?')
        if command == 'mark': q.add_argument('text')
        if command == 'tag': q.add_argument('key_value', help='key=value, e.g. level_name=BowerLake')
    args = p.parse_args()
    if args.cmd == 'launch': result = launch(args.display_sync == 'on', record=args.record, checkpoint=args.checkpoint, save=args.save, background=not args.foreground)
    elif args.cmd == 'replay': result = launch(replay=args.recording, checkpoint=args.checkpoint, allow_build_mismatch=args.allow_build_mismatch, save=args.save, background=not args.foreground)
    elif args.cmd == 'saves': result = list_saves()
    elif args.cmd == 'import-save': result = import_save(args.name, args.source, args.hero, args.note)
    elif args.cmd == 'telemetry': result = telemetry(args.session)
    elif args.cmd == 'stop-recording': result = stop_recording(args.session)
    elif args.cmd == 'cancel-replay':
        (args.session / 'test-state/control/cancel-replay').touch()
        result = 'cancellation requested'
    elif args.cmd == 'capture':
        result = capture_at(args.session, args.at_frame) if args.at_frame else capture(args.session, args.screen)  # noqa
    elif args.cmd == 'close': result = close(args.session)
    elif args.cmd == 'press': result = press(args.session, args.key, args.hold, wait=not args.no_wait)
    elif args.cmd == 'down': result = key_down(args.session, args.key)
    elif args.cmd == 'up': result = key_up(args.session, args.key)
    elif args.cmd == 'state': result = state(args.session)
    elif args.cmd == 'dump': result = dump(args.session, args.reason)
    elif args.cmd == 'mark': result = mark(args.session, args.text)
    elif args.cmd == 'tag': result = tag(args.session, *args.key_value.split('=', 1))
    elif args.cmd == 'summary': result = summarize(args.session, args.last)
    else: result = request(args.session, args.cmd)
    print(json.dumps({'ok': True, 'result': result}, indent=2))

if __name__ == '__main__': main()
