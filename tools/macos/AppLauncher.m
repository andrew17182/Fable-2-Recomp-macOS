// Native Finder entry point. Game content and writable state stay outside the app.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void Fail(NSString *message) {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  [NSApp activateIgnoringOtherApps:YES];
  NSAlert *alert = [NSAlert new];
  alert.messageText = @"Fable II could not start";
  alert.informativeText = message;
  [alert runModal];
  exit(1);
}
static BOOL ValidContent(NSString *path) {
  if (!path.length) return NO;
  NSFileManager *fm = NSFileManager.defaultManager;
  BOOL directory = NO;
  // Reject directories named default.xex before handing the path to the engine.
  return path.isAbsolutePath &&
      [fm fileExistsAtPath:[path stringByAppendingPathComponent:@"default.xex"] isDirectory:&directory] && !directory &&
      [fm fileExistsAtPath:[path stringByAppendingPathComponent:@"data"] isDirectory:&directory] && directory;
}
// Portable build (tools/fable2_portable.py, docs/portable-build.md): runs Contents/MacOS/FableIIPrepare
// and returns its exit status (0 ready, 3 cancelled, other = failed).
static int RunPrepare(NSString *helper, NSArray<NSString *> *arguments) {
  NSMutableArray<NSString *> *all = [NSMutableArray arrayWithObject:helper];
  [all addObjectsFromArray:arguments];
  char **argv = calloc(all.count + 1, sizeof(char *));
  for (NSUInteger i = 0; i < all.count; ++i) argv[i] = strdup(all[i].UTF8String);
  pid_t pid = 0;
  if (posix_spawn(&pid, helper.fileSystemRepresentation, NULL, NULL, argv, environ) != 0) return 1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
static NSDictionary *PortableMarker(NSString *path) {
  NSData *data = [NSData dataWithContentsOfFile:[path stringByAppendingPathComponent:@".fable2-portable.json"]];
  NSDictionary *marker = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
  return [marker isKindOfClass:NSDictionary.class] ? marker : nil;
}
static BOOL PortableDataComplete(NSString *path) { return [PortableMarker(path)[@"complete"] boolValue]; }
static void AppendLog(NSString *path, NSString *line) {
  NSString *text = [NSString stringWithFormat:@"[%@] %@\n", [NSDate date], line];
  int fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return;
  NSData *bytes = [text dataUsingEncoding:NSUTF8StringEncoding];
  write(fd, bytes.bytes, bytes.length);
  close(fd);
}
// First launch without game files of our own: reuse a finished extraction from a sibling app's data folder
// (the benchmark kit's, or the normal app's for another variant) instead of asking for the disc image again.
// The source must be marked complete with this build's default.xex hash. It is APFS-cloned (clonefile, like
// the kit's own reuse in tools/macos/Benchmark/main.swift) and only ever read. FABLE2_PORTABLE_REUSE_FROM
// (colon-separated GameData folders) replaces the sibling list for tests; set but empty disables reuse.
static BOOL ReuseSiblingGameData(NSString *target, NSString *state, NSString *expectedXex, NSDictionary *environment,
                                 NSString *logPath) {
  NSFileManager *fm = NSFileManager.defaultManager;
  if (!expectedXex.length || [fm fileExistsAtPath:target]) return NO;
  NSArray<NSString *> *candidates;
  NSString *override = environment[@"FABLE2_PORTABLE_REUSE_FROM"];
  if (override) {
    candidates = override.length ? [override componentsSeparatedByString:@":"] : @[];
  } else {
    NSString *support = state.stringByDeletingLastPathComponent;
    NSMutableArray *list = [NSMutableArray array];
    for (NSString *name in @[@"Fable II Recomp", @"Fable II Recomp Benchmark"])
      [list addObject:[[support stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"GameData"]];
    candidates = list;
  }
  for (NSString *source in candidates) {
    if (!source.isAbsolutePath || [source.stringByStandardizingPath isEqualToString:target.stringByStandardizingPath]) continue;
    if (![fm fileExistsAtPath:source]) continue;
    NSDictionary *marker = PortableMarker(source);
    if (![marker[@"complete"] boolValue] || !ValidContent(source)) {
      AppendLog(logPath, [NSString stringWithFormat:@"[portable] reuse: %@ is not a complete extraction; skipped", source]);
      continue;
    }
    NSString *sha = marker[@"xexSHA256"];
    if (![sha isKindOfClass:NSString.class] || [sha caseInsensitiveCompare:expectedXex] != NSOrderedSame) {
      AppendLog(logPath, [NSString stringWithFormat:@"[portable] reuse: %@ has default.xex %@, this build expects %@; skipped",
                                                    source, sha ?: @"(unknown)", expectedXex]);
      continue;
    }
    [fm createDirectoryAtPath:target.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    NSDate *start = [NSDate date];
    if (clonefile(source.fileSystemRepresentation, target.fileSystemRepresentation, 0) != 0) {
      int code = errno;
      AppendLog(logPath, [NSString stringWithFormat:@"[portable] reuse: clonefile %@ -> %@ failed: %s; skipped", source, target,
                                                    strerror(code)]);
      // EEXIST would mean the target appeared meanwhile; never remove what we did not create.
      if (code != EEXIST) [fm removeItemAtPath:target error:nil];
      continue;
    }
    if (!PortableDataComplete(target) || !ValidContent(target)) {
      AppendLog(logPath, [NSString stringWithFormat:@"[portable] reuse: clone of %@ is incomplete; removed", source]);
      [fm removeItemAtPath:target error:nil];
      continue;
    }
    AppendLog(logPath, [NSString stringWithFormat:@"[portable] reuse: game data cloned from %@ (APFS clone, %.2f s, xex %@)",
                                                  source, -start.timeIntervalSinceNow, sha]);
    return YES;
  }
  AppendLog(logPath, @"[portable] reuse: no sibling game data to reuse; asking for the disc image");
  return NO;
}
// Copies the bundled shader cache (Resources/native-shaders, made by tools/fable2_portable.py) into the
// state folder's cache once: shareable pipeline lists, and the artifact store into this Mac's GPU folder.
// Existing files are never replaced.
static void SeedShaderCache(NSString *resources, NSString *state, NSBundle *bundle) {
  NSString *seed = [resources stringByAppendingPathComponent:@"native-shaders"];
  NSString *seedID = [bundle objectForInfoDictionaryKey:@"Fable2ShaderSeedID"];
  NSFileManager *fm = NSFileManager.defaultManager;
  if (!seedID.length || ![fm fileExistsAtPath:seed]) return;
  NSString *done = [state stringByAppendingPathComponent:@"cache/portable-shader-seed.txt"];
  if ([[NSString stringWithContentsOfFile:done encoding:NSUTF8StringEncoding error:nil] isEqualToString:seedID]) return;
  NSString *shaders = [state stringByAppendingPathComponent:@"cache/shaders"];
  NSMutableArray<NSString *> *pairs = [NSMutableArray array];
  for (NSString *name in [fm contentsOfDirectoryAtPath:[seed stringByAppendingPathComponent:@"shareable"] error:nil]) {
    [pairs addObject:[@"shareable" stringByAppendingPathComponent:name]];
    [pairs addObject:[[shaders stringByAppendingPathComponent:@"shareable"] stringByAppendingPathComponent:name]];
  }
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  NSMutableString *gpu = [NSMutableString stringWithString:device.name ?: @"unknown"];
  for (NSUInteger i = 0; i < gpu.length; ++i) {  // same folder name as GetShaderStorageDeviceTag()
    unichar c = [gpu characterAtIndex:i];
    if (!(c < 128 && isalnum(c))) [gpu replaceCharactersInRange:NSMakeRange(i, 1) withString:@"_"];
  }
  NSString *local = [seed stringByAppendingPathComponent:@"local"];
  for (NSString *relative in [fm subpathsOfDirectoryAtPath:local error:nil]) {
    BOOL directory = NO;
    if (![fm fileExistsAtPath:[local stringByAppendingPathComponent:relative] isDirectory:&directory] || directory) continue;
    [pairs addObject:[@"local" stringByAppendingPathComponent:relative]];
    [pairs addObject:[[[shaders stringByAppendingPathComponent:@"local/metal"] stringByAppendingPathComponent:gpu]
                         stringByAppendingPathComponent:relative]];
  }
  for (NSUInteger i = 0; i + 1 < pairs.count; i += 2) {
    NSString *destination = pairs[i + 1];
    if ([fm fileExistsAtPath:destination]) continue;
    [fm createDirectoryAtPath:destination.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    NSError *error = nil;
    if ([fm copyItemAtPath:[seed stringByAppendingPathComponent:pairs[i]] toPath:destination error:&error])
      NSLog(@"[portable] seeded shader cache: %@", destination);
    else
      NSLog(@"[portable] could not seed %@: %@", destination, error.localizedDescription);
  }
  [seedID writeToFile:done atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
static void CopyDefault(NSString *source, NSString *destination) {
  NSFileManager *fm = NSFileManager.defaultManager;
  if ([fm fileExistsAtPath:destination]) return;
  NSError *error = nil;
  if (![fm copyItemAtPath:source toPath:destination error:&error])
    Fail([NSString stringWithFormat:@"Could not create %@: %@", destination, error.localizedDescription]);
}
int main(int argc, char **argv) {
  @autoreleasepool {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSBundle *bundle = NSBundle.mainBundle;
    NSString *resources = bundle.resourcePath;
    NSString *engine = [bundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"fable_2"];
    NSDictionary *environment = NSProcessInfo.processInfo.environment;
    NSString *state = environment[@"FABLE2_APP_STATE_ROOT"];
    if (!state.length) {
      NSURL *support = [fm URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
      // The release app has its own folder (Info.plist Fable2StateFolder, docs/release-app.md); others share
      // "Fable II Recomp".
      NSString *folder = [bundle objectForInfoDictionaryKey:@"Fable2StateFolder"];
      state = [support.path stringByAppendingPathComponent:folder.length ? folder : @"Fable II Recomp"];
    }
    if (!state.isAbsolutePath) Fail(@"The application data folder must be an absolute path.");
    NSError *error = nil;
    for (NSString *part in @[@"", @"logs", @"saves", @"cache", @"metadata"]) {
      if (![fm createDirectoryAtPath:[state stringByAppendingPathComponent:part]
          withIntermediateDirectories:YES attributes:nil error:&error]) Fail(error.localizedDescription);
    }
    // The release app ships no game code: the game is built on this Mac (tools/fable2_game_build.py) into
    // <state>/Game/Fable II.bundle, a bundle with this app's identity, ad-hoc signed here.
    if ([[bundle objectForInfoDictionaryKey:@"Fable2BuildsGame"] boolValue]) {
      engine = [state stringByAppendingPathComponent:@"Game/Fable II.bundle/Contents/MacOS/fable_2"];
      if (![fm isExecutableFileAtPath:engine])
        Fail(@"The game has not been built on this Mac yet. Open Fable II again: it builds the game first.");
    }
    // Hold this lock through exec (the game keeps it until it exits). It also serializes first-run
    // setup. This per-folder lock is the only single-instance rule: games with their own application
    // data folders (dev/test sessions, the playtest copy) may run side by side (local agent docs).
    int lock = open([[state stringByAppendingPathComponent:@"app.lock"] fileSystemRepresentation], O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) Fail(@"Another copy of Fable II is already using this application data folder.");

    NSString *settingsPath = [state stringByAppendingPathComponent:@"launcher.plist"];
    NSDictionary *settings = [NSDictionary dictionaryWithContentsOfFile:settingsPath];
    NSDictionary *local = [NSDictionary dictionaryWithContentsOfFile:[resources stringByAppendingPathComponent:@"LocalDefaults.plist"]];
    NSString *fromEnvironment = environment[@"FABLE2_GAME_DATA_ROOT"];
    if (!fromEnvironment.length) fromEnvironment = nil;  // set but empty = unset
    NSString *content = fromEnvironment ?: settings[@"GameDataRoot"] ?: local[@"GameDataRoot"];
    NSMutableDictionary *remember = [NSMutableDictionary dictionaryWithDictionary:settings ?: @{}];
    // Launcher-only arguments (portable build); everything else is passed to the game.
    NSMutableArray<NSString *> *passArgs = [NSMutableArray array];
    NSString *isoArgument = nil;
    BOOL chooseImage = NO;
    for (int i = 1; i < argc; ++i) {
      NSString *arg = [NSString stringWithUTF8String:argv[i]];
      if ([arg hasPrefix:@"-psn_"]) continue;  // Finder may provide a legacy process-serial-number argument
      if ([arg isEqualToString:@"--choose-iso"]) { chooseImage = YES; continue; }
      if ([arg isEqualToString:@"--iso"] && i + 1 < argc) { isoArgument = [NSString stringWithUTF8String:argv[++i]]; continue; }
      [passArgs addObject:arg];
    }
    BOOL portable = [[bundle objectForInfoDictionaryKey:@"Fable2Portable"] boolValue];
    if (portable) {
      // Portable build: the game files come from the player's disc image, extracted by FableIIPrepare into a
      // per-user folder (FABLE2_PORTABLE_GAME_DATA overrides it for tests). Holding Option at launch or
      // --choose-iso picks the image again; --iso PATH skips the file dialog (testing).
      NSString *managed = environment[@"FABLE2_PORTABLE_GAME_DATA"];
      if (!managed.length) managed = [state stringByAppendingPathComponent:@"GameData"];
      if (!managed.isAbsolutePath) Fail(@"FABLE2_PORTABLE_GAME_DATA must be an absolute path.");
      // The player app (tools/macos/Player/main.swift, the portable app's main executable) uses Option to show
      // its window and has already prepared the game files when it starts this launcher (FABLE2_PLAYER_LAUNCH=1).
      BOOL fromPlayer = [environment[@"FABLE2_PLAYER_LAUNCH"] isEqualToString:@"1"];
      if (!fromPlayer && ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0) chooseImage = YES;
      if (isoArgument) chooseImage = YES;
      if (!fromEnvironment && [environment[@"FABLE2_PORTABLE_GAME_DATA"] length]) content = managed;
      if (!content) content = managed;
      BOOL isManaged = [content.stringByStandardizingPath isEqualToString:managed.stringByStandardizingPath];
      NSString *helper = [bundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"FableIIPrepare"];
      NSString *lua = [resources stringByAppendingPathComponent:@"recomp-lua"];
      NSString *expectedXex = [bundle objectForInfoDictionaryKey:@"Fable2XexSHA256"];
      NSString *prepareLog = [state stringByAppendingPathComponent:@"logs/prepare.log"];
      BOOL needsData = chooseImage || !ValidContent(content) || (isManaged && !PortableDataComplete(content));
      if (!fromEnvironment && needsData && !chooseImage && ReuseSiblingGameData(managed, state, expectedXex, environment, prepareLog)) {
        content = managed;
        needsData = NO;
        remember[@"AllowXexMismatch"] = @NO;
      }
      if (!fromEnvironment && needsData) {
        content = managed;
        NSString *result = [state stringByAppendingPathComponent:@"prepare-result.plist"];
        [fm removeItemAtPath:result error:nil];
        NSMutableArray *prepareArgs = [NSMutableArray arrayWithArray:@[@"--target", content, @"--lua", lua, @"--result", result]];
        if (expectedXex.length) [prepareArgs addObjectsFromArray:@[@"--expected-xex", expectedXex]];
        if (isoArgument) [prepareArgs addObjectsFromArray:@[@"--iso", isoArgument]];
        if ([environment[@"FABLE2_PORTABLE_HEADLESS"] isEqualToString:@"1"]) [prepareArgs addObject:@"--headless"];
        int saved = dup(STDOUT_FILENO), logFd = open(prepareLog.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (logFd >= 0) { dup2(logFd, STDOUT_FILENO); close(logFd); }
        int status = RunPrepare(helper, prepareArgs);
        if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }
        if (status == 3) return 0;  // the player cancelled
        if (status != 0)
          Fail([NSString stringWithFormat:@"The game files could not be prepared. Details: %@", prepareLog]);
        NSDictionary *prepared = [NSDictionary dictionaryWithContentsOfFile:result];
        // A different disc image was accepted with "Continue Anyway": let the game start with it.
        remember[@"AllowXexMismatch"] = @((BOOL)(prepared && ![prepared[@"XexMatches"] boolValue]));
      }
      if (!fromEnvironment) {
        // Lua scripts + dir.manifest lines and the skip-intro movies (fast, idempotent).
        if (RunPrepare(helper, @[@"--stage", @"--target", content, @"--lua", lua]) != 0)
          NSLog(@"[portable] staging the game's side files failed; continuing");
      }
      if ([remember[@"AllowXexMismatch"] boolValue] && !getenv("FABLE2_XEX_MISMATCH_OK")) setenv("FABLE2_XEX_MISMATCH_OK", "1", 1);
      // Dev convenience: the portable test build skips the logo movies unless told otherwise.
      if (!getenv("FABLE2_SKIP_INTRO_MOVIES") && [[bundle objectForInfoDictionaryKey:@"Fable2SkipIntroDefault"] boolValue])
        setenv("FABLE2_SKIP_INTRO_MOVIES", "1", 1);
      // Same shader-converter target on every Mac (docs/portable-build.md), so the bundled shader cache's keys match.
      NSString *pin = [bundle objectForInfoDictionaryKey:@"Fable2MSCMinTarget"];
      if (pin.length && !getenv("FABLE2_MSC_MIN_TARGET")) setenv("FABLE2_MSC_MIN_TARGET", pin.UTF8String, 1);
      // Toolchain of the bundled optimized shaders: used when this Mac has no Metal Toolchain.
      NSString *toolchain = [bundle objectForInfoDictionaryKey:@"Fable2NSToolchainHash"];
      if (toolchain.length && !getenv("FABLE2_NS_CACHE_TOOLCHAIN")) setenv("FABLE2_NS_CACHE_TOOLCHAIN", toolchain.UTF8String, 1);
      SeedShaderCache(resources, state, bundle);
    }
    if (!ValidContent(content)) {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
      [NSApp activateIgnoringOtherApps:YES];
      NSOpenPanel *panel = [NSOpenPanel openPanel];
      panel.title = @"Choose your Fable II game folder";
      panel.message = @"Choose the folder containing default.xex and the data folder from your copy of the game.";
      panel.canChooseFiles = NO;
      panel.canChooseDirectories = YES;
      panel.allowsMultipleSelection = NO;
      if ([panel runModal] != NSModalResponseOK) return 0;
      content = panel.URL.path;
      if (!ValidContent(content)) Fail(@"That folder does not contain default.xex and a data folder. Open the app again and choose your extracted game folder.");
      fromEnvironment = nil;  // a folder picked by the user is remembered
    }
    // A per-launch FABLE2_GAME_DATA_ROOT (e.g. the playtest copy's own game data) is not remembered.
    remember[@"GameDataRoot"] = content;
    if (!fromEnvironment && ![remember writeToFile:settingsPath atomically:YES])
      Fail(@"Could not save the game folder selection.");
    for (NSString *name in @[@"fable2_config.toml", @"fable2_patches.toml"])
      CopyDefault([resources stringByAppendingPathComponent:name], [state stringByAppendingPathComponent:name]);
    setenv("FABLE2_APP_STATE_ROOT", state.fileSystemRepresentation, 1);
    // The frozen playtest copy (tools/fable2_playtest.py marks its Info.plist) titles its window
    // "Fable II Playtest <version>" however it is opened (local agent docs).
    if ([[bundle objectForInfoDictionaryKey:@"Fable2Playtest"] boolValue] && !getenv("FABLE2_INSTANCE_ROLE"))
      setenv("FABLE2_INSTANCE_ROLE", "playtest", 1);
    if (!getenv("FABLE_STARTUP_LOCK_HANDOFF")) setenv("FABLE_STARTUP_LOCK_HANDOFF", "1", 1);
    if (chdir(state.fileSystemRepresentation) != 0) Fail(@"Could not open the application data folder.");
    NSString *log = [state stringByAppendingPathComponent:@"logs/launcher.log"];
    int output = open(log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0) Fail(@"Could not open the launch log.");
    dup2(output, STDOUT_FILENO);
    dup2(output, STDERR_FILENO);
    close(output);
    BOOL metal = ![[bundle objectForInfoDictionaryKey:@"Fable2Backend"] isEqualToString:@"vulkan"];
    if (!metal) {
      if (!getenv("REX_VULKAN_LOG_DEBUG_MESSAGES")) setenv("REX_VULKAN_LOG_DEBUG_MESSAGES", "0", 1);
      if (!getenv("MVK_CONFIG_LOG_LEVEL")) setenv("MVK_CONFIG_LOG_LEVEL", "1", 1);
    }
    // No --fullscreen here: a command-line value would override the display mode chosen in
    // fable2_config.toml [graphics] fullscreen / FABLE2_FULLSCREEN (src/core/fable2_graphics_config.h).
    NSMutableArray<NSString *> *args = [NSMutableArray arrayWithArray:@[
      engine, @"--game_data_root", content,
      [@"--log_file=" stringByAppendingString:[state stringByAppendingPathComponent:@"logs/fable_2.log"]]]];
    // Keep the tuned Metal profile aligned with run_macos_metal.command.
    // Diagnostic logging remains opt-in through the environment.
    if (metal) [args addObjectsFromArray:@[
      @"--gpu_plugin=metal", @"--async_shader_compilation=true", @"--metal_pipeline_creation_threads=2",
      @"--metal_type0_register_batching=true", @"--metal_constant_payload_cache=false",
      @"--metal_force_bc_decompress=false", @"--metal_backend_telemetry=false",
      @"--metal_root_rebuild_detail_telemetry=false", @"--metal_probe_capture_interval=0"]];
    else [args addObject:@"--gpu_plugin=xenos"];
    [args addObjectsFromArray:passArgs];
    char **child = calloc(args.count + 1, sizeof(char *));
    for (NSUInteger i = 0; i < args.count; ++i) child[i] = strdup(args[i].UTF8String);
    execv(engine.fileSystemRepresentation, child);
    Fail([NSString stringWithFormat:@"Could not launch the game: %s", strerror(errno)]);
  }
}
