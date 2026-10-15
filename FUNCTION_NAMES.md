# Fable 2 (ReXGlue) — Function Names & Reference

Every `[entrypoint.functions]` entry in `fable_2_manifest.toml` now carries a `name` field. This document explains each one: what it does, its name, and how it is triggered.

**Total functions named: 520** (no entrypoint was removed).

## How these were determined

The guest code is PowerPC (Xbox 360). Each function was analyzed from its disassembly (the PPC instruction stream preserved as comments in `generated/default/*.cpp`) plus the imports and other functions it calls. Signals used:

- **Import calls** (e.g. `NtAllocateVirtualMemory`, `RtlEnterCriticalSection`, `XamLoaderTerminateTitle`) identify the SDK subsystem.
- **Instruction shape** identifies thunk families: vtable dispatch, switch jump-table, const/global return, tail-call trampoline.
- **In-game annotations** you left in the manifest (e.g. "Swimming", "First Combat childhood fight") pin down the story beats.

**Memory map:** code `.text` = `0x82000000`–`0x83438000`; data ≥ `0x83438000`. The `0x82C00000`–`0x831FFFFF` range is the Xbox 360 **SDK runtime**; elsewhere is **game** code.

**Confidence legend:** **high** = shape is unambiguous · medium = subsystem clear, exact role inferred · low = best guess from limited signal.


## A. Story & Gameplay

_The functions you annotated with in-game moments. These are virtual-call thunks (and a couple of getters) that the engine fires at specific story beats. Highest confidence._

**19 functions.**

### `GetNewGameLoadingGlobal`

- **Address:** `0x822142D0` · **Size:** 3 insns · **Category:** global · **Region:** game · **Confidence:** medium

- **Role:** returns the new-game loading-screen global pointer

- **Does:** Const thunk: builds the data address 0x8331950C (lis r11,-31950; lwz r3,-27380(r11)) and returns the pointer stored there in r3. Head of the 'loading screen when starting a new game' group.

- **Trigger:** Loading screen when starting a new game (internal getter).

### `LoadingScreen_VectorOffset`

- **Address:** `0x822D6678` · **Size:** 6 insns · **Category:** tailcall · **Region:** game · **Confidence:** low

- **Role:** loading-screen vector offset add (tails to sub_822AADE0)

- **Does:** Loads a 128-bit vector constant from a data table (lis r11,-31926; addi r10,r11,-21152; lvx128), adds it to the in-flight float vectors v1/v2 (vaddfp), then tails into sub_822AADE0. Applies a fixed positional offset, part of the new-game loading-screen group.

- **Trigger:** Loading screen when starting a new game (internal helper).

- **Calls:** `sub_822AADE0`

### `Story_JobSignBlacksmith`

- **Address:** `0x823A1810` · **Size:** 4 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Role:** interacting with the blacksmith job sign

- **Does:** Loads a global object pointer (data at ~0x833xxxxx built from lis r11,-31927; lwz r3,27596(r11)), forwards the incoming r3 as r4, then tail-calls sub_82297F88. Handles the interaction with the job/quest sign for the blacksmith.

- **Trigger:** Interact with the job sign (blacksmith) in the town.

- **Calls:** `sub_82297F88`

### `Story_EnterWarehouseAsChild`

- **Address:** `0x8274B738` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** entering the warehouse as child (vtable slot 38)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 38 (offset 152) -> bctr. Triggers the scripted beat for entering the warehouse during the childhood prologue.

- **Trigger:** Walk into the warehouse as the child in the Albion prologue.

### `LoadingScreen_Virtual42`

- **Address:** `0x8274B748` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 42)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 42 (offset 168) -> bctr. Part of the loading-screen function group used when starting a new game.

- **Trigger:** Loading screen when starting a new game (internal dispatch).

### `LoadingScreen_Virtual39`

- **Address:** `0x8274B758` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 39)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 39 (offset 156) -> bctr. Part of the loading-screen function group used when starting a new game.

- **Trigger:** Loading screen when starting a new game (internal dispatch).

### `Story_TatterSpireGetMoving`

- **Address:** `0x8274B768` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** Tatter Spire 'Get Moving' beat (vtable slot 20)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 20 (offset 80) -> bctr. Fires when arriving at the Tatter Spire and the man says 'Get Moving'.

- **Trigger:** Arrive at the Tatter Spire when the man says 'Get Moving'.

### `LoadingScreen_Virtual28`

- **Address:** `0x8274B778` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 28)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 28 (offset 112) -> bctr. Part of the loading-screen function group used when starting a new game.

- **Trigger:** Loading screen when starting a new game (internal dispatch).

### `Story_SisterAfterFirstFight`

- **Address:** `0x8274B788` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** interacting with sister after first fight (vtable slot 37)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 37 (offset 148) -> bctr. Fires the interaction with the player's sister immediately after the first childhood fight.

- **Trigger:** Talk to your sister right after the first childhood battle.

### `Swimming`

- **Address:** `0x8297EB28` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** childhood swimming interaction (vtable slot 23)

- **Does:** Virtual-call thunk: loads the vtable pointer from the object in r3, reads slot 23 (byte offset 92), and tail-calls it. Reached when the child character swims.

- **Trigger:** Swim as the child. (Name already fixed by the user; preserved verbatim.)

### `Story_FirstChildCombat`

- **Address:** `0x8297EB48` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** first childhood fight (vtable slot 25)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 25 (offset 100) -> bctr. Triggers the first combat encounter of the childhood (Albion) prologue.

- **Trigger:** The first fight as the child in the opening Albion sequence.

### `Story_AfterNewGameVideo`

- **Address:** `0x82988EC8` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** after new-game video plays (vtable slot 19)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 19 (offset 76) -> bctr. Triggers the beat immediately after the new-game intro video finishes.

- **Trigger:** Right after the new-game intro video plays.

### `LoadingScreen_Virtual43`

- **Address:** `0x82988ED8` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 43)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 43 (offset 172) -> bctr. Part of the loading-screen function group used when starting a new game.

- **Trigger:** Loading screen when starting a new game (internal dispatch).

### `Skill_AimRangeWeapon`

- **Address:** `0x82988EE8` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** medium

- **Role:** aiming a range weapon (skill), vtable slot 23 on r4

- **Does:** Virtual-call thunk reading the object from r4 (not r3): vtable slot 23 (offset 92) -> bctr. Same slot as Swimming but dispatched on a different register, used when aiming a ranged weapon (skill).

- **Trigger:** Aim a ranged weapon / use the aiming skill.

### `LoadingScreen_Virtual48`

- **Address:** `0x82988F28` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 48)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 48 (offset 192) -> bctr. Part of the loading-screen function group used when starting a new game.

- **Trigger:** Loading screen when starting a new game (internal dispatch).

### `Story_TalkToCameraMan`

- **Address:** `0x82988F38` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** talking to the camera man, child scene (vtable slot 17)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 17 (offset 68) -> bctr. Triggers the conversation with the camera man in the childhood scene.

- **Trigger:** Talk to the camera man during the child (Albion) scene.

### `Story_LookLittleSparrow`

- **Address:** `0x82988F48` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Role:** 'Look, little sparrow' LT prompt (vtable slot 45)

- **Does:** Virtual-call thunk: object in r3 -> vtable slot 45 (offset 180) -> bctr. Fires after the new-game intro, tied to the 'look, little sparrow' moment gated on the LT button.

- **Trigger:** After the new-game intro video, at the 'look, little sparrow' LT prompt.

### `Story_ShootBeetleInWarehouse`

- **Address:** `0x82DDC4D8` · **Size:** 7 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** **high**

- **Role:** shooting the Beetle in the warehouse (vtable slot 9, arg-shuffled)

- **Does:** Variant virtual-call thunk: takes an object in r4 (moves it to r3), reads vtable[0] then slot 9 (offset 36) of the nested object, and tail-calls it with a shifted argument set (r4<-r5). Fires the shooting interaction on the Beetle target in the warehouse.

- **Trigger:** Shoot the Beetle (target) in the warehouse as the child.

### `LoadingScreen_Virtual36`

- **Address:** `0x82EAA980` · **Size:** 9 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** low

- **Role:** loading-screen virtual call (vtable slot 36, null-checked)

- **Does:** Null-checked virtual-call thunk: checks r4 for null, swaps args (r4<-r3, r3<-r4), reads vtable slot 36 (offset 144) of the object, and tail-calls it. Part of the new-game loading-screen group.

- **Trigger:** Loading screen when starting a new game (internal dispatch).


## B. Memory Management (heap)

_The custom heap allocator / free / manager. Identified by NtAllocateVirtualMemory / NtFreeVirtualMemory plus critical-section guards._

**22 functions.**

### `MemSet_SdkRuntime_82CA3190`

- **Address:** `0x82CA3190` · **Category:** leaf (fill) · **Region:** SDK (0x82CA) · **Confidence:** **high**

- **Role:** the SDK memset -- fill `size` bytes at dst with the byte value (r3 = dst, r4 = value, r5 = size) — 479 callers

- **Does:** fill byte-by-byte (*r6++ = r4) until dst is 4-byte aligned; build a 32-bit word from the byte (rlwimi r4 = 0xNNNNNNNN); fill word-by-word in 16-byte chunks; handle the tail. The memory-family sibling of `MemCpy_SdkRuntime` / `MemMove_SdkRuntime`.

- **Calls:** none

### `MemMove_SdkRuntime_82CAA2E0`

- **Address:** `0x82CAA2E0` · **Category:** leaf (copy) · **Region:** SDK (0x82CA) · **Confidence:** **high**

- **Role:** a memmove (overlap-safe copy) -- copy `size` bytes from src to dst, handling overlapping regions — 80 callers

- **Does:** args: dst (r3), src (r4), size (r5). If dst == src, return. If dst < src (no dangerous overlap), delegate to `MemCpy_SdkRuntime_82CA2C60`. Else (dst >= src, a backward overlap): copy BACKWARDS -- byte-by-byte until dst is 4-byte aligned (from the end), then 4-byte word-by-word backwards -- so the source isn't clobbered. Sits next to `MemCpy_SdkRuntime` in the memory family.

- **Calls:** `MemCpy_SdkRuntime_82CA2C60`

### `Allocate_Init_821A9258`

- **Address:** `0x821A9258` · **Category:** body (allocation) · **Region:** game (0x821A) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a specialized allocation entry point — allocates `size` bytes via the primary malloc, with conditional setup for size >= 2 — 4 callers

- **Does:** args: size (r3), flag (r4, always 0 in callers). If size == 0 or size == 1 (via (-1/size) >= 1), skip the setup. Else (size >= 2) run a setup referencing two fixed global strings (~0x81FF563C/0x81FF5630) + sub_822F1F00. Then always call `Allocate_OrDbgBreak_8221F388(size)` and return the allocated pointer.

- **Calls:** `sub_822F1F00`, `Allocate_OrDbgBreak_8221F388`

### `CopyAssign_RefCounted_821F0108`

- **Address:** `0x821F0108` · **Category:** body (refcount copy-assign) · **Region:** game · **Confidence:** **high**

- **Role:** the primary refcounted copy-assignment (operator=) — **2524 callers**, one of the hottest functions in the binary; the AddRef counterpart to `Release_RefCounted_821C67D8`

- **Does:** args: obj (r3, this), src (r4, other). Sets obj->0 = 0, atomically increments a global counter. If src == obj, return (self-assign guard). Else: (1) RELEASE the old — if obj->0 != 0, call `Release_RefCounted_821C67D8(obj)`; (2) ACQUIRE the new — if src->0 != 0, copy it into obj->0 and atomically increment the new resource's refcount at (src->0 + 12). Returns obj. The classic "Release(old), copy pointer, AddRef(new)".

- **Calls:** `Release_RefCounted_821C67D8`

### `Release_RefCounted_821C67D8`

- **Address:** `0x821C67D8` · **Category:** body (refcount release) · **Region:** game · **Confidence:** **high**

- **Role:** a major refcount Release — **690 callers**; a sibling of `Release_RefCounted_829FF648` but over the `obj->0` resource

- **Does:** arg: obj (r3). Loads `obj->0` (the refcounted resource); if 0, return. Else atomically decrement the refcount at `obj->0 + 12` via an lwarx/stwcx. retry loop under the global lock. If the new count is still > 0, return. If it reaches <= 0, call the destructor `sub_82B39A30(obj->0)` then `Free_SizeBucketed_Thunk_8221BE68(obj->0)`. The COM-style "decrement, destroy + free on zero" contract, but the resource lives at obj->0 (refcount at +12).

- **Calls:** `sub_82B39A30`, `Free_SizeBucketed_Thunk_8221BE68`

### `Free_Thunk_824FE010`

- **Address:** `0x824FE010` · **Size:** 1 insn · **Category:** thunk (free) · **Region:** game · **Confidence:** **high**

- **Role:** a free-forwarding thunk — **743 callers**

- **Does:** 1-instruction body: `b Free_SizeBucketed_Thunk_8221BE68`. Forwards its argument straight to the game's primary free(). A distinct free entry point (most likely a vtable operator-delete / destructor-tail) that delegates to the size-bucketed free.

- **Calls:** `Free_SizeBucketed_Thunk_8221BE68`

### `Heap_CarveBlock_83231058`

- **Address:** `0x83231058` · **Size:** ~90 insns · **Category:** body (custom heap) · **Region:** game (0x8323 allocator) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a low-level custom-heap block carve/split — a sibling of the named `Pool_*` / `Allocate_SizeBucketed` family

- **Does:** args: obj (r3), a key/value (r4), a pointer to the sorted-list head (r5), and a pointer to an offset (r6). Walks a **sorted** linked list (`node->0` = key, `node->12` = next) to find the first node whose key is >= the value (else return 0). On a hit it runs alignment/mask arithmetic (`obj->0` is an alignment mask, 16-byte block overhead), calls `sub_83230D00` to carve/split the block, then **unlinks** the node from the list (fixing the predecessor's `->12` and the head) and rewrites the block's next/size fields. Single caller: sub_83231590 (the spin-locked pool carve). Named for the mechanism: carving an aligned block out of a sorted free-list node.

- **Trigger:** Internal — the allocator carve path via sub_83231590.

- **Calls:** `sub_83230D00`

### `Allocate_OrDbgBreak_8221F388`

- **Address:** `0x8221F388` · **Size:** ~20 insns · **Category:** body (allocator wrapper) · **Region:** game · **Confidence:** **high**

- **Role:** the game's **primary allocation entry point** (the dominant "malloc") — **3725 call sites**, OOM is fatal

- **Does:** arg: size (r3). Calls `Allocate_SizeBucketed_Thunk_8221F3F0(size)` (→ `Allocate_SizeBucketed_83231D20`) to get a pointer. If the result is **0 (OOM)**: read a global function pointer (~0x82000B0C) and, if non-zero, call it with `r4 = 0` as an OOM error-report callback, then call `sub_82CBBB58` — a 1-insn thunk to **`__imp__DbgBreakPoint`** (debug breakpoint). Returns the pointer. Net effect: "allocate(size); on failure report + debug-break" — so callers never NULL-check (the wrapper guarantees non-NULL or a breakpoint). Sits one layer **above** the low-level custom heap (`Heap_AllocateBlock_82238828`).

- **Trigger:** Internal — the 3725 call sites; the main allocation path the engine uses everywhere.

- **Calls:** `Allocate_SizeBucketed_Thunk_8221F3F0`, `sub_82CBBB58` (→ `__imp__DbgBreakPoint`)

### `Allocate_SizeBucketed_83231D20`

- **Address:** `0x83231D20` · **Size:** ~70 insns · **Category:** body (allocator core) · **Region:** game (0x83231 region) · **Confidence:** **high** (mechanism)

- **Role:** the **core size-bucketed allocator** — dispatches a requested size to the right pool

- **Does:** arg: size (r3); returns a pointer or 0 if unsatisfiable. (1) Lazy init: if an init-flag byte is 0, call `sub_83231B60()`. (2) **Fast path**: linear-scan a **6-entry table of cached sizes**; if `size` matches, take a per-size fast path. (3) Otherwise dispatch by size to a pool: **size ≤ 64** → index `(size+3)` into a pre-built node pool and `Pool_AcquireNode_83230688` the selected node (0 if over that pool's cap); **64 < size ≤ 512** → spin-locked pool (`SpinLock_Acquire(pool+40)`, `sub_83231590(pool, size)`, release →40 = 0); **512 < size ≤ 10240** → a second spin-locked pool; **10240 < size ≤ 10485760 (10 MB)** → a third spin-locked pool; **size > 10 MB** → `sub_83230900(size)` (large-allocation path). Each pool has a max-size cap (checked via a global); exceeding it returns 0. Only 2 callers (the thunk `Allocate_SizeBucketed_Thunk_8221F3F0` and `sub_82BEAB88`) reach it directly.

- **Trigger:** Internal — via the thunk/wrapper above; the actual size→pool dispatch.

- **Calls:** `sub_83231B60` (init), `SpinLock_Acquire_822D7408`, `sub_83231590` (pool carve), `Pool_AcquireNode_83230688`, `sub_83230900` (large path)

### `Allocate_SizeBucketed_Thunk_8221F3F0`

- **Address:** `0x8221F3F0` · **Size:** 1 insn · **Category:** thunk (tail-branch) · **Region:** game · **Confidence:** **high**

- **Role:** plain alias to the core size-bucketed allocator

- **Does:** `b 0x83231d20` — a single tail-branch to `Allocate_SizeBucketed_83231D20`, passing `(size=r3)` through and returning its pointer. **142 callers** use this entry directly — they handle the 0/NULL result themselves rather than going through the OOM-crashing wrapper `Allocate_OrDbgBreak_8221F388`.

- **Trigger:** Internal — the 142 direct allocator callers.

- **Calls:** `Allocate_SizeBucketed_83231D20`

### `Free_SizeBucketed_Thunk_8221BE68`

- **Address:** `0x8221BE68` · **Size:** 1 insn · **Category:** thunk (tail-branch) · **Region:** game · **Confidence:** **high**

- **Role:** the game's **primary free() entry point** — **4498 call sites** (hotter than the allocate entry at 3725)

- **Does:** `b 0x83231be8` — a single tail-branch to `Free_SizeBucketed_83231BE8`, passing the pointer through. Plain `free(ptr)`: returns 1 if the pointer was released (or was NULL), 0 if not owned by this allocator. Sits one layer above the low-level custom heap (`Heap_FreeBlock_822394F0`).

- **Trigger:** Internal — the 4498 free call sites; the dominant free path.

- **Calls:** `Free_SizeBucketed_83231BE8`

### `Free_SizeBucketed_83231BE8`

- **Address:** `0x83231BE8` · **Size:** ~60 insns · **Category:** body (free core) · **Region:** game (0x83231 region) · **Confidence:** **high** (mechanism)

- **Role:** the **core size-bucketed free()** — the exact mirror of `Allocate_SizeBucketed_83231D20`

- **Does:** arg: pointer (r3); returns 1 if freed, 0 if not owned. (1) If the allocator's init byte is 0, return 0. (2) NULL → return 1 (freeing NULL is a valid no-op). (3) Try the **small pools** first via `Pool_FreeByAddress_832304E8(ptr, pool)` for the ≤64 and 64–512 pools; if either frees it, return 1. (4) Then the **spin-locked pools** (512–10240, 10240–10 MB, and one more) via `SpinLock_Acquire(pool+40)` + `sub_83231518(pool, ptr)` + release; return 1 if one frees it. (5) Otherwise the **large path**: `sub_83230848(ptr)`; return the freed bit. Walks the same size buckets the allocator used. 2 callers (the thunk and `sub_82BEAB88`).

- **Trigger:** Internal — via the thunk above; the actual size→pool free dispatch.

- **Calls:** `Pool_FreeByAddress_832304E8`, `SpinLock_Acquire_822D7408`, `sub_83231518` (spin-locked pool free), `sub_83230848` (large-path free)

### `Pool_FreeByAddress_832304E8`

- **Address:** `0x832304E8` · **Size:** ~30 insns · **Category:** body (pool free) · **Region:** game (0x83230 region) · **Confidence:** **high** (mechanism)

- **Role:** find the block containing an address in a pool and free it

- **Does:** args: ptr (r3), pool (r4). Walks the pool's block list (head at `r4->28`; each node `->0` = start offset, `->12` = next; base at `r4->20`) looking for the block whose range contains ptr. If found: align ptr down to the pool's granularity (`r4->0`), load the block descriptor at aligned+12, call `Pool_FreeNode_83230320(desc, ptr)` to release it, and return **1**. If no block contains ptr, return **0**. 4 callers: the two small pools of `Free_SizeBucketed_83231BE8`, plus `sub_828456C8` and `sub_8240DAA8`.

- **Trigger:** Internal — the small-pool free path.

- **Calls:** `Pool_FreeNode_83230320`

### `Pool_FreeNode_83230320`

- **Address:** `0x83230320` · **Size:** ~90 insns · **Category:** body (pool free) · **Region:** game (0x83230 region) · **Confidence:** **high** (mechanism)

- **Role:** the spin-locked single-block free — release counterpart of `Pool_AcquireNode_83230688`

- **Does:** args: pool (r31), ptr (r4). `SpinLock_Acquire(pool+36)`. Aligns ptr to the block granularity, pushes the address onto its block's free list (head at `block->0`, in-use count at `block->4`, decrementing it), and advances the pool's window (`pool->24` / `pool->28`, step `pool->4`). If the block's in-use count reaches 0, unlinks the whole block from the pool's free-block list (head `pool->16`, via `block->8` next). Calls `sub_83235FC0(pool->20)` as a helper, recomputes the window, then releases the lock (`pool+36 = 0`). Single caller: `Pool_FreeByAddress_832304E8`.

- **Trigger:** Internal — the small-pool free path.

- **Calls:** `SpinLock_Acquire_822D7408`, `sub_83235FC0`

### `Release_RefCounted_829FF648`

- **Address:** `0x829FF648` · **Size:** ~40 insns · **Category:** body (refcount release) · **Region:** game · **Confidence:** **high**

- **Role:** the game's **primary reference-counted `Release()`** — **2794 call sites**, the dominant reference-release path

- **Does:** arg: wrapper object (r3) holding a reference-counted resource at `wrapper->4`. If `wrapper->4` is NULL, just clear the wrapper and return. Otherwise **atomically decrement** the resource's reference count `*(wrapper->4)` using an `lwarx`/`stwcx.` loop under the global lock (`mfmsr`/`mtmsrd`). If the count is still > 0, another holder remains — clear the wrapper's fields and return. If it reaches **0 (last reference)**: call the resource's destructor callback (`resource->4`, invoked with `resource->8` as the object), then free the resource via `Free_SizeBucketed_Thunk_8221BE68` (the main free). Finally clear `wrapper->0` and `wrapper->4`. A classic COM-style `Release()`: drop one reference; if this was the last, destroy + free.

- **Trigger:** Internal — the 2794 call sites; runs whenever the engine drops a reference to a shared resource.

- **Calls:** `Free_SizeBucketed_Thunk_8221BE68` (and the resource's destructor callback via `bctrl`)

### `Heap_AllocateBlock_82238828`

- **Address:** `0x82238828` · **Size:** 495 insns · **Category:** body · **Region:** game · **Confidence:** medium

- **Role:** heap block allocator (game-side memory manager)

- **Does:** Large (495-instruction) memory allocator. Guards state with RtlEnter/LeaveCriticalSection, carves blocks out of a region (linked-list free lists at fixed object offsets), and falls back to NtAllocateVirtualMemory when the region is exhausted; raises RtlRaiseException on failure. This is the core block-alloc path of the game's custom heap.

- **Trigger:** Internal â€” runs whenever the engine allocates a heap block.

- **Imports:** `__imp__NtAllocateVirtualMemory`, `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`, `__imp__RtlRaiseException`

- **Calls:** `sub_82238FCC`, `sub_8223900C`, `sub_82CA3190`, `sub_82CBE800`, `sub_82CBF130`

### `Heap_FreeBlock_822394F0`

- **Address:** `0x822394F0` · **Size:** 124 insns · **Category:** body · **Region:** game · **Confidence:** medium

- **Role:** heap block free (game-side memory manager)

- **Does:** 124-instruction companion to the allocator: returns a block to the free list under RtlEnter/LeaveCriticalSection and releases whole regions via NtFreeVirtualMemory when they become empty. Part of the same custom heap as Heap_AllocateBlock_82238828.

- **Trigger:** Internal â€” runs whenever the engine frees a heap block.

- **Imports:** `__imp__NtFreeVirtualMemory`, `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`

- **Calls:** `sub_82239058`, `sub_822396C8`, `sub_82239708`, `sub_82CBE800`, `sub_82CBF370`

### `MemMgr_82CBF740`

- **Address:** `0x82CBF740` · **Size:** 325 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** MemMgr routine (325 insns, SDK)

- **Does:** [SDK runtime] Body function, 325 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeGetCurrentProcessType, __imp__NtAllocateVirtualMemory, __imp__NtFreeVirtualMemory, __imp__NtQueryVirtualMemory, __imp__RtlInitializeCriticalSection. Calls: sub_82CA3190, sub_82CAA2E0, sub_82CBEFC8, sub_82CBF760. First: `lwz r11,0(r8)`; last: `b 0x82ca2c1c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeGetCurrentProcessType`, `__imp__NtAllocateVirtualMemory`, `__imp__NtFreeVirtualMemory`, `__imp__NtQueryVirtualMemory`, `__imp__RtlInitializeCriticalSection`

- **Calls:** `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CBEFC8`, `sub_82CBF760`

### `MemMgr_82CBF760`

- **Address:** `0x82CBF760` · **Size:** 317 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** MemMgr routine (317 insns, SDK)

- **Does:** [SDK runtime] Body function, 317 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeGetCurrentProcessType, __imp__NtAllocateVirtualMemory, __imp__NtFreeVirtualMemory, __imp__NtQueryVirtualMemory, __imp__RtlInitializeCriticalSection. Calls: sub_82CA3190, sub_82CBEFC8. First: `mr r8,r8`; last: `b 0x82ca2c1c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeGetCurrentProcessType`, `__imp__NtAllocateVirtualMemory`, `__imp__NtFreeVirtualMemory`, `__imp__NtQueryVirtualMemory`, `__imp__RtlInitializeCriticalSection`

- **Calls:** `sub_82CA3190`, `sub_82CBEFC8`

### `MemMgr_82CBF770`

- **Address:** `0x82CBF770` · **Size:** 319 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** MemMgr routine (319 insns, SDK)

- **Does:** [SDK runtime] Body function, 319 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeGetCurrentProcessType, __imp__NtAllocateVirtualMemory, __imp__NtFreeVirtualMemory, __imp__NtQueryVirtualMemory, __imp__RtlInitializeCriticalSection. Calls: sub_82CA3190, sub_82CBEFC8. First: `mr r30,r3`; last: `b 0x82ca2c1c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeGetCurrentProcessType`, `__imp__NtAllocateVirtualMemory`, `__imp__NtFreeVirtualMemory`, `__imp__NtQueryVirtualMemory`, `__imp__RtlInitializeCriticalSection`

- **Calls:** `sub_82CA3190`, `sub_82CBEFC8`

### `MemFree_82CBFD74`

- **Address:** `0x82CBFD74` · **Size:** 446 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** MemFree routine (446 insns, SDK)

- **Does:** [SDK runtime] Body function, 446 insns, no prologue (mid-function target or leaf). Calls imports: __imp__NtFreeVirtualMemory, __imp__RtlCompareMemoryUlong, __imp__RtlEnterCriticalSection, __imp__RtlRaiseException. Calls: sub_82238790, sub_82239468, sub_82CA3190, sub_82CAA2E0, sub_82CBE800. First: `clrlwi. r11,r23,31`; last: `b 0x82cc04b0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__NtFreeVirtualMemory`, `__imp__RtlCompareMemoryUlong`, `__imp__RtlEnterCriticalSection`, `__imp__RtlRaiseException`

- **Calls:** `sub_82238790`, `sub_82239468`, `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CBE800`, `sub_82CBEA48`, `sub_82CC031C`, `sub_82CC0494`

### `MemFree_82CBFD9C`

- **Address:** `0x82CBFD9C` · **Size:** 436 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** MemFree routine (436 insns, SDK)

- **Does:** [SDK runtime] Body function, 436 insns, no prologue (mid-function target or leaf). Calls imports: __imp__NtFreeVirtualMemory, __imp__RtlCompareMemoryUlong, __imp__RtlRaiseException. Calls: sub_82238790, sub_82239468, sub_82CA3190, sub_82CAA2E0, sub_82CBE800. First: `addi r30,r20,-16`; last: `b 0x82cc04b0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__NtFreeVirtualMemory`, `__imp__RtlCompareMemoryUlong`, `__imp__RtlRaiseException`

- **Calls:** `sub_82238790`, `sub_82239468`, `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CBE800`, `sub_82CBEA48`, `sub_82CC031C`, `sub_82CC0494`


## C. Synchronization, Threads & Kernel

_Critical-section and thread primitives and kernel bug-check paths (RtlEnter/Leave/InitializeCriticalSection, ExTerminateThread, KeBugCheck), plus the 0x832B end-of-.text object family._

**28 functions.**

### `CriticalSection_EnterOrTry_82200688`

- **Address:** `0x82200688` · **Size:** ~25 insns · **Category:** body (CS acquire) · **Region:** game · **Confidence:** **high**

- **Role:** a general critical-section acquire — enter (blocking) or try-enter (non-blocking), recording success — **270 call sites**

- **Does:** args: lock-state obj (r3), CS pointer (r4), mode flag (r5). Stores the CS pointer at `obj->0`. If the flag (low byte of r5) is **0**: call `__imp__RtlEnterCriticalSection(CS)` (blocking). If **nonzero**: call `__imp__RtlTryEnterCriticalSection(CS)` (non-blocking); if it returns != 1 (failed to acquire), set the acquired flag to 0. On success set `obj->4 = 1` (acquired), on try-enter failure `obj->4 = 0`. The obj is a small lock-state wrapper: `obj->0` = CS pointer, `obj->4` = acquired flag. The matching release is a direct `__imp__RtlLeaveCriticalSection` call at the callers (gated on the acquired flag).

- **Trigger:** Internal — the 270 lock-acquire call sites across the engine.

- **Imports:** `__imp__RtlEnterCriticalSection`, `__imp__RtlTryEnterCriticalSection`

### `BugCheck_82CA9660`

- **Address:** `0x82CA9660` · **Size:** 54 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** BugCheck routine (54 insns, SDK)

- **Does:** [SDK runtime] Body function, 54 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeBugCheck. Calls: sub_82CA95D8, sub_82CA9710, sub_82CA9758. First: `lis r11,-31949`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeBugCheck`

- **Calls:** `sub_82CA95D8`, `sub_82CA9710`, `sub_82CA9758`

### `BugCheck_82CA9710`

- **Address:** `0x82CA9710` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** BugCheck routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeBugCheck. Calls: sub_82CA9758. First: `mr r8,r8`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeBugCheck`

- **Calls:** `sub_82CA9758`

### `Sync_82CAF2C0`

- **Address:** `0x82CAF2C0` · **Size:** 89 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (89 insns, SDK)

- **Does:** [SDK runtime] Body function, 89 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection. Calls: sub_82CA3C68, sub_82CA5DC0, sub_82CA88E0, sub_82CA8970, sub_82CAF40C. First: `mr r30,r25`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`

- **Calls:** `sub_82CA3C68`, `sub_82CA5DC0`, `sub_82CA88E0`, `sub_82CA8970`, `sub_82CAF40C`, `sub_82CAF424`, `sub_82CAFE08`, `sub_82CB5B78`

### `BugCheck_82CB57CC`

- **Address:** `0x82CB57CC` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** BugCheck routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeBugCheck. First: `mtctr r11`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeBugCheck`

### `BugCheck_82CB57D4`

- **Address:** `0x82CB57D4` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** BugCheck routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeBugCheck. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeBugCheck`

### `BugCheck_82CB57E0`

- **Address:** `0x82CB57E0` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** BugCheck routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). Calls imports: __imp__KeBugCheck. First: `lis r3,-16384`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__KeBugCheck`

### `Sync_82CB8D48`

- **Address:** `0x82CB8D48` · **Size:** 28 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (28 insns, SDK)

- **Does:** [SDK runtime] Body function, 28 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection. Calls: sub_82CB5B78, sub_82CB8D7C, sub_82CB8DB8. First: `lwz r11,8(r30)`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`

- **Calls:** `sub_82CB5B78`, `sub_82CB8D7C`, `sub_82CB8DB8`

### `Sync_82CB8D7C`

- **Address:** `0x82CB8D7C` · **Size:** 15 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (15 insns, SDK)

- **Does:** [SDK runtime] Body function, 15 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection. Calls: sub_82CB8DB8. First: `mr r8,r8`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`

- **Calls:** `sub_82CB8DB8`

### `Sync_82CB8E64`

- **Address:** `0x82CB8E64` · **Size:** 115 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (115 insns, SDK)

- **Does:** [SDK runtime] Body function, 115 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection, __imp__RtlLeaveCriticalSection. Calls: sub_82CAAE18, sub_82CAFF48, sub_82CB5B78, sub_82CB8CF8, sub_82CB8F00. First: `lis r11,-31921`; last: `b 0x82ca2c28`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`

- **Calls:** `sub_82CAAE18`, `sub_82CAFF48`, `sub_82CB5B78`, `sub_82CB8CF8`, `sub_82CB8F00`, `sub_82CB9018`, `sub_82CB9030`, `sub_82CB9054`

### `Sync_82CB8ECC`

- **Address:** `0x82CB8ECC` · **Size:** 50 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (50 insns, SDK)

- **Does:** [SDK runtime] Body function, 50 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection, __imp__RtlLeaveCriticalSection. Calls: sub_82CB5B78, sub_82CB8F00, sub_82CB9018, sub_82CB9054. First: `lwz r11,8(r30)`; last: `b 0x82cb8e74`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`

- **Calls:** `sub_82CB5B78`, `sub_82CB8F00`, `sub_82CB9018`, `sub_82CB9054`

### `Sync_82CB8F00`

- **Address:** `0x82CB8F00` · **Size:** 37 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Sync routine (37 insns, SDK)

- **Does:** [SDK runtime] Body function, 37 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlEnterCriticalSection, __imp__RtlLeaveCriticalSection. Calls: sub_82CB9018, sub_82CB9054. First: `mr r8,r8`; last: `b 0x82cb8e74`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`

- **Calls:** `sub_82CB9018`, `sub_82CB9054`

### `Thread_82CCA428`

- **Address:** `0x82CCA428` · **Size:** 22 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Thread routine (22 insns, SDK)

- **Does:** [SDK runtime] Body function, 22 insns, no prologue (mid-function target or leaf). Calls imports: __imp__ExTerminateThread. Calls: sub_82CBBED0, sub_82CC16C0. First: `li r3,1`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__ExTerminateThread`

- **Calls:** `sub_82CBBED0`, `sub_82CC16C0`

### `Thread_82CCA448`

- **Address:** `0x82CCA448` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Thread routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). Calls imports: __imp__ExTerminateThread. Calls: sub_82CBBED0. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__ExTerminateThread`

- **Calls:** `sub_82CBBED0`

### `Thread_82CCA454`

- **Address:** `0x82CCA454` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Thread routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls imports: __imp__ExTerminateThread. Calls: sub_82CBBED0. First: `lwz r3,80(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__ExTerminateThread`

- **Calls:** `sub_82CBBED0`

### `WaitEntrySetLpWakeTime`

- **Address:** `0x822D7508` · **Size:** 10 insns · **Category:** body · **Region:** game · **Confidence:** high

- **Role:** fills the `NTAPI_WAIT_ENTRY.LpWakeTime` from a millisecond timeout

- **Does:** `*r3 = (r4 == -1) ? 0 : (uint32)r4 * -10000`. Converts a millisecond timeout into 100ns units: -1 (INFINITE) maps to 0 (no wake time); any other value becomes a *negative* LpWakeTime meaning "wake this many 100ns units from now" (1 ms = 10,000 x 100ns). This is the exact LpWakeTime fill logic the Windows kernel uses for `NtWaitForSingleObjectEx`/`NtWaitForMultipleObjectsEx` wait entries. Called constantly: every alertable event/mutex/semaphore wait funnels through it, so it tops function-trace logs during load-in.

- **Trigger:** Everywhere - internal wait infrastructure.

- **Callers:** `KeWaitForSingleObject_Guest` (0x822D74A0), `KeWaitForMultipleObjects_Guest` (0x822D7528)

### `KeWaitForSingleObject_Guest`

- **Address:** `0x822D74A0` · **Size:** 33 insns · **Category:** body · **Region:** game · **Confidence:** high

- **Role:** alertable KeWaitForSingleObject-style wrapper over `NtWaitForSingleObjectEx` (guest-image-internal; NOT the xboxkrnl XAPI import — see the collision warning in `fable_2_manifest.toml`)

- **Does:** args: handle (r3), timeout ms (r5, -1 = INFINITE). Builds a stack wait entry at r1+80 via `WaitEntrySetLpWakeTime`, then calls `__imp__NtWaitForSingleObjectEx` (0x832B22AC) with Alertable=1 and the entry pointer; loops while the return is STATUS_USER_APC (257) and a timeout was given. On error calls sub_82CC1C38 and returns -1.

- **Trigger:** Everywhere - internal wait infrastructure.

- **Imports:** `__imp__NtWaitForSingleObjectEx`

- **Calls:** `WaitEntrySetLpWakeTime`, `sub_82CC1C38`

### `KeWaitForMultipleObjects_Guest`

- **Address:** `0x822D7528` · **Size:** ~100 insns · **Category:** body · **Region:** game · **Confidence:** high

- **Role:** alertable KeWaitForMultipleObjects-style wrapper over `NtWaitForMultipleObjectsEx` (guest-image-internal; NOT the xboxkrnl XAPI import — see the collision warning in `fable_2_manifest.toml`)

- **Does:** args: object count (r3), object-handle array (r4), mode (r5: 0 = WaitAll, nonzero = WaitAny), timeout ms (r6, -1 = INFINITE), APC-retry/Restart flag (r7: 0 = no retry on STATUS_USER_APC and Restart=0, nonzero = retry on 257 and Restart = low byte). Rejects count > 64 (KE_WAIT_VARIABLE limit) via sub_82CC1C18, copies the handle array to the stack at r1+96 via `MemCpy_SdkRuntime_82CA2C60`, fills LpWakeTime via `WaitEntrySetLpWakeTime` (from r6), then calls `__imp__NtWaitForMultipleObjectsEx` (0x832B22BC) with Alertable=1; loops while the return is STATUS_USER_APC (257) and r7 != 0. On error calls sub_82CC1C18 and returns -1. (The original notes here mislabeled r5 as the timeout — re-verified against the register flow and the four live call sites of the thunk below.)

- **Trigger:** Everywhere - internal wait infrastructure.

- **Imports:** `__imp__NtWaitForMultipleObjectsEx`

- **Calls:** `WaitEntrySetLpWakeTime`, `MemCpy_SdkRuntime_82CA2C60`, `sub_82CC1C18`

### `KeWaitForMultipleObjects_NoApcRetry`

- **Address:** `0x82179FB0` · **Size:** 2 insns · **Category:** thunk · **Region:** game · **Confidence:** **high**

- **Role:** KeWaitForMultipleObjects_Guest with the APC-retry flag (r7) forced to 0 — an APC interrupts the wait instead of being absorbed

- **Does:** 2-instruction thunk: `li r7,0` then tail-calls `KeWaitForMultipleObjects_Guest`. With r7==0 the wrapper returns STATUS_USER_APC (257) straight to the caller on an APC (no retry) and passes Restart=0 to the NT call. The only caller-facing difference from calling the wrapper directly.

- **Trigger:** Internal — 4 call sites: batch WaitAll locking of N mutex handles (sub_832B8C28 with a 1 ms timeout; sub_832B8B80 with 2 handles and a variable timeout) and single-object waits (sub_822BA4F0 infinite, sub_8262DDD0 variable timeout).

- **Calls:** `KeWaitForMultipleObjects_Guest`

### `MutexLock_WaitInfinite`

- **Address:** `0x82196C58` · **Size:** 2 insns · **Category:** thunk · **Region:** game · **Confidence:** high

- **Role:** the game's mutex-lock primitive — infinite blocking wait on a single kernel object (a 360 mutex is a mutant)

- **Does:** 2-instruction thunk: `li r5,0` (timeout 0) then tail-calls `KeWaitForSingleObject_Guest`. Timeout 0 → `WaitEntrySetLpWakeTime` writes LpWakeTime = 0 (no timer), so the wait is **infinite**, not a zero-wait poll (and -1 also maps to 0, so 0 cannot mean "don't block"). Blocks until the object is signaled; because timeout==0, APCs are not retried, so STATUS_USER_APC (257) can be returned to the caller. r4 (wait reason) is ignored by the wrapper; callers still pass -1/0/flags there. On success returns 0.

- **Trigger:** Internal — 42 call sites: audio/video/rendering worker threads blocking on per-object handles (e.g. r30+364, r31+20), SDK alert/exit loops. The lock half of pairs released via `MutexUnlock_ReleaseMutant` (0x83004F30), e.g. sub_832B8DA8.

- **Calls:** `KeWaitForSingleObject_Guest`

### `MutexUnlock_ReleaseMutant`

- **Address:** `0x83004F30` · **Size:** ~20 insns · **Category:** wrapper · **Region:** SDK · **Confidence:** high

- **Role:** the game's mutex-unlock primitive — `NtReleaseMutant` with bool result

- **Does:** args: mutant handle (r3). Calls `__imp__NtReleaseMutant` (0x832B31CC) with r4=0 (no previous-count out). If the NTSTATUS is >= 0 returns 1; otherwise logs the status via sub_82CC1C38 (the same error logger `KeWaitForSingleObject_Guest` uses) and returns 0.

- **Trigger:** Internal — 22 call sites, the release half of lock pairs acquired via `MutexLock_WaitInfinite` (handles often loaded from object offset +20).

- **Imports:** `__imp__NtReleaseMutant`

- **Calls:** `sub_82CC1C38`

### `MutexCreate_OutHandle`

- **Address:** `0x83004EA8` · **Size:** ~45 insns · **Category:** wrapper · **Region:** SDK · **Confidence:** high

- **Role:** the game's mutex-create primitive — `NtCreateMutant` writing the handle to an out pointer

- **Does:** args: r3 = out handle pointer, r4 = initial owned count, r5 = optional object-attributes pointer. When r5 != 0 it builds attributes via sub_82CC1DF0 (stack at r1+88/96), else passes NULL attrs. Calls `__imp__NtCreateMutant` (0x832B31BC) with the low byte of the initial count. On success sanity-checks the returned handle (a 0x10000 comparison feeds error code 183 through sub_82CC0750) and returns the handle; on failure logs via sub_82CC1C38 and returns 0.

- **Trigger:** Internal — SDK runtime mutex construction.

- **Imports:** `__imp__NtCreateMutant`

- **Calls:** `sub_82CC1DF0`, `sub_82CC0750`, `sub_82CC1C38`

### `CriticalSection_ProcessEntryArray_832B3700`

- **Address:** `0x832B3700` · **Size:** ~190 insns · **Category:** body · **Region:** game (end-of-.text) · **Confidence:** medium (mechanism certain, object identity unconfirmed)

- **Role:** locked per-entry processing loop over a 388-byte-entry array — the core routine of one object family

- **Does:** args: obj (r3). Guards: tail word `*(obj->280 + obj->276*388 - 40) != 0` and `obj->692 != 0` (else no-op). Locks the mutex at `(obj+852)->+4` via `MutexLock_WaitInfinite`; if the lock fails it returns without processing. Loops i over `obj->276` entries (array at obj->280, stride 388): vcall `entry->348` (entry skipped if it returns 0), range checks over entry fields 0/8/12/16/24/48/52/68, vcall `entry->352` (fetches buffer info into stack slots r1+80/84), then a byte transform that toggles each byte's high bit (`b+128` == `b XOR 0x80` — 8-bit signed<->unsigned conversion) between the entry buffer and the obj->12 buffer, with `MemCpy_SdkRuntime_82CA2C60` moves and entry->24 offset bookkeeping, then vcall `entry->356` and `entry->348` again. Unlocks via `MutexUnlock_ReleaseMutant`.

- **Trigger:** Internal — 7 call sites, all sibling methods in the same 0x832B end-of-.text family; each gates the call on a different non-zero object field (e.g. `EntryArray_ProcessAndStoreResult_832B3D90` on the entry count at `base->276`).

- **Calls:** `MutexLock_WaitInfinite`, `MutexUnlock_ReleaseMutant`, `MemCpy_SdkRuntime_82CA2C60`, plus virtual calls at entry offsets 348/352/356.

- **Note:** the XOR-0x80 byte churn is the signature of 8-bit sample conversion — plausibly an audio voice/sound pipeline — but the vtable contents are unresolved, so the name stays mechanism-based.

### `EntryArray_ProcessAndStoreResult_832B3D90`

- **Address:** `0x832B3D90` · **Size:** ~15 insns · **Category:** body (vtable method) · **Region:** game (end-of-.text) · **Confidence:** high (mechanism), low (purpose)

- **Role:** the entry-array method of the 0x832B family — runs the processing loop, then records a result word

- **Does:** args: r3 = the sub-object at `base+852`, r4 = a 32-bit value. `base = r3 - 852`. If `base->276` (the entry count) is non-zero it calls `CriticalSection_ProcessEntryArray_832B3700(base)`, then unconditionally stores r4 into `base->848`. No in-family reader of +848 was found, so it is likely status/result state consumed elsewhere in the family.

- **Trigger:** Virtual dispatch only — **zero static call sites** (registered, but reached through the family vtable).

- **Calls:** `CriticalSection_ProcessEntryArray_832B3700`

### `RingBuffer_TickPadWithFillByte_832B68A0`

- **Address:** `0x832B68A0` · **Size:** ~450 insns · **Category:** body (vtable method) · **Region:** game (end-of-.text) · **Confidence:** high (mechanism), low (object identity)

- **Role:** time-gated tick of a ring-buffer object — pads free space with a constant fill byte and re-syncs the read cursor from a consumer probe

- **Does:** args: obj (r3). Guards: `obj->212 == 0`, `obj->56 != 0`, `obj->208 != 0`. Layout: `obj->188` = capacity, `obj->216` = write cursor, `obj->228` = read cursor, `obj->192` = watermark W, `obj->184` = buffer pointer P, `obj->244` = fill byte, `obj->236` = last tick time, `obj->240` = tick rate limit (ms), `obj->92` = consumer sub-object S. `now = GetElapsedMsSinceStart()`; if `now - obj->236 > 12` the space check is skipped (starvation override). `free = (write - read) mod capacity` — the standard formula, verified in both branches. If `free >= 1024` the candidate length is `(free - 1024) & ~3` (a 1024-byte reserve is kept, 4-aligned), else 0. Watermark gates vs `obj->192`/`obj->232` (flag `obj->220` bypasses). When it passes: `obj->84 = 1`, then `MemSet` (sub_82CA3190) fills `2*W` bytes at `P + write` with byte `obj->244` (two chunks if it wraps), and the write cursor advances `2*W mod capacity`. Then a probe loop calls `sub_82CD2DF8(S, &out)` (three probes, looping while the out-difference is ≥ 1024) and stores the final result into the **read cursor** (`obj->228`) plus `now` into `obj->236`; if `now - old >= obj->240` it re-enters the fill step, else re-checks free space. Returns 1 (acted) or 0 (skipped).

- **Trigger:** Virtual dispatch only — **zero static call sites**.

- **Calls:** `GetElapsedMsSinceStart`, `MemSet` (sub_82CA3190 — the SDK memset, `memset(r3, r4, r5)`), `CsGuardedVcall_Slot60_82CD2DF8` (consumer probe)

- **Note:** strong audio hypothesis — with 8-bit samples a fill byte of 0x80 is signed silence, the sibling `CriticalSection_ProcessEntryArray_832B3700` does XOR-0x80 sample conversion, and the 12 ms / 1024-byte / rate-limit-ms constants fit sample streaming. But the fill byte and probe are runtime values, so the name stays mechanism-based.

### `PriorityArbitrate_PendingMutexRequests_832B8C28`

- **Address:** `0x832B8C28` · **Size:** ~500 insns · **Category:** body · **Region:** game (end-of-.text) · **Confidence:** high (mechanism), medium (semantics)

- **Role:** priority-ordered multi-mutex arbitration loop — the scheduler of the 0x832B request-node family

- **Does:** args: manager M (r3). Layout: `M->0` = state (must be 1 to accept), `M->24` = pending-list head, `M->28` = generation counter (incremented once per entry). Node layout: `node->0` = next pending, `node->4` = mutex handle, `node->8` = next in acquired list, `node->12` = priority, `node->16` = "prepare" vtable slot, `node->20` = "granted" vtable slot. While `M->0 == 1` and the pending list is non-empty: drain every pending node — `priority = vcall node->16(node, M->28)`; `node->12 = priority`; 0 = skip; otherwise insert into an acquired list sorted by **descending** priority (head = highest). After each drain pass build stack arrays (cap 64): `handles[i] = node->4`, `objs[i] = node`, then `ret = KeWaitForMultipleObjects_NoApcRetry(count, handles, mode=0 (WaitAll), timeout=1 ms)`. If `ret >= count` (timeout/APC) drain again; on success run the "granted" callback `vcall objs[ret]->20(objs[ret])` and immediately `MutexUnlock_ReleaseMutant(handles[ret])` — the highest-priority requester is notified and its gate released, the other acquired mutexes stay held (released by sibling family methods).

- **Trigger:** Internal — sole caller `sub_832B8DA8`, a sibling method that first locks `M->8` and `M->20` (`MutexLock_WaitInfinite`) and gates the call on `M->24 != 0` (the pending head — confirming the layout).

- **Calls:** `KeWaitForMultipleObjects_NoApcRetry`, `MutexUnlock_ReleaseMutant`, plus the node vtable slots (prepare at +16, granted at +20).

- **Note:** the "prepare" vcall is passed the generation counter, so node priorities can change per arbitration pass; the exact arbitration policy (why only `objs[0]` gets the callback on a WaitAll success, and where the other held mutexes are released) needs the vtable contents to confirm.

### `CsGuardedVcall_Slot60_82CD2DF8`

- **Address:** `0x82CD2DF8` · **Size:** ~20 insns · **Category:** body (proxy wrapper) · **Region:** SDK · **Confidence:** high (mechanism), low (interface identity)

- **Role:** critical-section-guarded forward of one method of the SDK's synchronized proxy class (the 0x82CD2Cxx–2Fxx cluster)

- **Does:** args: (owner=r3, out=r4). `RtlEnterCriticalSection(0x82CD0324)` (a single global CS shared by the whole cluster); `target = owner->76` (a C++ object, vptr at +0); calls `vtable[+60/4 = slot 15]` with `(this=target, out)`; `RtlLeaveCriticalSection`; returns the method's result. One of ~11 uniform siblings — slot offsets 28/56/60/64/72/80/92/96 plus two that forward to helpers (sub_82CDC180/sub_82CDCFE8) — i.e. a synchronized proxy class around one core object, every method serialized by the same CS.

- **Trigger:** Internal — 2 callers, 6 call sites: `RingBuffer_TickPadWithFillByte_832B68A0` (polled 3×, looping while successive out-values differ by ≥ 1024 — a consumer progress query) and `sub_832B6A98` (called after sibling slots 72/64 in a fetch-and-advance sequence).

- **Calls:** `__imp__RtlEnterCriticalSection`, `__imp__RtlLeaveCriticalSection`, plus the target's vtable slot 15.

- **Note:** the vtable contents are unresolved, so the name stays mechanism-based (byte offset of the slot). Naming the whole cluster coherently will come with the vtable.

### `SpinLock_Acquire_822D7408`

- **Address:** `0x822D7408` · **Size:** ~70 insns · **Category:** body (spin-lock) · **Region:** game (wait infra) · **Confidence:** **high**

- **Role:** spin-lock acquire (test-and-set with spin) — the lock half of a binary-flag spin lock

- **Does:** args: `addr` (r3) → a u32 flag where 0 = free / 1 = held. Read `*addr`; if 0, atomically CAS it to 1 (claim) and return; if non-zero (held), spin re-reading `*addr` until it reads 0, then claim it. Uses an `lwarx`/`stwcx.` atomic pair inside the recompiler's global-lock region (mfmsr/mtmsrd MSR sync), an `lwsync` after the claim, and a `db16cyc` delay hint once the spin counter passes 40 (bus-contention back-off). Returns void. Sits immediately before `KeWaitForSingleObject` (0x822D74A0) in the wait/sync infrastructure.

- **Trigger:** Internal — 15 callers (21 raw call sites), all in a data-processing subsystem: each loads a global object, adds an offset (a per-object lock word at +12/+32/+36/+40/−7828 etc.), and calls this **before** reading that object's fields — i.e. a spin-lock guard.

- **Note:** the matching release (setting the flag back to 0) is a separate function, not yet named.


## D. Exceptions & Fatal Errors

_RtlRaiseException paths and the fatal-error handler (DbgPrint + XamLoaderTerminateTitle)._

**6 functions.**

### `FatalError_82CBB98C`

- **Address:** `0x82CBB98C` · **Size:** 115 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** FatalError routine (115 insns, SDK)

- **Does:** [SDK runtime] Body function, 115 insns, no prologue (mid-function target or leaf). Calls imports: __imp__DbgPrint, __imp__XamLoaderTerminateTitle. Calls: sub_822EA8C0, sub_82CA97B8, sub_82CAC798, sub_82CBB570, sub_82CBB788. First: `lis r9,-31921`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__DbgPrint`, `__imp__XamLoaderTerminateTitle`

- **Calls:** `sub_822EA8C0`, `sub_82CA97B8`, `sub_82CAC798`, `sub_82CBB570`, `sub_82CBB788`, `sub_82CBBED0`, `sub_82CC16C0`, `sub_82CC1990`

### `FatalError_82CBBB24`

- **Address:** `0x82CBBB24` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** FatalError routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls imports: __imp__XamLoaderTerminateTitle. Calls: sub_82CBBED0. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__XamLoaderTerminateTitle`

- **Calls:** `sub_82CBBED0`

### `FatalError_82CBBB30`

- **Address:** `0x82CBBB30` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** FatalError routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls imports: __imp__XamLoaderTerminateTitle. Calls: sub_82CBBED0. First: `bl 0x832b230c`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__XamLoaderTerminateTitle`

- **Calls:** `sub_82CBBED0`

### `Exception_82CC02FC`

- **Address:** `0x82CC02FC` · **Size:** 86 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Exception routine (86 insns, SDK)

- **Does:** [SDK runtime] Body function, 86 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlRaiseException. Calls: sub_82238790, sub_82239468, sub_82CA3190, sub_82CAA2E0, sub_82CC031C. First: `lhz r11,2(r11)`; last: `b 0x82cc04b0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlRaiseException`

- **Calls:** `sub_82238790`, `sub_82239468`, `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CC031C`, `sub_82CC0494`, `sub_82CC04B0`

### `Exception_82CC031C`

- **Address:** `0x82CC031C` · **Size:** 78 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Exception routine (78 insns, SDK)

- **Does:** [SDK runtime] Body function, 78 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlRaiseException. Calls: sub_82238790, sub_82239468, sub_82CA3190, sub_82CAA2E0, sub_82CC0494. First: `mr r8,r8`; last: `b 0x82cc04b0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlRaiseException`

- **Calls:** `sub_82238790`, `sub_82239468`, `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CC0494`, `sub_82CC04B0`

### `Exception_82CC032C`

- **Address:** `0x82CC032C` · **Size:** 88 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Exception routine (88 insns, SDK)

- **Does:** [SDK runtime] Body function, 88 insns, no prologue (mid-function target or leaf). Calls imports: __imp__RtlRaiseException. Calls: sub_82238790, sub_82239468, sub_82CA3190, sub_82CAA2E0, sub_82CC0494. First: `lwz r30,124(r31)`; last: `b 0x82cc04b0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__RtlRaiseException`

- **Calls:** `sub_82238790`, `sub_82239468`, `sub_82CA3190`, `sub_82CAA2E0`, `sub_82CC0494`, `sub_82CC04B0`


## E. Filesystem / Volume

_Volume / filesystem queries (NtQueryVolumeInformationFile)._

**1 functions.**

### `FsVolume_82CBC820`

- **Address:** `0x82CBC820` · **Size:** 81 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** FsVolume routine (81 insns, SDK)

- **Does:** [SDK runtime] Body function, 81 insns, no prologue (mid-function target or leaf). Calls imports: __imp__NtQueryVolumeInformationFile. Calls: sub_82CAA2E0, sub_82CBC930, sub_82CBC97C, sub_82CBC9C4, sub_82CC0750. First: `cmplwi cr6,r28,0`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Imports:** `__imp__NtQueryVolumeInformationFile`

- **Calls:** `sub_82CAA2E0`, `sub_82CBC930`, `sub_82CBC97C`, `sub_82CBC9C4`, `sub_82CC0750`, `sub_82CC1C18`


## F. String & Math Helpers

_SDK memory/string copy and float->integer conversion helpers._

**15 functions.**

### `Ctype_Pred_Bit2_82CA6B10`

- **Address:** `0x82CA6B10` · **Category:** leaf (ctype) · **Region:** SDK (0x82CA) · **Confidence:** **high** (mechanism), low (which property)

- **Role:** a ctype predicate -- look up a char in the u16 ctype table and return bit 2 (0x4) -- a member of the 0x82CA6AB0-0x82CA6BB0 ctype cluster — 48 callers

- **Does:** arg: code (r3). Loads the table base from a global (~0x82CA70C8), loads the u16 entry table[code] (lhzx at base + code*2), returns bit 2 of it (r3 = entry & 0x4). Sibling sub_82CA6AF0 returns a different bit (one function per ctype bit, bit 0 = _UPPER).

- **Calls:** none

### `FNV1Hash_CaseInsensitive_82448030`

- **Address:** `0x82448030` · **Category:** leaf (hash) · **Region:** game · **Confidence:** **high**

- **Role:** a case-INSENSITIVE FNV-1 32 string hash — the ToLower-per-byte sibling of `FNV1Hash_821F3C28` — 2 callers

- **Does:** args: str (r3, null-terminated), seed (r4). Scans for length; if empty return seed. Else FNV-1 loop: lowercase each byte via `ToLower_SdkRuntime_82CA4288`, then hash = lowercased_byte ^ (hash * 0x01000193). Returns the 32-bit case-insensitive FNV-1 hash.

- **Calls:** `ToLower_SdkRuntime_82CA4288`

### `StrCmpIgnoreCase_82CA6320`

- **Address:** `0x82CA6320` · **Size:** ~25 insns · **Category:** leaf (string compare) · **Region:** SDK · **Confidence:** **high**

- **Role:** a case-INSENSITIVE string compare (stricmp) — **211 callers**

- **Does:** args: str1 (r3), str2 (r4). Walks both strings byte-by-byte; on the first difference it folds both bytes to lowercase (`| 32` for 'A'..'Z') before deciding the sign. Returns 0 if equal ignoring case, else a nonzero signed value. The signed-byte (case-sensitive) variant is `StrCmp_Signed_8226D7A8`.

- **Trigger:** Internal — the 211 case-insensitive comparison sites.

### `MemCpy_SIMDKernel_82CC9DA0`

- **Address:** `0x82CC9DA0` · **Category:** SIMD kernel (memcpy) · **Region:** game (0x82CC) · **Confidence:** **high** (mechanism)

- **Role:** a SIMD (vector) memcpy KERNEL — the 128-bytes-per-iteration vectorized loop a vectorized memcpy dispatches to for large blocks

- **Does:** args: dst (r3), src (r4), length (r5). For length >= 128: load 8 x 128-bit vectors from src (lvx at 0/16/32/48/63/64/80/96 + vperm alignment), store to dst (stvx at the same offsets), advance dst/src by 128, length -= 128, loop while length >= 128 (with dcbt/dcbzl cache hints). A scalar path handles length < 128. Single caller: `MemCpy_Vectorized_822085D0` (its large-block kernel).

- **Calls:** (none — pure SIMD copy)

### `FNV1Hash_821F3C28`

- **Address:** `0x821F3C28` · **Size:** ~20 insns · **Category:** leaf (hash) · **Region:** game · **Confidence:** **high**

- **Role:** the game's **primary 32-bit string hash** — FNV-1 (case-SENSITIVE), **2167 callers**

- **Does:** args: str (r3, null-terminated), seed (r4, initial hash). Scans str for its length (stop at 0); if empty, return the seed. Else run the FNV-1 loop over each byte: `hash = (hash * 0x01000193) ^ byte`, where 0x01000193 (16777619) is the FNV-1 32-bit prime. Returns the 32-bit hash. Callers pass seed = 0x811C9DC5 (the standard FNV-1 offset basis), so in practice this is the canonical FNV-1 32 hash. The case-SENSITIVE variant; the case-insensitive FNV-1 is sub_82448030.

- **Trigger:** Internal — the 2167 string-hash sites (name/ID lookups).

### `Math_FloatIndexStore_824D56C8`

- **Address:** `0x824D56C8` · **Size:** 15 insns · **Category:** body · **Region:** game · **Confidence:** low

- **Role:** float -> integer index store

- **Does:** Loads a float from a global table and a scalar, computes an integer via fcfid/fmadd/fctiwz (round-to-int of a scaled value), then stores that integer at *(r3 + 420). Converts a floating coordinate/parameter into a quantized integer index stored on an object.

- **Trigger:** Internal math helper (no direct player trigger).

### `Str_BoundedCopy_82C8D630`

- **Address:** `0x82C8D630` · **Size:** 28 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Role:** SDK bounded string copy (strncpy-like)

- **Does:** Copies bytes from src to dst up to a maximum length, stopping at a 0x80-high-byte terminator (the SDK's wide/extended string sentinel). On normal copy it stores the resulting length; on the short-init path it writes a 2-byte length header (sth) and a sentinel byte. Classic Xbox 360 SDK string copy with a size field.

- **Trigger:** Internal SDK string handling (no direct player trigger).

### `StrCmp_Signed_8226D7A8`

- **Address:** `0x8226D7A8` · **Size:** ~20 insns · **Category:** leaf (string compare) · **Region:** SDK · **Confidence:** **high**

- **Role:** a signed-byte string compare — strcmp with sign-extended bytes

- **Does:** args: str1 (r3), str2 (r4). Loop: load `*str1` as a **signed** byte (`lbz` then `extsb`) into r10; if `*str1 != 0` go to compare; else (str1 hit its 0 terminator) load `*str2` and if `*str2 == 0` return 0 (both empty / equal), else fall through to compare (where 0 < `*str2`, so str1 is shorter). Compare: load `*str2` as a signed byte into r11; if r10 < r11 return **-1**, if r10 > r11 return **1**, else advance both and loop. Returns -1 (str1 < str2), 0 (equal), 1 (str1 > str2). The `extsb` sign-extension makes this a **signed** byte compare (bytes >= 0x80 sort as negative), distinct from an unsigned strcmp. 169 call sites. Named `StrCmp_Signed` to convey the signed-byte ordering and to keep the guest symbol distinct from host libc.

- **Trigger:** Internal — the 169 string-comparison sites.

### `MemCpy_SdkRuntime_82CA2C60`

- **Address:** `0x82CA2C60` · **Size:** ~290 insns · **Category:** runtime · **Region:** SDK · **Confidence:** **high**

- **Role:** the SDK C-runtime memcpy — the image's dominant memory-copy primitive

- **Does:** `memcpy(dest=r3, src=r4, len=r5)`, returns dest (saved at -8(r1) on entry, restored on every exit). ~1.2 KB of hand-unrolled PPC: `dcbt` prefetch on entry, byte-wise dest alignment (with a fast 4-byte pre-loop), 4/8-byte misaligned-source paths, an 8x8-byte (64-byte) unrolled main loop, 128-byte `dcbt` block prefetch for len >= 128, and `dcbtst` writeback hints on the tail. 1310 call sites across game and SDK code (fixed-length copies like `li r5,12`, computed sizes, the handle-array copy inside `KeWaitForMultipleObjects_Guest`, the buffer moves inside `CriticalSection_ProcessEntryArray_832B3700`). Named `MemCpy_SdkRuntime` rather than bare `memcpy` to keep the guest symbol distinct from host libc in traces and to avoid global-namespace ambiguity.

- **Trigger:** Everywhere — any byte copy in the engine.

### `MemCpy_Vectorized_822085D0`

- **Address:** `0x822085D0` · **Size:** ~200 insns · **Category:** runtime (SIMD) · **Region:** game · **Confidence:** **high**

- **Role:** the game's SIMD-optimized byte-copy kernel — a second memcpy, distinct from the SDK C-runtime `MemCpy_SdkRuntime_82CA2C60`

- **Does:** `copy(dst=r3, src=r4, len=r5)`, returns dst (r26). Forward-only, **no overlap/direction handling** (plain memcpy semantics). Two paths: `len < 16` takes a small path (optional 8-byte head if src is 8-aligned, then an aligned 4-byte chunk via sub_82CA30E8, then a 4-byte remainder loop); `len >= 16` takes a vectorized path using 128-bit SIMD (`lvx128`/`vperm128`/`stvx128`) for 16-byte main-loop moves with `dcbt` (source) and `dcbzl` (destination) 128-byte prefetch, chunked at 1024. 34 call sites across the engine. Named `MemCpy_Vectorized` to distinguish it from the SDK memcpy and to keep the guest symbol distinct from host libc (the `MemCpy_SdkRuntime` convention).

- **Trigger:** Internal — the 34 engine call sites (large/performance-critical copies).

### `MemCpy_Aligned_82CA30E8`

- **Address:** `0x82CA30E8` · **Size:** ~40 insns · **Category:** runtime · **Region:** SDK · **Confidence:** **high**

- **Role:** a word-aligned memcpy — the aligned-chunk helper behind the SIMD copy, and a standalone memcpy

- **Does:** `memcpy(dst=r3, src=r4, len=r5)`, returns void. Classic PPC **byte-align → word-copy → tail-bytes** structure: a byte loop aligns dst to a 4-byte boundary, then a 4-byte word loop (with a separate unaligned-source branch) copies the bulk, then a final byte loop copies the <4 remainder. Forward-only, no overlap handling (plain memcpy semantics). Sits in the SDK runtime region immediately before `memset` (sub_82CA3190). **34 direct call sites** (e.g. `r4=src; r5=496; bl`), **and** it is the aligned-chunk helper that `MemCpy_Vectorized_822085D0` calls for its small-path (len<16) word copy. Named `MemCpy_Aligned` to distinguish it from `MemCpy_SdkRuntime_82CA2C60` (the ~1.2 KB unrolled CRT memcpy) and `MemCpy_Vectorized_822085D0` (SIMD), and to keep the guest symbol distinct from host libc.

- **Trigger:** Internal — the 34 direct copy sites + the SIMD memcpy's small path.

### `StoreUnitTimesCount_82CE32B0`

- **Address:** `0x82CE32B0` · **Size:** 5 insns · **Category:** body (vtable method) · **Region:** SDK · **Confidence:** medium (field roles pinned by the inverse operation)

- **Role:** the count → total direction of a unit/count field pair — `*out = unit × count`

- **Does:** `*(uint32_t*)r4 = (uint16_t)obj->108 * obj->120;` returns 0. Field roles are pinned down by the inverse operation `sub_82CE37C0`, which (spin-lock protected — `KeRaiseIrqlToDpcLevel` + `KeAcquireSpinLockAtRaisedIrql` on global 0x82CA7060, with a `twllei` div-by-zero trap guard) computes `obj->120 = (*total) / obj->108` and sets `obj->272 = 1`. So +108 is the unit/divisor and +120 the total/unit count; this function is the classic `frameSize × frameCount` / `sampleSize × sampleCount` byte-length math. Siblings in the 0x82CE region write the same fields (sub_82CE3BE0/sub_82CE3400/sub_82CE24B0/sub_82CE0C20 store the u16; sub_82CE3CF0/sub_82CE1588 update the u32).

- **Trigger:** Virtual dispatch only — **zero static call sites**.

### `Ctype_IsUpper_82CA6AD0`

- **Address:** `0x82CA6AD0` · **Size:** 5 insns · **Category:** leaf · **Region:** SDK · **Confidence:** **high** (mechanism), high (bit meaning — pinned by its consumer)

- **Role:** the SDK C-runtime's "is uppercase" character-class predicate — bit 0 of the 256-entry u16 ctype table

- **Does:** `return (u16table[c] & 1);` — the table base is runtime-loaded from global 0x82CCC8C4, entry = `*(base + c*2)` (zero-extended halfword). One of **9 uniform siblings** in the 0x82CA6AB0–0x82CA6BB0 block, each extracting a different bit/mask of the same u16 entry: bit 0 (this one), bits 0|1|8 (sub_82CA6AB0: `andi 259`, sub_82CA6B90: `andi 263`), and single bits 24/26/27/28/29/30 (sub_82CA6B30/BB0/B70/B50/B10/AF0 — beyond the 16-bit load, so they read zero in the current table layout).

- **Trigger:** Internal — 2 call sites: `ToLower_SdkRuntime_82CA4288` (gates the A-Z → a-z fold) and the dispatch table `sub_82A25230` (routes to the sibling predicates by index).

- **Note:** the _UPPER meaning of bit 0 is pinned by usage — its only non-cluster consumer gates an uppercase→lowercase conversion on it.

### `ToLower_SdkRuntime_82CA4288`

- **Address:** `0x82CA4288` · **Size:** ~10 insns · **Category:** body · **Region:** SDK · **Confidence:** **high**

- **Role:** the SDK C-runtime tolower (locale-aware variant)

- **Does:** classic CRT shape: `if (0 <= c < 256 && Ctype_IsUpper_82CA6AD0(c)) return ToLower_Ascii_821EE9E8(c); else return c;` where `ToLower_Ascii_821EE9E8` is the plain ASCII fold (A-Z → a-z, else c unchanged — 22 callers image-wide, so it doubles as the general tolower). The ctype guard makes this the locale-aware variant. Named `ToLower_SdkRuntime` (not bare `tolower`) to keep the guest symbol distinct from host libc, matching the `MemCpy_SdkRuntime` convention.

- **Trigger:** Internal — 2 call sites, both string processing: `sub_82448030` is a **case-insensitive FNV-1 string hash** (`hash = tolower(byte) ^ (hash * 16777619)`, 16777619 = 0x01000193 = the 32-bit FNV prime) and `sub_83002B80` converts a character to a 0–25 letter index (`result - 96`, 'a' = 97).

- **Calls:** `Ctype_IsUpper_82CA6AD0`, `ToLower_Ascii_821EE9E8`.

### `ToLower_Ascii_821EE9E8`

- **Address:** `0x821EE9E8` · **Size:** 6 insns · **Category:** leaf · **Region:** SDK · **Confidence:** **high**

- **Role:** plain ASCII tolower — the raw fold without the ctype guard

- **Does:** `if (c < 65) return c; if (c > 90) return c; return c + 32;` — A-Z → a-z, everything else passes through unchanged. 22 call sites image-wide (32 raw call occurrences), so this is the general tolower used across the engine. No prologue (leaf). Named `ToLower_Ascii` to distinguish it from the locale-aware `ToLower_SdkRuntime_82CA4288` (which gates the same fold on `Ctype_IsUpper_82CA6AD0`) and to keep the guest symbol distinct from host libc (the `MemCpy_SdkRuntime` convention).


## G. Virtual-Dispatch Thunks (vtable)

_`lwz vtptr,0(rX); lwz m,OFF(vtptr); mtctr; bctr` — call the object's virtual method at slot OFF/4. The slot number is the method; the address is the binding site._

**50 functions.**

### `VtableSlotX13_82C010B0`

- **Address:** `0x82C010B0` · **Size:** 4 insns · **Category:** vtable_x (thunk) · **Region:** game (0x82C0 region) · **Confidence:** **high** (mechanism)

- **Role:** a pure vtable thunk — forwards to vtable slot 13; the offset-52 sibling of `VtableSlotX14_82C01090`

- **Does:** arg: wrapper (r3). Dereferences r3 to get the object, loads vtable slot 13 (offset 52 / 4), and `bctr`s straight into it. 2 callers (sub_82C15068, sub_82C4A410).

- **Trigger:** Internal — the single/all-success vcall chains above.

### `VtableSlotX14_82C01090`

- **Address:** `0x82C01090` · **Size:** 4 insns · **Category:** vtable_x (thunk) · **Region:** game (0x82C0 region) · **Confidence:** **high** (mechanism)

- **Role:** a pure vtable thunk — forwards to vtable slot 14

- **Does:** arg: wrapper (r3). Dereferences r3 to get the object, loads vtable slot 14 (offset 56 / 4), and `bctrl`s straight into it, passing the args through unmodified. Single caller: sub_82C14F38 (as the middle step of that all-success chain).

- **Trigger:** Internal — the single caller sub_82C14F38.

### `VtableSlotX3173_822A2AB0`

- **Address:** `0x822A2AB0` · **Size:** 59 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 3173 (offset 12692) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot29_824F0600`

- **Address:** `0x824F0600` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Virtual-call thunk: reads the vtable pointer from the object, selects slot 29 (byte offset 116), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX6922_825EF568`

- **Address:** `0x825EF568` · **Size:** 7 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 6922 (offset 27688) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot5_82903F70`

- **Address:** `0x82903F70` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Virtual-call thunk: reads the vtable pointer from the object, selects slot 5 (byte offset 20), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot2_82903F80`

- **Address:** `0x82903F80` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Virtual-call thunk: reads the vtable pointer from the object, selects slot 2 (byte offset 8), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX3_829FCB00`

- **Address:** `0x829FCB00` · **Size:** 6 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 3 (offset 12) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX4_82B56870`

- **Address:** `0x82B56870` · **Size:** 14 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 4 (offset 16) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX4765_82BBC898`

- **Address:** `0x82BBC898` · **Size:** 8 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 4765 (offset 19060) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX1_82BE9680`

- **Address:** `0x82BE9680` · **Size:** 559 insns · **Category:** vtable_x · **Region:** game · **Confidence:** medium

- **Does:** [game code] Variant virtual dispatch: selects vtable slot 1 (offset 4) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82BE9F3C`, `sub_82BEA25C`, `sub_82BEA3D0`, `sub_82BEA438`, `sub_82BEA698`, `sub_82BEA6E8`, `sub_82BEA860`

### `VtableSlot1_82BFA4A8`

- **Address:** `0x82BFA4A8` · **Size:** 4 insns · **Category:** vtable · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Virtual-call thunk: reads the vtable pointer from the object, selects slot 1 (byte offset 4), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX17_82C09188`

- **Address:** `0x82C09188` · **Size:** 9 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 17 (offset 68) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX7_82C0B400`

- **Address:** `0x82C0B400` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 7 (offset 28) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot7_82C1DC50`

- **Address:** `0x82C1DC50` · **Size:** 4 insns · **Category:** vtable · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Virtual-call thunk: reads the vtable pointer from the object, selects slot 7 (byte offset 28), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot6_82C1DC60`

- **Address:** `0x82C1DC60` · **Size:** 4 insns · **Category:** vtable · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Virtual-call thunk: reads the vtable pointer from the object, selects slot 6 (byte offset 24), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX3_82C1DC80`

- **Address:** `0x82C1DC80` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 3 (offset 12) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX12_82C2F500`

- **Address:** `0x82C2F500` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 12 (offset 48) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX10_82C4C2C8`

- **Address:** `0x82C4C2C8` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 10 (offset 40) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX11_82C4C2E8`

- **Address:** `0x82C4C2E8` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 11 (offset 44) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX6_82C4C300`

- **Address:** `0x82C4C300` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 6 (offset 24) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX2_82C4C320`

- **Address:** `0x82C4C320` · **Size:** 7 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 2 (offset 8) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX2_82C4C5A8`

- **Address:** `0x82C4C5A8` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 2 (offset 8) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX1_82C4C5C8`

- **Address:** `0x82C4C5C8` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 1 (offset 4) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX1_82C4C5E8`

- **Address:** `0x82C4C5E8` · **Size:** 9 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 1 (offset 4) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX4_82CE5D50`

- **Address:** `0x82CE5D50` · **Size:** 18 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 4 (offset 16) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX3_82EF41D8`

- **Address:** `0x82EF41D8` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 3 (offset 12) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX4_82EF41F0`

- **Address:** `0x82EF41F0` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 4 (offset 16) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot26_82F04420`

- **Address:** `0x82F04420` · **Size:** 4 insns · **Category:** vtable · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Virtual-call thunk: reads the vtable pointer from the object, selects slot 26 (byte offset 104), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX10_82F1D508`

- **Address:** `0x82F1D508` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 10 (offset 40) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX4_82F1D520`

- **Address:** `0x82F1D520` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 4 (offset 16) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX11_82F1D538`

- **Address:** `0x82F1D538` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 11 (offset 44) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX5_82F1D550`

- **Address:** `0x82F1D550` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 5 (offset 20) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX6_82F1D568`

- **Address:** `0x82F1D568` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 6 (offset 24) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX7_82F1D580`

- **Address:** `0x82F1D580` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 7 (offset 28) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX8_82F1D598`

- **Address:** `0x82F1D598` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 8 (offset 32) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX9_82F1D5B0`

- **Address:** `0x82F1D5B0` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 9 (offset 36) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX14_82F2C3A8`

- **Address:** `0x82F2C3A8` · **Size:** 6 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 14 (offset 56) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX3_82F67E18`

- **Address:** `0x82F67E18` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 3 (offset 12) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX6_82F67E40`

- **Address:** `0x82F67E40` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 6 (offset 24) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX7_82F67E68`

- **Address:** `0x82F67E68` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 7 (offset 28) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX2_82FE7850`

- **Address:** `0x82FE7850` · **Size:** 66 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 2 (offset 8) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX2_83046BE8`

- **Address:** `0x83046BE8` · **Size:** 69 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 2 (offset 8) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlot95_83056810`

- **Address:** `0x83056810` · **Size:** 4 insns · **Category:** vtable · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Virtual-call thunk: reads the vtable pointer from the object, selects slot 95 (byte offset 380), and tail-calls it.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX133_83056BC0`

- **Address:** `0x83056BC0` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 133 (offset 532) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX134_83056BE8`

- **Address:** `0x83056BE8` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 134 (offset 536) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX133_83056C10`

- **Address:** `0x83056C10` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 133 (offset 532) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX134_83056C38`

- **Address:** `0x83056C38` · **Size:** 10 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 134 (offset 536) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX151_83057068`

- **Address:** `0x83057068` · **Size:** 8 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 151 (offset 604) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `VtableSlotX3_831C5CD8`

- **Address:** `0x831C5CD8` · **Size:** 20 insns · **Category:** vtable_x · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Variant virtual dispatch: selects vtable slot 3 (offset 12) with extra arg shuffling / null checks, then bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.


## H. Switch / Jump-Table Dispatchers

_Index a jump table by an integer argument (rlwinm/lwzx/mtctr/bctr) and tail-call the selected case._

**74 functions.**

### `StateDispatch_82BCA7D8`

- **Address:** `0x82BCA7D8` · **Category:** switch dispatch · **Region:** game (0x82BC) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 7-way state dispatch (state-machine switch) on the byte `(obj->4 - 4)` (values 0-6) -- a sibling of `StateDispatch_82BCBB40` — 17 callers

- **Does:** args: obj (r3), state obj (r4, ->4 = switch key, ->5 = status byte). Clears the low 2 bits of obj->5, computes key = obj->4 - 4, and (if 0..6) switches on it; each case (0-6) performs inline register operations (no external sub-calls). 17 callers.

- **Trigger:** Internal — the 17 callers (sub_822DF280, sub_82BCC6F0, sub_82BD3A90, ...).

### `StateDispatch_82BCBB40`

- **Address:** `0x82BCBB40` · **Category:** switch dispatch · **Region:** game (0x82BC) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 5-way state dispatch (state-machine switch) on the byte `obj->16->21` (values 0-4) — 2 callers

- **Does:** arg: obj (r3). Reads the state byte at obj->16->21; if > 4, default path. Else switch (via a function-pointer table) on it: case 0 → sub_82BCB7E8(obj) return 0; cases 1-4 → other 0x82BC handler paths (sub_82BC9860, sub_82BCB0A8, sub_82BCB610, sub_82BCB6E0, sub_82BCB8D0, sub_82BCD698, sub_8227BB58, ...). The individual states/handlers aren't resolvable, so the name states the mechanism.

- **Trigger:** Internal — the 2 callers (sub_82BC62B8, sub_8227B8B8).

### `SwitchDispatch_8217F250`

- **Address:** `0x8217F250` · **Size:** 35 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8222B3C8`

- **Address:** `0x8222B3C8` · **Size:** 31 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822D7C50`

- **Address:** `0x822D7C50` · **Size:** 18 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822D7CB0`

- **Address:** `0x822D7CB0` · **Size:** 23 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822D7D58`

- **Address:** `0x822D7D58` · **Size:** 31 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822D7DF8`

- **Address:** `0x822D7DF8` · **Size:** 30 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822D7EA0`

- **Address:** `0x822D7EA0` · **Size:** 21 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_822EAA98`

- **Address:** `0x822EAA98` · **Size:** 24 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_823A5A28`

- **Address:** `0x823A5A28` · **Size:** 66 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_824063B0`

- **Address:** `0x824063B0` · **Size:** 56 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82415A20`

- **Address:** `0x82415A20` · **Size:** 105 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8241BAA8`

- **Address:** `0x8241BAA8` · **Size:** 67 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8242C5E0`

- **Address:** `0x8242C5E0` · **Size:** 25 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_824B7E70`

- **Address:** `0x824B7E70` · **Size:** 32 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_824B7560`, `sub_824B7B40`, `sub_824B7BF0`, `sub_824B7C98`

### `SwitchDispatch_824BDE60`

- **Address:** `0x824BDE60` · **Size:** 34 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_824CAC90`

- **Address:** `0x824CAC90` · **Size:** 82 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_824D2800`

- **Address:** `0x824D2800` · **Size:** 27 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8250B778`

- **Address:** `0x8250B778` · **Size:** 15 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82520C00`

- **Address:** `0x82520C00` · **Size:** 22 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82522E18`

- **Address:** `0x82522E18` · **Size:** 83 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82592C80`

- **Address:** `0x82592C80` · **Size:** 21 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82583140`, `sub_825832B0`, `sub_828CE240`

### `SwitchDispatch_8259AF48`

- **Address:** `0x8259AF48` · **Size:** 26 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_823F7678`

### `SwitchDispatch_825A0B58`

- **Address:** `0x825A0B58` · **Size:** 24 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82583140`, `sub_825A0BE8`, `sub_828CE240`

### `SwitchDispatch_82622360`

- **Address:** `0x82622360` · **Size:** 39 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82275368`

### `SwitchDispatch_826224B8`

- **Address:** `0x826224B8` · **Size:** 35 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82275368`

### `SwitchDispatch_8272B2C0`

- **Address:** `0x8272B2C0` · **Size:** 79 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82771250`

- **Address:** `0x82771250` · **Size:** 24 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_827D30C0`

- **Address:** `0x827D30C0` · **Size:** 135 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8283A008`

- **Address:** `0x8283A008` · **Size:** 25 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8283FF98`

- **Address:** `0x8283FF98` · **Size:** 23 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8289FB40`

- **Address:** `0x8289FB40` · **Size:** 146 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_8299F668`

- **Address:** `0x8299F668` · **Size:** 32 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82A2F3E8`

- **Address:** `0x82A2F3E8` · **Size:** 65 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82A34680`

- **Address:** `0x82A34680` · **Size:** 41 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82B38270`

- **Address:** `0x82B38270` · **Size:** 47 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82B57148`

- **Address:** `0x82B57148` · **Size:** 24 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82B809E8`

- **Address:** `0x82B809E8` · **Size:** 133 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82B8F5E8`

- **Address:** `0x82B8F5E8` · **Size:** 30 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82BC8260`

- **Address:** `0x82BC8260` · **Size:** 27 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82BC9B28`

- **Address:** `0x82BC9B28` · **Size:** 70 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82BE0100`

- **Address:** `0x82BE0100` · **Size:** 13 insns · **Category:** switchdisp · **Region:** game · **Confidence:** medium

- **Does:** [game code] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C56828`

- **Address:** `0x82C56828` · **Size:** 25 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C89840`

- **Address:** `0x82C89840` · **Size:** 22 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C89638`, `sub_82C898B8`, `sub_82C89A0C`

### `SwitchDispatch_82C898B8`

- **Address:** `0x82C898B8` · **Size:** 24 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C89960`

### `SwitchDispatch_82C89960`

- **Address:** `0x82C89960` · **Size:** 21 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C8C9F0`

- **Address:** `0x82C8C9F0` · **Size:** 64 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C8A698`

### `SwitchDispatch_82C8CF00`

- **Address:** `0x82C8CF00` · **Size:** 37 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C8D3F0`

- **Address:** `0x82C8D3F0` · **Size:** 72 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C8D750`

- **Address:** `0x82C8D750` · **Size:** 46 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C8DAE8`

- **Address:** `0x82C8DAE8` · **Size:** 22 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C97120`

- **Address:** `0x82C97120` · **Size:** 13 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C99CB8`

- **Address:** `0x82C99CB8` · **Size:** 49 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C99E38`

### `SwitchDispatch_82C99ED0`

- **Address:** `0x82C99ED0` · **Size:** 49 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C9A060`

### `SwitchDispatch_82C9A118`

- **Address:** `0x82C9A118` · **Size:** 49 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C9A2A8`

### `SwitchDispatch_82C9B420`

- **Address:** `0x82C9B420` · **Size:** 33 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C9D280`

- **Address:** `0x82C9D280` · **Size:** 78 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C9DB00`

- **Address:** `0x82C9DB00` · **Size:** 49 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C9E878`

- **Address:** `0x82C9E878` · **Size:** 29 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82C9E9B8`

- **Address:** `0x82C9E9B8` · **Size:** 43 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82CA1A00`

- **Address:** `0x82CA1A00` · **Size:** 42 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82CA1E98`

- **Address:** `0x82CA1E98` · **Size:** 41 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA1C38`

### `SwitchDispatch_82D7F5B0`

- **Address:** `0x82D7F5B0` · **Size:** 76 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82D82B48`

- **Address:** `0x82D82B48` · **Size:** 78 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82D97098`

- **Address:** `0x82D97098` · **Size:** 225 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82D9E998`

- **Address:** `0x82D9E998` · **Size:** 17 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82D9E9F0`

- **Address:** `0x82D9E9F0` · **Size:** 25 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82DA60B0`

- **Address:** `0x82DA60B0` · **Size:** 184 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82DA8888`

### `SwitchDispatch_82DDC208`

- **Address:** `0x82DDC208` · **Size:** 113 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82DE4C10`

- **Address:** `0x82DE4C10` · **Size:** 118 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82DE6750`

- **Address:** `0x82DE6750` · **Size:** 57 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82E0F3A8`

- **Address:** `0x82E0F3A8` · **Size:** 44 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `SwitchDispatch_82E33B90`

- **Address:** `0x82E33B90` · **Size:** 36 insns · **Category:** switchdisp · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Switch / jump-table dispatcher: indexes a table by an integer argument and tail-calls the selected case.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.


## I. Trampolines & Getters

_Small helpers that return a constant / global / adjusted pointer or tail-call another function._

**98 functions.**

### `TailCall_82238FCC`

- **Address:** `0x82238FCC` · **Size:** 6 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Does:** [game code] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C20.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_8223900C`

### `TailCall_822396C8`

- **Address:** `0x822396C8` · **Size:** 6 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Does:** [game code] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82239708`

### `GetCurrentTimeMs`

- **Address:** `0x82266070` · **Size:** 4 insns · **Category:** getter · **Region:** game · **Confidence:** **high**

- **Role:** the game's master millisecond clock — cached in a global singleton

- **Does:** no arguments; returns `*( *(0x82000908) + 16 )` (double indirection: global pointer to a singleton, field at offset 16). 147 call sites in 89 files, almost all `stw r3, N(obj)` — storing the value into per-object timestamp fields (lastUpdate/startTime/nextFire). A few compare it against an adjacent previous-time field (32-bit wrap handling, e.g. sub_822EEC08) or double-sample it, storing two reads 4 bytes apart (before/after bracketing, e.g. sub_8237E3E0). Millisecond scale proven by callers: rate limiters compute `consumption = rate * (now - last) / 1000` (sub_82FFA508), poll loops use `deadline = saved + 100` (sub_82388658), and the 2^30 jump guard in `GetElapsedMsSinceStart` only makes sense in ms (~12.4 days). No other code in the recompiled image touches 0x82000908 directly — the singleton pointer is copied elsewhere at init and the field updated through the copy.

- **Trigger:** Everywhere — it is the time source for the whole engine (timers, rate limiters, deadlines, timestamp fields).

### `RetAddr_82267568`

- **Address:** `0x82267568` · **Size:** 3 insns · **Category:** const · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Const thunk: builds and returns the data address 0x83499170.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetAddr_82267C88`

- **Address:** `0x82267C88` · **Size:** 3 insns · **Category:** const · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Const thunk: builds and returns the data address 0x8334E2D4.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetConst67_8274B798`

- **Address:** `0x8274B798` · **Size:** 2 insns · **Category:** retimm · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Returns the immediate constant 67 in r3.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetConst173_82996258`

- **Address:** `0x82996258` · **Size:** 2 insns · **Category:** retimm · **Region:** game · **Confidence:** **high**

- **Does:** [game code] Returns the immediate constant 173 in r3.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82B84350`

- **Address:** `0x82B84350` · **Size:** 2 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Does:** [game code] Small trampoline (2 insns) that sets up a value and tail-calls 0x821FC1F0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821FC1F0`

### `TailCall_82BE9668`

- **Address:** `0x82BE9668` · **Size:** 6 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Does:** [game code] Small trampoline (6 insns) that sets up a value and tail-calls 0x82BE9680.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82BE9680`

### `TailCall_82BEA25C`

- **Address:** `0x82BEA25C` · **Size:** 5 insns · **Category:** tailcall · **Region:** game · **Confidence:** medium

- **Does:** [game code] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C1C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82BEA3D0`

### `TailCall_82C000F8`

- **Address:** `0x82C000F8` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C106A8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C106A8`

### `TailCall_82C00108`

- **Address:** `0x82C00108` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C0B660.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C0B660`

### `TailCall_82C00110`

- **Address:** `0x82C00110` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C0B678.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C0B678`

### `TailCall_82C00118`

- **Address:** `0x82C00118` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C0B680.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C0B680`

### `TailCall_82C00120`

- **Address:** `0x82C00120` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C0B688.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C0B688`

### `TailCall_82C00128`

- **Address:** `0x82C00128` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C0B690.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C0B690`

### `TailCall_82C0B680`

- **Address:** `0x82C0B680` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C49670.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C49670`

### `TailCall_82C0B688`

- **Address:** `0x82C0B688` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82C4BDD8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C4BDD8`

### `TailCall_82C14D58`

- **Address:** `0x82C14D58` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82C1DBA8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C1DBA8`

### `TailCall_82C5CFBC`

- **Address:** `0x82C5CFBC` · **Size:** 1 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (1 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82C867E8`

- **Address:** `0x82C867E8` · **Size:** 3 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (3 insns) that sets up a value and tail-calls 0x82C863E8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C863E8`

### `TailCall_82CA36C4`

- **Address:** `0x82CA36C4` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C24.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA36DC`

### `TailCall_82CA4DFC`

- **Address:** `0x82CA4DFC` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA4DD0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4E30`

### `TailCall_82CA5064`

- **Address:** `0x82CA5064` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA4FC0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA50F4`

### `TailCall_82CA53D0`

- **Address:** `0x82CA53D0` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5408`

### `TailCall_82CA7300`

- **Address:** `0x82CA7300` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C30.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA7318`

### `TailCall_82CA7C90`

- **Address:** `0x82CA7C90` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C3C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CA7C9C`

- **Address:** `0x82CA7C9C` · **Size:** 3 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (3 insns) that sets up a value and tail-calls 0x82CA2C3C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CA8060`

- **Address:** `0x82CA8060` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CA806C`

- **Address:** `0x82CA806C` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CA8154`

- **Address:** `0x82CA8154` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA81A0`

### `TailCall_82CA827C`

- **Address:** `0x82CA827C` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C30.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA82C0`

### `TailCall_82CA91E8`

- **Address:** `0x82CA91E8` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C24.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA9220`

### `TailCall_82CAB2D4`

- **Address:** `0x82CAB2D4` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CAB2A4.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB308`

### `TailCall_82CAF40C`

- **Address:** `0x82CAF40C` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAF424`

### `TailCall_82CAF658`

- **Address:** `0x82CAF658` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAF690`

### `TailCall_82CAFAFC`

- **Address:** `0x82CAFAFC` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAFB34`

### `TailCall_82CAFC88`

- **Address:** `0x82CAFC88` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAFCC0`

### `TailCall_82CAFEE4`

- **Address:** `0x82CAFEE4` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAFF18`

### `TailCall_82CB0184`

- **Address:** `0x82CB0184` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB01BC`

### `TailCall_82CB49B0`

- **Address:** `0x82CB49B0` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CB4934.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CB4EAC`

- **Address:** `0x82CB4EAC` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `TailCall_82CB4FDC`

- **Address:** `0x82CB4FDC` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CB4FEC`

- **Address:** `0x82CB4FEC` · **Size:** 3 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (3 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `TailCall_82CB5918`

- **Address:** `0x82CB5918` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C30.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB5930`

### `TailCall_82CB68F4`

- **Address:** `0x82CB68F4` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB692C`

### `TailCall_82CB6C90`

- **Address:** `0x82CB6C90` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C28.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB6CC8`

### `TailCall_82CB9018`

- **Address:** `0x82CB9018` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C28.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB9030`

### `TailCall_82CBA370`

- **Address:** `0x82CBA370` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C3C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBA3A8`

### `TailCall_82CBA8D0`

- **Address:** `0x82CBA8D0` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C3C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBA8E4`

### `TailCall_82CC0494`

- **Address:** `0x82CC0494` · **Size:** 4 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (4 insns) that sets up a value and tail-calls 0x82CC04B0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CC04B0`

### `TailCall_82CC04B0`

- **Address:** `0x82CC04B0` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C14.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CC04E8`

### `TailCall_82CD1690`

- **Address:** `0x82CD1690` · **Size:** 1 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (1 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82CE1510`

- **Address:** `0x82CE1510` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE0C68.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE0C68`

### `TailCall_82CE1520`

- **Address:** `0x82CE1520` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE11E0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE11E0`

### `TailCall_82CE1548`

- **Address:** `0x82CE1548` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE0C20.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE0C20`

### `TailCall_82CE3750`

- **Address:** `0x82CE3750` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3400.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3400`

### `TailCall_82CE3760`

- **Address:** `0x82CE3760` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3220.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3220`

### `TailCall_82CE3768`

- **Address:** `0x82CE3768` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3238.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3238`

### `TailCall_82CE3770`

- **Address:** `0x82CE3770` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3460.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3460`

### `TailCall_82CE3798`

- **Address:** `0x82CE3798` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE0BE8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE0BE8`

### `TailCall_82CE37A0`

- **Address:** `0x82CE37A0` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3228.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3228`

### `TailCall_82CE37A8`

- **Address:** `0x82CE37A8` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82CE3640.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CE3640`

### `TailCall_82E7E4F8`

- **Address:** `0x82E7E4F8` · **Size:** 4 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (4 insns) that sets up a value and tail-calls 0x82E7E388.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82E7E388`

### `RetAddr_82E8F9E8`

- **Address:** `0x82E8F9E8` · **Size:** 3 insns · **Category:** const · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Const thunk: builds and returns the data address 0x83345BAC.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetAddr_82EB7ED0`

- **Address:** `0x82EB7ED0` · **Size:** 3 insns · **Category:** const · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Const thunk: builds and returns the data address 0x8334CD2C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetAddr_82EC1918`

- **Address:** `0x82EC1918` · **Size:** 3 insns · **Category:** const · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Const thunk: builds and returns the data address 0x8334E1C0.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetAddr_82EC4E58`

- **Address:** `0x82EC4E58` · **Size:** 3 insns · **Category:** const · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Const thunk: builds and returns the data address 0x8334E94C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `RetConst0_82F04430`

- **Address:** `0x82F04430` · **Size:** 2 insns · **Category:** retimm · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Returns the immediate constant 0 in r3.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82F0BAF8`

- **Address:** `0x82F0BAF8` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82F10110.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82F10110`

### `TailCall_82F0BB08`

- **Address:** `0x82F0BB08` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82F10110.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82F10110`

### `RetConst1_82F279D8`

- **Address:** `0x82F279D8` · **Size:** 2 insns · **Category:** retimm · **Region:** SDK · **Confidence:** **high**

- **Does:** [SDK runtime] Returns the immediate constant 1 in r3.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_82F38DA0`

- **Address:** `0x82F38DA0` · **Size:** 4 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (4 insns) that sets up a value and tail-calls 0x82F328D8.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82F328D8`

### `TailCall_82F56690`

- **Address:** `0x82F56690` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82F57290.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82F57290`

### `TailCall_82FE7EF8`

- **Address:** `0x82FE7EF8` · **Size:** 2 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (2 insns) that sets up a value and tail-calls 0x82FEC260.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82FEC260`

### `TailCall_82FE7F00`

- **Address:** `0x82FE7F00` · **Size:** 3 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (3 insns) that sets up a value and tail-calls 0x82FEC558.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82FEC558`

### `TailCall_83001184`

- **Address:** `0x83001184` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C24.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830011BC`

### `TailCall_830013A8`

- **Address:** `0x830013A8` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C30.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830013C0`

### `TailCall_8300164C`

- **Address:** `0x8300164C` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C34.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_83001684`

### `TailCall_830017A0`

- **Address:** `0x830017A0` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830017D8`

### `TailCall_83002F18`

- **Address:** `0x83002F18` · **Size:** 5 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (5 insns) that sets up a value and tail-calls 0x82CA2C3C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_83002F2C`

### `TailCall_83003414`

- **Address:** `0x83003414` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x8300339C.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_83003494`

### `TailCall_8300342C`

- **Address:** `0x8300342C` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_83003444`

### `IndirectCall_83095890`

- **Address:** `0x83095890` · **Size:** 68 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `TailCall_830F0980`

- **Address:** `0x830F0980` · **Size:** 6 insns · **Category:** tailcall · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Small trampoline (6 insns) that sets up a value and tail-calls 0x82CA2C38.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830F09B8`

### `IndirectCall_8310FC68`

- **Address:** `0x8310FC68` · **Size:** 32 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831B31B8`

- **Address:** `0x831B31B8` · **Size:** 30 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831FBA60`

- **Address:** `0x831FBA60` · **Size:** 26 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831FBAC8`

- **Address:** `0x831FBAC8` · **Size:** 25 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831FBB30`

- **Address:** `0x831FBB30` · **Size:** 23 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831FBB90`

- **Address:** `0x831FBB90` · **Size:** 102 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `IndirectCall_831FBD28`

- **Address:** `0x831FBD28` · **Size:** 91 insns · **Category:** indirect · **Region:** SDK · **Confidence:** medium

- **Does:** [SDK runtime] Indirect-call thunk: computes a target and jumps via mtctr/bctr.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `CheckPrefixedWord_ReturnZeroOrComplement_832B3D78`

- **Address:** `0x832B3D78` · **Size:** 6 insns · **Category:** leaf · **Region:** game (end-of-.text gap) · **Confidence:** low (unreachable)

- **Role:** tag/checksum-style verifier over the 32-bit word stored immediately before a buffer — dead in the current build

- **Does:** `v = *(r3 - 4)`; if `v == r4` returns 0, else returns `~v` (0xFFFFFFFF - v, a non-zero token encoding the actual value). 6-instruction leaf, no calls. **Zero call sites** in the generated code: it sits in the gap region 0x832B3D6C-0x832B3DE0 and was registered by the gap walk (its original branch source was re-segmented away in a later codegen run), so it is unreachable in this build. Neighbors in the region are object methods that lock a critical section via sub_832B3700 before touching fields of the object at `r3 - 852`, which is consistent with a small verify-helper for the same object family, but the purpose is not recoverable without the lost caller — the name describes the mechanism only.

- **Trigger:** Unreachable (no references in the recompiled image).

### `GetElapsedMsSinceStart`

- **Address:** `0x832B9398` · **Size:** ~24 insns · **Category:** getter · **Region:** game (end-of-.text) · **Confidence:** **high**

- **Role:** elapsed milliseconds since first call, with tick-wrap/reset protection

- **Does:** `X = GetCurrentTimeMs()`; base `A` = global 0x81FFBDFC (captured from X on the first call, when A == 0); `delta = X - A`. A second global `B` = 0x81FFBE00 caches the last returned delta: if `(delta - B) > 0xC0000000` unsigned (~12.4 days of ms), the underlying tick counter has wrapped or reset, so the update is skipped and the old B is returned. Otherwise B = delta and delta is returned.

- **Trigger:** Internal — 24 call sites, all in the 0x832B end-of-.text region, all passing the result into tick/update methods (e.g. sub_832B5530 → sub_832B50C0(obj, now)).

- **Calls:** `GetCurrentTimeMs`

### `ConstTrue_Predicate_82C43198`

- **Address:** `0x82C43198` · **Size:** 2 insns · **Category:** leaf (constant stub) · **Region:** game · **Confidence:** high (mechanism), low (purpose)

- **Role:** constant-true predicate / stub — always returns 1

- **Does:** `li r3,1; blr` — ignores every argument and returns 1. 6 callers (11 raw call sites), all treating the result as a boolean gate: `sub_83205F58` sets up `(r3=*(obj+16), r4, r6, r7)` then `cmpwi r3,1; bne` to choose between `(byte & 0x1F)` and a fallback (so it always takes the `& 0x1F` path); four 0x8304-region callers (sub_83040418, Func_83040FEC, Func_8304100C, Func_8304101C) share an identical `lwz r3,192(r31); li r5,0; li r6,0; bl` shape; `sub_8301D688` gates on a byte == 118 ('v') before the call.

- **Trigger:** Internal — the 6 gate/dispatch call sites above.

- **Note:** the call shape (this + args, result used as yes/no) suggests a query method the compiler folded to a constant (a feature always on, an "accept all" policy, or a debug gate compiled out). The original semantic is not recoverable without the lost vtable/branch source, so the name describes the mechanism only.

### `MemCpy_Vectorized_Thunk_82B93038`

- **Address:** `0x82B93038` · **Size:** 1 insn · **Category:** thunk (tail-branch) · **Region:** game · **Confidence:** **high**

- **Role:** plain (unswapped) alias to the vectorized memcpy kernel

- **Does:** `b 0x822085d0` — a single tail-branch to `MemCpy_Vectorized_822085D0`, passing `(dst=r3, src=r4, len=r5)` through unchanged. Single caller: `sub_830FB4E8`, which sets up `(r3=r23=dst, r4=r31=src, r5=*(r25)=len)` — a straightforward forward copy. Its 8-byte neighbor `sub_82B93040` is the **swap-direction sibling** (swaps r3/r4 then tail-calls the same kernel, single caller `sub_83045C00`), so the pair offers both argument orders of the same SIMD copy kernel.

- **Trigger:** Internal — the single caller `sub_830FB4E8`.

- **Calls:** `MemCpy_Vectorized_822085D0`

### `NoOp_Stub_829CE870`

- **Address:** `0x829CE870` · **Size:** 1 insn · **Category:** leaf (no-op stub) · **Region:** game · **Confidence:** high (mechanism), low (purpose)

- **Role:** pure no-op stub — a single `blr`, does nothing

- **Does:** `blr` — returns immediately, returning whatever was in r3. One of the hottest functions in the image with **396 call sites**. The dominant caller shape is "call a helper (`sub_82317E50` / `sub_82317F98` / …) then `addi r3,r1,N; bl` it" — invoked on a stack slot (often **twice**, at two different offsets) right after constructing/using an object; a second shape is `mr r3,obj; bl` it (on an object, then another op follows).

- **Trigger:** Internal — the 396 call sites above.

- **Note:** a no-op called this uniformly on objects/stack slots is the signature of an **empty virtual method** — most likely a no-op **destructor / release / delete** for an object type whose teardown does nothing in this build (the call is still emitted for ABI/dispatch correctness). The exact interface slot isn't resolvable without the vtables, so the name states the observable fact: it is a no-op.

### `GetPtr_Field32_824C23D0`

- **Address:** `0x824C23D0` · **Size:** 2 insns · **Category:** leaf (getter) · **Region:** game · **Confidence:** medium (mechanism), low (field identity)

- **Role:** returns the pointer field at object offset +32

- **Does:** `lwz r3, 32(r3); blr` — loads and returns the u32/pointer at offset +32 of the object in r3. 6 callers (all in the 0x830F–0x831B region); the result is passed as the first arg to various functions, and one caller null-checks it (`cmplwi r3,0`), so offset 32 holds a **NULL-able pointer** (a sub-object / back-reference). The owning class isn't resolved, so the name states the mechanism (get the pointer field at offset 32) + address suffix.

- **Trigger:** Internal — the 6 getter call sites above.


## J. Unnamed Body Functions

_Functions with real code but no resolved import or strong signature (many are mid-function pointer targets). Named by address; description states only what the assembly shows. Lowest confidence._

**207 functions.**

### `RemoveAndFreeNode_826A4978`

- **Address:** `0x826A4978` · **Category:** body (tree op) · **Region:** game (0x826A) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a remove-and-free tree-node wrapper -- remove obj->4 from the tree, free it, clear the slots — 20 callers

- **Does:** arg: obj (r3, obj->4 = the node, obj->8 = a related field). `Tree_RemoveNode_826A4A20(obj, obj->4, &out1, &out2)` to detach the node, then `Free_SizeBucketed_Thunk_8221BE68(obj->4)`, then obj->4 = obj->8 = 0.

- **Calls:** `Tree_RemoveNode_826A4A20`, `Free_SizeBucketed_Thunk_8221BE68`

### `Tree_FindNode_82A120B0`

- **Address:** `0x82A120B0` · **Category:** body (tree op) · **Region:** game (0x82A1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a tree find-and-process -- walk the tree to locate the node / insertion point for a key, then process it -- a sibling of `Tree_Insert_82A00620` — 1 caller

- **Does:** args: obj (r3, ->4 = first node), key wrapper (r4, *r4 = the key). Nodes: ->8 right, ->0 parent, ->12 key, ->25 state byte. Walks (right if node->12 < key, else up) to the target, then processes it (sub_82A122F8 + Release_RefCounted).

- **Calls:** `ProcessTreeNode_82A122F8`, `Release_RefCounted_829FF648`

### `ProcessTreeNode_82A122F8`

- **Address:** `0x82A122F8` · **Category:** body (tree processor) · **Region:** game (0x82A1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a large (364-line) tree-node processor -- the slow-path handler called by `Tree_FindNode_82A120B0`, with a fast path (sub_82A128B0) and a validated slow path — 1 caller

- **Does:** args: (r3), obj (r4, ->4 and ->8 fields), plus r5, r6. If obj->8 == 0 (fast): `ProcessNodeString_82A128B0(obj->4, r6, 1)`. Else (slow): validate (`trap 22`) and do the heavier processing (sub_823D4F20, sub_82498700, sub_82A126D0, sub_82A128B0).

- **Calls:** `ProcessNodeString_82A128B0`, `sub_823D4F20`, `sub_82498700`, `sub_82A126D0`

### `ProcessNodeString_82A128B0`

- **Address:** `0x82A128B0` · **Category:** body (string processor) · **Region:** game (0x82A1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a large (329-line) node string-processing handler in the 0x82A1 tree cluster -- the fast-path / deep handler behind `ProcessTreeNode_82A122F8` — 2 callers

- **Does:** args: (r3), obj (r4, ->8 = size/length), plus r5, r6, r7. Compares obj->8 to a constant (0x01555554); if below, one path; else build a local string via `SetString_822F2020`, convert via `sub_826C3EF0`, then `sub_822F1F00` / `sub_826C3FA8`, and (longer path) sub_82171810 / sub_827C6448 / sub_827C64C0 / `AllocateNode28_82A12B90`.

- **Calls:** `SetString_822F2020`, `AllocateNode28_82A12B90`, `sub_826C3EF0`, `sub_822F1F00`

### `AllocateNode28_82A12B90`

- **Address:** `0x82A12B90` · **Category:** body (alloc) · **Region:** game (0x82A1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 28-byte node allocation + initialization -- the allocation step called by `ProcessNodeString_82A128B0` — 1 caller

- **Does:** args: (r3), a (r4), b (r5), c (r6), src (r7). `Allocate_OrDbgBreak_8221F388(28)`; on success init block->0 = a, block->4 = b, block->8 = c, block->12/16/20 = src->0/4/8. If src->8 (a linked field) is nonzero, acquire the global lock and do an atomic (lwarx) splice. Returns the block (or null).

- **Calls:** `Allocate_OrDbgBreak_8221F388`

### `Construct20Byte_83215640`

- **Address:** `0x83215640` · **Category:** body (constructor) · **Region:** game (0x8321) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 20-byte object constructor -- set the vtable (obj->0) and allocate + zero a 20-byte payload — 21 callers

- **Does:** arg: obj (r3). Sets obj->0 to a fixed global (~0x82C3BBC0, plausibly a vtable). Allocates 20 bytes via `Allocate_OrDbgBreak_8221F388(20)` and zeroes the block. 21 callers (the 0x83215 constructor cluster).

- **Calls:** `Allocate_OrDbgBreak_8221F388`

### `Destruct20Byte_832156E0`

- **Address:** `0x832156E0` · **Category:** body (destructor) · **Region:** game (0x8321) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** the DESTRUCTOR counterpart of `Construct20Byte_83215640` (same vtable ~0x82C3BBC0): destroy the tree and free the node — 31 callers

- **Does:** arg: obj (r3). Sets obj->0 to the vtable. `DestroyTreeNodes_82859340(obj->8, obj->8->4)` (recursively destroy the tree rooted at obj->8), cleanup (obj->12 = 0 + self-reference rewiring), then `RemoveAndFreeNode_826A4978(obj+4)`.

- **Calls:** `DestroyTreeNodes_82859340`, `RemoveAndFreeNode_826A4978`

### `UpdateTreeNodeValue_83218598`

- **Address:** `0x83218598` · **Category:** body (refcounted setter) · **Region:** game (0x8321) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a refcounted value setter that updates a tree node's value -- release the old value, store the new one, increment the new value's refcount — 3 callers

- **Does:** args: obj (r3, tree at obj+12), value wrapper (r4, ->0 = source, ->4 = new value). Vcall on *r4->0 (vtable slot 1); find the node via `Tree_FindNode_82A120B0(obj+12, ...)`; if node->4 differs from r4->4: `Release_RefCounted_829FF648`, set node->0/->4, and (if new value non-null) value->0 += 1.

- **Calls:** `Tree_FindNode_82A120B0`, `Release_RefCounted_829FF648`

### `UpdateBufferRange_822F1F60`

- **Address:** `0x822F1F60` · **Category:** body (buffer update) · **Region:** game (0x822F) · **Confidence:** medium (mechanism), low (exact semantics)

- **Role:** the in-place buffer-range update core for the small-buffer object (0x822F) -- grow if needed, clamp, shift, resize, null-terminate — 3 callers

- **Does:** args: obj (r3, obj->4 = buffer, obj->20 = length, obj->24 = size field), pos (r4), len (r5). Grow via sub_82CD12C8 if obj->20 < pos; clamp len; resolve the buffer base (small-buffer indirection); `CheckedCopy_82CA3808` to shift; obj->20 -= len; null-terminate. Consistent with an in-place range erase/truncate. The in-place core behind WriteRange_AtPos / RangeUpdate.

- **Calls:** `CheckedCopy_82CA3808`, `sub_82CD12C8`

### `Buffer_PushByte_82CAE378`

- **Address:** `0x82CAE378` · **Category:** leaf (buffer append) · **Region:** SDK (0x82CA) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a byte-append to a growable buffer -- decrement remaining capacity, write the byte, advance — 3 callers

- **Does:** args: byte (r3), buffer obj (r4, ->0 = write pointer, ->4 = remaining capacity, ->8 = secondary pointer, ->12 = flags), plus r5. Checks a flag bit (bit 5 of obj->12); decrements obj->4; if capacity remains, writes the byte to *obj->0 and advances (obj->0 += 1).

- **Calls:** none

### `ConstructFromArray_82B4BFD8`

- **Address:** `0x82B4BFD8` · **Category:** body (constructor) · **Region:** game (0x82B4) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a build-object-from-array -- derive a size from an array range, construct the object, and (if non-empty) copy the array data in — 5 callers

- **Does:** args: obj (r3), arr (r4, arr->4 = start, arr->8 = end). count = (arr->8 - arr->4) >> 2; `Construct_FromValue_82B4CB58(obj, count)`; if the result byte is nonzero, validate (arr->4 <= arr->8, `trap 22`) and copy via `CheckedCopy_82CA3808`.

- **Calls:** `Construct_FromValue_82B4CB58`, `CheckedCopy_82CA3808`

### `Array_Push28_82C644D0`

- **Address:** `0x82C644D0` · **Category:** body (dynamic array) · **Region:** game (0x82C6) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 28-byte-stride array push (sibling of `Array_Push12_82C66E00`) -- if room, init a new 28-byte element and advance — 2 callers

- **Does:** arg: obj (r3, array: obj->4 = start, obj->8 = current, obj->12 = end, 28-byte stride). count = (obj->8 - obj->4)/28, capacity = (obj->12 - obj->4)/28; if no room, return. Else init a new 28-byte element at obj->8 (zero fields, set constants, via `RangeUpdate_8218EA38` + sub_82C643F0) and advance obj->8 by 28.

- **Calls:** `RangeUpdate_8218EA38`, `sub_82C643F0`

### `Tree_FindNode2_82A12230`

- **Address:** `0x82A12230` · **Category:** body (tree op) · **Region:** game (0x82A1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a self-contained tree find (12 callers, no external callees -- all inline) with the same node layout/walk as `Tree_FindNode_82A120B0` and `Tree_Insert_82A00620` — 12 callers

- **Does:** args: (r3), obj (r4, ->4 = first node), key wrapper (r5, *r5 = the key). Walks the tree (right if node->12 < key, else up to parent) to the target, then processes it entirely inline (no external sub-calls). A sibling of `Tree_FindNode_82A120B0`.

- **Calls:** none

### `VcallSlot0_Flag1_829681C0`

- **Address:** `0x829681C0` · **Category:** body (vcall wrapper) · **Region:** game (0x8296) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable-slot-0 call with a null check and a flag of 1 -- the classic virtual destructor-call shape -- 0 direct callers (a virtual method)

- **Does:** arg: obj (r3). If null, return. Else load *(obj->0) = the vtable and call vtable[0] (slot 0) with obj and r4 = 1 (bctr). Returns the callee's result. Slot 0 is typically the destructor (Itanium ABI); r4=1 is plausibly a "deleting" flag.

- **Calls:** (virtual) obj vtable slot 0

### `SetRefcountedWrapper_8238A848`

- **Address:** `0x8238A848` · **Category:** body (refcounted setter) · **Region:** game (0x8238) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a refcounted value setter that also builds an owning 12-byte wrapper -- release current obj->0, store the new value, and (if non-null) allocate a refcounted wrapper — 171 callers

- **Does:** args: obj (r3, obj->0 = value, obj->4 = wrapper slot), value (r4). Releases the current obj->0 via `Release_RefCounted_829FF648`, sets obj->0 = value. If value != null, allocate 12 bytes via `Allocate_OrDbgBreak_8221F388` and init a wrapper (wrapper->0 = 1 refcount, wrapper->4 = a fixed constant, wrapper->8 = value), then obj->4 = wrapper (or 0 on failure).

- **Calls:** `Release_RefCounted_829FF648`, `Allocate_OrDbgBreak_8221F388`

### `VcallSlot3_ReturnBit_82372268`

- **Address:** `0x82372268` · **Category:** body (vcall wrapper) · **Region:** game (0x8237) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable-slot-3 call on the sub-object obj->0 (offset 12) that passes a value pointer and returns a BIT derived from the result -- a slot-3 family member — 8 callers

- **Does:** args: obj (r3, ->0 = sub-object, ->5 = flag byte), value (r4). A bit test on obj->5 selects which stack slot holds the value pointer (a 2-element struct {value, 0}). Vcall `(*obj->0)->vtable[3](obj->0, &{value, 0}, 1)`; returns a bit from the result (clz, rotate-right-27, mask 1).

- **Calls:** (virtual) obj->0 vtable slot 3

### `GetOrCreateByNameHash_8236E1B0`

- **Address:** `0x8236E1B0` · **Category:** body (name lookup) · **Region:** game (0x8236) · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** a get-or-create-by-name -- hash a name, binary-search a 12-byte-stride array for it, and (if not found) insert a new element — 2 callers

- **Does:** args: obj (r3, array at +4, 12-byte stride), name (r4). Hashes the name via `InitNameHash_8236D358`, validates the array ranges (`trap 22`), binary-searches comparing element->0 (the stored name hash) to the computed hash; on a miss inserts via `Array_InsertOne_8236FDD8`.

- **Calls:** `InitNameHash_8236D358`, `Array_InsertOne_8236FDD8`

### `GetGlobal_82C1E350`

- **Address:** `0x82C1E350` · **Category:** leaf (global getter) · **Region:** game (0x82C1) · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** a constant-return global getter -- returns a fixed global address (~0x83654CE0) -- 0 direct callers (a virtual getter for a global/singleton)

- **Does:** takes no meaningful arguments and always returns the same global pointer (lis/addi load + blr). The exact global's meaning needs the vtable + class.

- **Calls:** none

### `Compare3Way_Signed32_82C13458`

- **Address:** `0x82C13458` · **Category:** leaf (comparator) · **Region:** game (0x82C1) · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** a SIGNED 3-way comparator of two 32-bit values: returns -1 if a < b, 0 if a == b, 1 if a > b (a = *r4, b = *r5) — 0 direct callers (a virtual comparison method)

- **Does:** args: (r3), a wrapper (r4, a = *r4), b wrapper (r5, b = *r5). Signed compare: if a < b return -1; else return the LSB of the borrow of (b - a) via subfc/subfe -- 0 when a == b, 1 when a > b. The classic sign-of-(a-b) comparator.

- **Calls:** none

### `VcallSlot12_82C010D0`

- **Address:** `0x82C010D0` · **Category:** body (vcall wrapper) · **Region:** game (0x82C0) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable-slot-12 call wrapper -- deref obj, load vtable offset 48 (slot 12), call it with obj, return the result — 1 caller

- **Does:** arg: obj (r3, ->0 = vtable). Loads vtable[48] (slot 12, 48/4) and calls it with obj (bctr). Returns the callee's result. Caller: sub_82C14E08.

- **Calls:** (virtual) obj vtable slot 12

### `VcallChain_6_12_7_82C14E08`

- **Address:** `0x82C14E08` · **Category:** body (vcall chain) · **Region:** game (0x82C1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a 3-step vcall chain (slots 6 -> 12 -> 7), each gated on the previous result's low byte being nonzero -- a sibling of `VcallTriple_AllSuccess` (6,8,7) and `VcallChain_AllSuccess` (6,14,7) — 2 callers

- **Does:** args: obj (r3, ->0 = vtable), (r4), value (r5). Step 1: vcall vtable[6]; if result byte == 0 return 0. Step 2: `VcallSlot12_82C010D0(obj, value)`; if result byte == 0 return 0. Step 3: vcall vtable[7]; return (result byte != 0). All-or-nothing.

- **Calls:** (virtual) vtable slots 6, 7, + `VcallSlot12_82C010D0`

### `LoadString_FastOrSlow_823725E8`

- **Address:** `0x823725E8` · **Category:** body (string load) · **Region:** game (0x8237) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a string/buffer load into obj with a conditional FAST and SLOW path, selected by `control->20` — 6 callers

- **Does:** args: obj (r3, destination small-buffer object), control (r4, ->20 selects path), source (r5). If control->20 == 0 (fast): `LoadFrom_Source_822F2398(obj, source)`. Else (slow): resolve the source buffer (small-buffer indirection) and copy through `ClampAndUpdate_822F6380` + sub_8217AA20 / sub_8217AB30 / sub_822F5208 / sub_822F6CB0, freeing temporaries via `Free_SizeBucketed_Thunk_8221BE68`.

- **Calls:** `LoadFrom_Source_822F2398`, `ClampAndUpdate_822F6380`, `Free_SizeBucketed_Thunk_8221BE68`

### `BinarySearch_12Stride_Simple_82B429A0`

- **Address:** `0x82B429A0` · **Category:** body (search) · **Region:** game (0x82B4) · **Confidence:** **high** (mechanism)

- **Role:** a simple 12-byte-stride binary search (comparing element->0 to a key over [start, end)) — a simpler sibling of `BinarySearch_12Stride_8236F0F0` — 2 callers

- **Does:** args: out (r3), start (r4), end (r5), key wrapper (r6). count = (end-start)/12; binary-search: element = start + mid*12; if element->0 >= key move left, else right; store the result into *out.

- **Calls:** none

### `Identity_ReturnArg2_82C2CE58`

- **Address:** `0x82C2CE58` · **Category:** leaf (identity) · **Region:** game (0x82C2) · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** an identity/passthrough — returns its 2nd argument (r4) unchanged (mr r3,r4; blr) — 3 callers (a virtual method's default impl)

- **Does:** returns r4 unchanged. Callers pass (constant, value) and get the value back -- the classic default/identity implementation of a virtual method.

- **Calls:** none

### `ProcessElements_Vcall4_82C46C10`

- **Address:** `0x82C46C10` · **Category:** body (vcall loop) · **Region:** game (0x82C4) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a process-elements loop -- walk a linked structure and, per element, dispatch 4 vmethods (slots 0-3) on a target object — 24 callers

- **Does:** args: obj (r3, ->0 = vtable, ->4 = end marker), target (r4, ->0 = vtable), plus two values (r5, r6). Loop while element != obj->4: call target->vtable[0](target, elem->16), [1](target, r5), [2](target, prev), [3](target, r6), then advance the element (elem->0 or elem->4 by the results). Return 0.

- **Calls:** (virtual) target vtable slots 0-3

### `Construct_FromValue_82B4CB58`

- **Address:** `0x82B4CB58` · **Category:** body (constructor) · **Region:** game (0x82B4) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a constructor/initializer for a 12-byte (three u32) object from a value, with a small/large distinction (tagged-value init) — 3 callers

- **Does:** args: obj (r3, zero-inits obj->4/8/12), value (r4). If value == 0, return 0. Else if value <= 0x3FFFFFFF (30-bit inline): small-value init. Else (large/pointer): `SetString_822F2020` (from a global) + `sub_826C3EF0` conversion. Returns a value callers mask to a byte.

- **Calls:** `SetString_822F2020`, `sub_826C3EF0`

### `Lookup_ByVirtualCmp_82C17350`

- **Address:** `0x82C17350` · **Category:** body (lookup) · **Region:** game (0x82C1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a lookup-by-key that binary-searches an array (wrapper->4) with a virtual comparison, returning the found element's field 4 — the caller of `BinarySearch_VirtualCmp_82C15850` — 0 direct callers (virtual/dead)

- **Does:** args: wrapper (r3, array at +4), key (r4). `BinarySearch_VirtualCmp_82C15850(wrapper->4, key, &found)`; if found and the element is non-null, return element->4, else 0.

- **Calls:** `BinarySearch_VirtualCmp_82C15850`

### `Tree_Insert_82A00620`

- **Address:** `0x82A00620` · **Category:** body (tree op) · **Region:** game (0x82A0) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a tree insertion — walk to find the insertion point for a key, then insert (with a fixup call) — 7 callers

- **Does:** args: root (r3, first node at ->4), key wrapper (r4). Nodes carry ->8 (right), ->0 (parent), ->12 (key), ->21 (state byte). Walks (up to parent if node->12 >= key, else right) to the insertion point, splices a node in, then calls sub_82419280 (fixup/rebalance). 7 callers.

- **Calls:** `sub_82419280`

### `Stub_Return4_826A0520`

- **Address:** `0x826A0520` · **Category:** leaf (const stub) · **Region:** game (0x826A) · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** a constant-return stub — returns the integer 4 (li r3,4; blr) — 0 direct callers (a virtual property/type-code method)

- **Does:** returns the constant 4. Likely a virtual method whose value is 4 (a size, count, or type code); needs the vtable + class to confirm.

- **Calls:** none

### `BinarySearch_12Stride_8236F0F0`

- **Address:** `0x8236F0F0` · **Category:** body (search) · **Region:** game (0x8236) · **Confidence:** **high** (mechanism)

- **Role:** a binary search (lower_bound) over a 12-byte-stride array, comparing each element's field 0 to a key — a sibling of `BinarySearch_82C0BC08` (4-byte) and `BinarySearch_VirtualCmp_82C15850` — 2 callers

- **Does:** args: obj (r3, array: obj->4 = start, obj->8 = current, obj->12 = end, 12-byte stride), key wrapper (r4). Validates ranges (`trap 22`), then binary-searches: element = base + mid*12; if element->0 < key move right, else left; returns the insertion point / match.

- **Calls:** none

### `DestroyList_Recursive_82443440`

- **Address:** `0x82443440` · **Category:** body (recursive destroy) · **Region:** game (0x8244) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a recursive linked-list destroy — release each node's ->0 sub-object and free the node — 6 callers

- **Does:** args: obj (r3), node (r4; node->8 = next, node->0 = refcounted payload, node+16 = refcount slot, node->25 = flag byte). Recurses on node->8, then for the current node releases node->0 via `Release_RefCounted_829FF648` (at node+16) and frees the node via `Free_SizeBucketed_Thunk_8221BE68`, advancing via node->0 while the node->25 flag is 0.

- **Calls:** `Release_RefCounted_829FF648`, `Free_SizeBucketed_Thunk_8221BE68`

### `VcallSlot3_LoopOrSingle_82C66330`

- **Address:** `0x82C66330` · **Category:** body (vcall loop) · **Region:** game (0x82C6) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable-slot-3 caller with a loop/single mode, selected by a flag bit in obj->5 — 1 caller

- **Does:** args: obj (r3), value (r4), count (r5). A bit test on obj->5 selects the mode. MODE A (loop): loop `count` times, each vcall `(*obj->0)->vtable[3](obj->0, &{value++, 0}, 1)`, then return 1. MODE B (single): one vcall with count as an arg, returning a derived bit. The slot-3 method isn't resolvable without the vtable.

- **Calls:** (virtual) *obj->0 vtable slot 3

### `ProcessObject_82C669C0`

- **Address:** `0x82C669C0` · **Category:** body (stateful processor) · **Region:** game (0x82C6) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a stateful object processor that orchestrates a gated slot-3 store, a buffer update, and a slot-3 loop/single call, returning a byte flag — 2 callers

- **Does:** args: (r3), obj a (r4), obj b (r5). If obj a->4 != 0: `SubObject_VcallSlot3_GatedStore32_82C842C8(obj a, &out)`, store result into obj a->4. Update a buffer via sub_822F1F60 (if out-1 > obj b->20) or sub_826C2FF8 (else). Resolve obj b's buffer base (small-buffer indirection), then `VcallSlot3_LoopOrSingle_82C66330(obj a, base, out)`. Return the byte obj a->4.

- **Calls:** `SubObject_VcallSlot3_GatedStore32_82C842C8`, `sub_822F1F60`, `sub_826C2FF8`, `VcallSlot3_LoopOrSingle_82C66330`

### `Array_Push12_82C66E00`

- **Address:** `0x82C66E00` · **Category:** body (dynamic array) · **Region:** game (0x82C6) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a push of a 12-byte element into a 12-byte-stride array (with growth) — a sibling of `Array_PushElement_823758C0` (36-byte) — 1 caller

- **Does:** args: obj (r3, array: obj->4 = start, obj->8 = current, obj->12 = end, 12-byte stride), src (r4, a 12-byte struct). If room (current element count < total): copy the 12 bytes from src into the slot at obj->8, then advance obj->8 by 12. Else (full): validate (obj->4 <= obj->8, else `trap 22`) and grow via sub_82C66BE0, then push.

- **Calls:** `sub_82C66BE0` (grow)

### `BinarySearch_VirtualCmp_82C15850`

- **Address:** `0x82C15850` · **Category:** body (search) · **Region:** game (0x82C1) · **Confidence:** **high** (mechanism)

- **Role:** a binary search with a VIRTUAL (polymorphic) comparison — a sibling of `BinarySearch_82C0BC08` — 1 caller

- **Does:** args: obj (r3, array: obj->0 = base, obj->8 = count), target (r4), out flag (r5). Sets *out = 0. Binary-search: mid=(low+high)/2, element=base[mid]; call `(element->4)->vtable[1]` (a virtual compare) to compare element to target; narrow low/high on the result. Returns the index/insertion point and sets *out on a match.

- **Calls:** (virtual) (element->4) vtable slot 1

### `Tree_RemoveNode_826A4A20`

- **Address:** `0x826A4A20` · **Category:** body (tree op) · **Region:** game (0x826A) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a validated tree-node removal — destroys the node's subtree and rewires the links — 11 callers

- **Does:** args: obj (r3), node (r4), two pointers (r5, r6). Validates with `trap 22` assertions (r5/r6 nonzero, != node, match node's fields). On success destroys the node's subtree via `DestroyTreeNodes_82859340(node, node->4)`, then rewires: obj->0 = node, clears node->8, sets self-reference fields, copies node->4->0 into obj->4 (splicing the node out).

- **Calls:** `DestroyTreeNodes_82859340`

### `GrowBuffer_821A9170`

- **Address:** `0x821A9170` · **Category:** body (small-buffer grow) · **Region:** game (0x821A) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a buffer grow/realloc for the small-buffer object — the grow path called by `ProcessValue_FlagNonZero_821A9090` — 1 caller

- **Does:** args: obj (r3), new size request (r4), new length (r5). Computes the new size (r4 | 15, or a combined size from obj->24), allocates (new_size+1) via `Allocate_Init_821A9258`, copies old data into the new buffer via `Buffer_CopyOrZero_Error_82CA3730` (if length nonzero), frees the old external buffer (if obj->24 >= 16) via `Free_SizeBucketed_Thunk_8221BE68`, updates obj->4/obj->24/obj->20, and null-terminates the external buffer. A data-preserving realloc.

- **Calls:** `Allocate_Init_821A9258`, `Buffer_CopyOrZero_Error_82CA3730`, `Free_SizeBucketed_Thunk_8221BE68`

### `RangeUpdate_8218EA38`

- **Address:** `0x8218EA38` · **Category:** body (small-buffer update) · **Region:** game (0x8218) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a clamped range update — a sibling of `ClampAndUpdate_822F6380` (in-place helper is sub_822F1F60 instead of sub_822F6E18) — 41 callers

- **Does:** args: obj a (r3), pos/obj b (r4), length (r5), extra (r6). If (b->20 < length) grow via sub_82CD12C8; compute clamped = min(extra, b->20 - length). If obj a == obj b, update IN PLACE via sub_822F1F60 twice; else a cross-object path. The in-place core that `WriteRange_AtPos_821A8F68` calls.

- **Calls:** `sub_82CD12C8`, `sub_822F1F60`

### `Array_PushBack_82C21E08`

- **Address:** `0x82C21E08` · **Category:** body (dynamic array) · **Region:** game (0x82C2) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a push_back / append to a u32 dynamic array (4-byte stride) — 38 callers

- **Does:** args: obj (r3, the array: obj->0 = base, obj->8 = index, obj->4 = capacity), value (r4). If (obj->4 - obj->8) < 1, call sub_82C21C90 (ensure-capacity/grow). Then store value at base[obj->8] (*(obj->0 + obj->8*4) = value) and increment obj->8.

- **Calls:** `sub_82C21C90`

### `BinarySearch_82C0BC08`

- **Address:** `0x82C0BC08` · **Category:** body (search) · **Region:** game (0x82C0) · **Confidence:** **high** (mechanism)

- **Role:** a binary search (lower_bound-style) over a sorted array of keyed objects, returning the index or insertion point plus a found flag — 11 callers

- **Does:** args: obj (r3, array: obj->0 = base, obj->8 = count), key ptr (r4, target at r4->0), out flag (r5). Sets *out = 0. Binary-search: low=0, high=count-1; mid=(low+high)/2; key=base[mid]->0; if key > target high=mid-1, if key == target set *out=1 and return mid, if key < target low=mid+1. On miss, return the insertion point. A lower_bound with a found flag.

- **Calls:** `NoOp_Stub_829CE870` (a no-op debug hook)

### `Array_PushElement_823758C0`

- **Address:** `0x823758C0` · **Category:** body (dynamic array) · **Region:** game (0x8237) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a push of a 36-byte element (a small-buffer object) into a 36-byte-stride array — 7 callers

- **Does:** args: obj (r3, array: obj->4 = start, obj->8 = current, obj->12 = end, 36-byte stride), source element (r4). If there is room, copy the source element into the slot at obj->8 via `LoadFrom_Source_822F2398`, then advance obj->8 by 36. Else (full) route to sub_82376118 (grow).

- **Calls:** `LoadFrom_Source_822F2398`, `sub_82376118`

### `CheckedCopy_82CA3808`

- **Address:** `0x82CA3808` · **Category:** body (checked copy) · **Region:** SDK (0x82CA) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a bounds-checked copy that validates its parameters and delegates to sub_82CAA2E0 — a sibling of `Buffer_CopyOrZero_Error_82CA3730` — 137 callers

- **Does:** args: dst (r3), capacity (r4), src (r5), len (r6). Guards: len != 0, dst != 0, src != 0 (else error 22); capacity >= len (else error 34). Else call sub_82CAA2E0(dst, src, len) and return 0. Returns 0 on success, 22 (null) or 34 (insufficient capacity) on failure.

- **Calls:** `sub_82CAA2E0`, (error path) `sub_82CAB770`, `sub_82CAB630`

### `LoadFrom_Source_822F2398`

- **Address:** `0x822F2398` · **Category:** body (small-buffer class) · **Region:** game (0x822F) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a load-from-source (copy-assign) for the 0x822F small-buffer object — 9 callers

- **Does:** args: obj (r3), src (r4). Sets obj->20 = 0, obj->24 = 7, 16-bit obj->4 = 0; calls `ClampAndUpdate_822F6380(obj, src, 0, -1)` to load the buffer part; copies src->28 -> obj->28 and src->32 -> obj->32. The "init to fixed defaults + clamp/copy the buffer + copy the tail" shape is a load-from / copy-assign.

- **Calls:** `ClampAndUpdate_822F6380`

### `DestroyTreeNodes_82859340`

- **Address:** `0x82859340` · **Category:** body (recursive destructor) · **Region:** game (0x8285) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a recursive tree/list destructor that frees its nodes — 19 callers

- **Does:** args: obj (r3, passed down unchanged), node (r4). If `node->17` (byte flag) is nonzero, STOP. Else loop: recurse on child `node->8`, FREE the current node via `Free_SizeBucketed_Thunk_8221BE68`, advance node = `node->0` (next), continue while new `node->17` == 0. Recursively destroys the +8 child subtree, frees each node, walks the +0 chain.

- **Calls:** (recursive) self, `Free_SizeBucketed_Thunk_8221BE68`

### `WriteRange_AtPos_821A8F68`

- **Address:** `0x821A8F68` · **Category:** body (small-buffer class) · **Region:** game (0x821A) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a write-a-range-at-position on the small-buffer object — **32 callers**

- **Does:** args: obj (r3), position (r4), length (r5). Uses the small-buffer inline-vs-pointer indirection (base = *obj->4 when obj->24 >= 16 else &obj->4). If position lies within [base, base + obj->20) it updates IN PLACE via sub_8218EA38(obj, position - base, length); else routes through `ProcessValue_FlagNonZero_821A9090(obj, length, 0)` (the grow path).

- **Calls:** `sub_8218EA38`, `ProcessValue_FlagNonZero_821A9090`

### `LazyGetData_822F2088`

- **Address:** `0x822F2088` · **Category:** body (lazy getter) · **Region:** game (0x822F) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a lazy/memoized getter for the data pointer (obj->28) — 19 callers

- **Does:** arg: obj (r3). If obj->28 is set, return it. Else if obj->20 (length) is 0, return obj->28 (0). Else resolve the buffer base (inline vs external per the small-buffer indirection), call sub_822CA580(base), cache the result in obj->28, and return it.

- **Calls:** `sub_822CA580`

### `Array_InsertAt_82370A98`

- **Address:** `0x82370A98` · **Category:** body (dynamic array) · **Region:** game (0x8237) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** an insert-at-position (with possible growth) on a 12-byte-stride dynamic array — 2 callers

- **Does:** args: obj (r3), value (r4), count/offset (r5), data triple (r6). The obj holds a 12-byte-stride array: start = obj->4, current = obj->8, end/capacity = obj->12. Computes element indices ((obj->12 - obj->4)/12 etc.), clamps the insertion point, and reallocates when it must grow (frees via `Free_SizeBucketed_Thunk_8221BE68` + sub_82684B38 for the new block).

- **Calls:** `Free_SizeBucketed_Thunk_8221BE68`, `sub_8264EFE0`, `sub_82684B38`

### `Array_InsertOne_8236FDD8`

- **Address:** `0x8236FDD8` · **Category:** body (dynamic array) · **Region:** game (0x8236) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a validated single-element insert — the count==1 wrapper around `Array_InsertAt_82370A98` — 3 callers

- **Does:** args: obj (r3), array (r4), value (r5), data (r6). Validates array preconditions with `trap 22` assertions, computes the current element index (array->8 - array->4)/12, then calls `Array_InsertAt_82370A98` (array, value, count=1, data).

- **Calls:** `Array_InsertAt_82370A98`

### `VcallChain_AllSuccess_82C15068`

- **Address:** `0x82C15068` · **Category:** body (vcall gate) · **Region:** game (0x82C1) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a chained all-success vcall gate — sibling of `VcallChain_AllSuccess_82C14F38` (middle thunk differs) — 16 callers

- **Does:** args: wrapper (r3), arg (r5). (1) vcall `(*wrapper)->vtable[6]` (offset 24); if 0, return 0. (2) Else call `VtableSlotX13_82C010B0(wrapper, arg)`; if 0, return 0. (3) Else vcall `(*wrapper)->vtable[7]` (offset 28); return 1 if nonzero else 0. Returns 1 iff all three virtual steps succeed.

- **Calls:** (virtual) wrapper vtable slots 6 and 7, `VtableSlotX13_82C010B0`

### `SetString_822F2020`

- **Address:** `0x822F2020` · **Category:** body (small-buffer class) · **Region:** game (0x822F) · **Confidence:** **high** (mechanism)

- **Role:** a string setter / copy-constructor for the small-buffer object — **330 callers**

- **Does:** args: obj (r3), src (r4, null-terminated). Initializes the object (obj->20 = 0 length, obj->24 = 15 max inline size, clears the inline byte at obj->4), computes strlen(src), then copies the string in via `WriteRange_AtPos_821A8F68(obj, src, strlen)`. Resets this object and makes it hold a copy of the C string.

- **Calls:** `WriteRange_AtPos_821A8F68`

### `InitNameHash_8236D358`

- **Address:** `0x8236D358` · **Category:** body (hash init) · **Region:** game (0x8236) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a name-hash initializer — computes the FNV-1 hash of a fixed global string and stores it in obj->0 — 3 callers

- **Does:** arg: obj (r3). Loads a fixed global string (~0x81FF7B98), copies it into a temp small-buffer via `SetString_822F2020`, computes its FNV-1 32 hash via `FNV1Hash_821F3C28` (seed 0x811C9DC5), stores the hash in obj->0, frees the temp buffer if external (`Free_SizeBucketed_Thunk_8221BE68`), and clears bit 0 of obj->4 (obj->4 &= 0x1FFFFFFF).

- **Calls:** `SetString_822F2020`, `FNV1Hash_821F3C28`, `Free_SizeBucketed_Thunk_8221BE68`

### `LazyGet_Field40_82B4F958`

- **Address:** `0x82B4F958` · **Size:** ~25 insns · **Category:** body (vtable method) · **Region:** game (0x82B4 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a lazy-initialized (memoized) getter for a 64-bit field — a **vtable method** (0 direct callers)

- **Does:** arg: obj (r3). Loads `obj->40`; if it is already nonzero, return it immediately. If it is 0: call `sub_82CC1460(obj->8, &out)` to compute the value, cache the result at `obj->40`, then return it. A classic "compute once, cache, return" memoized getter for the `obj->40` resource (a 64-bit pointer/handle, stored with a full 64-bit stw).

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** `sub_82CC1460`

### `Buffer_CopyOrZero_Error_82CA3730`

- **Address:** `0x82CA3730` · **Size:** ~55 insns · **Category:** body · **Region:** SDK (0x82CA) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a bounded buffer operation that copies or zero-fills and raises an error code on the failure paths — **61 call sites**

- **Does:** args (r3, r4, r5, r6): guards on `r6 != 0` (else return 0) and `r3 != 0` (else raise error 22). Compares two length fields (r4 vs r6): when the first is >= the second it copies via `MemCpy_SdkRuntime_82CA2C60`; when `r5 == 0` it zero-fills via the SDK memset (sub_82CA3190) and then raises error 22 (or 34 on one length-branch). The error is raised via a `sub_82CAB770()` call, a store of the code (22 or 34) into an out-slot, then `sub_82CAB630()` — the SDK exception/bug path. Returns the error code (22 or 34) on failure paths and 0 otherwise. The exact class/args aren't resolvable (the recompiler reuses r3), so the name states the observable behavior: a length-checked buffer copy (or zero) that raises an error code on the failure paths.

- **Trigger:** Internal — the 61 call sites.

- **Calls:** `MemCpy_SdkRuntime_82CA2C60`, `sub_82CA3190` (SDK memset), `sub_82CAB770`, `sub_82CAB630` (error path)

### `VcallSlot3_Bool_82C01E20`

- **Address:** `0x82C01E20` · **Size:** ~25 insns · **Category:** body (vtable method) · **Region:** game (0x82C0 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable method (0 direct callers) — the fifth known member of the recurring vtable-slot-3 interface, over the +16 sub-object (same offset and 0x82C01E region as `SubObject_VcallSlot3_Bool_82C01E78`)

- **Does:** args: wrapper (r3), arg (r4). Loads the sub-object at `wrapper->16` and calls `sub_obj->vtable[3](sub_obj, &{arg, 0}, 1)` (vtable slot at offset 12 / index 3). From the returned value it derives a boolean through a `clz`/`clz`/`rotate`/`xor-1` bit idiom and returns 0 or 1 (a negated/derived bit of the slot-3 result). The slot-3 method isn't resolvable without the vtable, so the name states the mechanism: a slot-3 vcall that returns a derived bool.

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** (virtual) sub-object vtable slot 3

### `Stub_ReturnZero_831FD318`

- **Address:** `0x831FD318` · **Size:** 2 insns · **Category:** leaf (stub) · **Region:** SDK · **Confidence:** **high** (mechanism), low (purpose)

- **Role:** a trivial return-0 stub — `li r3,0; blr` — a no-op default method / interface fallback

- **Does:** unconditionally returns 0. 20 call sites, all in the SDK region (sub_82CAB7E0, sub_82C00C90, sub_82CADF00, ...). The same shape as `NoOp_Stub_829CE870` but returning 0 instead of void.

- **Trigger:** Internal — the 20 call sites above.

### `VcallChain_AllSuccess_82C14F38`

- **Address:** `0x82C14F38` · **Size:** ~45 insns · **Category:** body · **Region:** game (0x82C1 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a chained all-success vcall gate — a sibling of `VcallTriple_AllSuccess_82C011E8` (same "run a sequence of virtual calls, short-circuit on the first failure, return 1 only if every step succeeds" pattern)

- **Does:** args: wrapper (r3), arg (r5). (1) vcall `(*wrapper)->vtable[6]` (offset 24); if it returns 0, return 0. (2) Otherwise call `VtableSlotX14_82C01090(wrapper, arg)`; if it returns 0, return 0. (3) Otherwise vcall `(*wrapper)->vtable[7]` (offset 28); return 1 if nonzero, else 0. So it returns 1 iff all three virtual steps succeed. 5 callers, all in the 0x82C1-0x82C3 region.

- **Trigger:** Internal — the 5 call sites above.

- **Calls:** (virtual) wrapper vtable slots 6 and 7, `VtableSlotX14_82C01090`

### `ProcessValue_FlagNonZero_8217A5E0`

- **Address:** `0x8217A5E0` · **Size:** ~55 insns · **Category:** body · **Region:** game (0x8217 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a value/state processor — a **sibling of `ProcessValue_FlagNonZero_821A9090`** (same structure, different thresholds and helpers)

- **Does:** args: obj (r3), value (r4), flag (r5). Every exit path evaluates the `(r11 = 0 - value; r10 = -1 + carry; return r10 & 1)` idiom, so the RETURN is simply `(value != 0)`. The real work is value-dependent **side effects**: if `value > 32767` call `sub_82CD11D0()`; if `obj->24 < value` call `sub_8217A4F8(obj, value, obj->20)`; else (`obj->24 >= value`), if `flag != 0` and `value < 8` and `value < obj->20` call a helper and do bookkeeping. 7 callers (sub_826C3AA8, sub_822F6CB0, sub_822F52A8, ...), all consuming the low byte as a boolean gate.

- **Trigger:** Internal — the 7 call sites above.

- **Calls:** `sub_82CD11D0`, `sub_8217A4F8`

### `ClampAndUpdate_822F6380`

- **Address:** `0x822F6380` · **Size:** ~60 insns · **Category:** body · **Region:** game (0x822F region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a hot (**51 callers**) range/clamp + object-update operator in the 0x822F value/buffer subsystem (the same region as the `ProcessValue_FlagNonZero_*` pair, which it calls)

- **Does:** args: obj a (r3), obj b (r4), offset (r5), value (r6). If `(b->20 < offset)` call `sub_82CD12C8()` (a grow/extend); then compute a **clamped** amount = `min(value, b->20 - offset)`. If obj a == obj b: call `sub_822F6E18` twice to update in place. Otherwise (obj a != obj b): call `ProcessValue_FlagNonZero_8217A5E0(obj a, clamped, 0)` and update both objects' fields (`->4` / `->24`), using a small-buffer inline-vs-pointer indirection (when the size field is < 8, treat the field as an inline 4-byte slot; otherwise dereference it). The object class isn't resolvable, so the name states the observable behavior: clamp a value to a range (growing if needed) and update the objects.

- **Trigger:** Internal — the 51 call sites.

- **Calls:** `sub_82CD12C8`, `sub_822F6E18`, `ProcessValue_FlagNonZero_8217A5E0`

### `VcallSlot3_WithProduct_82305270`

- **Address:** `0x82305270` · **Size:** ~18 insns · **Category:** body (vtable method) · **Region:** game (0x8230 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable method (0 direct callers) that computes a 32-bit product and writes it via vtable slot 3

- **Does:** args: arg (r3), factor a (r4), factor b (r5), and a pointer-to-object (r6). Computes `product = (int32)a * (int32)b`, dereferences r6 to get the object, and calls `object->vtable[3](r6, arg, product, 0)` (vtable slot at offset 12 / index 3 — the same slot-3 interface as the other `SubObject_VcallSlot3_*` wrappers, but here the object is a dereferenced pointer and the product is passed in). Returns the product. A "unit × count, write it" shape (cf. `StoreUnitTimesCount_82CE32B0`). The slot-3 method isn't resolvable without the vtable, so the name states the mechanism: a slot-3 write carrying a product.

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** (virtual) object vtable slot 3

### `AccumulateGlobalCallback_82BC30D0`

- **Address:** `0x82BC30D0` · **Size:** ~35 insns · **Category:** body (vtable method) · **Region:** game (0x82BC region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a vtable method (0 direct callers) that accumulates the results of calling a global function-pointer callback in a loop

- **Does:** args: obj (r3), base (r4), count (r5). Guards: if the two byte fields `obj->8 == obj->9`, or `count == 1`, take a single-iteration/special path. Otherwise loop (count-1) times: each iteration calls the global callback (a fixed function pointer in game data) with `(base + descending_counter, 1, obj->4, 1)` and adds the return to an accumulator. Returns the accumulated sum. The callback's identity and the object class aren't resolvable without more context, so the name states the mechanism: a looped global-callback accumulator.

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** (indirect) a global function-pointer callback

### `ProcessValue_FlagNonZero_821A9090`

- **Address:** `0x821A9090` · **Size:** ~60 insns · **Category:** body · **Region:** game (0x821A region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a value/state processor that returns the non-zero-ness of its value argument

- **Does:** args: obj (r3), value (r4), flag (r5). Every exit path evaluates the idiom `(r11 = 0 - value; r10 = -1 + carry; return r10 & 1)`, which is **1 iff value != 0** and 0 iff value == 0 — so the RETURN is simply `(value != 0)`. The real work is value-dependent **side effects** along the way: if `value > -2` call `sub_82CD11D0()`; if `obj->24 < value` call `sub_821A9170(obj, value, obj->20)`; else (`obj->24 >= value`), if `flag != 0` and `value < 16` and `value < obj->20` call `sub_822F1DB0(obj, 1, value)`, and if `value == 0` and `obj->20 < 16` store 0 to `obj->20` and a byte to `obj->4`. 8 callers, all consuming the low byte of the result as a boolean gate. The object class and helper identities aren't resolvable, so the name states the observable contract: process a value (with side effects) and return whether it was non-zero.

- **Trigger:** Internal — the 8 call sites above.

- **Calls:** `sub_82CD11D0`, `sub_821A9170`, `sub_822F1DB0`

### `CsGuarded_HandlerDispatch_82B43CF0`

- **Address:** `0x82B43CF0` · **Size:** ~90 insns · **Category:** body (vtable method) · **Region:** game (0x82B43 family) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a CS-guarded handler-table dispatch — a **vtable method** (0 direct callers, reached only via virtual dispatch)

- **Does:** args: obj (r3), a pointer (r4), an offset (r5), and a value (r6). Guards (else return **0x80004005**): `obj->12 != 0`, `*r4 != 0`, and `(r6_low32 + r5) <= obj->32` (a 64-bit bounds check). On success: acquires a CS via `CriticalSection_EnterOrTry_82200688` on the wrapper at `obj+40`, makes a virtual call on `obj->36` (vtable slot 4) plus `sub_82C6F8E8`, and if that returns 1, computes a handler-table index and dispatches to a global function-pointer table with `(*r4, r5 + obj->12, r6, ...)`; releases the CS (`__imp__RtlLeaveCriticalSection`) and returns 0. Any failed guard/step returns 0x80004005. Same 0x80004005 code and global-handler-table pattern as the 0x82B48/0x82B49 chunk family. The interface method and handler entries aren't resolvable without the vtables, so the name states the mechanism: a bounds-checked, CS-protected dispatch.

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** `CriticalSection_EnterOrTry_82200688`, `sub_82C6F8E8`, `__imp__RtlLeaveCriticalSection`, (virtual) obj->36 vtable slot 4 + global handler-table entries

### `VcallSlot3_AdvancePosition_82C62EB0`

- **Address:** `0x82C62EB0` · **Size:** ~25 insns · **Category:** body (vcall write) · **Region:** game (0x82C6 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a position-advancing write via vtable slot 3 — a member of the same slot-3 interface as `SubObject_VcallSlot3_Bool_82C01E78` and `SubObject_VcallSlot3_GatedStore32_82C842C8`, but over a +8 sub-object

- **Does:** args: obj (r3), arg (r4), count (r5). Guards (else return): `count > 0` and `obj->8 != 0`. Loads the sub-object at `obj->8` and calls `sub_obj->vtable[3](sub_obj, &{arg, 0}, sign_extend(obj->12), count)` (vtable slot at offset 12 / index 3), then advances the position counter `obj->12 += count`. Returns void. Single caller: sub_829FCBE0. The slot-3 interface method isn't resolvable without the vtable, so the name states the mechanism: a slot-3 write that advances a position by the count.

- **Trigger:** Internal — the single caller sub_829FCBE0.

- **Calls:** (virtual) sub-object vtable slot 3

### `VcallTriple_AllSuccess_82C011E8`

- **Address:** `0x82C011E8` · **Size:** ~35 insns · **Category:** body (vcall sequence) · **Region:** game (0x82C0 region) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a triple-vcall gate — three virtual methods in sequence, all-or-nothing

- **Does:** args: obj (r3), arg (r5). Reads the vtable at `*(obj->0)` and, in order: (1) calls vtable slot 6 (offset 24); if it returns 0, bail with 0. (2) calls vtable slot 8 (offset 32) passing the arg; if it returns 0, bail with 0. (3) calls vtable slot 7 (offset 28); the low byte of its result is the return value (**1 = all three succeeded, 0 = some step failed**). 40 callers, all in the 0x82C0–0x82C4 region (sub_82C033D8, sub_82C49680, sub_82C2D040, ...). The three interface methods (slots 6/8/7) aren't resolvable without the vtable, so the name states the mechanism: a three-step all-or-nothing virtual sequence.

- **Trigger:** Internal — the 40 call sites above.

- **Calls:** (virtual) obj vtable slots 6, 8, 7

### `WriteChunk_TableDispatch_82B486D0`

- **Address:** `0x82B486D0` · **Size:** ~70 insns · **Category:** body (chunk writer) · **Region:** game (0x82B48 family) · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a table-dispatched chunked write/emit — the write-side sibling of `ProcessChunk_TableDispatch_82B49110`

- **Does:** args: obj (r3), a pointer to a 64-bit descriptor (r4), and a total length (r5). The descriptor's **high 32 bits** select a handler from a global function-pointer table (index = `(hi<<2) & ~3`) and its **low 32 bits** is the data pointer being emitted. Guards: if the descriptor is 0, return **0x80004005**. Then loops: computes the chunk length as `min(obj->24 - obj->20, remaining)` (calling `sub_82B48A40(obj)` first when `obj->24` is 0, returning 0x80004005 if that fails), looks up the handler, calls it with `(data, obj->16 + position, chunk_len, ...)`, and advances the position (`obj->24`) and a 64-bit byte offset (`obj->40`); repeats until the length is exhausted. Returns 0 on success, 0x80004005 on failure. Single caller: sub_82B48898.

- **Trigger:** Internal — the single caller sub_82B48898.

- **Calls:** `sub_82B48A40`, (virtual) global handler-table entries

### `SubObject_VcallSlot3_Bool_82C01E78`

- **Address:** `0x82C01E78` · **Size:** ~20 insns · **Category:** body (vtable method) · **Region:** game · **Confidence:** medium (mechanism), low (purpose)

- **Role:** a **vtable method** (0 direct callers — reached only via virtual dispatch) that delegates to a sub-object's slot-3 method and returns a boolean

- **Does:** args: obj (r3), arg1 (r4). Loads the sub-object at `obj->16`, builds a 2-word arg block on the stack `{arg1, 0}`, and calls `sub_obj->vtable[3](sub_obj, &argblock, 4)` (vtable = `*(sub_obj->0)`; slot at vtable offset 12 / index 3). Reduces the call's result to a 0/1 boolean via a `clz`-based bit test and returns it. The interface method (slot 3) isn't resolvable without the vtable, so the name states the mechanism: a delegating vtable method over the +16 sub-object returning a bool.

- **Trigger:** Internal — only via virtual dispatch (no direct `bl` call sites).

- **Calls:** (virtual) sub-object vtable slot 3

### `SubObject_VcallSlot3_GatedStore32_82C842C8`

- **Address:** `0x82C842C8` · **Size:** ~35 insns · **Category:** body (vtable delegate) · **Region:** game · **Confidence:** medium (mechanism), low (purpose)

- **Role:** conditionally delegates to a sub-object's slot-3 method and stores a 32-bit result

- **Does:** args: obj (r3), dest (r4). Reads a flag byte at `obj->5` and, when a `clz`-based bit test on it is set, loads the sub-object at `obj->0`, builds a 2-word arg block `{dest, 0}` on the stack, and calls `sub_obj->vtable[3](sub_obj, &argblock, 4)` (the same vtable slot 3 / offset 12 as `SubObject_VcallSlot3_Bool_82C01E78`). Stores the call's 32-bit result into `*dest`. 17 callers (sub_82C680F8, sub_82372108, sub_82A4AF78, ...). The interface method (slot 3) isn't resolvable without the vtable, so the name states the mechanism: a flag-gated vtable-slot-3 delegate over the +0 sub-object that writes a u32 result.

- **Trigger:** Internal — the 17 call sites above.

- **Calls:** (virtual) sub-object vtable slot 3

### `Pool_AcquireNode_83230688`

- **Address:** `0x83230688` · **Size:** ~80 insns · **Category:** body (vtable method) · **Region:** game (data-processing subsystem) · **Confidence:** medium (mechanism), low (object identity)

- **Role:** spin-locked pool / free-list node acquisition

- **Does:** arg: obj (r3) — the 4 callers select the per-slot object by indexing an array (base = caller's `obj->4`, 4-aligned index). Layout: obj->16 = head of a node list, obj->4 = step/size, obj->24 = lower window bound, obj->28 = upper window bound, obj+36 = spin-lock word (via `SpinLock_Acquire_822D7408`). Acquire the lock; if the list is empty call `sub_83230568(obj)` to produce a node (else release + return 0); otherwise take the head node, advance its internal chain (`node->0 = *node->0`), increment a counter (`node->4 += 1`), and if the chain is exhausted (`*node->0 == 0`) unlink the node from the list (via the `node->8` next pointers, fixing the head if needed); advance the allocation window (`obj->24 += obj->4`, `obj->28 -= obj->4`); release; return the new chain head (node->0) or 0.

- **Trigger:** Internal — the 4 callers (sub_83236650, sub_8240DAA8, sub_828456C8, sub_83231D20), each indexing an array to pick the per-slot object.

- **Calls:** `SpinLock_Acquire_822D7408`, `sub_83230568`

- **Note:** a synchronized block/node dispense from a pool. Object identity unresolved (a per-slot object in the data-processing subsystem), hence the mechanism name + address suffix.

### `ProcessChunk_TableDispatch_82B49110`

- **Address:** `0x82B49110` · **Size:** ~291 insns · **Category:** body (vtable method) · **Region:** game (0x82B49 family) · **Confidence:** medium (mechanism), low (object identity)

- **Role:** bounds-checked, position-advancing, handler-table-dispatched chunk processing — a buffer/stream reader of some kind

- **Does:** args: obj (r3), src (r4, a pointer to a u64), len (r5). `obj->16` is a 64-bit read position (it has a public getter, `sub_82B495F8`: `ld r3,16(r3)`); `obj->24` a 64-bit bound; `obj->32` a size; `obj->36` an index into a 32-byte-stride entry array. Bounds-checks `position + len <= bound` (else returns the 0x80004005 failure code), then loops: (a) reads a value from src, (b) dispatches to a handler from a **global function-pointer table** indexed by the high 32 bits of `*src`, passing (bound, computed_ptr, chunk_len), (c) advances the position (`obj->16`) by the chunk, and (d) calls sibling `sub_82B49730(obj)` for a status (low byte checked) and `sub_82B49678(obj, flag)` on the overflow path. Returns 0 on success, a 0x8000xxxx status on failure.

- **Trigger:** Virtual dispatch only — **zero static call sites**.

- **Note:** one of the two largest methods in the 0x82B49 region (near-identical twin `sub_82B49320`). The object identity and handler table are unresolved, so the name describes the mechanism only and keeps the address suffix.

### `Func_82B387D0`

- **Address:** `0x82B387D0` · **Size:** 14 insns · **Category:** body · **Region:** game · **Confidence:** low

- **Role:** Func routine (14 insns, game)

- **Does:** [game code] Body function, 14 insns, no prologue (mid-function target or leaf). Calls: sub_82CC0D58. First: `addi r6,r31,80`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CC0D58`

### `Func_82B387E8`

- **Address:** `0x82B387E8` · **Size:** 8 insns · **Category:** body · **Region:** game · **Confidence:** low

- **Role:** Func routine (8 insns, game)

- **Does:** [game code] Body function, 8 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82C1DC98`

- **Address:** `0x82C1DC98` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CA6320. First: `lis r10,-32247`; last: `b 0x82ca6320`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA6320`

### `Func_82C59698`

- **Address:** `0x82C59698` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_82C597F0. First: `bl 0x82c597f0`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C597F0`

### `Func_82C5AC4C`

- **Address:** `0x82C5AC4C` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CA8210. First: `bl 0x82ca8210`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA8210`

### `Func_82C5DA68`

- **Address:** `0x82C5DA68` · **Size:** 18 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (18 insns, SDK)

- **Does:** [SDK runtime] Body function, 18 insns, no prologue (mid-function target or leaf). Calls: sub_82C60BD8, sub_82C60E18, sub_82C61080. First: `bl 0x82c60e18`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C60BD8`, `sub_82C60E18`, `sub_82C61080`

### `Func_82C5DA94`

- **Address:** `0x82C5DA94` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_82C60BD8. First: `bl 0x82c60bd8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82C60BD8`

### `Func_82C89A0C`

- **Address:** `0x82C89A0C` · **Size:** 3 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (3 insns, SDK)

- **Does:** [SDK runtime] Body function, 3 insns, no prologue (mid-function target or leaf). First: `stw r4,0(r6)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82C8D820`

- **Address:** `0x82C8D820` · **Size:** 30 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (30 insns, SDK)

- **Does:** [SDK runtime] Body function, 30 insns, no prologue (mid-function target or leaf). First: `lwz r10,0(r6)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CA33AC`

- **Address:** `0x82CA33AC` · **Size:** 20 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (20 insns, SDK)

- **Does:** [SDK runtime] Body function, 20 insns, no prologue (mid-function target or leaf). Calls: sub_82CA97A8, sub_82CAC520, sub_82CBBF60, sub_82CC0728. First: `lwz r3,88(r11)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA97A8`, `sub_82CAC520`, `sub_82CBBF60`, `sub_82CC0728`

### `Func_82CA33E4`

- **Address:** `0x82CA33E4` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). Calls: sub_82CA97A8. First: `bl 0x82ca97a8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA97A8`

### `Func_82CA3614`

- **Address:** `0x82CA3614` · **Size:** 50 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (50 insns, SDK)

- **Does:** [SDK runtime] Body function, 50 insns, no prologue (mid-function target or leaf). Calls: sub_82CA36C4, sub_82CA36DC, sub_82CA8570, sub_82CAACD0. First: `lis r24,-31921`; last: `b 0x82ca2c24`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA36C4`, `sub_82CA36DC`, `sub_82CA8570`, `sub_82CAACD0`

### `Func_82CA47C4`

- **Address:** `0x82CA47C4` · **Size:** 28 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (28 insns, SDK)

- **Does:** [SDK runtime] Body function, 28 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4854, sub_82CAB770, sub_82CAF038, sub_82CAF450. First: `lbz r11,0(r30)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4854`, `sub_82CAB770`, `sub_82CAF038`, `sub_82CAF450`

### `Func_82CA4814`

- **Address:** `0x82CA4814` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4854. First: `mr r8,r8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4854`

### `Func_82CA4A64`

- **Address:** `0x82CA4A64` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4920, sub_82CA4AB8. First: `mr r3,r30`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4920`, `sub_82CA4AB8`

### `Func_82CA4A70`

- **Address:** `0x82CA4A70` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4AB8. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4AB8`

### `Func_82CA4DE4`

- **Address:** `0x82CA4DE4` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4AF0, sub_82CA4E30. First: `mr r6,r30`; last: `b 0x82ca4dd0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4AF0`, `sub_82CA4E30`

### `Func_82CA4FB0`

- **Address:** `0x82CA4FB0` · **Size:** 51 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (51 insns, SDK)

- **Does:** [SDK runtime] Body function, 51 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4F00, sub_82CA5064, sub_82CA507C, sub_82CA50F4, sub_82CA88E0. First: `lis r11,-31921`; last: `b 0x82ca4fc0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4F00`, `sub_82CA5064`, `sub_82CA507C`, `sub_82CA50F4`, `sub_82CA88E0`

### `Func_82CA5004`

- **Address:** `0x82CA5004` · **Size:** 30 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (30 insns, SDK)

- **Does:** [SDK runtime] Body function, 30 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4F00, sub_82CA5064, sub_82CA50F4. First: `lwz r11,0(r29)`; last: `b 0x82ca4fc0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4F00`, `sub_82CA5064`, `sub_82CA50F4`

### `Func_82CA507C`

- **Address:** `0x82CA507C` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CA50A4. First: `mr r8,r8`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA50A4`

### `Func_82CA51C0`

- **Address:** `0x82CA51C0` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4F00, sub_82CA521C. First: `mr r3,r30`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4F00`, `sub_82CA521C`

### `Func_82CA51CC`

- **Address:** `0x82CA51CC` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CA521C. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA521C`

### `Func_82CA53BC`

- **Address:** `0x82CA53BC` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5260, sub_82CA5408. First: `mr r5,r29`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5260`, `sub_82CA5408`

### `Func_82CA56DC`

- **Address:** `0x82CA56DC` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5440, sub_82CA5730. First: `mr r3,r30`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5440`, `sub_82CA5730`

### `Func_82CA56E8`

- **Address:** `0x82CA56E8` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5730. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5730`

### `Func_82CA71C8`

- **Address:** `0x82CA71C8` · **Size:** 84 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (84 insns, SDK)

- **Does:** [SDK runtime] Body function, 84 insns, no prologue (mid-function target or leaf). Calls: sub_8223F990, sub_82CA7300, sub_82CA7318, sub_82CAB4E0, sub_82CAB5B8. First: `lwz r11,12(r30)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_8223F990`, `sub_82CA7300`, `sub_82CA7318`, `sub_82CAB4E0`, `sub_82CAB5B8`, `sub_82CAB630`, `sub_82CAB770`, `sub_82CAF6C8`

### `Func_82CA7C70`

- **Address:** `0x82CA7C70` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). Calls: sub_82CAC610. First: `bl 0x82cac610`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAC610`

### `Func_82CA8038`

- **Address:** `0x82CA8038` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CA8060. First: `addic. r27,r27,-1`; last: `b 0x82ca8038`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA8060`

### `Func_82CA8128`

- **Address:** `0x82CA8128` · **Size:** 16 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (16 insns, SDK)

- **Does:** [SDK runtime] Body function, 16 insns, no prologue (mid-function target or leaf). Calls: sub_82CA81A0. First: `addic. r28,r28,-1`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA81A0`

### `Func_82CA8248`

- **Address:** `0x82CA8248` · **Size:** 18 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (18 insns, SDK)

- **Does:** [SDK runtime] Body function, 18 insns, no prologue (mid-function target or leaf). Calls: sub_82CA82C0. First: `stw r28,80(r31)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA82C0`

### `Func_82CA8B14`

- **Address:** `0x82CA8B14` · **Size:** 23 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (23 insns, SDK)

- **Does:** [SDK runtime] Body function, 23 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4AF0, sub_82CA8B90, sub_82CAB4E0, sub_82CAB5B8. First: `mr r3,r30`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4AF0`, `sub_82CA8B90`, `sub_82CAB4E0`, `sub_82CAB5B8`

### `Func_82CA8B44`

- **Address:** `0x82CA8B44` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CA8B90. First: `mr r8,r8`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA8B90`

### `Func_82CA9064`

- **Address:** `0x82CA9064` · **Size:** 103 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (103 insns, SDK)

- **Does:** [SDK runtime] Body function, 103 insns, no prologue (mid-function target or leaf). Calls: sub_82CA91E8, sub_82CA9220, sub_82CAB630, sub_82CAB770, sub_82CAF6C8. First: `lwz r11,12(r30)`; last: `b 0x82ca2c24`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA91E8`, `sub_82CA9220`, `sub_82CAB630`, `sub_82CAB770`, `sub_82CAF6C8`, `sub_82CB5958`

### `Func_82CAB2B8`

- **Address:** `0x82CAB2B8` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_82CAAF88, sub_82CAB308. First: `mr r7,r30`; last: `b 0x82cab2a4`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAAF88`, `sub_82CAB308`

### `Func_82CAF620`

- **Address:** `0x82CAF620` · **Size:** 20 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (20 insns, SDK)

- **Does:** [SDK runtime] Body function, 20 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAF478, sub_82CAF658, sub_82CAF690. First: `lwzx r11,r27,r28`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAF478`, `sub_82CAF658`, `sub_82CAF690`

### `Func_82CAFAB0`

- **Address:** `0x82CAFAB0` · **Size:** 25 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (25 insns, SDK)

- **Does:** [SDK runtime] Body function, 25 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAB7A8, sub_82CAF788, sub_82CAFAFC, sub_82CAFB34. First: `lwzx r11,r27,r28`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAB7A8`, `sub_82CAF788`, `sub_82CAFAFC`, `sub_82CAFB34`

### `Func_82CAFC28`

- **Address:** `0x82CAFC28` · **Size:** 30 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (30 insns, SDK)

- **Does:** [SDK runtime] Body function, 30 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAB7A8, sub_82CAFC88, sub_82CAFCC0, sub_82CB8C28. First: `lwzx r11,r28,r29`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAB7A8`, `sub_82CAFC88`, `sub_82CAFCC0`, `sub_82CB8C28`, `sub_82CC0758`, `sub_82CC1130`

### `Func_82CAFE98`

- **Address:** `0x82CAFE98` · **Size:** 25 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (25 insns, SDK)

- **Does:** [SDK runtime] Body function, 25 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5DC0, sub_82CAB770, sub_82CAFEE4, sub_82CAFF18, sub_82CB5B78. First: `lwzx r11,r29,r30`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5DC0`, `sub_82CAB770`, `sub_82CAFEE4`, `sub_82CAFF18`, `sub_82CB5B78`

### `Func_82CB0138`

- **Address:** `0x82CB0138` · **Size:** 25 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (25 insns, SDK)

- **Does:** [SDK runtime] Body function, 25 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAB7A8, sub_82CAFFA8, sub_82CB0184, sub_82CB01BC. First: `lwzx r11,r27,r28`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAB7A8`, `sub_82CAFFA8`, `sub_82CB0184`, `sub_82CB01BC`

### `Func_82CB0FB0`

- **Address:** `0x82CB0FB0` · **Size:** 68 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (68 insns, SDK)

- **Does:** [SDK runtime] Body function, 68 insns, no prologue (mid-function target or leaf). Calls: sub_82CB1040, sub_82CB10E0. First: `cmpwi cr6,r29,8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB1040`, `sub_82CB10E0`

### `Func_82CB1040`

- **Address:** `0x82CB1040` · **Size:** 32 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (32 insns, SDK)

- **Does:** [SDK runtime] Body function, 32 insns, no prologue (mid-function target or leaf). Calls: sub_82CB10E0. First: `mr r8,r8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB10E0`

### `Func_82CB1390`

- **Address:** `0x82CB1390` · **Size:** 32 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (32 insns, SDK)

- **Does:** [SDK runtime] Body function, 32 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5DC0, sub_82CB13EC, sub_82CB1410. First: `lwz r10,4(r30)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5DC0`, `sub_82CB13EC`, `sub_82CB1410`

### `Func_82CB13EC`

- **Address:** `0x82CB13EC` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). Calls: sub_82CB1410. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB1410`

### `Func_82CB4938`

- **Address:** `0x82CB4938` · **Size:** 36 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (36 insns, SDK)

- **Does:** [SDK runtime] Body function, 36 insns, no prologue (mid-function target or leaf). Calls: sub_82CB4748, sub_82CB49B0, sub_82CB49DC, sub_82CB5800, sub_82CBA3E0. First: `cmpw cr6,r28,r27`; last: `b 0x82cb4934`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB4748`, `sub_82CB49B0`, `sub_82CB49DC`, `sub_82CB5800`, `sub_82CBA3E0`

### `Func_82CB4974`

- **Address:** `0x82CB4974` · **Size:** 21 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (21 insns, SDK)

- **Does:** [SDK runtime] Body function, 21 insns, no prologue (mid-function target or leaf). Calls: sub_82CB4748, sub_82CB49B0, sub_82CBA3E0. First: `lwz r11,4(r11)`; last: `b 0x82cb4934`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB4748`, `sub_82CB49B0`, `sub_82CBA3E0`

### `Func_82CB49C0`

- **Address:** `0x82CB49C0` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). First: `lwz r27,204(r31)`; last: `b 0x82cb4934`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB49DC`

- **Address:** `0x82CB49DC` · **Size:** 15 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (15 insns, SDK)

- **Does:** [SDK runtime] Body function, 15 insns, no prologue (mid-function target or leaf). Calls: sub_82CB4748, sub_82CB4A18, sub_82CB5800. First: `mr r8,r8`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB4748`, `sub_82CB4A18`, `sub_82CB5800`

### `Func_82CB4B1C`

- **Address:** `0x82CB4B1C` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). First: `lwz r3,24(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB4B28`

- **Address:** `0x82CB4B28` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB4B38`

- **Address:** `0x82CB4B38` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). Calls: sub_82CB57A0. First: `bl 0x82cb57a0`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `Func_82CB4CB8`

- **Address:** `0x82CB4CB8` · **Size:** 129 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (129 insns, SDK)

- **Does:** [SDK runtime] Body function, 129 insns, no prologue (mid-function target or leaf). Calls: sub_82CAA2E0, sub_82CB4E9C, sub_82CB5800, sub_82CBA500. First: `rlwinm. r11,r10,0,28,28`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAA2E0`, `sub_82CB4E9C`, `sub_82CB5800`, `sub_82CBA500`

### `Func_82CB4E9C`

- **Address:** `0x82CB4E9C` · **Size:** 8 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (8 insns, SDK)

- **Does:** [SDK runtime] Body function, 8 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB4F18`

- **Address:** `0x82CB4F18` · **Size:** 55 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (55 insns, SDK)

- **Does:** [SDK runtime] Body function, 55 insns, no prologue (mid-function target or leaf). Calls: sub_82CB4C48, sub_82CB4FDC. First: `mr r6,r29`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB4C48`, `sub_82CB4FDC`

### `Func_82CB56B0`

- **Address:** `0x82CB56B0` · **Size:** 15 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (15 insns, SDK)

- **Does:** [SDK runtime] Body function, 15 insns, no prologue (mid-function target or leaf). First: `lwz r3,0(r30)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB56BC`

- **Address:** `0x82CB56BC` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB56CC`

- **Address:** `0x82CB56CC` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CB57A0. First: `bl 0x82cb57a0`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `Func_82CB582C`

- **Address:** `0x82CB582C` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). Calls: sub_82CB57A0. First: `mtctr r11`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `Func_82CB5834`

- **Address:** `0x82CB5834` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_82CB57A0. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `Func_82CB5840`

- **Address:** `0x82CB5840` · **Size:** 4 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (4 insns, SDK)

- **Does:** [SDK runtime] Body function, 4 insns, no prologue (mid-function target or leaf). Calls: sub_82CB57A0. First: `bl 0x82cb57a0`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB57A0`

### `Func_82CB5894`

- **Address:** `0x82CB5894` · **Size:** 33 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (33 insns, SDK)

- **Does:** [SDK runtime] Body function, 33 insns, no prologue (mid-function target or leaf). Calls: sub_82CA49D8, sub_82CA5DC0, sub_82CB5918. First: `li r28,3`; last: `b 0x82cb58a0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA49D8`, `sub_82CA5DC0`, `sub_82CB5918`

### `Func_82CB5BB4`

- **Address:** `0x82CB5BB4` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). First: `mtctr r11`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB5BC0`

- **Address:** `0x82CB5BC0` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CB5BD0`

- **Address:** `0x82CB5BD0` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CC0750. First: `lis r11,-16384`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CC0750`

### `Func_82CB68A8`

- **Address:** `0x82CB68A8` · **Size:** 25 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (25 insns, SDK)

- **Does:** [SDK runtime] Body function, 25 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAB7A8, sub_82CB6168, sub_82CB68F4, sub_82CB692C. First: `lwzx r11,r27,r28`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAB7A8`, `sub_82CB6168`, `sub_82CB68F4`, `sub_82CB692C`

### `Func_82CB6C48`

- **Address:** `0x82CB6C48` · **Size:** 24 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (24 insns, SDK)

- **Does:** [SDK runtime] Body function, 24 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAB7A8, sub_82CB6AA0, sub_82CB6C90, sub_82CB6CC8. First: `lwzx r11,r27,r28`; last: `b 0x82ca2c28`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAB7A8`, `sub_82CB6AA0`, `sub_82CB6C90`, `sub_82CB6CC8`

### `Func_82CB89E8`

- **Address:** `0x82CB89E8` · **Size:** 23 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (23 insns, SDK)

- **Does:** [SDK runtime] Body function, 23 insns, no prologue (mid-function target or leaf). Calls: sub_82CB8428, sub_82CB8A64. First: `mr r8,r6`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB8428`, `sub_82CB8A64`

### `Func_82CB8A08`

- **Address:** `0x82CB8A08` · **Size:** 15 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (15 insns, SDK)

- **Does:** [SDK runtime] Body function, 15 insns, no prologue (mid-function target or leaf). Calls: sub_82CB8A64. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CB8A64`

### `Func_82CBA360`

- **Address:** `0x82CBA360` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CBA120, sub_82CBA3A8. First: `mr r4,r30`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBA120`, `sub_82CBA3A8`

### `Func_82CBA76C`

- **Address:** `0x82CBA76C` · **Size:** 94 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (94 insns, SDK)

- **Does:** [SDK runtime] Body function, 94 insns, no prologue (mid-function target or leaf). Calls: sub_82CA5DC0, sub_82CAB678, sub_82CBA8E4, sub_82CBAF58, sub_82CBAFC0. First: `bl 0x82cbb0c0`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA5DC0`, `sub_82CAB678`, `sub_82CBA8E4`, `sub_82CBAF58`, `sub_82CBAFC0`, `sub_82CBB028`, `sub_82CBB090`, `sub_82CBB0A0`

### `Func_82CBAE6C`

- **Address:** `0x82CBAE6C` · **Size:** 16 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (16 insns, SDK)

- **Does:** [SDK runtime] Body function, 16 insns, no prologue (mid-function target or leaf). Calls: sub_82CBA740, sub_82CBAE88, sub_82CBAEAC. First: `lwz r11,27636(r30)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBA740`, `sub_82CBAE88`, `sub_82CBAEAC`

### `Func_82CBAE88`

- **Address:** `0x82CBAE88` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). Calls: sub_82CBAEAC. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBAEAC`

### `Func_82CBAF00`

- **Address:** `0x82CBAF00` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CBAB80, sub_82CBAF34. First: `mr r3,r30`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBAB80`, `sub_82CBAF34`

### `Func_82CBAF0C`

- **Address:** `0x82CBAF0C` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_82CBAF34. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBAF34`

### `Func_82CBC888`

- **Address:** `0x82CBC888` · **Size:** 55 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (55 insns, SDK)

- **Does:** [SDK runtime] Body function, 55 insns, no prologue (mid-function target or leaf). Calls: sub_82CAA2E0, sub_82CBC930, sub_82CBC97C, sub_82CBC9C4, sub_82CC0750. First: `cmplwi cr6,r26,0`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAA2E0`, `sub_82CBC930`, `sub_82CBC97C`, `sub_82CBC9C4`, `sub_82CC0750`

### `Func_82CBC930`

- **Address:** `0x82CBC930` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82CBC9C4. First: `mr r8,r8`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBC9C4`

### `Func_82CBC940`

- **Address:** `0x82CBC940` · **Size:** 16 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (16 insns, SDK)

- **Does:** [SDK runtime] Body function, 16 insns, no prologue (mid-function target or leaf). Calls: sub_82CAF450, sub_82CC1C18. First: `lis r3,-16384`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAF450`, `sub_82CC1C18`

### `Func_82CBC97C`

- **Address:** `0x82CBC97C` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_82CBC9C4. First: `mr r8,r8`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CBC9C4`

### `Func_82CC04A4`

- **Address:** `0x82CC04A4` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). Calls: sub_82CC04E8. First: `li r11,0`; last: `b 0x82ca2c14`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CC04E8`

### `Func_82CD182C`

- **Address:** `0x82CD182C` · **Size:** 8 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (8 insns, SDK)

- **Does:** [SDK runtime] Body function, 8 insns, no prologue (mid-function target or leaf). Calls: sub_822F2020. First: `bl 0x822f2020`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_822F2020`

### `Func_82CD18AC`

- **Address:** `0x82CD18AC` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). Calls: sub_822F1DB0, sub_82CD1608. First: `bl 0x82cd1608`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_822F1DB0`, `sub_82CD1608`

### `Func_82CD7948`

- **Address:** `0x82CD7948` · **Size:** 4 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (4 insns, SDK)

- **Does:** [SDK runtime] Body function, 4 insns, no prologue (mid-function target or leaf). First: `lwz r11,12(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CD8650`

- **Address:** `0x82CD8650` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). First: `li r11,6`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CDBAE0`

- **Address:** `0x82CDBAE0` · **Size:** 5 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (5 insns, SDK)

- **Does:** [SDK runtime] Body function, 5 insns, no prologue (mid-function target or leaf). First: `mr r10,r3`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82CE5D98`

- **Address:** `0x82CE5D98` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). First: `lbz r10,60(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82E86188`

- **Address:** `0x82E86188` · **Size:** 17 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (17 insns, SDK)

- **Does:** [SDK runtime] Body function, 17 insns, no prologue (mid-function target or leaf). First: `lwz r9,8(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82E87A10`

- **Address:** `0x82E87A10` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). First: `lis r11,-32256`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82EF8D20`

- **Address:** `0x82EF8D20` · **Size:** 5 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (5 insns, SDK)

- **Does:** [SDK runtime] Body function, 5 insns, no prologue (mid-function target or leaf). First: `li r11,-1`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82F2C2E8`

- **Address:** `0x82F2C2E8` · **Size:** 5 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (5 insns, SDK)

- **Does:** [SDK runtime] Body function, 5 insns, no prologue (mid-function target or leaf). First: `lwz r11,28(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82F2C3C0`

- **Address:** `0x82F2C3C0` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). First: `lwz r11,28(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82F435A8`

- **Address:** `0x82F435A8` · **Size:** 9 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (9 insns, SDK)

- **Does:** [SDK runtime] Body function, 9 insns, no prologue (mid-function target or leaf). First: `lfs f0,72(r4)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_82FB7470`

- **Address:** `0x82FB7470` · **Size:** 4 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (4 insns, SDK)

- **Does:** [SDK runtime] Body function, 4 insns, no prologue (mid-function target or leaf). First: `lwz r11,24(r4)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_83000858`

- **Address:** `0x83000858` · **Size:** 103 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (103 insns, SDK)

- **Does:** [SDK runtime] Body function, 103 insns, no prologue (mid-function target or leaf). Calls: sub_82170CC8, sub_82CA3C68, sub_82CAB678, sub_82CAB770, sub_82CAC520. First: `cmpwi cr6,r27,0`; last: `b 0x82ca2c28`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82170CC8`, `sub_82CA3C68`, `sub_82CAB678`, `sub_82CAB770`, `sub_82CAC520`, `sub_830005D8`, `sub_83000728`, `sub_830009AC`

### `Func_830009AC`

- **Address:** `0x830009AC` · **Size:** 18 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (18 insns, SDK)

- **Does:** [SDK runtime] Body function, 18 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_830009F4. First: `mr r8,r8`; last: `b 0x82ca2c28`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_830009F4`

### `Func_83000AF0`

- **Address:** `0x83000AF0` · **Size:** 140 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (140 insns, SDK)

- **Does:** [SDK runtime] Body function, 140 insns, no prologue (mid-function target or leaf). Calls: sub_82170CC8, sub_8221EE38, sub_82CA6CF8, sub_82CAB678, sub_82CAB770. First: `lis r11,-31946`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82170CC8`, `sub_8221EE38`, `sub_82CA6CF8`, `sub_82CAB678`, `sub_82CAB770`, `sub_82CAF298`, `sub_82CAF558`, `sub_82CB8AE8`

### `Func_83000CF4`

- **Address:** `0x83000CF4` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_83000D40. First: `mr r8,r8`; last: `b 0x82ca2c2c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_83000D40`

### `Func_83001060`

- **Address:** `0x83001060` · **Size:** 79 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (79 insns, SDK)

- **Does:** [SDK runtime] Body function, 79 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB630, sub_82CAB770, sub_82CAF6C8, sub_83001184, sub_830011BC. First: `lwz r11,12(r30)`; last: `b 0x82ca2c24`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB630`, `sub_82CAB770`, `sub_82CAF6C8`, `sub_83001184`, `sub_830011BC`

### `Func_83001318`

- **Address:** `0x83001318` · **Size:** 42 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (42 insns, SDK)

- **Does:** [SDK runtime] Body function, 42 insns, no prologue (mid-function target or leaf). Calls: sub_82CA3C68, sub_82CA4E68, sub_82CAF720, sub_830013A8, sub_830013C0. First: `mr r3,r30`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA3C68`, `sub_82CA4E68`, `sub_82CAF720`, `sub_830013A8`, `sub_830013C0`

### `Func_83001514`

- **Address:** `0x83001514` · **Size:** 84 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (84 insns, SDK)

- **Does:** [SDK runtime] Body function, 84 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB630, sub_82CAB770, sub_82CAF6C8, sub_82CB5958, sub_8300164C. First: `lwz r11,12(r30)`; last: `b 0x82ca2c34`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB630`, `sub_82CAB770`, `sub_82CAF6C8`, `sub_82CB5958`, `sub_8300164C`, `sub_83001684`

### `Func_8300172C`

- **Address:** `0x8300172C` · **Size:** 35 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (35 insns, SDK)

- **Does:** [SDK runtime] Body function, 35 insns, no prologue (mid-function target or leaf). Calls: sub_82CAF6C8, sub_830017D8. First: `lwz r11,12(r30)`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAF6C8`, `sub_830017D8`

### `Func_83002974`

- **Address:** `0x83002974` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_82CA97A8, sub_82CC0728. First: `lwz r3,88(r11)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA97A8`, `sub_82CC0728`

### `Func_8300298C`

- **Address:** `0x8300298C` · **Size:** 6 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (6 insns, SDK)

- **Does:** [SDK runtime] Body function, 6 insns, no prologue (mid-function target or leaf). Calls: sub_82CA97A8. First: `bl 0x82ca97a8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA97A8`

### `Func_83002E84`

- **Address:** `0x83002E84` · **Size:** 42 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (42 insns, SDK)

- **Does:** [SDK runtime] Body function, 42 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4E68, sub_82CB0068, sub_83002F18, sub_83002F2C. First: `mr r3,r30`; last: `b 0x82ca2c3c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4E68`, `sub_82CB0068`, `sub_83002F18`, `sub_83002F2C`

### `Func_8300338C`

- **Address:** `0x8300338C` · **Size:** 40 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (40 insns, SDK)

- **Does:** [SDK runtime] Body function, 40 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4920, sub_82CA88E0, sub_83003414, sub_8300342C, sub_83003494. First: `lis r11,-31921`; last: `b 0x8300339c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4920`, `sub_82CA88E0`, `sub_83003414`, `sub_8300342C`, `sub_83003494`

### `Func_830033E0`

- **Address:** `0x830033E0` · **Size:** 19 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (19 insns, SDK)

- **Does:** [SDK runtime] Body function, 19 insns, no prologue (mid-function target or leaf). Calls: sub_82CA4920, sub_83003414, sub_83003494. First: `lwz r11,0(r30)`; last: `b 0x8300339c`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CA4920`, `sub_83003414`, `sub_83003494`

### `Func_830046B4`

- **Address:** `0x830046B4` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_830043D0, sub_83004708. First: `mr r3,r30`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830043D0`, `sub_83004708`

### `Func_830046C0`

- **Address:** `0x830046C0` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_83004708. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_83004708`

### `Func_83040FEC`

- **Address:** `0x83040FEC` · **Size:** 224 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (224 insns, SDK)

- **Does:** [SDK runtime] Body function, 224 insns, no prologue (mid-function target or leaf). Calls: sub_82B56750, sub_82C43198, sub_8301DE30, sub_83026048, sub_8302D8A8. First: `lwz r3,8(r30)`; last: `b 0x83040db8`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82B56750`, `sub_82C43198`, `sub_8301DE30`, `sub_83026048`, `sub_8302D8A8`

### `Func_8304100C`

- **Address:** `0x8304100C` · **Size:** 216 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (216 insns, SDK)

- **Does:** [SDK runtime] Body function, 216 insns, no prologue (mid-function target or leaf). Calls: sub_82B56750, sub_82C43198, sub_8301DE30, sub_83026048, sub_8302D8A8. First: `mr r8,r8`; last: `b 0x83040db8`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82B56750`, `sub_82C43198`, `sub_8301DE30`, `sub_83026048`, `sub_8302D8A8`

### `Func_8304101C`

- **Address:** `0x8304101C` · **Size:** 227 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (227 insns, SDK)

- **Does:** [SDK runtime] Body function, 227 insns, no prologue (mid-function target or leaf). Calls: sub_82B56750, sub_82C43198, sub_8301DE30, sub_83026048, sub_8302D8A8. First: `lwz r30,676(r31)`; last: `b 0x83040db8`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82B56750`, `sub_82C43198`, `sub_8301DE30`, `sub_83026048`, `sub_8302D8A8`

### `Func_830ACB28`

- **Address:** `0x830ACB28` · **Size:** 30 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (30 insns, SDK)

- **Does:** [SDK runtime] Body function, 30 insns, no prologue (mid-function target or leaf). First: `lwz r6,4(r4)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_830F07A8`

- **Address:** `0x830F07A8` · **Size:** 28 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (28 insns, SDK)

- **Does:** [SDK runtime] Body function, 28 insns, no prologue (mid-function target or leaf). Calls: sub_82CAB770, sub_82CAF450, sub_830F0838, sub_830F09F0. First: `lhz r11,0(r30)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82CAB770`, `sub_82CAF450`, `sub_830F0838`, `sub_830F09F0`

### `Func_830F07F8`

- **Address:** `0x830F07F8` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). Calls: sub_830F0838. First: `mr r8,r8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830F0838`

### `Func_830F0914`

- **Address:** `0x830F0914` · **Size:** 33 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (33 insns, SDK)

- **Does:** [SDK runtime] Body function, 33 insns, no prologue (mid-function target or leaf). Calls: sub_82240578, sub_82CB7DA0, sub_82CC1798, sub_830F0980, sub_830F09B8. First: `bl 0x82240578`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82240578`, `sub_82CB7DA0`, `sub_82CC1798`, `sub_830F0980`, `sub_830F09B8`

### `Func_830F1208`

- **Address:** `0x830F1208` · **Size:** 23 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (23 insns, SDK)

- **Does:** [SDK runtime] Body function, 23 insns, no prologue (mid-function target or leaf). Calls: sub_830F0C48, sub_830F1284. First: `mr r8,r6`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830F0C48`, `sub_830F1284`

### `Func_830F1228`

- **Address:** `0x830F1228` · **Size:** 15 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (15 insns, SDK)

- **Does:** [SDK runtime] Body function, 15 insns, no prologue (mid-function target or leaf). Calls: sub_830F1284. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830F1284`

### `Func_830FB43C`

- **Address:** `0x830FB43C` · **Size:** 28 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (28 insns, SDK)

- **Does:** [SDK runtime] Body function, 28 insns, no prologue (mid-function target or leaf). Calls: sub_830FA9D8, sub_830FB490. First: `lwz r11,236(r31)`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FA9D8`, `sub_830FB490`

### `Func_830FB490`

- **Address:** `0x830FB490` · **Size:** 7 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (7 insns, SDK)

- **Does:** [SDK runtime] Body function, 7 insns, no prologue (mid-function target or leaf). First: `mr r8,r8`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_830FB4A0`

- **Address:** `0x830FB4A0` · **Size:** 14 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (14 insns, SDK)

- **Does:** [SDK runtime] Body function, 14 insns, no prologue (mid-function target or leaf). First: `lis r11,-32248`; last: `b 0x82ca2c38`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_830FD788`

- **Address:** `0x830FD788` · **Size:** 41 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (41 insns, SDK)

- **Does:** [SDK runtime] Body function, 41 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_82B96C90, sub_830FD3B0, sub_830FD478, sub_830FD7A8. First: `lwz r3,772(r30)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_82B96C90`, `sub_830FD3B0`, `sub_830FD478`, `sub_830FD7A8`, `sub_830FD7E4`

### `Func_830FD7A8`

- **Address:** `0x830FD7A8` · **Size:** 33 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (33 insns, SDK)

- **Does:** [SDK runtime] Body function, 33 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_82B96C90, sub_830FD478, sub_830FD7E4. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_82B96C90`, `sub_830FD478`, `sub_830FD7E4`

### `Func_830FD7B8`

- **Address:** `0x830FD7B8` · **Size:** 30 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (30 insns, SDK)

- **Does:** [SDK runtime] Body function, 30 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_82B96C90, sub_830FD478, sub_830FD7E4. First: `lwz r30,132(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_82B96C90`, `sub_830FD478`, `sub_830FD7E4`

### `Func_830FD7C8`

- **Address:** `0x830FD7C8` · **Size:** 26 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (26 insns, SDK)

- **Does:** [SDK runtime] Body function, 26 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_82B96C90, sub_830FD478, sub_830FD7E4. First: `lwz r3,768(r30)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_82B96C90`, `sub_830FD478`, `sub_830FD7E4`

### `Func_830FD7E4`

- **Address:** `0x830FD7E4` · **Size:** 19 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (19 insns, SDK)

- **Does:** [SDK runtime] Body function, 19 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_830FD478. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_830FD478`

### `Func_830FD7F4`

- **Address:** `0x830FD7F4` · **Size:** 16 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (16 insns, SDK)

- **Does:** [SDK runtime] Body function, 16 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_830FD478. First: `lwz r30,132(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_830FD478`

### `Func_830FDCEC`

- **Address:** `0x830FDCEC` · **Size:** 20 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (20 insns, SDK)

- **Does:** [SDK runtime] Body function, 20 insns, no prologue (mid-function target or leaf). Calls: sub_82D0DAE0, sub_830FD760. First: `lwz r4,140(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82D0DAE0`, `sub_830FD760`

### `Func_830FDCFC`

- **Address:** `0x830FDCFC` · **Size:** 16 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (16 insns, SDK)

- **Does:** [SDK runtime] Body function, 16 insns, no prologue (mid-function target or leaf). Calls: sub_82D0DAE0. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82D0DAE0`

### `Func_830FDD08`

- **Address:** `0x830FDD08` · **Size:** 13 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (13 insns, SDK)

- **Does:** [SDK runtime] Body function, 13 insns, no prologue (mid-function target or leaf). Calls: sub_82D0DAE0. First: `lwz r11,132(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_82D0DAE0`

### `Func_830FE168`

- **Address:** `0x830FE168` · **Size:** 58 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (58 insns, SDK)

- **Does:** [SDK runtime] Body function, 58 insns, no prologue (mid-function target or leaf). Calls: sub_830FCE20, sub_830FDBD8, sub_830FDCB8, sub_830FDD48, sub_830FDE28. First: `lwz r5,260(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FCE20`, `sub_830FDBD8`, `sub_830FDCB8`, `sub_830FDD48`, `sub_830FDE28`, `sub_830FE220`

### `Func_830FE220`

- **Address:** `0x830FE220` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_830FDCB8. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FDCB8`

### `Func_830FE230`

- **Address:** `0x830FE230` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_830FCEA8, sub_830FDCB8. First: `lwz r4,244(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FCEA8`, `sub_830FDCB8`

### `Func_830FE2E8`

- **Address:** `0x830FE2E8` · **Size:** 50 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (50 insns, SDK)

- **Does:** [SDK runtime] Body function, 50 insns, no prologue (mid-function target or leaf). Calls: sub_830FCE20, sub_830FDBD8, sub_830FDCB8, sub_830FDD48, sub_830FE010. First: `lwz r5,88(r31)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FCE20`, `sub_830FDBD8`, `sub_830FDCB8`, `sub_830FDD48`, `sub_830FE010`, `sub_830FE380`

### `Func_830FE380`

- **Address:** `0x830FE380` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_830FDCB8. First: `mr r8,r8`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FDCB8`

### `Func_830FE390`

- **Address:** `0x830FE390` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_830FCEA8, sub_830FDCB8. First: `li r4,0`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_830FCEA8`, `sub_830FDCB8`

### `Func_83117EDC`

- **Address:** `0x83117EDC` · **Size:** 61 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (61 insns, SDK)

- **Does:** [SDK runtime] Body function, 61 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18, sub_831176A0, sub_831178D8, sub_83117BA0. First: `mr r6,r30`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`, `sub_831176A0`, `sub_831178D8`, `sub_83117BA0`

### `Func_83117F88`

- **Address:** `0x83117F88` · **Size:** 12 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (12 insns, SDK)

- **Does:** [SDK runtime] Body function, 12 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18. First: `mr r8,r8`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`

### `Func_83117F94`

- **Address:** `0x83117F94` · **Size:** 10 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (10 insns, SDK)

- **Does:** [SDK runtime] Body function, 10 insns, no prologue (mid-function target or leaf). Calls: sub_821F5F18. First: `lwz r30,84(r31)`; last: `b 0x82ca2c30`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_821F5F18`

### `Func_831C5D28`

- **Address:** `0x831C5D28` · **Size:** 8 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (8 insns, SDK)

- **Does:** [SDK runtime] Body function, 8 insns, no prologue (mid-function target or leaf). First: `lwz r11,16(r3)`; last: `blr`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

### `Func_831FFA08`

- **Address:** `0x831FFA08` · **Size:** 11 insns · **Category:** body · **Region:** SDK · **Confidence:** low

- **Role:** Func routine (11 insns, SDK)

- **Does:** [SDK runtime] Body function, 11 insns, no prologue (mid-function target or leaf). Calls: sub_831DF3D0. First: `mr r11,r3`; last: `b 0x831df3d0`.

- **Trigger:** Internal call — reached as part of normal engine/SDK operation, no direct player action.

- **Calls:** `sub_831DF3D0`

