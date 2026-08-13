# ADR-0049: Put the run and the moment in the address

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The instrument now holds three published runs rather than one, and a run is
several minutes of model time that a reader moves about in. Two things follow.
Something has to say which run is loaded, and something has to be sendable: the
useful sentence about a galaxy collision is "look at what the tidal tail is
doing at t = 120", and it is useless without a way of arriving there.

The client is a static page on GitHub Pages. There is no server to ask.

## Decision

Which run is loaded and which moment the transport is at are both properties of
the address, as query parameters: `?run=cluster&t=12.5`. Nothing else the
interface holds is.

Query parameters rather than a path. A path such as `/instrument/cluster` needs
the server to serve the client for an address that is not a file, and a GitHub
Pages project site will not: it answers with its own 404 page. The usual
workaround, a copy of the client at `404.html`, is a second entry point that has
to be kept in step with the first, and it exists to make the address look tidy.

Query parameters rather than a fragment. A fragment would work, and it is not
sent to the server at all, which is a small advantage. The client already reads
`?renderer=webgl2` to pin the backend, and having two mechanisms for reading the
address means two places to look when one of them is wrong.

The gallery's entries are anchors with real `href`s to those addresses, and
choosing one is intercepted so that the client is not fetched again. The thing
being intercepted has to work on its own: a link that only works when JavaScript
has run cannot be opened in a second tab, copied from a context menu, or read by
anything that reads links. `popstate` is answered for the same reason, so that
back and forward move between runs rather than changing the address under a page
that has not noticed.

The address is rewritten when the transport is moved by hand and not while a run
is playing. A moment worth sending someone is one that was chosen, and following
playback would produce sixty history entries a second.

Nothing else goes in. The exposure, the sprite radius, the tone curve and which
registers are shown are preferences, and an address carrying them would be an
address nobody could read and a link that imposed the sender's settings on the
person who opened it.

## What an address that names nothing gets

The collision, which is the run the repository demonstrates. An address naming a
run that is not published gets the same, rather than an error: a reader
following a link to a run that has since been renamed wanted, at least
approximately, what the gallery holds now. A moment that is not a number, or is
before the run starts, names no moment and the run opens at its beginning.

## Consequences

A moment named by an address is not always reachable when the page loads. The
trajectory is read in order and a moment near the end of a long run may be a
minute of network away, so the seek is attempted again each time more of the run
arrives, until the frame for it has been decoded.

The instrument goes to the frame nearest the moment asked for rather than to the
moment itself, because a trajectory is written at a stride and the instants
between two frames are instants the run did not record. The clock then shows the
frame's own time rather than the one in the address, which is the instant being
drawn.

The transport's step follows from the same fact and was changed to match: it
moves by one trajectory frame rather than by one integrator step, so an arrow
key always changes the picture.
