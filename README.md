# How do I submit patches to Sunlight Common Kernels

1. BEST: Make all of your changes to upstream Linux. If appropriate, backport to the stable releases.
   These patches will be merged automatically in the corresponding common kernels. If the patch is already
	@@ -8,22 +8,21 @@ additions of `EXPORT_SYMBOL_GPL()` require an in-tree modular driver that uses t
the new driver or changes to an existing driver in the same patchset as the export.
   - When sending patches upstream, the commit message must contain a clear case for why the patch
is needed and beneficial to the community. Enabling out-of-tree drivers or functionality is not
not a persuasive case.

2. LESS GOOD: Develop your patches out-of-tree (from an upstream Linux point-of-view). Unless these are
   fixing an Sunlight-specific bug, these are very unlikely to be accepted unless they have been
   coordinated with ionut_n2001@yahoo.com. If you want to proceed, post a patch that conforms to the
   patch requirements below.

# Common Kernel patch requirements

- All patches must conform to the Linux kernel coding standards and pass `scripts/checkpatch.pl`
- Patches shall not break gki_defconfig or allmodconfig builds for arm, arm64, x86, x86_64 architectures
- If the patch is not merged from an upstream branch, the subject must be tagged with the type of patch:
`UPSTREAM:`, `BACKPORT:`, `FROMGIT:`, `FROMLIST:`, `ANDROID:` or `SUNLIGHT:`.
- All patches must have a `Change-Id:` tag (see https://gerrit-review.googlesource.com/Documentation/user-changeid.html)
- If an Sunlight bug has been assigned, there must be a `Bug:` tag.
- All patches must have a `Signed-off-by:` tag by the author and the submitter

Additional requirements are listed below based on patch type
	@@ -111,7 +110,7 @@ must be a stable maintainer branch (not rebased, so don't use `linux-next` for e
- If the patch has been submitted to LKML, but not accepted into any maintainer tree
    - tag the patch subject with `FROMLIST:`
    - add a `Link:` tag with a link to the submittal on lore.kernel.org
    - add a `Bug:` tag with the Sunlight bug (required for patches not accepted into
a maintainer tree)
    - if changes were required, use `BACKPORT: FROMLIST:`
    - Example:
	@@ -148,3 +147,22 @@ a maintainer tree)
    - tag the patch subject with `ANDROID:`
    - add a `Bug:` tag with the Android bug (required for android-specific features)

## Requirements for Sunlight-specific patches: `SUNLIGHT:`

- If the patch is fixing a bug to Sunlight-specific code
    - tag the patch subject with `SUNLIGHT:`
    - add a `Fixes:` tag that cites the patch with the bug
    - Example:
```
        SUNLIGHT: fix sunlight-specific bug in foobar.c
        This is the detailed description of the important fix
        Fixes: 1234abcd6789 ("foobar: add cool feature")
        Change-Id: A4caaba566ea080fa148c5e768bb1a0b6f7201f04
        Signed-off-by: Joe Smith <joe.smith@foo.org>
```

- If the patch is a new feature
    - tag the patch subject with `SUNLIGHT:`
    - add a `Bug:` tag with the Sunlight bug (required for sunlight-specific features)
