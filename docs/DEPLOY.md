# Deploying the group service

The Linux box `beagle@10.0.0.115` runs the appserver plus one `carrierService`
process per group. A restart takes **every** group offline for a few seconds,
including group 1 with 400+ members, so restarts are the operator's call, never
an automatic step.

## Never `git checkout` in the live directory

`~/devs/Elastos.Service.CarrierGroup` is what systemd runs: `ExecStart` points
at `linux/ui/appserver/startCarrierByServer.sh`, and the manager `execv`s
`./carrierService` relative to that directory. Switching branches there rewrites
files underneath a running service.

Until the commit that added this file, the repo also **tracked the built
binary** (`linux/ui/appserver/carrierService`). A checkout could therefore
replace the deployed binary with a stale committed one — the next restart would
silently launch old code — or delete it outright when moving to a commit that no
longer tracks it. The binary, `libcarrierManager.so` and the per-machine
`chatrobot_config.ini` are now ignored rather than tracked.

Build in a separate worktree and copy the artifacts in.

## Build

```bash
# once: the SDK, installed to its own prefix (not /usr/local)
cmake -S ~/devs/agentnet-v2-native -B ~/devs/agentnet-v2-native/build/linux \
      -DCMAKE_BUILD_TYPE=Release -DENABLE_APPS=FALSE -DENABLE_TESTS=FALSE \
      -DCMAKE_INSTALL_PREFIX=~/devs/agentnet-v2-native/dist
cmake --build ~/devs/agentnet-v2-native/build/linux -j4 --target install

# the service, against that SDK
cmake -S ~/devs/agentnet-v2-group/linux/service -B ~/devs/agentnet-v2-group/build/service \
      -DCARRIER_SDK_ROOT=~/devs/agentnet-v2-native -DCMAKE_BUILD_TYPE=Release
cmake --build ~/devs/agentnet-v2-group/build/service -j4
```

If a dependency download fails, see `BUILD-DEPS.md` in the native repo: three
upstream tarball URLs are dead, and a failed download leaves a 0-byte file that
is then treated as a valid cache entry.

Since 2026-11-03 the service runs native `e8eb0b0` (0xli
`feat/agentnet-proto-v2`: Express pull from every node, per-node watermark),
built in its own worktree `~/devs/agentnet-v2-native-e8eb0b0` with its own
`dist/`, and the service is built in `build/service-e8` with
`-DCARRIER_SDK_ROOT=~/devs/agentnet-v2-native-e8eb0b0`. Never reinstall a
`dist/lib` the running groups have mapped: build a new prefix and switch
`CARRIER_LIB_DIR`. The SDK's dependency tarballs are read from
`<sdk>/build/.tarballs`; seed it (hashes in the SDK's `docs/BUILD-DEPS.md`).

## Stage, then swap

Copy in, then `mv` — never `cp` over the running binary, which fails with
"Text file busy" because the group processes are executing it.

```bash
A=~/devs/Elastos.Service.CarrierGroup/linux/ui/appserver
cp -p $A/carrierService $A/carrierService.bak-$(date +%Y%m%d)
cp ~/devs/agentnet-v2-group/build/service/carrierService $A/carrierService.new
chmod 755 $A/carrierService.new && mv -f $A/carrierService.new $A/carrierService
```

Python and shell files are staged the same way (`py_compile` / `bash -n` first).
Leave `chatrobot_config.ini` alone: it is machine-local.

## Library path

The service links the SDK you built; the old bundled `.so` files are gone.
`CARRIER_LIB_DIR` must point at the installed prefix, via a systemd drop-in:

```ini
# ~/.config/systemd/user/carriergroup.service.d/override.conf
[Service]
Environment=CARRIER_LIB_DIR=/home/beagle/devs/agentnet-v2-native-e8eb0b0/dist/lib
```

`startCarrierByServer.sh` exits 1 if `libcarrier.so` is missing there. Check with
`ldd carrierService`: libcarrier, libcarriersession and libcrystal should all
resolve from that prefix, not `/usr/local/lib`.

## Restart and verify

```bash
systemctl --user restart carriergroup.service
systemctl --user is-active carriergroup.service     # active
ps -ef | grep -c '[C]arrierService'                 # one per group
curl -sS http://127.0.0.1:5000/groups               # nicknames intact
journalctl --user -u carriergroup.service | grep OnCarrierConnection
```

End to end, with throwaway identities and a throwaway group only (never a real
group): `linux/tools/cg_e2e.c` is a one-identity Carrier member that joins a
group, sends files byte for byte as single messages and records what it receives
(build line in its header). `docs/GROUP_FILE_MESSAGE.md` has the 2026-11-03 run.
A new binary can be tried first on a second appserver instance that shares
nothing with the live one: copy `server.py`, `chatrobot_restful_api.py`,
`__init__.py`, `libcarrierManager.so` and the new `carrierService` to a scratch
directory and run the venv python there with `--port 5099 --manager_port 3399
--data_path <scratch>/rt` and `LD_LIBRARY_PATH=$CARRIER_LIB_DIR`.

Keep a rollback ready: if the service is not active, or fewer processes than
groups come up, restore the `.bak-*` binary and script, remove the drop-in,
`daemon-reload` and restart. A first deploy of this branch did exactly that when
every group process segfaulted at startup, and the groups were back within a
minute.

Group display names are only published when a group starts, so a rename via
`/update` needs a restart before contacts see it.
