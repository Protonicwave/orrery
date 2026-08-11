# The trajectory format

What a run looked like, frame by frame. Written by `sim/trajectory.hpp`, and
specified here so that something which is not this program can read one.
ADR-0033 records why this is a separate format from the checkpoint.

## Conventions

All integers are unsigned and little-endian, whatever the machine that wrote
them. All floating-point values are IEEE 754, stored as the bit pattern of their
width, so a negative zero, an infinity and a NaN all survive a round trip.

`R` is the size of a scalar: 8 bytes in a double-precision build, 4 in a
single-precision one. The header says which, and a reader compiled the other way
round refuses the file rather than reading it at the wrong stride.

Checksums are 64-bit FNV-1a over the bytes covered, with offset basis
`0xcbf29ce484222325` and prime `0x100000001b3`.

## Layout

A header, then any number of frames, then the end of the file.

### Header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Magic, the ASCII bytes `ORRERYTJ` |
| 8 | 4 | Format version. Currently 1 |
| 12 | 4 | Flags |
| 16 | 8 | `N`, the number of particles |
| 24 | R | The timestep |
| 24 + R | N × R | The masses, in particle order |
| 24 + R + N×R | 8 | Checksum of every byte from offset 0 to here |

Flags:

| Bit | Meaning |
| --- | --- |
| 0 | The file was written in single precision, so `R` is 4 |
| 1 | Frames carry velocities as well as positions |

Every other bit is reserved and must be zero. A reader refuses a file that sets
one, rather than ignoring a field it does not understand.

The masses are in the header because they do not change during a run. A
million-particle trajectory of a thousand frames would otherwise carry eight
gigabytes of a number that was already known, and having them at all is what lets
a reader compute an energy from a frame without the configuration that produced
it.

### Frame

Frames follow the header back to back. Each is:

| Size | Field |
| --- | --- |
| 8 | The step number, counting from zero at the state the run started from |
| R | The simulated time, which is the step number times the timestep |
| N × R | All `x` positions, in particle order |
| N × R | All `y` positions |
| N × R | All `z` positions |
| N × R | All `x` velocities, only if bit 1 of the flags is set |
| N × R | All `y` velocities, likewise |
| N × R | All `z` velocities, likewise |
| 8 | Checksum of this frame, from its step number to here |

One component of every particle, then the next: this is the structure-of-arrays
layout the solvers use (ADR-0004), written out as it is held. A reader that wants
particle 5's position reads element 5 of three arrays.

A frame is therefore `8 + R + 3NR` bytes without velocities and `8 + R + 6NR`
with them, plus 8 for the checksum.

## Why there is no frame count

There is nowhere to put one. The field would have to be in the header, and the
header is written before the run has taken a step. A run that is killed by a full
disc, a closed lid or a scheduler is exactly the run whose output someone will
want to look at, and its header would then describe frames that are not in the
file.

So a reader consumes frames until the file ends. The cost is that counting them
means reading the file; the benefit is that a file from an interrupted run is a
valid file that stops early rather than a broken one.

The checksum is per frame for the same reason. A whole-file checksum can only be
verified once the file is complete, so it would be absent from precisely the
files that most need checking. Per frame, a reader accepts every frame that was
written completely and rejects a final one that was cut in half, and can say
which is which.

## Reading one

```
orrery inspect cluster.otj
```

reports the particle count, the timestep, whether velocities are present, how
many frames there are and the time of the last one.

A reader in another language needs about thirty lines. In Python, with NumPy,
for a double-precision file without velocities:

```python
import numpy as np

with open("cluster.otj", "rb") as file:
    assert file.read(8) == b"ORRERYTJ"
    version, flags = np.frombuffer(file.read(8), dtype="<u4")
    assert version == 1 and flags & 1 == 0        # double precision
    count = int(np.frombuffer(file.read(8), dtype="<u8")[0])
    timestep = float(np.frombuffer(file.read(8), dtype="<f8")[0])
    masses = np.frombuffer(file.read(8 * count), dtype="<f8")
    file.read(8)                                   # the header checksum

    frames = []
    while chunk := file.read(16):                  # step and time
        step, time = np.frombuffer(chunk, dtype="<u8")[0], \
                     np.frombuffer(chunk[8:], dtype="<f8")[0]
        xyz = np.frombuffer(file.read(3 * 8 * count), dtype="<f8")
        file.read(8)                               # the frame checksum
        frames.append((step, time, xyz.reshape(3, count)))
```

Phase 13's bindings will provide this properly. It is written out here because a
format specification that has never been read by anything but its own writer is
a specification nobody has checked.

## What this is not

A checkpoint. A trajectory frame may hold no velocities, holds no accelerations
even when it does, and is written at whatever stride suited looking at the run. A
run cannot be resumed from one. `docs/formats/checkpoint.md` is the format for
that.
