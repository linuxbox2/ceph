# Rename: why it is hard, and why nsfs can do it anyway

## Purpose

`rgw_rename()` follows the NFS operation, whose contract is atomicity
with respect to other operations "where possible".  RGW cannot honour
that in general, and the reason is structural rather than an
implementation shortcoming.  This document records the structural
argument, the property that exempts nsfs from it, what the existing tests
were really telling us, and the semantics chosen for it.

Handle identity is the axis underneath most of what follows and is
treated separately in [HANDLE_IDENTITY.md](HANDLE_IDENTITY.md) — including
the measured result that a filesystem's own persistent handle *is*
rename-invariant, which would remove the constraint in §5.3.

Sibling documents: [DESIGN.md](DESIGN.md),
[ETAG_STRATEGY.md](ETAG_STRATEGY.md),
[NOOBAA_VARIANCE.md](NOOBAA_VARIANCE.md).

---

## 1. Why S3 cannot rename

S3 has no rename operation, and the absence is not an oversight.  A key
is an opaque string; the `/` in `a/b/c.txt` is a display convention that
`ListObjects` exposes through `prefix` and `delimiter`.  There is no
directory to move.

So renaming what a client sees as a directory means re-keying every
object beneath it.  That is unbounded work — a prefix may cover millions
of objects — and it cannot be made atomic, because there is no single
object whose mutation commits the whole change.  `RGWLibFS::rename()`
says as much in its own comment:

```
/* forbid renaming of directories (unreasonable at scale) */
```

and returns `-EPERM` for a directory.  For a file it does a
`RGWCopyObjRequest` followed by an unlink — O(size), not atomic, and it
loses the object's identity.

On rados this is the worst case.  The bucket index *is* the namespace, so
re-keying means rewriting N index entries, and version ids are stored
data, so preserving object identity across the move means deliberately
rewriting identity.  Development work moving namespace metadata into an
external store is expected to change this, at which point rados could
rename a metadata subtree — see §6.

---

## 2. Why nsfs is different

`resolve_parent_dir()` (`rgw_sal_nsfs.cc:768`) walks the key on `/` and
creates a real `Directory` for each component.  An S3 key `a/b/c.txt` is
genuinely `<bucket>/a/b/c.txt` on the filesystem.

The property that matters is stronger than "we have directories":

**An object's key is derived from its position in the tree, not stored
anywhere.**  `fill_cache()` composes it — `decode_obj_key(path_prefix +
get_name())` (`:1325`) — and no on-disk attribute contains a path.  The
nsfs xattr set is `bucket_info`, `mp_upload`, `object_type`,
`multipart_part_count`, `multipart_part_sizes`, `version_id`,
`delete_marker`, `non_current_timestamp`; the RGW attrs beside them
(etag, ACL, content-type) are equally position-independent.

Therefore moving a directory re-keys everything beneath it with no
per-object work.  One `renameat()`:

- **atomic**, by the filesystem's own guarantee, against concurrent
  operations on that name;
- **O(1)** in the number of objects beneath, where rados is O(N).

**That holds for a prefix, not for a single versioned object.**  nsfs puts
an object's history beside it rather than inside it —

```
<bucket>/photo.jpg                    <- current
<bucket>/.versions/photo.jpg_<verid>  <- older versions
```

— so renaming one versioned object is the leaf plus N version entries:
N+1 renames, and the set is *not* atomic.  Interrupted midway it leaves
versions orphaned under the old name.  Still cheap, since nothing but
metadata moves, but the atomicity claim above belongs to prefix rename
only.

| case | cost | atomic |
|---|---|---|
| unversioned object | 1 rename | yes |
| prefix / directory | 1 rename | yes |
| versioned object | 1 + N versions | **no** |

This is a consequence of our layout, not of versioned rename as such.
posix is heading for objects-as-directories, where an object's versions
live *inside* the object's own directory — one `renameat` moves the object
and its whole history atomically, and the row above disappears.  Noted as
context rather than a proposal:  rgw-standalone owns that schema, and
whether nsfs should follow is a separate question from rename.

Prefix fanout is planned there too — splitting wide directories by prefix
— and whether that preserves single-`renameat` prefix rename depends on
where the split lands relative to the logical directory.  Deliberately not
designed here;  treat it as an unknown that will need answering when it
arrives.

Three further properties fall out without special handling:

- `.versions/` and `.shadow/` are created with `mkdirat` on the
  directory's own descriptor (`:570`, `:692`), so they are children and
  move with it.
- **Version ids and non-MD5 etags survive.**  Both are
  `mtime-<base36>-ino-<base36>`, and renaming a directory touches neither
  its children's mtimes nor their inodes.  Object identity is preserved
  for free, which on rados would have to be reconstructed.
- The `.folder` sentinel carrying a directory's own attributes moves with
  the directory.

**posix caveat.**  In this tree posix derives keys from `path_prefix`
the same way and stores no key on disk, so the same approach would work.
The rgw-standalone direction — objects-as-directories with fanout —
may remove that property if names are hashed into fanout buckets, in
which case the hierarchy stops being the index.  Confirm with
rgw-standalone before assuming posix can or cannot follow.

---

## 3. What the tests said, and what was actually wrong

An earlier revision of this note reported all three tests in
`ceph_test_librgw_file_rename` failing on nsfs on a clean root, and
concluded that the v1 create sequence had been broken by FSIO — that
`rgw_open`/`rgw_write`/`rgw_close` no longer worked and the fix was a
compatibility decision about the v1 API.

**That was wrong.**  Rename works today for files, cross-bucket included;
the suite was reporting on itself.  Fixed in "test: open for write and for
create when creating an object".

`make_object()` called `rgw_open(fs, fh, 0 /* posix flags */, 0)` — that
is `O_RDONLY` with no `RGW_OPEN_FLAG_CREATE`.  On a driver with an FSIO
view `rgw_open()` opens the file for real, so it returned `-ENOENT`;
`rgw_write()` then had no open and returned `-EPERM`; nothing was written;
and every rename failed `-ENOENT` on a source that did not exist.
`librgw_file_nfsns.cc` has always passed
`O_RDWR` with `RGW_OPEN_FLAG_V3|RGW_OPEN_FLAG_CREATE`, and passes.

Two things made this hard to see, and both are the same shape:

- `make_object()` assigned four results to one `ret` and returned the
  last, so it reported `rgw_close()`'s status and swallowed the two
  failures that mattered.
- `RGWLibFS::rename()`'s step 0 declares its own `int rc` inside the
  `case 0:` block, shadowing the function's `rc`, which was initialised to
  `-EINVAL`.  A failed copy therefore returns `-EINVAL` and the real errno
  is visible only at `debug_rgw=1`.  **Still unfixed** — two lines, no
  design content.

The suite was also not re-run safe:  fixed names, `-EEXIST` from
`rgw_mkdir` on a second run, three failures for that instead.  Also fixed.

### 3.1 What the green suite does not tell you

It asserts only that the destination resolves.  Nothing checks that the
source is gone, that content survived, or that the etag or version id is
unchanged — so a rename that copied and forgot to delete passes all three.
That matters more now that it is green, because green invites trust.  Any
rename work wants those assertions first;  inode equality across the move
is the direct control that distinguishes a real rename from
copy-then-delete.

### 3.2 An unwired flag, noticed on the way

`RGWFileHandle` carries `FLAG_CREATING` with `creating()`,
`open_for_create()` and `clear_creating()` (`rgw_file_int.h:327, 730,
795, 800`), and nothing in the tree calls any of them.  It is not the
mechanism above — `RGW_OPEN_FLAG_CREATE` is, and it works — so this is a
separate question:  whether handle *state* ("this handle names an object
that does not exist yet") expresses something the per-open request flag
does not, or whether it is redundant and should go.  Unresolved;  recorded
so it is not mistaken for the cause of anything.

## 4. The semantic question

This is the part that implementation cannot decide.  S3 has no rename, so
what does an S3 client see afterwards — particularly under versioning,
where the objects being moved have version history?

**(a) Namespace operation, transparent to S3.**  Objects appear at the
new keys with version history intact, because version ids survive the
move.  The only option that keeps atomicity and O(1).  But no S3
implementation moves version history, so an S3 client observes something
the API cannot express.  A deliberate, documented divergence — the same
category as the `check_empty` decision.

**(b) Copy plus delete-marker.**  New keys receive new versions, old keys
receive delete markers.  Exactly correct S3 semantics, and it discards
every advantage in §2.  This is today's behaviour.

**(c) Refuse under versioning.**  Rename on unversioned buckets only,
`-EPERM` otherwise.  Honest, cheap, and leaves versioned buckets — the
configuration this driver is being built for — without the operation.

### 4.1 What (a) means concretely

An S3 client sees the object leave the old key and appear at the new one,
carrying its versions:  `ListObjectVersions` at the new key returns the
same versions with the **same version ids**, since ids are
`mtime-<base36>-ino-<base36>` and a rename changes neither.

Cross-bucket follows a rule rather than a fudge:  history can only move
somewhere able to hold it.

- source has no history -> any target;
- source has history -> target **versioned or suspended**:  history moves;
- source has history -> target **unversioned**:  refuse, *unless* the
  caller asks for it explicitly.

Suspended qualifies because such a bucket still holds non-current
versions, it merely stops minting new ones.

The explicit case is worth having rather than a flat refusal:  flattening
an object to its current version is a legitimate thing to want, and what
makes it dangerous is doing it *silently*.  So gate it on a flag —
`rgw_rename()` already takes a `uint32_t flags` and defines only
`RGW_RENAME_FLAG_NONE`, so this is additive with no ABI change.  With the
flag the move slices the history off, keeping the current version;
without it the rename fails rather than discarding anything.

The granularity that falls out is the right one.  `rename(2)` has no such
flag, so **no client can ask for slicing on a particular `mv`** — an NFS
user moving an object between buckets gets an error, never quiet history
loss.  But it is not unreachable from NFS either:  Ganesha has places to
bind policy, the Export block among them, so an administrator can decide
that a given export slices, once and deliberately, and the FSAL passes the
flag.  Account-scoped properties would give a second binding point when
they exist.

That is the correct shape for this:  a standing administrative decision
rather than a per-operation one, made by someone who can see what the
export is for, and never inferred from a client's `mv`.

**Recommendation: (a), stated as a divergence.**  For a gateway whose
primary interface is NFS, the filesystem's semantics are the contract and
the S3 view is derived.  (b) is what we have and it is not worth keeping.
(c) is defensible as a first step if (a) is contentious, and it is
strictly on the way to (a).

---

## 5. What has to be built

Assuming (a), and assuming §3 is resolved first:

1. **A rename path in the driver.**  `FSStrategy` gains a rename, and
   nsfs implements it as `renameat()` between resolved parent
   descriptors.  Cross-bucket is the same call — buckets are top-level
   directories — provided both are on one filesystem; `EXDEV` needs a
   defined answer rather than a copy fallback that silently reintroduces
   O(size).
2. **Listing-cache invalidation.**  LMDB entries are keyed by composed
   name, so every entry beneath a renamed prefix is stale.  Purging the
   bucket's cache and letting the next LIST rebuild from the store is
   correct by construction — the path a cold bucket already takes — and
   costs one rebuild with no per-object work in the rename itself.
   Rewriting the affected key range instead is an optimisation, not a
   requirement.
3. **Invalidation — the part that already has design behind it.**  See
   §5.2;  it is the substantive piece, not a detail.
4. **Keep the open-file refusal**, and work out whether it extends to a
   subtree.  See §5.3.
5. **Directory rename** stops being `-EPERM` on drivers that report the
   capability, and stays `-EPERM` elsewhere.  It should be a driver
   capability, not an `#ifdef` — rados may acquire it later (§6).

### 5.2 Invalidation

What exists today, and it is deliberate:  `rename()` calls `set_times(t)`
on **both** `src_fh` and `dst_fh`, moving ctime/mtime/atime on the two
directory handles.  A Linux NFS client watches a directory's change
attribute, so moving it is what makes the client drop cached readdir and
lookup results and re-resolve — which is how a client detects a moved
object.  This is the mechanism;  it is not incidental.

The upcall is a separate surface.  `RGWFileHandle::invalidate()` fires
`fs->invalidate_cb(arg, fh_hk)`, registered through
`rgw_register_invalidate()`, and tells the consumer to drop a cached
handle.  Today its only caller is the GC path (`rgw_file.cc:1607`), on
readdir expiry, guarded so it does not fire mid-readdir-cycle.  `rename()`
does not use it.

**The handle is derived from the path, and that is the crux.**  Lookup is
by `fh_key`, a hash tuple rather than a name comparison — but the tuple is

    fh_hk.bucket = XXH64(tenant : bucket)
    fh_hk.object = XXH64(tenant : full-path-from-bucket-root)

`make_fhk()` composes the object half through `make_key_name()`, which is
`full_object_name() + "/" + name`.  So the key is position-dependent even
though nothing compares names.

`rgw_fh_hk` is not private bookkeeping.  It is public API
(`include/rados/rgw_file.h:51`), it *is* `struct rgw_file_handle`'s
identity — the header calls it a "content-addressable hash" — and
`rgw_lookup_handle()` takes one.  That is the path by which a consumer
turns a wire NFS filehandle back into a live handle.

Two consequences, and the first corrects a natural assumption:

- **Clients are not automatically fine.**  In a real filesystem an NFS
  filehandle names the inode, so it survives a rename;  here it is
  content-addressed over the path, so a rename *changes the object's
  filehandle*.  A client holding the old one gets nothing back from
  `rgw_lookup_handle()`.  This is why the invalidation work was
  load-bearing rather than a nicety:  the handles genuinely do go stale,
  and the change-attribute bump is what drives the client to re-resolve by
  name before it tries to use one.
- **Directory rename multiplies it.**  Every descendant's
  `full_object_name()` changes, so every descendant's key changes — for
  cached handles inside librgw and for whatever filehandles clients are
  holding.  Any client with files open beneath the subtree is exposed to
  ESTALE unless the rename drives invalidation across the whole moved
  subtree.

The addressing scheme should stay as it is;  making keys
position-independent would be a far larger change and is not wanted.  The
implication is that rename must *deliberately move handles* — re-key each
cached handle beneath the subtree and drive the invalidations that make
clients re-look-up — rather than assume anything survives on its own.

This also refines the cost claim in §2.  The filesystem work is O(1):  one
`renameat()`, no per-object I/O.  The bookkeeping is O(cached handles
beneath the subtree), which is bounded by the handle cache rather than by
the number of objects, and is memory-only.  That is still categorically
better than rados's O(N) index rewrite, but it is not free, and it is the
part that needs care.

### 5.3 Why open files are refused — the guard is correct

An earlier revision of this note called the `-EPERM` on open files an
artefact of copy-then-unlink with no semantic behind it.  That is wrong,
and the reason follows directly from §5.2.

**What an NFS client has open is a filehandle**, and the filehandle is
exactly the bits that change when the object moves, because it is a hash
of the path.  There is no transformation the client can follow:

- it holds the *old* bits, and nothing pushes it new ones;
- for an already-open file it has no name to re-resolve from — under v4 it
  may have opened by handle and never known one;
- `ESTALE` on an open descriptor is fatal to the application.  Unlike a
  failed lookup, there is no recovery path.

So re-keying an open handle repairs librgw's internal view while leaving
the client's reference dead.  It fixes the half that does not matter.
Refusing the rename is the correct behaviour for a content-addressed
handle, and the guard should stay.

**This is the real constraint on directory rename**, more than any
bookkeeping cost.  Moving a subtree changes the filehandle of every object
beneath it, so it cannot be done while any of them are open without
handing those clients ESTALE.  The options are:

- **Refuse when anything beneath is open.**  Extends the existing
  file-level guard to a subtree.  librgw knows its own open handles, so
  the test is a bounded scan;  the cost is that a single open file
  anywhere beneath blocks the rename.
- **Accept ESTALE for open files under a moved subtree**, and document it.
  Defensible only if directory rename is rare and the exposure is stated.
- **Stop deriving handles from the path**, which is a much larger change
  and is not wanted — see below for why it is not merely a matter of
  taste.

### 5.3.1 Why the handles are content-addressed at all

An NFS filehandle must be *persistent*:  a client may present one after a
server restart, and the server has to resolve it.  Hashing the path makes
a handle reconstructible with no persistent handle table — that is what
the "content-addressable hash" in the public header buys, and it is a
real architectural property rather than a convenience.

The cost is precisely the rename problem:  a handle that is derived from
where an object *is* cannot survive the object moving.  Making handles
stable across renames means introducing a durable handle-to-object
mapping — which is the same external-metadata-store direction that would
let rados rename a subtree (§6).  The two problems converge on the same
answer, which is an argument for not solving this one locally in a way
that has to be undone later.

### 5.4 Tests

The existing three only assert that the destination resolves.  None
checks that the source is gone, that content survived, or that the etag
or version id is unchanged — so a rename that copied and failed to delete
would pass all three.  Any rename work needs, at minimum: source absent
afterwards, content identical, etag unchanged, version id unchanged
(which is the O(1) claim), and a directory rename over a prefix holding
several objects and several versions.  A control that distinguishes
`renameat` from copy-then-delete is essential — inode equality across the
move is the direct one.

---

## 6. Generality, and why the interface shape is the risk

The external metadata schema being designed for rados introduces a
**name-to-object-id indirection**, precisely so that objects can be renamed
without moving data — rename becomes a remapping of names onto ids.  So
rename in that world is not a filesystem operation at all, and NFS rename
will be required there.

Two things follow, and they pull in opposite directions.

### 6.1 The right place to prototype, the wrong place to generalise from

nsfs is the only backend that can do **prefix** rename cheaply today:  one
`renameat` moves a whole subtree, because the keys beneath it re-derive
(§2).  That makes it the natural place to build and exercise the operation,
and it is a genuine precursor to the metadata work rather than a detour.

But in full generality the metadata feature is **prefix rename**, and the
SAL interface for that is probably *not* "rename one object".  Single-object
rename is the degenerate case of a prefix operation, not the foundation of
one — build the easy case first and the general case arrives bolted onto
something shaped wrong.  Two specific traps:

- **Shape.**  If the eventual rados mechanism is a name-to-id rebinding,
  then the primitive that generalises is closer to *rebind this name (or
  name prefix) to this location* than to *rename this file*.  nsfs fulfils
  a rebind with `renameat`;  metadata-rados fulfils it with an index
  update.  An interface modelled on the filesystem call would not survive
  the translation.
- **Guarantees.**  The complexities genuinely differ and do not converge.
  Prefix rename is one call on nsfs and O(n-objects) name updates under the
  indirection.  So the interface must let a driver *report* what it can
  guarantee — atomic, or linear and interruptible, or unsupported — rather
  than promise atomicity or O(1).  Validating a prefix interface against
  nsfs alone invites encoding nsfs's single-call atomicity into the
  contract, which would mislead rados later.

We will not know for some time whether an operation designed now is the
right one.  That argues for the capability seam used elsewhere in this
note, and against freezing a signature early.

### 6.2 What generalises regardless

The *semantics* in §4 do:  if rename is a namespace operation that carries
history, that is a property of RGW's filesystem-like namespaces generally,
and one argument covers nsfs, posix and a later metadata-rados alike.  The
divergence from S3 is then not an nsfs-local special case.

---

## 7. Status

Design only; nothing implemented, and **not blocked**.  An earlier revision
said it was, on a v1 API compatibility question that turned out not to
exist (§3).  Rename works today for files, cross-bucket included, and
`ceph_test_librgw_file_rename` is green and re-run safe.

§4 is settled:  (a), history moves with the object, cross-bucket gated on
the target being able to hold it, slicing available as export policy.

What is actually outstanding:

- the assertions the suite is missing (§3.1) — worth having before any
  rename work, not after;
- the `rc` shadowing in `RGWLibFS::rename()` (§3) — two lines;
- directory rename itself (§5), whose real constraint is the filehandle
  (§5.2, §5.3);
- whether `FLAG_CREATING` is wanted or should go (§3.2).
