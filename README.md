# trivialInit

A mixin-composed C++ init system that reads systemd unit files.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     InitSystem                          │
│  (MixinCompose<InitSystem, Journal, Filesystem,         │
│    Signal, Process, UnitScan, UnitParse, UnitExec, Tui>)│
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Phase Pipeline (type-dispatched via execute(phase))    │
│                                                         │
│  EarlyMount ──► SignalSetup ──► UnitDiscovery           │
│       │              │               │                  │
│  FilesystemMixin SignalMixin   UnitScanMixin            │
│                                      │                  │
│                              UnitParse ──► DepResolve   │
│                                   │            │        │
│                             UnitParseMixin  UnitExecMixin│
│                                                │        │
│                                          UnitExecute    │
│                                                │        │
│                                           TuiStart      │
│                                                │        │
│                                          ┌─────┴─────┐  │
│                                          │ Main Loop  │  │
│                                          │  poll()    │  │
│                                          │  reap()    │  │
│                                          │  restart() │  │
│                                          │  render()  │  │
│                                          └─────┬─────┘  │
│                                                │        │
│                                           Shutdown      │
└─────────────────────────────────────────────────────────┘
```

## Mixin Axes

| Mixin | Role | Phase(s) |
|-------|------|----------|
| `JournalMixin` | Structured logging | All (cross-cutting) |
| `FilesystemMixin` | Mount /proc, /sys, /dev | EarlyMount, Shutdown |
| `SignalMixin` | signalfd + epoll | SignalSetup |
| `ProcessMixin` | fork/exec, reaping | Runtime |
| `UnitScanMixin` | Find .service/.target/.mount | UnitDiscovery |
| `UnitParseMixin` | Parse INI-format unit files | UnitParse |
| `UnitExecMixin` | Topological sort + execute | DependencyResolve, UnitExecute |
| `TuiMixin` | ncurses status display | TuiStart |

Each mixin inherits `MixinBase<Derived>` (CRTP), giving it `self()` to access
every other mixin's members on the composed type.

## Building

```bash
# Normal build (for testing on host)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build

# Static build (for initramfs / QEMU)
cmake -B build-static -DTINIT_STATIC=ON -DTINIT_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-Os"
cmake --build build-static -j$(nproc) --target trivialInit
```

## Testing with QEMU

### Quick test (initramfs only, no disk)
```bash
./scripts/qemu_test.sh --kernel /boot/vmlinuz-linux
```

### Full Arch Linux disk image
```bash
./scripts/setup_arch_qemu.sh
```

## Binaries

- `trivialInit` — the init binary (runs as PID 1 or in test mode)
- `trivialctl` — TUI monitor that reads unit files without executing

## systemd Compatibility

Reads standard unit file locations in priority order:
1. `/etc/systemd/system` (admin overrides)
2. `/run/systemd/system` (runtime)
3. `/usr/lib/systemd/system` (packages)
4. `/lib/systemd/system` (legacy)

Supported unit types: `.service`, `.target`, `.mount`

Supported directives:
- `[Unit]`: Description, Wants, Requires, After, Before, Conflicts
- `[Service]`: Type, ExecStart, ExecStop, Restart, User, Group, WorkingDirectory, Environment
- `[Install]`: WantedBy, RequiredBy, Alias
- `[Mount]`: What, Where, Type, Options
