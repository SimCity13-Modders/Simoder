# Private copied values and bounded storage

U3b implements the native version-1 copied-value wire contract from the separately
versioned SDK `Native/value-contract.md`. This private implementation does not
activate a native semantic facet, authority provider, Lua facade or scheduler.
There is one parser; future bindings consume its immutable representation rather
than creating another JSON parser.

`sc13::runtime::copied::Storage::Decode` accepts a caller-reviewed schema and a
nonzero **host-issued** owner/generation identifier. Decoding an identifier from
the input is never an authority admission. A `Ref` node holds only the validated
reference kind, slot and generation. Resolving a live context and retaining its
private lease belongs to a later integration unit.

## Representation and lifetime

An `Envelope` is an intrusive lease on one immutable allocation. Its contiguous
layout is `Block`, an array of `Node`, decoded text/bytes, and the original valid
wire bytes. Nodes preserve integer and binary64 bits without conversion through
double. List/map children use arena-local indexes and sibling links. Map keys are
decoded and compared by unsigned UTF-8 bytes. Null is a distinct tag. No closure,
facade, VM address, executable code or credentials object exists in this model.

`At` returns a const record or null. `Text` and `Key` check arena-relative ranges;
callers should use records obtained from that envelope. These are data accessors,
not authorization interfaces. Borrowed record pointers and string views remain
valid only while at least one envelope lease retaining that block exists.

`Encode` exposes the validated original wire as a borrowed immutable view. This
is a complete zero-allocation representation of an already-decoded envelope,
including its reviewed schema. It is **not** a general builder/serializer for
caller-created node graphs. Order and insignificant whitespace remain unchanged;
the SDK explicitly defines wire bytes as noncanonical and unsuitable as semantic
identity or signatures. A recipient requiring its own independent storage calls
`Decode` and pays for the simultaneous copy.

Input wire/schema bytes must remain alive and immutable throughout `Decode`.
Concurrent mutation of caller-owned input is a data race and violates that C++
contract. Using views from the existing output as inputs is supported: the old
lease remains retained internally until copying or failure has finished. Failures
always leave the output empty.

## Allocation and accounting proof

Pass one validates and counts using stack locals only. Tagged recursion is at
most 16; the exact JSON shape is consumed by tag-specific code, so arbitrary
additional object/array nesting cannot bypass this bound. JSON strings are
streamed, including escapes/surrogates and strict UTF-8 validation. Key comparisons
re-read two validated string tokens without a decoded scratch allocation. The
maximum local numeric/tag buffers are fixed. No generic JSON tree is allocated.

For a successful pass one, the exact requested copied-storage charge is:

```
sizeof(Block) + tagged_node_count * sizeof(Node)
              + decoded_text_and_bytes_count + input_wire_size
```

`sizeof(Block)` is aligned for `Node`, and `sizeof(Node)` is an alignment multiple;
there is no trailing-node padding omitted from this equation. Each term is bounded:
wire <= 1,048,576 bytes, nodes <= 4,096; retained decoded strings/keys/schema/bytes
consume fewer bytes than their disjoint corresponding input tokens. Their total
is bounded by the wire size. Thus the sum fits 32-bit `size_t` as well as the
64-bit host. `Node` offsets/counts also fit `uint32_t`. The second pass checks each
node and text write against the exact allocated regions before writing.

One atomic-under-mutex reservation checks both the 16 MiB aggregate and 4 MiB
owner ceilings **before** `operator new(charge, nothrow)`. If allocation fails,
the reservation is returned. An intrusive lease copy allocates nothing and does
not charge a second payload. Independently decoded copies each charge their full
block. The final lease destroys the block and returns its exact charge; no early
release occurs while another thread retains a lease. The pool state survives
destruction of its `Storage` handle until the last block is released.

Each pool has one fixed shared state with a mutex and 256 inline owner counters.
There is no dynamic map of owners or per-owner allocation. Exhausting those slots
returns `ResourceLimit`, and final release recycles them. This bookkeeping bound
is separate from the later scheduler's proposed 128 control-record pool. Storage
copies share the same pool. Future Core integration must supply the same runtime
pool to all transports/publications; constructing independent pools is not a way
to enforce a runtime-wide aggregate and is not the production integration yet.

The copied-storage limits account for requested allocation capacities, including
the allocation header and all retained wire/model copies. They do not purport to
measure process-private memory, allocator heap metadata, fixed pool state/shared
control-block overhead, thread stacks, Lua, native images or logs. Those fixed
bookkeeping allocations are created at pool construction, never per message.
The OS/CRT may consume additional heap bookkeeping bytes. No allocation is hidden
inside a per-message shared-pointer control block.

Task admission's future maximum-result reservation before effects is not provided
by a decoded value alone. U3e must reserve its bounded task/result storage before
commit and retain it until actual result ownership ends. U3b enables no operation
effects and cannot be used as proof of that later gate.

## Verification

`simoder-copied-value-tests` covers every tag, exact integer boundaries, signed
zero, finite-only f64, strict strings/UTF-8, duplicate keys (including escaped
spellings), contiguous JSON literals, ordering, invalid references, shape errors,
schema mismatch, all inclusive/exceeded SDK sizes, method limits, simultaneous
copies, owner/global exhaustion, owner-table recycling, alias lifetime, concurrent
leases and deterministic payload-allocation failure. A test-only nothrow allocator
interceptor does not introduce a production fault injection interface.

No claim of gameplay, Lua scheduling, permission enforcement or crash handling
follows from these native codec tests. Those remain separate accepted units.
