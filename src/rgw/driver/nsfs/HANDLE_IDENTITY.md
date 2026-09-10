# Handle identity: what an `rgw_fh_hk` names

## Purpose

`rgw_file`'s file handle is a hash of an object's *path*.  That choice is
the reason rename is hard (see [RENAME_DESIGN.md](RENAME_DESIGN.md)), and
it is a question in its own right — larger than rename and separable from
it.  This document records what the handle is today, what a
filesystem-backed driver could use instead, what that costs, and what an
abstraction has to provide for it to be worth doing.

**Scope note.**  The subject is `rgw_file`'s handle contract, which is
driver-agnostic; the document lives here because this is where the working
notes are, not because it is nsfs-specific.  If anything, the likely first
consumer is posix — see §3.

---

## 1. What the handle is today

`rgw_fh_hk` is public API (`include/rados/rgw_file.h:51`), it is
`struct rgw_file_handle`'s identity, the header describes it as a
"content-addressable hash", and `rgw_lookup_handle()` accepts one.  It is
the bits a consumer turns into a wire NFS filehandle.

It is 128 bits, two `uint64_t`:

```
fh_hk.bucket = XXH64(tenant ":" bucket)
fh_hk.object = XXH64(tenant ":" full-path-from-bucket-root)
```

`make_fhk()` composes the object half via `make_key_name()`, which is
`full_object_name() + "/" + name`, and `full_object_name()` includes the
bucket segment with a leading `/`.  A child inherits its parent's bucket
half unchanged.  So for `bucket1/randy/foo`:

| handle | bucket half | object half |
|---|---|---|
| `bucket1` | `XXH64(t:bucket1)` | `XXH64(t:bucket1)` |
| `randy` | inherited | `XXH64(t:/bucket1/randy)` |
| `foo` | inherited | `XXH64(t:/bucket1/randy/foo)` |

Two observations follow.

**Any rename changes the handle.**  Including a rename within one
directory: `mv bucket1/randy/foo bucket1/randy/bar` yields
`XXH64(t:/bucket1/randy/bar)`.  The only rename-invariant part is the
bucket half, which is not the half that identifies the object.

**The allocation is lopsided.**  64 bits of hash for a bucket name is
sized for a namespace that does not exist — buckets per tenant number in
the thousands.  The bucket name is also present in *both* halves, since
the object path includes it.  Meanwhile the object half, which has to
distinguish every object in the store, gets the same 64 bits.

### 1.1 Why it is content-addressed

This is not arbitrary, and it is the reason the design resists change.  An
NFS filehandle must be **persistent**: a client may present one after a
server restart and the server has to resolve it.  Hashing the path makes a
handle reconstructible with no persistent handle table anywhere.  That is
a real property, and any replacement has to supply it.

---

## 2. The filesystem already has a persistent handle

`name_to_handle_at(2)` and `open_by_handle_at(2)` are exactly the
mechanism knfsd resolves NFS filehandles with.  Measured on the xfs data
root under this build:

```
handle_bytes=12  handle_type=129  bytes=0afe2b0000000000 a06d0e0d
                                        ^ino (64, LE)     ^gen (32)
after rename: IDENTICAL -- handle is rename-invariant
```

- **96 bits**, `XFS_FILEID_INO64_GEN`: a 64-bit inode plus a 32-bit
  generation.
- **Rename-invariant for files _and_ directories** — verified by renaming
  each and re-deriving.  A directory rename does not disturb the handle of
  the directory or, by construction, of anything beneath it.
- The **generation number is the correctness component**.  It is what
  stops a stale handle resolving to a *different* file after inode reuse.
  Dropping it to save bits reintroduces exactly the aliasing bug knfsd's
  gen field exists to prevent.

### 2.1 The bit budget works, with one caveat

96 bits of `(ino, gen)` fits inside the existing 128-bit `rgw_fh_hk` with
32 bits left for a bucket discriminator — no widening of the public
struct.  That is the re-apportionment §1's lopsidedness invites.

The caveat is **fsid**.  `(ino, gen)` is unique within one filesystem.  A
single data root makes the fsid implicit and 96 bits sufficient.
Per-account filesystem roots — which the multi-account design
contemplates — would need an fsid component too, and 128 bits then gets
tight.  Decide whether per-account roots are in scope before committing to
a layout.

### 2.2 The capability is not a concern

`open_by_handle_at()` requires `CAP_DAC_READ_SEARCH`.  Measured:

```
open_by_handle_at FAILED: Operation not permitted (euid=1000)
```

`setcap cap_dac_read_search=ep` on the binary is the mechanism, and file
capabilities survive exec, so no part of this needs to run as root.

The more useful observation is *which* process resolves handles.  It is
the librgw consumer — an NFS server — not `radosgw`.  **nfs-ganesha on VFS
is entirely built on open-by-handle**, so a Ganesha exporting a local
filesystem already runs with this capability;  the requirement is
satisfied in exactly the deployment that would use this, and asks nothing
new of it.

That also says something about fit.  A handle-based librgw is not an
imposition on Ganesha's model — it *is* Ganesha's model.  The present
path-hash scheme is the thing that diverges from how FSAL_VFS works.

That matters because `CAP_DAC_READ_SEARCH` is broad — it bypasses read
and search permission checks process-wide — and granting it to a
network-facing HTTP daemon would be a genuine expansion of blast radius.
Granting it to an NFS server that must already resolve handles is not a
new posture.  `radosgw` serving S3 never resolves a filehandle and needs
nothing.

Two practical notes rather than design ones:  file capabilities are lost
whenever the binary is replaced, so a build-test loop has to reapply them
after every link;  and in a containerised deployment the capability must
be in the container's bounding set, not merely set on the file.

---

## 3. Why this is worth doing now, and for whom

The weak argument is that implementation experience would transfer to the
metadata work.  That is true but not sufficient on its own.

The concrete argument is a potential consumer: a **standalone deployment
on xfs — the flash appliance** — which could take advantage of this if it
became interested in NFS export.  *(Assumed to be the posix driver rather
than nsfs;  worth confirming, though see below — it matters less than it
first appears.)*

And NFS is expected to come to posix in due course regardless.  So posix
is a **first-class consumer of this abstraction, not a hypothetical one**,
whichever driver the appliance turns out to run.  That settles the shape
of the work: the abstraction cannot encode nsfs assumptions and cannot be
built by reaching into nsfs internals, even though nsfs is where it would
be exercised first.  rgw-standalone owns posix, so it has to be something
they can adopt without our having modified their driver — and something
that still fits once NFS arrives there.

---

## 4. The abstraction

`rgw_file` is driver-agnostic, so the only honest form is a
**driver-supplied derivation of the object half**, behind the same kind of
capability flag proposed for directory rename:

- **rados** — path hash, i.e. today's behaviour.  Not a stopgap: rados has
  no inode and no `name_to_handle_at` to borrow, and object identity *is*
  the key.  This is a structural absence, not unfinished work.
- **nsfs, posix** — the filesystem's persistent handle.
- **later, rados** — whatever the external metadata store supplies, through
  the same seam rather than a new one.

For the abstraction to be worth insisting on rather than hacking around,
it should:

1. keep filesystem specifics out of `rgw_file` — the driver returns
   opaque identity bits and answers whether it can resolve them;
2. preserve the persistence property in §1.1 for every driver, since that
   is what the current scheme buys and what a consumer relies on;
3. state its uniqueness scope explicitly (per filesystem, per data root,
   per cluster), because §2.1 turns on it;
4. carry a generation or equivalent, so a stale handle cannot alias a
   different object (§2);
5. be adoptable by posix without nsfs-specific plumbing (§3).

---

## 5. What it does not solve

**The transition is a flag day.**  Changing the derivation invalidates
every outstanding filehandle: clients holding one across the upgrade get
`ESTALE` — the same failure this work is meant to remove, relocated to
upgrade time.  `fh_key` carries a `uint32_t version`, but it lives in the
encoded `RGW_ATTR_UNIX_KEY1` attribute, not in the 128-bit wire tuple, so
a server has no in-band way to recognise an old-format handle and resolve
it compatibly.  Any re-derivation therefore wants a deliberate migration
story, and the bit-allocation argument should not make it look cheap.

**It does not make rename free.**  It makes the handle survive one, which
removes the ESTALE exposure that currently justifies refusing to rename
open files, and removes the O(cached handles) re-keying.  The listing-cache
invalidation and the S3 semantic question in `RENAME_DESIGN.md` §4 are
untouched.

---

## 6. Relationship to the metadata work

A durable handle-to-object mapping is where the future metadata work is
going, and it is what would let rados rename a prefix atomically.  This is
the same property arriving early on drivers that can get it from the
filesystem for free — not a competing design.  Which is the argument for
building the seam now and letting rados fill it in later, rather than
solving it locally in a way that has to be undone.

---

## 7. Status

Design only.  Decisions needed, none of them ours alone:

- whether per-account filesystem roots are in scope (§2.1, sets the layout)
- whether the standalone/flash-appliance consumer is posix or nsfs (§3) --
  lower stakes than it looks, since NFS is expected on posix regardless
- what the handle-format migration looks like (§5)
