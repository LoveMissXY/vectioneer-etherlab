Etherlab Master unofficial patchset changelog
===========================

# 20190904

#### From stable branch

* Most new changes were imported as new patches as-is.
* Changeset a73e8a537 “Fixed working counter calculation when process data of the same slave are split into separate datagrams.” was not applied; the default branch already contains a completely different implementation of the affected functions by Dave Page (for overlapping PDO support).  I’m not familiar enough with the code to tell whether it requires a similar change, but it looks like it was already resolved.
* There are now 45 patches in here, which is getting a little silly.  I hope they get merged upstream soon.
* **CAUTION**: these patches include ones which change the installation directory and library filename.  Changes may be required to applications or installation procedures to accommodate this.
    * This is evidently some strange usage of the word “stable” that I wasn’t previously aware of.

#### Changes

* Some of the patches have been simply renumbered without other changes, due to changes to other patches.  These won’t be explicitly called out.
* `linux/0001-debugif-3.17.patch` (was `base/0031-debugif-3.17.patch`)
    * Moved earlier in the patch series so that compilation of only the `stable` and `linux` groups (and possibly some of the `devices`) works, if that’s all you want.
* `linux/0002-kern_cont.patch` (was `base/0029-kern-cont.patch`)
    * Moved similarly.
* `linux/0003-vm_fault-4.10.patch` (was `base/0005-support-vm_fault-kernel-v4.10.patch`)
    * Moved similarly.
* `linux/0004-signal-4.11.patch` (was `base/0032-signal-4.11.patch`)
    * Moved similarly.
* `devices/0002-update.patch`
    * Simplified a bit since the stable patches now include an update script for the IGB driver.
* `devices/0006-linux-4.4.patch`
    * While this patch has largely been dropped because its contents are now in stable patches, I did notice some bugs in the stable version of the e1000e driver, which this corrects.
    * **CAUTION**: if you're using e1000e in Linux 4.4 or above, be sure to read the warning in the README.
* `devices/0006-linux-4.9.patch`
    * Patch refreshed from the latest version of Linux 4.9.
* `devices/0007-linux-4.14.patch`
    * Patch refreshed from the latest version of Linux 4.14.
* `features/master-redundancy/0001-e1000e-fix-watchdog-redundancy.patch` (was `devices/0002-RX-does-not-reset-watchdog-breaks-redundancy.patch`)
    * Renamed and updated description to provide more explanation of intent.
    * The patch proposed by Wilfried Holzke in http://lists.etherlab.org/pipermail/etherlab-users/2018/003435.html is a direct revert of this patch, and suggests that it may cause problems for users of RTAI.  I’m not sure why; this does kick some work off to a background thread, but that’s necessary to reduce latency and should only be beneficial.  (Unless perhaps RTAI is unable to tell Linux to schedule_work?  That seems like a bug in RTAI if so.)
        * Consequently, I’ve demoted this patch to a feature patch to make it clearer what it’s for and when it’s needed.
* `features/eoe-rtdm/*`
    * Updates to EoE from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000689.html.
        * Note that this also proposes a new patch, which has instead been added as `base/0020-eoe-ip.patch`, as it’s an important bugfix.
    * I’ve made a few additional modifications to these patches in order to compile clean without warnings.

#### Added

* `linux/0005-tty-4.15.patch`
    * Timer API changes and other fixes to TTY module for kernel 4.15 and later, sent privately from Michael Graichen.
    * I’ve modified his original patch a little, to fix some related compiler warnings and to remove the “old API emulation” that he put in, as the code is cleaner without it.
* `devices/0001-update.patch`
    * Fixes some inconsistencies in the upstream device update scripts (after stable patches).
* `devices/0008-linux-4.19.patch`
    * EtherCAT device drivers for Linux 4.19.  As usual, these are mechanically patched and do compile, but I have not actually tested that they work.
    * The r8169 driver is not included, as it has been significantly rewritten in Linux 4.19 and I’m not sure how to safely patch it.
* `devices/0012-r8152-4.9.patch`, `devices/0013-r8152-3.18.patch`, `devices/0014-r8152-4.4.patch`
    * Support for Realtek RTL8152/RTL8153, from http://lists.etherlab.org/pipermail/etherlab-users/2018/003495.html
* `base/0004-fix-eoe-clear.patch`
    * Fixes typo in EoE request cancellation on slave disposal, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000705.html
* `base/0005-disable-eoe.patch`
    * Fixes compilation with `--disable-eoe`, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000692.html
    * Some changes in the original patch have been folded into patches that appear later in the patchset instead.
* `base/0020-eoe-ip.patch`
    * Fixes EoE mailbox conflict between IP state machine and packet state machine, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000689.html.
* `base/0028-ec_master_exec_slave_fsms-external-datagram-fix.patch`
    * Don’t consume a slave ring datagram when not actually using it, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000693.html
* `features/xenomai3/0001-Support-Xenomai-version-3.patch`
    * Support for Alchemy RTDM in Xenomai v3, from http://lists.etherlab.org/pipermail/etherlab-users/2018/003494.html
* `features/sii-wait-load/0001-slave-scan-retry.patch`
    * Retry reading SII if an error occurs, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000691.html
* `features/sii-wait-load/0002-fsm_sii-loading-check.patch`
    * Retry reading SII if the “loaded” bit is not set, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000691.html
* `features/pcap/0001-pcap-logging.patch`
    * `ethercat pcap` command, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000698.html
    * I had to add a missing include in order to get this to compile.
    * Note that the current implementation is not suitable for continuous logging, but just for snapshot logging; it will record the first block of packets sent and received during startup, and then stop once the buffer is full.  The `ethercat pcap` command can be used both to fetch the current buffer and also to clear the buffer, but some packets may be cleared without being reported.
    * There is also most likely a race condition between the record and clear operations; clearing may be ignored if the buffer is not full and a packet is sent/received concurrently.
        * Standard (non-RTDM) userspace applications should be immune from this race, due to locks being held.  Kernel and RTDM apps have fewer locks.
        * Clearing should be respected for all application types once the buffer is full, but then of course some packets will not be recorded.
* `features/pcap/0002-runtime-size.patch`
    * Modifies the above to make the logging size 0 by default but configurable at module load time (via the ethercat config file).  This means that you can enable logging without recompiling the master; just restarting it.
* `features/pcap/0003-report-size.patch`
    * Modifies some internals of the kernel/tool interface to fix a race condition that could cause “ethercat pcap” to report an error if the log is not yet full.
    * Note that the race on the clear operation is still present.
* `features/pcap/0004-high-precision.patch`
    * Uses microsecond-precision timestamps in the pcap files instead of jiffie-precision (typically 1-10ms).
    * This is disabled (and jiffie precision is still used) when compiling for RTDM, due to Graeme Foot’s note that `do_gettimeofday` is troublesome under RTAI.
* `features/mbg/0001-mailbox-gateway.patch`
    * EtherCAT Mailbox Gateway server, from http://lists.etherlab.org/pipermail/etherlab-dev/2019/000706.html
    * I had to add some missing includes and fix a typo to get this to compile.  I have not verified its behaviour myself.

#### Dropped

* `devices/0008-linux-4.13.patch`
    * Patch dropped because 4.13 didn’t end up being an LTS release.  Use 4.14 instead.
* `base/0004-dc_user-tabs.patch`
    * Patch dropped because stable patches fix the original problem.

# 20180622

#### Changes

* Refreshed all patches; they now contain a little more context.
* `devices/0009-cx2100-2.6.patch` and `0010-cx2100-4.9.patch`: incorporated logging changes provided by Graeme Foot.
* `base/0000-version-magic.patch`: bumped version numbers since new patches have introduced new API.
* `base/0032-signal-4.11.patch`: due to stable/0017 this patch was rewritten (but is still required because stable is still missing an include).
* `features/rt-slave/0001-allow-app-to-process-slave-requests-from-realtime.patch`: incorporated changes provided by Graeme Foot.

#### Added

* Pulled stable/0017 through 0023 from stable-1.5 branch into default branch.
* `devices/0011-linux-4.14.patch`: Update device drivers for Linux 4.14.
* `devices/0012-e1000-unused-variable.patch`: Avoid uninitialzed variable warning in e1000, provided by Graeme Foot.
* `base/0033-dc-sync1-offset.patch`: use both `sync1_cycle` and `sync1_offset` to determine SYNC1 register value; resolves issue with using SYNC1 shifts.  Provided by Graeme Foot. 
    * Note that I’ve modified this slightly from the version posted in the ML; it now forces SYNC1 to 0 if you try to set SYNC1 values without a valid SYNC0 time, instead of setting up an incorrect SYNC0 cycle.
* `features/eoe-rtdm/0001-eoe-addif-delif-tools.patch`: explicit EoE interfaces, provided by Graeme Foot. 
    * Also modified slightly to resolve some compile errors and incorrect printfs.
* `features/eoe-rtdm/0002-eoe-via-rtdm.patch`: application-controlled EoE for RTDM, provided by Graeme Foot.

Note that other than a quick glance through and verifying it compiles I haven’t done much vetting on the EoE patches, as I don’t use EoE or RTDM myself.  Please let me know if there are any issues, or if I’ve overlooked some other patches.

# 20171108

* New patch `devices/0008-linux-4.13.patch`.  This adds device patches for kernel 4.13.  Just because.  Note that I haven't actually tested any of these (other than verifying compilation); I'm still using 4.9 myself.

* New patch `base/0031-debugif-3.17.patch`.  This is Ricardo Delgado's fix for kernel 3.17 and later when `--enable-debug-if` is specified.

* New patch `base/0032-signal-4.11.patch`.  This is Ricardo Delgado's fix for kernel 4.11 and later.

# 20171102

* I've added a new `stable` directory of patches; these contain commits that have been made upstream to the stable-1.5 branch but not yet to the default branch.  I only looked at recent history for these so it's entirely possible that I've missed a few.  But notable inclusions among these are an update to the CCAT driver and the new IGB driver.

* I've dropped the old driver patches for Linux 4.9 and replaced them with new patches for all drivers (including IGB) for Linux 3.18, 4.1, 4.4, and 4.9.  As usual since I don't have the hardware myself all I can guarantee is that the orig code matches what was in the kernel sources (specifically the vanilla+rt sources, in case it makes a difference) and that the EtherCAT versions do compile.  I've made my best effort to forward-port the patches but I can't make any promises that they'll work or have no bugs or memory leaks.

* New patch `base/0030-ext-timeout.patch`: if you have a large domain, a fast cycle, and many concurrent slave requests in progress, it can happen that there is too much data to safely fit into the cycle, so the master wants to defer some of the slave requests to the next cycle.  When it does this, it tries to check when they were originally queued and timeout the requests beyond a short interval to prevent them being stuck forever -- however the time it checks against is not actually the time they were queued but rather the time that the *previous* datagram using the same "slot" was actually sent, which is obviously silly and tends to always time out the request datagrams even when not necessary.  This patch fixes that.

# 20170914

* Updated to upstream 33b922.

* Added one patch `base/0029-kern-cont.patch`.

    * This fixes an issue on recent kernels where parts of logged messages (especially at debug 1 or higher) end up on separate lines in the logs instead of the same line as intended.  (Upstream linux would probably prefer that the values are buffered and only printed as complete lines, possibly replacing some usage with `print_hex_dump` calls, but that seemed like a larger change than I wanted to make at the moment.)

    * This probably should be higher up the list but I didn't want to renumber everything for such a minor change.

# 20170727

* Updated to upstream f5ffd187.

* I've left the `ethercat diag` command patches in for now but note that upstream has added a new command `ethercat crc` that does basically the same thing.  I'll probably drop `diag` in a future patchset but until then people can try them both out and see which one you prefer or if anything from `diag` should be patched into `crc`.

* Patches have been reorganised into directories and locally renumbered; you can check the history of the readme or of the patches themselves to follow their history (in some cases you might need to add the `-f` flag to tell hg to follow the history through renames).  This does mean that the patches are not fully alphabetic ordered anymore; intended application order is in the series file as usual, and complete instructions are in the readme.
    I'm open to suggestions for further reorganisation, especially of the `base` patches.

* I've also added a script and some notes for people who prefer applying patches directly in Git mostly without passing through Mercurial along the way.  (The preferred option is still to apply the patches in Mercurial first though.)  The script is a little dumb and can handle comments and removing lines from the series file (if you want to exclude some patches) but not guards.

* There are no longer any API "breaking change" patches (ie. existing app code based on master 1.5 should still compile with all patches applied; although sadly this does mean that there's a reverse breakage and code that compiled with the previous full patchset might need to be modified slightly for this one).

* However there are still ABI-breaking changes scattered over several patches (in general, anything that changes ecrt.h or ioctl.h).  Typically this just means that you need to recompile both the kernel and userspace components (for both Etherlab and your application).  I've added some example code in the readme showing how to detect incompatibility between app and library, which is recommended to add to your application startup if not already done.

* Folded former patch 0032 into `base/0021-Clear-slave-mailboxes-after-a-re-scan`.

* Folded former patch 0033 into `base/0023-Sdo-directory-now-only-fetched-on-request`.

* Folded former patch 0037 into `base/0016-If-enable-rtmutex-use-rtmutexes-instead-of-semaphore`.

* Patch `base/0017-Master-locks-to-avoid-corrupted-datagram-queue`
(formerly patch 0011) has been made no-op when compiled for RTDM to avoid introducing Linux locks (and possible secondary mode and/or deadlocks).
    Along with the change to base/0016 discussed below, I *think* this should make the patchset completely RTAI/Xenomai-safe, although it's possible patch base/0018 might need similar treatment.  (Let me know!)

* Patch `base/0009-Avoid-changing-running-slaves-DC-offset` (formerly patch 0029) has been tweaked slightly to avoid resetting the drift filter for slaves in SAFEOP or OP even if it updated their transmission delay.  I don't actually have any evidence of this being a problem either way but I suspect this will produce slightly smoother clock transitions for active slaves.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2016/000572.html (Knud Baastrup):

    * Patch 0061 was not actually applied but it made me realise the original problem with patch base/0016 (formerly patch 0004), so I have dropped former patch 0007 and reworked former patch 0004 slightly.  The patch should now be cleaner against upstream sources and should be safe for RTAI use as well now.  (Alternatively, you can think of it as folding patch 0007 and patch 0061 into 0004 and then moving one lock instance out to the calling method where it had originally been to make it RTAI safe.)  Note that I have still not actually tested this against RTAI but since there are now no new locks it should be ok.

    * Patch 0062 has been folded into patch `base/0019-Support-for-multiple-mailbox-protocols` (formerly patch 0005).

    * Patch 0063 has been folded into patch `base/0017-Master-locks-to-avoid-corrupted-datagram-queue` (formerly patch 0013).

    * Patch 0064 has been folded into (and thus replaced) patch `features/complete/0002-add-sdo-write-with-size` (formerly patch 0038).
      While I still personally prefer the single-API version, this makes it a non-ECRT-API-breaking change, although still an ABI-breaking change.

    * Patch 0065 has been folded into (and thus replaced) patch `features/complete/0003-sdo-requests-complete` (formerly patch 0039).

        Although I excluded `ecrt_slave_config_alloc_sdo_request` because it's not really any different from the other two create methods.  (You can always pass 0 for the index/subindex if you want that.)
        While I still personally prefer the single-API version, this makes it a non-ECRT-API-breaking change, although still an ABI-breaking change.

    * Patch 0066 has been rejected.  I don't think it's worthwhile adding an entirely separate API for this; just verify that your app was compiled against the correct version. I've added some notes in the readme about doing that, and added a patch (base/0000) to make it easier to distinguish patched and unpatched sources and binaries.  (And there are plenty of other patches that break ABI; it doesn't seem useful singling this one out.)

* From http://lists.etherlab.org/pipermail/etherlab-users/2016/003112.html (Dr.-Ing. Wilhelm Hagemeister):

    * I have not applied this patch, since it limits performance, it's a bit device-specific, and it came from IgH themselves.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000573.html (Patrick Brünn):

    * Imported as `base/0005-support-vm_fault-kernel-v4.10`.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000581.html (Gavin Lambert):

    * The changes suggested in this post (and the later correction) have been folded into `base/0019-Support-for-multiple-mailbox-protocols` (formerly patch 0005).

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000583.html (Graeme Foot):

    * Imported as `devices/0005-cx2100-2.6`, although it only supports Linux 2.6.32.
    * Theoretically forward-ported to Linux 4.9 as `devices/0006-cx2100-4.9`
-- it compiles, at least; I don't have the hardware to test it.
    * I assume that this is intended only for 32-bit systems, as there is some code that generates suspicious warnings when compiled for 64-bit.  I haven't tried to correct this.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000587.html (Nir Geller):

    * I'm not sure what to do about this patch.  AFAIK in theory the change shouldn't be needed (there's a separate EoE thread which should be running in Linux kernel mode, so app code shouldn't need to do anything in particular other than getting lock callbacks correct if not in vanilla
usermode) but as I don't use EoE myself I don't know enough about it to say for sure.  Any chance someone else can chime in on this one?

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000592.html (Steffen Dirkwinkel):

    * I haven't applied the suggested patch, but since `ssize_t` is only used in one place, and it really didn't need to be (especially since no existing callers appear to use the return value anyway), I've opted to remove this usage instead, as patch `base/0006-avoid-ssize_t`.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000595.html (Graeme Foot):

    * Imported patch 0001 as `base/0007-replace-fprintf-calls-with-EC_PRINT_ERR`.

    * Imported patch 0008 as `base/0008-read-reference-slave-clock-64bit-time`.  (Perhaps this should have been a feature patch instead, but it seems harmless enough.)

    * Imported patch 0010 as `features/rt-slave/0001-allow-app-to-process-slave-requests-from-realtime`.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000596.html (Ricardo Ribalda Delgado):

    * This patch is already included in the default branch.

* From http://lists.etherlab.org/pipermail/etherlab-dev/2017/000600.html (Gavin Lambert):

    * I have elected to not make the change mentioned here, as my analysis wasn't quite correct and it shouldn't make any practical difference.  And the change would introduce busy cycles, which is undesirable.

# 20160804

* Uploaded to Sourceforge.

#### Modified

* `0051-fsm_change-external-datagram.patch`
* `0052-fsm_slave_config-external-datagram.patch`
* `0053-fsm_slave_scan-external-datagram.patch`
    Fixed some spots where it was accessing the wrong datagram.

#### Added

* `0043-ethercat-diag.patch`
    Recently posted to the users list by Ralf Roesch, adding a `diag` command to the ethercat command line tool, to aid in locating lost links and other comms errors.

    Other than rebasing within the patchset, I’ve made the following tweaks to the code (otherwise it is largely unchanged):

    * Various whitespace fixes.
    * Moved a few lines in main.cpp for consistent grouping.
    * Changed `llc_reset` to a bool, since it was being used as a bool.
    * Tweaked a few lines from assignment to compound assignment to simplify.
    * Removed the data type lookup in `EscRegRead` and `EscRegWrite`.
        * Since this is all internal and only used to get the size, which is well known to the caller, the lookup seemed unnecessary.
    * Made `EscRegRead` and `EscRegWrite` output errors to stderr instead of stdout.
    * Fixed a printf format issue that generated a compiler warning.
    * Made `EscRegRead` and `EscRegWrite` treat errors as non-fatal.
        * Some slaves do not implement all of these registers, and so trying to read them will produce an “I/O error” exception.  In this case it makes more sense to continue reading the other registers than to abort.
        * For example, Beckhoff EL3062 does not implement register 0x030C.
        * 0x0308-0x030B can similarly be absent on some older slaves.

    I’m a little hesitant about the command name being `diag` – while it’s not a bad name for network diagnostics or error stats it might be confused with the “Diagnosis History” object as specified in ETG1020, which is an entirely different thing.  (And something that might be useful to add to the tool in the future.)  I’m open to alternative suggestions.

* `0044-diag-readwrite.patch`
    This is a further modification on top of the previous patch which replaces several separate read and write requests with a single read or read+write request (plus one additional read) per slave.  (So naturally it depends on patch 0026.)

    In theory this is more efficient, but most importantly since the reset occurs using the same datagram as the read, it’s now atomic and there’s no risk of losing counts (which could previously happen if the slave incremented its counter after the read but before the write).

* `0045-slave-config-position.patch`
    Adds a `position` field to the structure returned by `ecrt_slave_config_state`.  This allows you to quickly get the ring position of a slave from its relative alias:offset address, which in turn allows you to call other APIs that require this (eg. `ecrt_master_get_slave`).

    Note that the position is only valid if `online` is true, and that it is possible for the value to be stale (ie. the slave has moved to a different position) if the network changes and is consequently rescanned after this call.  So use it defensively.  (You’re reasonably safe in the period between requesting the master and activating it, as rescans are inhibited during this time.  OTOH, only the application can request the master; an external tool can’t.)

    I’m considering whether it would be useful to make a general function available for this conversion, to avoid duplicating the alias:offset conversion logic in too many places (eg. the tool requires it as well, but can’t use the `slave_config`-based conversion since it can’t request the master).

* `0046-e1000e-link-detection.patch`
    Fixed link detection in e1000e driver for 3.10-3.16.

    This is Christoph Permes’ 3.16 patch and 3.14-v2 patch, with the latter backported to 3.12 and 3.10.  I haven’t tested these personally.

* `0057-fsm_foe-simplify.patch`
    Removes some redundant fields from the FoE FSM; some were unused copy/paste holdovers from the CoE FSM while others were duplicated unnecessarily between the read and write operations, which can’t be concurrent for a given slave anyway.

    Also fixes the case where the incoming data exceeds the provided buffer to properly terminate the state machine instead of leaving things dangling.  Although note that this still leaves the FoE conversation itself dangling, so you’ll likely get an error on the next request if this occurs.

* `0058-foe-password.patch`
    Adds support for sending an FoE password along with read or write requests.

    Also implements the `-o` option for the `foe_read` command (which was documented but not implemented).

    Also makes the `ioctl` behind `foe_read` actually use the buffer size requested by the caller (instead of a hard-coded value); though note that `foe_read` itself still uses a hard-coded value of its own (but it’s larger, so bigger files should be readable now).  It’s possible that users on limited-memory embedded systems might need to reduce this value, but it’s still fairly conservative as RAM sizes go.

* `0059-foe-requests.patch`
    Makes FoE transfer requests into public ecrt_* API, similar to SDO requests.

    Primarily (following my goal of “parallel all the things”), this allows FoE transfers to be non-blocking so that transfers to multiple slaves can occur concurrently from the same requesting thread (previously this was only possible by using separate threads, since the only API was blocking).  Note that due to patch 0018 you can call `ecrt_master_deactivate` to delete these requests when you’re done with them, even if you haven’t called `ecrt_master_activate` yet.

    It has a possible side benefit that FoE transfers can now be started and monitored from realtime context, although as FoE is mostly used for firmware updates this is unlikely to be all that useful in practice.

    I considered a few alternative approaches to this (the next leading contender was to make async versions of the existing FoE ioctls), but this seemed more consistent with existing APIs.  I’m open to suggestions here too though, since it does feel like a slightly odd fit.  (But works quite nicely.)

* `0060-foe-request-progress.patch`
    Adds a way to get a “current progress” value (actually the byte offset) for async FoE transfers.

# 20160621

* `0026-Add-register-read-write-support.patch`
    Renamed the ethercat tool command from `reg_readwrite` to `reg_rdwr`, as the prefix matching was interfering with `reg_read`.

* `0029-Disable-DC-SYNC-before-updating-a-running-slave-s-sy.patch`
    Dropped.

* `0029-Avoid-changing-running-slaves-DC-offset.patch`
    Added as a replacement for the above.  It does the following:
    * If the System Time Offset and Delay registers for a given slave are already correct, it will not write to them at all.
    * If it wants to change the System Time Offset register but the slave is already in SAFEOP or OP, then it won’t change it (but will still update the System Time Delay with the transmission delay).
        * Modifying the System Time Offset register (0x0920) causes a step change in the System Time of the slave, which can cause it to miss the next sync time (for a 32-bit slave, it can take 4 seconds to recover, and for a 64-bit slave, it might never sync again).
        * Modifying the System Time Delay register (0x0928) just alters the value it uses when the normal time sync datagram circulates (as far as I can tell); this is drift compensated so it will gradually drift to the correct time instead of stepping straight to it, so shouldn't cause the above problem.
        * Patches 0001 and 0002 both make it more likely for the master to want to update the System Time Offset (though they do improve other things, and this is good for the initial startup case – just less so for the reconfigure-during-OP case).
    * If it updated either the offset or the delay it will also write register 0x0930 to reset the drift filter (this is recommended in the datasheets).

    This should now be cleaner and safer than the previous patch (which disabled and re-enabled sync outputs, which only works if the slave supports and enables AssignActivate 0x2000 and might miss some pulses), and better for general use, since this allows running slaves to always use drift compensation to adjust their clocks gradually rather than stepping instantly (while slaves being configured from PREOP can still step immediately).

    The patch now applies to all DC-capable slaves – previously it only affected slaves that use sync pulse generation (AssignActivate 0x0100), but step changes can be a problem for slaves that perform DC timestamping or other things too.

    It should also avoid DC timing shifts during operation, especially if you are using a slave as the reference clock and syncing the master clock to it rather than the reverse.  (Note that the dc_user example code does do the reverse and actually uses the master clock as the real reference.  If you’re not sure which is which, using `ecrt_master_reference_clock_time` to get the slave refclock time and use it in the master code is the former, while using `ecrt_master_sync_reference_clock` to send the master clock to the slave refclock is the latter.  Both approaches should be safe but the latter is subject to higher jitter and drift.)

    However bear in mind that the way I’m using DC I’m not going to notice small timing errors.  So I’d appreciate it if someone who is using DC more extensively (especially with motor slaves, which tend to be picky) could verify it.

    (Just to repeat the background from the original patch: if the network rescans while the master app is running [eg. due to a change in the number of responding slaves], then without any patch the reconfig process is likely to update the System Time Offset register, which causes an immediate step change in the slave’s DC clock and can in turn sometimes result in it missing pulses and possibly stopping altogether, as mentioned above.)

    FWIW, the datasheets also recommend sending a flood of resync datagrams (about 15,000) after changing the clocks, to make it drift in more quickly.  I haven’t done this as at the default update rate this would take quite a while (about a minute, in fact).  But master app code might want to consider detecting a change to the number of slaves during operation and calling `ecrt_master_sync_slave_clocks` more frequently afterwards (if a change to the number of slaves isn’t fatal to your application anyway).  Another possibility might be to temporarily increase the speed of the drift adjustment.

* `0042-print-sync-signed.patch`
    Corrects the DC sync log message to use signed format, since it can be given negative values.

And now here’s the major bundle (I skipped some numbers to show grouping):

* `0050-fsm_sii_external-datagram.patch`
* `0051-fsm_change-external-datagram.patch`
* `0052-fsm_slave_config-external-datagram.patch`
* `0053-fsm_slave_scan-external-datagram.patch`
    These are prep work for the following patches.  They each take one state machine and convert it from using a single fixed datagram object provided at init time for all operations (as is the style of `fsm_master`), to using a different one provided each time to the exec function (as is the style of `fsm_slave` and several low-level FSMs).
* `0054-fsm_slave-handles-all-sdos.patch`
    This moves the internal SDO requests and SDO dictionary requests (if you disable `EC_SKIP_SDO_DICT` from patch 0010; otherwise dictionary requests already were effectively moved) from `fsm_master` into `fsm_slave`.

    This does two important things: firstly it removes the fighting over the CoE mailbox between the internal and external SDO requests (making the busy-checking on each side unnecessary).  And secondly it allows both of these to occur in the background and in parallel between multiple slaves.

* `0055-fsm_slave_config_scan-to-fsm_slave.patch`
    This is a big patch (though not the largest in the set; but I couldn’t see a way to split it up any more without making intermediate states that wouldn’t compile or run), and it’s a doozy.

    Similar to the previous patch, this moves `fsm_slave_scan` and `fsm_slave_config` from `fsm_master` to `fsm_slave`.  This allows slave scanning and configuration to occur in parallel for multiple slaves.  (Note that scanning all slaves must complete before configuring any slave can begin.)

    This also adds `scan_required` to `ec_slave_info_t`; when true the other fields are unreliable (and should be ignored) as scanning has not yet started or is still in progress.

    The motivating case was a network of about 100 slave devices; while scanning is fast (under a second, after prior SII patches), the configuration process to bring the slaves from PREOP to OP took about 80 seconds (and you could see the lights coming on each slave in sequence).  After the patch it takes about 20 seconds.

    I actually originally intended to only move `fsm_slave_config`, but the structure of the code required moving `fsm_slave_scan` as well.  Logically they do both belong in the slave FSM anyway.

    Note that in this case “parallel” does not mean separate threads – all the FSMs (master and all slaves) still execute on a single thread.  But it can now include datagrams for multiple slaves in the same frame.  The existing throttling mechanism for `fsm_slave` is used, so it will configure slaves in chunks, not all at once (so the network won’t get overloaded if you have a large number of slaves, though network usage will be higher than it previously was).

* `0056-fsm-exec-simplify.patch`
    Now that most of the FSMs execute from `fsm_slave`, it’s not necessary for them to check the datagram state, as master.c’s `ec_master_exec_slave_fsms` does this in advance.  This simplifies the FSM exec functions.

These have been tested and appear to work as expected, at least with my networks.  However there are a few caveats:

* The “ready” state exposed in `ec_slave_config_state_t` and `ec_slave_info_t` by one of my earlier patches (integrated into default) is less useful than it used to be; it now turns on earlier when the slave is still awaiting scanning and configuration (in fact it will rarely be false unless you’re hammering the slave with requests).  Since this is relatively new I doubt it will bother anyone, but I’m open to suggestions.
* Previously since slaves were scanned and configured in order, you could check whether the last slave was ready and use this to decide that the network as a whole has finished configuration (and consequently that things like SDO requests would be acted on quickly).  Similarly since the last slave was the last brought to OP, you could have external equipment detect the network is ready by having an output on this slave (that turns off when not at OP).

    While the last slave should still be among the last slaves to be configured, it may no longer be the actual last slave configured, especially if some slaves are faster to configure than others.  Having said that, you can still assume that requests will be acted on quickly for an individual slave if that slave claims to be ready (unless it hasn’t been configured yet) – it’s only making inferences about other slaves that’s more problematic now.

* Network usage will be higher when scanning/configuring than it previously was (though it’s capped, as mentioned above).  The price for talking to more slaves at once is that frame sizes are larger (or it might send multiple frames).  There is a chance that this might overflow your cycle times if you’re running at a very high rate.  Up to `EC_EXT_RING_SIZE/2` slave FSMs (default 16) can be running in parallel; you may need to tune this lower in that case.  (Or you can tune it higher, if you have spare time and you want it to configure more slaves at once.)  Testing was done at 1ms cycle times on `PREEMPT_RT`.

Also note that as the numbering implies, they assume that all prior patches have been applied.  Some of the prior patches have effectively been “baked in” as this moves their changes from one file to another, so they can’t be trivially reordered.  Each successive patch will compile and run correctly before the next in the sequence is applied, however.

# 20160615

* `0013-Improved-EtherCAT-rescan-performance.patch`
    Fixed some minor whitespace (indentation) errors that crept in somewhere.
* `0041-load-sii-from-file.patch`
    Rewrote to eliminate some bugs and make the code considerably tidier.  Incorporates the async behaviour as standard.  (As before, the feature as a whole is disabled by default, but can be enabled at configure time.)
* `0042-load-sii-from-file-async.patch`
    Dropped, as it’s now folded into the prior patch.

# 20160613

* Adopted the filenames from Knud’s patchset.
* Replaced `0037-Breaking-change-rt_mutx_lock_interruptible-calls-for.patch` with a version that isn’t a breaking change (assuming the version numbers in the commit message were correct; I haven’t verified this).  This could probably be folded into patch 0004, but I left it separate for clarity.
* Added `0040-rescan-check-revision.patch`.  This modifies patch 0013 in four ways:
    1. The SII cache-and-reuse behaviour can be disabled via `–disable-sii-cache` at configure time, rather than requiring modifying a header file.
    2. The revision number is also verified before using the cached version (this resolves some issues when the device firmware is upgraded).
    3. If both the alias and serial are read as 0, it will no longer bother reading the vendor/product/revision, as it is now known that the SII is not in the cache.
    4. Several similar states are consolidated into one.
* Added `0041-load-sii-from-file.patch`, from Graeme Foot’s recent patch; but I’ve made the following modifications:
    1. The functionality is disabled by default.
    2. At configure time, you can use `–enable-sii-override` to activate it, using the standard udev/hotplug lookup process.
    3. At configure time, you can use `–enable-sii-override=/lib/firmware` (or another path) to activate it using a direct loading method.
    4. A bug where it would fail to load 6 words of the SII when not loading from a file has been corrected.
    5. Several places where it exited the state prematurely have been corrected.
    6. It will cooperate as expected with patch 0013, although note that it’s not as efficient as it could be (it will reload some of the values that patch 0013 already read when checking the SII cache; but trying to improve this would make the code really awkward).
* Added `0042-load-sii-from-file-async.patch`, which makes the above firmware loading asynchronous so it doesn’t block the master thread, since that makes me uncomfortable.  Again this could be folded into the prior patch but I left it separate so that the differences are apparent and easier to review.  I’m also pondering whether the firmware loading stuff (or perhaps even the whole SII loading stuff including the reuse cache) should be broken out into a separate sub-FSM file.  It’s not really complicated but it’s a bit wordy, particularly with the direct-loading support included.  Open to suggestions here.

(Note these additions aren’t breaking changes, but I didn’t want to renumber the patches just because of that.  They should be able to apply prior to the breaking change patches if you want to try it that way.)

# 20160502

* Updated to default branch 42b62867574d.
    * Dropped several patches already included in default branch:
        * frank-04-string-download.patch: in changeset a26dee45c467
        * frank-07-sdo-up-download.patch: in changeset 1aee02c1e294
        * frank-08-mrproper.patch: in changeset ae24ede76e16
        * frank-16-frame-corruption.patch: in changeset a380cce7d6f0
        * frank-29-init-restart.patch: in changeset ecef88726fc3
        * gavinl-0001-deactivate_unmap.patch: in changeset f99e5b11806c
        * gavinl-0002-dc_refclk_not_op.patch: in changeset 559f2f9c5b08
        * gavinl-0003-refclk_nxio.patch: in changeset 3affe9cd0b66
        * gavinl-0004-abort_slave_config_reg_requests.patch: in changeset f2bc4000e47a
        * gavinl-0005-abort_detached_requests.patch: in changeset 0e4d098db815
        * gavinl-1002-foe_read_debug.patch: in changeset 764801a0f2aa
        * knud-0003-Eoe-mac-address-now-derived-from-unique-mac.patch: in changeset e25af8bd3957
        * knud-0014-Maximum-length-of-foe-filename-extended-to-255.patch: in changeset d123727b805b
        * knud-0015-Internal-SDO-requests-now-synchronized-with-external.patch: in changeset a2701af27fde
* **Dropped** (because they wouldn't apply cleanly, and didn't appear needed):
    * knud-0001-Use-call-back-functions.patch
    * knud-0002-Input-output-counter-not-incremented-for-lrw.patch
* **Dropped** (because there are better alternatives):
    * knud-0004-Semaphores-replaced-with-mutexes.patch
    * gavinl-1006-sdo_dict_fast_arrays.patch
* **Added**:
    * 0001-graemef-dc_fixes_and_helpers.patch
        Adds `ecrt_master_setup_domain_memory` and `ecrt_master_deactivate_slaves`; fixes up application-selected reference clocks.  (from http://lists.etherlab.org/pipermail/etherlab-users/2016/002916.html)
    * 0002-junyuan-dc_sync_issues.patch
        This sorts out some timing issues to do with slave dc syncing.
    * 0003-frank-index-reuse.patch
        Do not reuse the index of a pending datagram.
    * 0004-gavinl-Semaphores-replaced-with-mutexes.patch
        Replaces knud-0004-Semaphores-replaced-with-mutexes.patch
        This is the same basic idea (allow using rt-mutexes) but instead of being replaced unconditionally this introduces a thin abstraction so that you can select regular semaphores or rt-mutexes at configure time.  The default is semaphores, use `--enable-rtmutex` to get rt-mutexes.  (I'm hoping that making them optional will make this easier to merge to mainline.)
    * 0102-gavinl-sdo_logging_verbosity.patch
        New patch; this makes changeset a2701af27fde "Internal SDO requests now synchronized with external requests." a bit less noisy.
    * 0103-gavinl-clear_on_deactivate.patch:
        New patch; it clears the master configuration when
`ecrt_master_deactivate` is called even if `ecrt_master_activate` has not been called prior (though it still logs a warning).  In particular, this allows creating slave configs and request objects and then discarding them.
(I wanted this so that I could use the `sdo_request` API to perform some requests asynchronously via the idle thread prior to activating the master.)

* **Updated**
    * Knud patches (0005-0013, 0016, 0017)
        These improve mailbox handling and SII scan performance.  They're mostly the same as before except that I've updated the locking for the above and added changes for the new EoE FSM; I don't use EoE myself so I haven't tested these, but it should be consistent with the rest of the mailbox FSMs at least.

## Older

Patchset changes before this point are more murky...
(the patchset was previously based on the `stable-1.5` branch)
