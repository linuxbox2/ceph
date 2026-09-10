# Rename: why it is hard, and why nsfs can do it anyway

## Purpose

`rgw_rename()` follows the NFS operation, whose contract is atomicity
with respect to other operations "where possible".  RGW cannot honour
that in general, and the reason is structural rather than an
implementation shortcoming.  This document records the structural
argument, the property that exempts nsfs from it, what is actually broken
today, and the semantic question that has to be answered before any of it
is built.

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

## 3. What is broken today

`ceph_test_librgw_file_rename` has three tests — `TOPDIR_RENAME`,
`SUBDIR_RENAME`, `CROSS_BUCKET_RENAME`.  **All three fail on nsfs on a
clean data root**, each reporting `-EINVAL`.  None of it is about rename
semantics.  Four independent defects, outermost first:

**3.1 The reported error is not the real one.**  In `RGWLibFS::rename()`
step 0 declares its own `int rc` inside the `case 0:` block, shadowing
the function's `rc`, which was initialised to `-EINVAL`.  When the copy
fails the real error stays in the inner variable and `-EINVAL` is
returned.  The actual error is `-ENOENT`, visible only at `debug_rgw=1`:

```
rename step 0 failed src=/wyndemere tommy1 dst=/wyndemere ricky1 rc -2
```

**3.2 The source object never exists.**  `copy_obj` cannot stat it
because nothing created it.  The suite's `make_object()` helper does
`rgw_lookup(FLAG_CREATE)`, `rgw_open`, `rgw_write`, `rgw_close`,
assigning each result to the same `ret` and returning only the last — so
it reports `rgw_close`'s status and discards every earlier failure.  It
cannot fail in the case that matters.

**3.3 The v1 create sequence no longer creates on an FSIO driver.**  This
is the root cause.  Measured, by instrumenting the suite's helper rather
than reading the guard:

```
lookup = 0     handle created
open   = -2    ENOENT
write  = -1    EPERM
close  =  0    <- make_object() returns this, hiding both failures
```

`rgw_lookup(RGW_LOOKUP_FLAG_CREATE)` creates a *handle*, not an object.
The suite then calls `rgw_open(fs, fh, 0 /* posix flags */, 0)`, and
`posix_flags == 0` is `O_RDONLY`.  On an FSIO driver `rgw_open()` passes
that straight to `open_global()`, which really opens a file — so it is an
`O_RDONLY` open of something that does not exist yet, and returns
`-ENOENT`.  No global open is established, and the subsequent `rgw_write()`
takes its `! open` branch and returns `-EPERM`.

The `-EPERM` is therefore a symptom, not the cause.  (An earlier revision
of this note said `rgw_open()` establishes no global open;  that was
inferred from reading the guard rather than observed, and is wrong — it
calls `open_global(posix_flags, flags)` directly.)

**What matters is that this is a semantic change in the v1 path, not a
test bug.**  `lookup(CREATE)` + `open` + `write` + `close` was the v1
create sequence, and it worked on rados because the open was bookkeeping
and the write transaction created the object at close.  On an FSIO driver
the open is a real `openat()`, so the same sequence fails unless the caller
asks for `O_RDWR`.  Either:

- `rgw_open()` on an FSIO driver treats a handle created by
  `FLAG_CREATE` as a creating open, preserving the v1 sequence;  or
- v1 callers must pass `O_RDWR`, and the behaviour change is documented.

The Samba consumers are new and track our evolution, and the v1 path may
be retired in a couple of releases — so this is a free choice rather than
a constraint.  It should still be made deliberately:  until it is, the
older sequence silently creates nothing on nsfs, which is how a test suite
came to report success while writing no objects at all.

**3.4 The suite is not re-run safe.**  On a dirty root, bucket and subdir
creation fail `-EEXIST` and three of four failures become that instead,
masking the above.  Same class as the `VER_` family fixed in
`librgw_file_write2`.

**Nothing here has been fixed.**  The list is the prerequisite set: no
rename design can be validated until §3.3 is settled, because the tests
cannot create an object to rename.  Note that §3.2 and §3.3 compound —
the helper discards the errors that would have made this obvious, so the
first visible symptom is three rename failures reporting the wrong
errno.

---

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

## 6. Generality

If the external metadata store lets rados rename a metadata subtree, that
is the *same shape* as §2 — move one node, keys beneath it re-derive.
Which argues for choosing (a)'s semantics now in a form that generalises,
rather than as an nsfs-local special case: the divergence from S3 is then
a property of RGW's filesystem-like namespaces generally, and one
argument covers both.

---

## 7. Status

Design only; nothing implemented.  Blocked on §3.3 — the legacy write
path's `-EPERM` on FSIO drivers — which is a compatibility question about
the v1 API and not ours to answer alone.  §4 is a decision, not a task.
