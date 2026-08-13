# Security policy

## Supported versions

The most recent release. This project is small enough that maintaining a fix on
an older line would mean claiming an attention it would not get, so a security
fix goes into the next release rather than being backported.

## What is worth reporting

Orrery is a simulator. It opens no sockets, starts no processes and asks for no
credentials, so its attack surface is the files it reads:

- The configuration reader in `src/sim/config_file.cpp`, which parses text.
- The trajectory and checkpoint readers in `src/sim/`, which parse binary. These
  are the ones to look at first. A checkpoint carries a whole simulation state,
  including array lengths, and a file that claims a length it does not have is
  the obvious shape for a fault of this kind.

A memory-safety fault reachable by opening a file that came from somewhere else
is a vulnerability here, and so is anything that gets a configuration file
treated as something other than data. Please report those privately.

What is not a vulnerability: a run that exhausts memory because it was asked for
more particles than the machine has, a numerical result that is wrong or
imprecise, and a crash on a file this project wrote and then truncated by hand.
The first is arithmetic, the second belongs in a bug report, and the third is
covered by the trajectory format's design, which treats a short file as a valid
file that stops early.

## Reporting

Use GitHub's private vulnerability reporting, from the Security tab of this
repository. It keeps the report and the discussion private until there is a fix
to publish.

Please do not open a public issue for a memory-safety fault in a parser.

A report is most useful with the input that triggers it, the build preset and
the compiler. If the fault was found under a sanitiser, the sanitiser's output
identifies it faster than anything else.

## What happens then

This project is maintained by one person and the honest expectation is days
rather than hours. You will get an acknowledgement that the report has been
read, and then either a fix or a statement of why the behaviour is intended.

The test suite runs under the address, undefined-behaviour and thread
sanitisers on every pull request, so a fix arrives with the case that would have
caught it.
