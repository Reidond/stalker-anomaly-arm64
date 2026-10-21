"""lldb broker for LuaJIT's TXM JIT region: Xcode / command-line lldb stand in for StikDebug.

On iPadOS 26+ devices with TXM a page cannot be executed until a debugger has written to
it. LuaJIT (luajit_txm_init) maps an R-X region and asks for the write with the StikDebug
"universal" protocol: `brk #0xf00d`, command in x16, arguments in x0/x1.

    x16 = 1  prepare region: x0 = address (0 = allocate), x1 = length -> x0 = address
    x16 = 0  detach

This script services those breakpoints from a target stop hook, exactly like StikDebug's
universal.js: one byte is written into every 16 KB page of the region (the debugger write is
what authorizes the page), pc is advanced past the brk, and the process continues.

Use it from Xcode (scheme > Run > Options > "LLDB Init File" = a file that imports this) or from
a command-line lldb attached to the app:

    (lldb) command script import tools/jit_lab/txm_jit_lldb.py

Environment:
    TXM_PAGES_PER_WRITE  pages authorized per debugger write (default 1, like StikDebug). A
                         write spanning k pages writes (k-1)*16 KB+1 bytes and authorizes all k.
    TXM_HONOR_DETACH     1: really detach when the app asks. Default: acknowledge and stay
                         attached, so an Xcode session keeps its console and breakpoints.
"""

import os
import time

import lldb

BRK_F00D = 0xD43E01A0  # brk #0xf00d
CMD_DETACH = 0
CMD_PREPARE_REGION = 1

PAGE_SIZE = 0x4000
FILL_BYTE = 0x69  # the byte StikDebug writes

PAGES_PER_WRITE = max(1, int(os.environ.get("TXM_PAGES_PER_WRITE", "1")))
HONOR_DETACH = os.environ.get("TXM_HONOR_DETACH") == "1"


def _read_u32(process, addr):
    err = lldb.SBError()
    data = process.ReadMemory(addr, 4, err)
    if not err.Success() or data is None or len(data) != 4:
        return None
    return int.from_bytes(data, "little")


def _authorize(process, address, length, log):
    """Write into every page of [address, address + length). Returns True on success."""
    first = address & ~(PAGE_SIZE - 1)
    last = (address + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
    pages = (last - first) // PAGE_SIZE
    started = time.monotonic()
    done = 0
    while done < pages:
        group = min(PAGES_PER_WRITE, pages - done)
        data = bytes([FILL_BYTE]) * ((group - 1) * PAGE_SIZE + 1)
        err = lldb.SBError()
        process.WriteMemory(first + done * PAGE_SIZE, data, err)
        if not err.Success():
            log("[txm] write failed at 0x%x after %d/%d pages: %s"
                % (first + done * PAGE_SIZE, done, pages, err.GetCString()))
            return False
        done += group
    log("[txm] authorized %d pages (%.1f MiB) at 0x%x in %.2f s"
        % (pages, pages * PAGE_SIZE / 1048576.0, address, time.monotonic() - started))
    return True


class Broker:
    """Target stop hook: answers `brk #0xf00d` and lets every other stop through."""

    def __init__(self, target, extra_args, internal_dict):
        pass

    def handle_stop(self, exe_ctx, stream):
        def log(message):
            stream.Print(message + "\n")

        process = exe_ctx.GetProcess()
        frame = exe_ctx.GetFrame()
        if not frame.IsValid():
            return True
        pc = frame.GetPC()
        if _read_u32(process, pc) != BRK_F00D:
            return True  # not ours: stop normally

        command = frame.FindRegister("x16").GetValueAsUnsigned()
        if command == CMD_PREPARE_REGION:
            address = frame.FindRegister("x0").GetValueAsUnsigned()
            length = frame.FindRegister("x1").GetValueAsUnsigned()
            if address == 0:
                err = lldb.SBError()
                address = process.AllocateMemory(
                    length, lldb.ePermissionsReadable | lldb.ePermissionsExecutable, err)
                if not err.Success():
                    log("[txm] allocation of %d bytes failed: %s" % (length, err.GetCString()))
                    return True
            if not _authorize(process, address, length, log):
                return True  # stay stopped: continuing would claim success
            frame.FindRegister("x0").SetValueFromCString("0x%x" % address)
        elif command == CMD_DETACH:
            if HONOR_DETACH:
                log("[txm] detaching")
                frame.SetPC(pc + 4)
                process.Detach()
                return False
            log("[txm] detach requested; staying attached")
        else:
            log("[txm] unknown command x16=%d at 0x%x" % (command, pc))
            return True

        if not frame.SetPC(pc + 4):
            log("[txm] could not advance pc past the brk at 0x%x" % pc)
            return True
        return False  # handled: continue


def txm_wait(debugger, command, exe_ctx, result, internal_dict):
    """txm_wait [seconds]: keep a batch lldb session alive while the app runs.

    After `device process attach` the app runs and the stop hook services its breakpoints; a
    batch session would exit (and kill the app) as soon as its commands are done. This waits until
    the process exits, is detached, or the optional timeout passes.
    """
    deadline = time.monotonic() + (float(command) if command.strip() else 1e9)
    process = debugger.GetSelectedTarget().GetProcess()
    ended = (lldb.eStateExited, lldb.eStateDetached, lldb.eStateCrashed, lldb.eStateInvalid)
    while time.monotonic() < deadline:
        state = process.GetState()
        if state in ended:
            break
        if state == lldb.eStateStopped:
            # A stop the broker did not take (another exception): report and resume.
            result.AppendMessage("[txm] process stopped: %s" % process.GetSelectedThread().GetStopDescription(200))
            process.Continue()
        time.sleep(0.2)
    result.AppendMessage("[txm] process state: %s" % debugger.StateAsCString(process.GetState()))


def __lldb_init_module(debugger, internal_dict):
    if os.environ.get("TXM_NO_HOOK") != "1":  # TXM_NO_HOOK=1 is for testing the no-broker case
        debugger.HandleCommand("target stop-hook add -P %s.Broker" % __name__)
    debugger.HandleCommand("command script add -f %s.txm_wait txm_wait" % __name__)
    print("[txm] LuaJIT TXM broker installed (%d page(s) per write, %s)"
          % (PAGES_PER_WRITE, "honoring detach" if HONOR_DETACH else "staying attached on detach"))
