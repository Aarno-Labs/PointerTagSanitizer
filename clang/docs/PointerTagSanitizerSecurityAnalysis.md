# PTSan Security Analysis and Limitations

## Scope

This document covers the two configurations intended for strict deployments:

- ID16 strict mode, without LAM: `-fsanitize=ptsan -fsanitize-ptsan-strict-mode`
- ID32 strict mode: `-fsanitize=ptsan -fsanitize-ptsan-strict-mode -fsanitize-ptsan-32-bit-ids`

ID32 indicator support may be enabled when an application needs it. All linked
modules must use the same indicator ABI, and boundary files that can receive
both encoded and native pointers may also need `-ptsan-may-decode-raw`.

The assumed deployment is a statically linked, fully instrumented musl build
with global instrumentation, matching PTSan flags in every module, and at least
`-O1`. ASLR is enabled. The default ID randomization is enabled and is seeded
from system randomness.

## Threat Model

The attacker is assumed to:

- know the complete contents of the program binary, including its code and
  read-only constants;
- control untrusted program inputs and be able to exercise a memory-safety bug
  in instrumented code;
- repeat attempts and influence allocation behavior where the application
  provides enough control to groom allocations or force ID reuse; and
- use any pointer, address, ID, or metadata disclosure that the application
  makes available.

Knowing the binary does not automatically reveal per-process addresses, live
pointer IDs, ID32 origins, or other randomized runtime state. Those values are
only treated as known when the application or another vulnerability discloses
them.

The OS, kernel, and hardware enforcement of page permissions are trusted. The
PTSan runtime and metadata, together with code deliberately built without
PTSan access checks, are part of the trusted computing base.

The security objective is to prevent a memory-safety bug from reading or
modifying runtime data outside the live storage that its pointer is authorized
to access. The contents of the binary itself are not confidential. PTSan exits
after detecting an error; it does not claim to preserve availability after a
memory error.

PTSan is an allocation-bounds and allocation-lifetime defense. It is **not** a
general C or C++ undefined-behavior detector. A standards violation is not, by
itself, a PTSan security failure.

## Security Limitations of the Model

This section answers: assuming the program builds and runs correctly under
PTSan, what memory-safety risks remain because of the protection model itself?

### Both ID16 and ID32

#### Bounds Are Per Allocation, Not Per Subobject

PTSan accepts any access contained within the allocation associated with the
pointer's ID. It does not detect overflows between subobjects, or between
logical objects that share one allocation. Variadic register-save slots
likewise share backing bounds rather than receiving per-argument bounds.

In-allocation corruption of an adjacent data or control field is therefore not
detected unless it later causes an allocation-level violation.

Subobject protection is potentially more practical with ID32's larger ID
namespace, but it would need additional compiler semantics and metadata. In
practice, many programs deliberately cross source-language subobject
boundaries, so a strict implementation would have substantial
compatibility costs.

#### PTSan Does Not Track Initializedness or Types

PTSan does not track whether bytes are initialized or whether an in-bounds
object has valid type or state. It therefore does not detect uninitialized
reads, in-allocation type confusion, or corrupt object state that remains
within the allocation.

This is distinct from subobject bounds: even exact member bounds would not
establish that the bytes contain a valid value of the expected type.

#### Argument and Environment Strings Share Broad Bounds

Argument and environment strings are registered as one broad contiguous range
to support process-title rewriting. Cross-string reads and writes within that
range can pass. Unlike read-only constants, these bytes can be writable runtime
state, so this is a real relaxation of allocation-level isolation.

#### Stack Slot Reuse Within a Live Frame

PTSan marks stack allocations as dead on function exit. This means that, within a still-active frame, a UAF on a dead stack buffer will not be caught.

#### Forged Pointers Use Current Metadata

PTSan does not authenticate pointer representations or record how they were
derived. A forged pointer is accepted whenever its ID and encoded address or
payload satisfy the current metadata for that ID. The ways an attacker can
construct such a pointer, and the resulting probabilities, differ between
ID16 and ID32 and are discussed below.

#### Racy Lifetime Changes Can Occur After a Check

PTSan cannot guarantee detection when another thread retires or replaces an
allocation after the relevant check has passed but before the protected access
executes. Check hoisting can widen this interval.

#### Trusted Computing Surface and Unchecked Accesses

Memory accesses in unchecked code are outside PTSan's enforcement:

- Assembly memory accesses are not instrumented.
- Selected musl allocator, string, and syscall internals are compiled without PTSan
  checks. Wrappers check user ranges, but the implementations of
  those functions are trusted.
- libunwind is built without PTSan checks; it performs too many PTSan-violating
  actions intentionally for PTSan checking to be practical.
- Four libc++abi sources are unchecked: `cxa_personality.cpp`,
  `cxa_exception.cpp`, `cxa_exception_storage.cpp`, and
  `private_typeinfo.cpp`. The last also implements `dynamic_cast`, making RTTI
  traversal through an invalid object pointer a known unchecked boundary.

These components are part of the trusted computing base. A bug
inside one can bypass PTSan checks even when application code is fully
instrumented.

#### Unmodeled Syscalls

Unmodeled syscalls forward arguments unchanged. Ordinary encoded pointers
normally fail in the kernel; a forged argument containing a usable native
address could bypass PTSan's buffer checks at this boundary.

#### Errors Outside the Memory-Access Model

PTSan does not detect integer overflow that never causes an invalid memory
access, logic or authorization errors, injection bugs, races, deadlocks, or
control-flow violations. Other defenses are needed for those classes.

### ID16

#### Temporal Reuse


For a fixed ID16 pointer containing ID `I` and low-48-bit address `A`, an
access of width `N` passes when `I`'s current base `B` and span `S` satisfy:

~~~text
B <= A
A + N <= B + S
~~~

Free or stack retirement clears `I`'s span, so any access through the stale
pointer fails until that numeric ID is reused; it then passes only if the
replacement bounds cover the access at `A`. A missed temporal error therefore
requires both ID reuse and address overlap.

For the purposes of this discussion, we assume an attacker is already capable
of a non-PTSan UAF; we assume they can already groom the allocator. Thus, in
order for an attacker to evade PTSan's UAF protection, they need to arrange
the replacement allocation to receive the stale pointer's ID.

PTSan randomizes ID recycling for its main ID pool, and works through the whole pool
before recycling. This means that an attacker has approximately a `1/size-of-recycled-ids`
chance of getting the correct ID onto the correct allocation for a UAF (or double-free etc).

ID16 has 2^16 total IDs. Discounting reserved IDs, there are
approximately 59,400 general-purpose IDs. Thus, with few IDs in use, the odds
of a temporal bypass are roughly 1/50,000. *As ID pressure increases, so do these
odds*; if all but one ID are in use, the one ID will be instantly recycled and
PTSan provides no temporal protection.

Fast-stack IDs follow a different policy. Returns clear their bounds and delay
cursor rewind in batches, but reuse within a thread's reserved bucket is
deterministic. The general-pool recycling probability does not apply. If an
attacker can control stack depth to arrange a later frame to reuse both
the ID and overlapping stack
storage, an old ID16 pointer can become valid again. Heap allocations cannot
claim those reserved stack IDs; grooming must reach an appropriate stack
allocation.

#### Pointer Forgery

An attacker can forge an ID16 pointer through sufficiently large tagged-pointer arithmetic, or through integer, union, or bytewise manipulation of its representation. The resulting pointer must satisfy the bounds condition described under temporal reuse, but no ID reuse or allocator grooming is required.

For a target access authorized by one ID, a uniformly random 16-bit ID guess succeeds with probability 1/65,536. ID pressure does not increase this probability. This assumes the attacker can already reach the target address—by knowing it, guessing it, or using a predictable offset from an existing pointer.

Fresh general-purpose IDs come from randomly selected 512-ID chunks with a
random starting offset. Knowing allocation order alone therefore does not
reduce the blind-guess uncertainty. Fast-stack IDs start at their chunk's first
usable ID; if the target's position is predictable, the attacker need only guess
the chunk, leaving approximately seven bits of uncertainty without ID disclosure.

IDs within a chunk remain sequential, with wraparound for general-purpose IDs,
so predictable relationships between allocations can still help an attacker
construct a forged pointer from an existing one. Recycled chunks also receive
a random rotation.

### ID32

#### Temporal Reuse

An ID32 pointer stores an ID and a 32-bit payload. Each allocation has an
**origin**, a normally randomized value stored in PTSan's metadata; the payload
is this origin plus the pointer's byte offset within the allocation.

For a pointer containing ID `I` and payload `P`, let `B`, `O`, and `S` be that
ID's current allocation base, origin, and span. An access of width `N` uses:

~~~text
offset  = P - O
address = B + offset

passes if 0 <= offset and offset + N <= S
~~~

Free or stack retirement clears `I`'s span, so a fixed stale pointer fails
until numeric-ID reuse. After reuse, it decodes relative to whichever
allocation now owns `I`, even if that allocation is at a different address.
The replacement's fresh ChaCha12-generated origin determines whether the
stale payload passes the check and where within the allocation the access
lands.

Origins are below 2 GiB and use a 4096-byte quantum by default for compatibility.
Stronger alignment increases this quantum and reduces origin uncertainty.

For an allocation of `size < 4096`, alignment no greater than 4096, and an
undisclosed random origin, the probability of an accepted stale access after ID
reuse is at most roughly `2^-19`. Protection degrades with allocation size and
can become ineffective near the 2 GiB allocation limit; allocations of 2 GiB
or larger are rejected.

For the general-purpose pool, hitting a **chosen allocation** additionally
requires that allocation to receive the stale ID, with roughly a
`1/size-of-recycled-ids` chance. With about 65,536 recycled candidates (the
default target), this contributes a `2^-16` factor, giving a combined
small-object bound of roughly `2^-35` for that allocation. This recycling
factor does not apply to fast-stack IDs.

ID32 fast frames clear spans and immediately rewind their stack-ID cursor on
return. A subsequent frame can reuse those IDs predictably; protection then
comes from the new frame's random salt, not a randomized choice of recycled ID.

A pointer decoded before reuse and later retagged with its stale ID using
current metadata loses the old-origin protection; acceptance then requires ID
reuse and address overlap, as in ID16.

#### Pointer Forgery

An attacker can forge an ID32 pointer through integer, union, or bytewise
manipulation of its representation. Ordinary pointer arithmetic cannot change
the ID: GEPs that might overflow the payload are stripped before arithmetic
and retain the original ID when retagged. The resulting pointer has the same
constraints as temporal errors above, but no ID reuse or allocator grooming
is required.

For a chosen allocation with one authorizing ID, a uniformly random ID guess
succeeds with probability `2^-32`, or `2^-31` with indicator support. This uses
the full ID namespace rather than the recycled pool; ID pressure alone does
not increase these blind-guess probabilities.

Fresh general-purpose IDs come from randomly selected 65,536-ID chunks with a
random starting offset, so allocation order alone does not reduce these
blind-guess probabilities. Fast-stack IDs start at their chunk's first usable
ID; predictable positions reduce ID uncertainty to about 16 bits, or 15 with
indicator support.

IDs within a chunk remain sequential, with wraparound for general-purpose IDs,
so predictable relationships between allocations can still help an attacker
forge an ID from an existing pointer. The payload must still satisfy the origin
condition above.

## Compatibility and Deployment Limitations

This section answers a different question: can an otherwise valid application
build and behave normally under PTSan? These issues can reject or change a
program, but do not imply that PTSan accepted an unauthorized memory access.

### Both ID16 and ID32

#### Supported Build and ABI

- PTSan requires 64-bit pointers and an x86-64 or AArch64 Linux ELF target.
- Strict mode relies on instrumented musl. It has only been tested with said instrumented musl statically linked.
- Every application module and dependency must be compiled under PTSan, with compatible settings.
- Prebuilt native libraries generally cannot be mixed into the strict pointer
  ABI without an explicit boundary.
- PTSan expects optimization, particularly `mem2reg`; builds below `-O1` are
  unsupported.
- Arbitrary linked assembly is unsupported. If assembly is deliberately
  included, its accesses are also outside the security enforcement.
- Inline assembly may receive encoded addresses when arithmetic or other
  transformations hide pointer inputs from PTSan's boundary recognition.

#### ID Pool Limits

PTSan exits if it runs out of IDs. For ID16 mode, this is plausible for some
large programs; approximately 1/3 of the SPEC ref workloads run out of IDs with
a pool of ~65,000. For ID32 mode, this is much less of a concern; we have never
seen a program use more than 3,000,000 simultaneously live IDs, out of
approximately `2^32` IDs, or `2^31` with indicator support.

#### Pointer Representation Conflicts

PTSan uses the high bits of every encoded data pointer: bits 48--63 in ID16
and bits 32--63 in ID32. Applications, runtimes, and other pointer-tagging
schemes cannot independently store their own state in those bits.

#### Numeric Pointer Idioms Are Recognized Locally

PTSan preserves ordinary within-allocation pointer arithmetic. Programs that
use pointers as numeric addresses—for ordering, subtraction, hashing, or
alignment—depend on PTSan recognizing those operations and decoding their
operands. Intervening arithmetic or storage can hide these patterns and produce
incorrect results.


#### Kernel and Process Interfaces

Unsupported or incomplete interfaces include forms of `ioctl`, socket options,
`prctl`, `semctl`, `mount`, `ptrace`, quota operations, `clone`, `vfork`,
alternate signal stacks, robust lists, `mremap`, `shmat`, `shmdt`, `fpregs` and
`munmap`. Most of these are supported to some extent but not fully.  Known-but-unsupported forms fail closed by terminating the
process.

General multithreaded `fork`-and-continue behavior is not guaranteed. The
supported process model is narrower than ordinary Linux.


#### Compiler and Code-Generation Gaps

- Some target-specific memory intrinsics, including explicit AVX2 gather
  intrinsics, remain unsupported.
- Compiler-generated memory operations introduced after the PTSan pass can
  lack IDs. The strict-musl
  [`frexp` XFAIL](https://github.com/Aarno-Labs/PointerTagSanitizer/blob/main/compiler-rt/test/ptsan/Musl/frexp_libcall_strict_xfail.c)
  is a valid-program rejection caused by a late backend temporary.
- Some AArch64 SVE uses, including taking the address of a scalable vector on
  the stack, are unsupported.

Host-glibc non-strict mode also has unresolved late-loaded global/TLS
registration and early-constructor thread cases. Those are outside the default
static-musl scope.

#### ID Leakage Race Condition
A signal followed by a nonlocal exit in the short interval between acquiring
and publishing a stack or nonstack ID can permanently leak that ID. Its bounds
have not been published, so this does not authorize an invalid access, but
repetition can accelerate ID exhaustion.

### ID16

#### Representation Compatibility

- ID16 retains only the low 48 native address bits. Mappings at or above
  `2^48`, including LA57/high fixed mappings, cannot be represented faithfully.
- Stripping an opaque all-ones pointer sentinel produces the low-48 address
  mask and can alias a different value:
  [`strip_opaque_all_ones_false_positive.c`](https://github.com/Aarno-Labs/PointerTagSanitizer/blob/main/compiler-rt/test/ptsan/strip_opaque_all_ones_false_positive.c).

### ID32

#### Representation Compatibility

ID32 strict mode can run with or without an indicator bit. Without an indicator
bit, all pointers are assumed to have been given an ID, and are unconditionally
decoded. This generally works, and is faster and has 2^32 IDs. Since an indicator
bit takes an ID bit space, indicator mode only has 2^31 IDs.

In indicatorless mode, raw function pointers above 4 GB
can be decoded as allocation pointers:
[`id32_high_function_pointer_xfail.c`](https://github.com/Aarno-Labs/PointerTagSanitizer/blob/main/compiler-rt/test/ptsan/Musl/id32_high_function_pointer_xfail.c).

Additional ID32 limitations are:

- Every allocation and supported in-place resize must be strictly smaller than
  2 GiB.
- An effective alignment used for runtime origin selection must be strictly
  below 2 GiB; exactly 2 GiB leaves no legal nonzero origin salt.
- Full-width metadata reserves approximately 84 GiB of *virtual* address space.
  Physical pages are populated lazily, but the virtual reservation can conflict
  with constrained or fixed mapping layouts.
- Programs that reload saved pointer values as opaque identities can break when
  PTSan interprets them using current-process metadata. Blender's serialized
  pointer identities are an example.
- Each allocation has at least one origin quantum (4 KiB by default) of representable negative-offset headroom. A farther-before-base
  pointer that must be re-encoded becomes null under compiler-generated strict
  packing. Temporary raw arithmetic that returns to the representable range
  can still work.
- The per-thread runtime stack-ID log has a finite configurable capacity which
  is less than the total number of IDs (2^20 vs 2^32) and aborts if exceeded.
- Negative pointer-valued sentinels can alias after decoding:
  [`id32_negative_pointer_sentinel.c`](https://github.com/Aarno-Labs/PointerTagSanitizer/blob/main/compiler-rt/test/ptsan/id32_negative_pointer_sentinel.c).

## Pathological Overhead Cases

### Both ID16 and ID32

Pointer chasing through linked lists, trees, and similar structures can require
new bounds metadata at every step, with little opportunity to hoist checks or
reuse metadata across nodes. When each node does little useful work, checking
and metadata cache misses can dominate execution time.

### ID16

#### Pointer Chasing

Each newly loaded pointer needs its ID masked off before dereferencing, adding
an operation to the chain of dependent node loads. Bounds lookups and checks
add further work and cache traffic. The raw address itself needs no metadata
load, so the CPU can overlap more of this work than in ID32.

LLVM MultiSource's `Ptrdist/ks` is a concrete example. It runs at ~115% overhead, while the total MultiSource geomean is ~25%.

### ID32

#### Pointer Chasing Requires Serial Metadata Loads

ID32 needs the allocation's decode bias from the bounds table to reconstruct
each node's address. A newly loaded pointer supplies the ID for that metadata
load; the load must finish before the next node can be dereferenced. This adds
a dependent memory load at each hop, making pointer chasing especially costly
when metadata misses in cache.

Like ID16, `Ptrdist/ks` is a good example: It ends up with ~200% overhead,
while Multisource geomean only rises to 30%.

## Things That Look Strange but Are Not Security Problems

### Shared IDs for Read-Only Binary Contents

Read-only constants in the same nonwritable ELF load region can share an ID and
region-wide bounds. An out-of-bounds read from one constant may therefore reach
another constant in that region.

This is not a confidentiality problem under the threat model: the attacker
already knows every byte of the binary. It is not an integrity problem because
the OS mapping is not writable. Individual IDs would consume resources without
protecting new information or writable state.

### Writable PTSan Metadata Is a TCB Property, Not an Immediate Bypass

The bounds, origin, and allocation-type tables are ordinary writable mappings.
They must be writable because the runtime updates them as objects are created
and retired. They are not isolated with MPK or pkeys.

A correctly checked application pointer cannot address these mappings: no
application allocation's bounds include them. Corrupting the tables therefore
requires an already-unchecked arbitrary write, such as a bug in trusted
assembly, the runtime, an unchecked library component, or an unmodeled kernel
boundary. At that point the attacker has already crossed PTSan's trusted
boundary.

Metadata isolation could provide useful defense in depth against trusted-base
bugs, but its absence is not an independent way for an ordinary checked
out-of-bounds access to disable PTSan.

### Standard-Only Undefined Behavior

PTSan does not need to diagnose behavior solely because the language standard
invalidates it. Examples include:

- an old alias that still addresses the same live bytes after a same-address
  in-place `realloc`;
- pointer provenance, comparison, or subtraction rules that do not change the
  actual memory accessed;
- forming an invalid or one-past pointer that is never dereferenced; and
- source-language lifetime violations where the implementation continues to
  use the same live storage and no allocation or security boundary is crossed.

Such behavior matters here only when it creates a real allocation escape,
accesses released or replaced storage, corrupts another live object, changes a
native-boundary address, or defeats a PTSan check.
