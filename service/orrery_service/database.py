"""The connection pool, and the schema it applies on the way up.

Both processes open one of these: the API to read and write jobs, the worker to
claim them. Neither owns the database, which is why the schema is applied by
whichever starts first and is written to be safe to apply again.
"""

from __future__ import annotations

import asyncio
import sys
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from importlib import resources

import psycopg
from psycopg.rows import dict_row
from psycopg_pool import AsyncConnectionPool


def use_compatible_event_loop() -> None:
    """Choose an event loop psycopg's async connections can work on.

    Windows defaults to the proactor loop, on which those connections open and
    then never answer. Called by both entry points and by the test suite rather
    than done when this module is imported, because a library that changes the
    event loop policy behind its caller's back is a library that breaks
    somebody else's program.

    The deployment is Linux, where this does nothing. It is here so that the
    service can be run and its suite taken on the machine somebody is working
    on.
    """
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())


#: The schema, read from the package rather than from a path beside it, so that
#: it is found the same way whether the service is running from a checkout or
#: from an installed wheel inside an image.
SCHEMA = resources.files("orrery_service").joinpath("schema.sql").read_text("utf-8")


async def apply_schema(connection: psycopg.AsyncConnection) -> None:
    await connection.execute(SCHEMA)
    await connection.commit()


@asynccontextmanager
async def pool(
    url: str, *, minimum: int = 1, maximum: int = 8
) -> AsyncIterator[AsyncConnectionPool]:
    """A pool over `url`, with the schema applied before anything else uses it.

    Small by default. The API's work per request is one or two short statements
    and the worker's is one claim every few minutes, so a pool sized for a busy
    application would be a pool of connections a PostgreSQL has to keep and
    nothing has to use.
    """
    connections = AsyncConnectionPool(
        url,
        min_size=minimum,
        max_size=maximum,
        # Rows come back as dictionaries throughout. The alternative, indexing
        # tuples by position, makes every query and its reader two things that
        # have to be changed together and cannot be checked.
        kwargs={"row_factory": dict_row},
        open=False,
    )
    await connections.open(wait=True)
    try:
        async with connections.connection() as connection:
            await apply_schema(connection)
        yield connections
    finally:
        await connections.close()
