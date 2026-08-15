"""Who is submitting, in the only sense this service needs.

There are no accounts, so the question is never who somebody is. It is whether
two submissions came from the same place, which is what a rate limit is made of,
and that is answerable without keeping anything a person could be identified
from afterwards.

So the address is hashed with a salt and the hash is what the job carries. It
tells two submitters apart for as long as the salt lives and it is not an
address in a table that somebody could later be asked for. Truncated to a
prefix, because a rate limit needs to distinguish a few hundred submitters and
sixteen hexadecimal characters distinguishes rather more than that.

The address itself comes from the socket unless the deployment says it is behind
a proxy. `X-Forwarded-For` is written by whatever spoke last, so a service that
believes it when nothing strips it has a rate limit keyed on a value the
submitter chooses, which is not a rate limit. A service that ignores it behind a
proxy has the opposite fault: every submission arrives from the proxy and counts
against one bucket. Neither is safe to guess, so it is a setting.
"""

from __future__ import annotations

import hashlib

from starlette.requests import HTTPConnection

#: How much of the hash is kept.
SUBMITTER_LENGTH = 16

#: What an address that could not be read is called.
#:
#: A connection with no peer is a test client or a Unix socket rather than
#: anything a rate limit is for, and grouping them under one name is better than
#: letting each be its own bucket.
UNKNOWN = "unknown"


def address_of(connection: HTTPConnection, *, trust_forwarded_for: bool) -> str:
    """Where a request came from, as far as this deployment can tell.

    The first entry in `X-Forwarded-For` when the header is trusted, which is
    the client as the nearest proxy saw it, and the socket's peer otherwise.
    """
    if trust_forwarded_for:
        forwarded = connection.headers.get("x-forwarded-for", "")
        first = forwarded.split(",")[0].strip()
        if first:
            return first
    return connection.client.host if connection.client else UNKNOWN


def submitter_of(address: str, salt: str) -> str:
    """The name this address is counted under.

    A salted hash, truncated. Not reversible without the salt, and stable while
    the salt is, which is exactly as long as a rate limit needs a name to mean
    the same thing.
    """
    digest = hashlib.sha256(f"{salt}:{address}".encode())
    return digest.hexdigest()[:SUBMITTER_LENGTH]
