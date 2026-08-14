"""Where trajectories live, which is not the database.

ADR-0057 records the decision. A trajectory is a few tens of megabytes of
Float32, it is written once and read as a stream of ranges, and it is the one
thing in this system that a database is a bad container for: it would be moved
through the connection pool a chunk at a time, it would sit in the same backups
as the metadata, and the ranged read the client depends on would become a query.

The client fetches through the API rather than from the store directly. That is
one origin instead of two, which means the browser needs no cross-origin policy
on a bucket, and it means the store's credentials and layout stay inside the
service. It costs the API the bandwidth of the download, which at this scale is
one trajectory at a time and is the right trade: a presigned URL would be the
answer at a scale this service is explicitly not built for.

boto3 is synchronous and is used synchronously. The two callers are the worker,
which is a process doing one thing at a time, and the API's download endpoint,
which hands Starlette a synchronous iterator and has it run in a thread. Neither
wants an async S3 client, and adding one would be a second way to do the same
thing.
"""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import boto3
from botocore.client import Config
from botocore.exceptions import ClientError

from .settings import Settings

#: How much of a trajectory is read from the store at a time on the way out.
#:
#: A quarter of a megabyte. Large enough that a forty megabyte trajectory is a
#: few hundred reads rather than a few thousand, and small enough that the
#: first bytes reach the client without a large buffer being filled first, which
#: is what the 500 ms budget on the first byte is about.
CHUNK = 256 * 1024


class StorageError(RuntimeError):
    """The object store could not do what was asked of it."""


@dataclass(frozen=True)
class Stored:
    """One object, once it is there."""

    key: str
    size: int


def trajectory_key(identifier: str) -> str:
    return f"jobs/{identifier}/trajectory.otj"


def diagnostics_key(identifier: str) -> str:
    return f"jobs/{identifier}/diagnostics.csv"


class Storage:
    """The bucket, and the four things this service does to it."""

    def __init__(self, settings: Settings) -> None:
        self._bucket = settings.storage_bucket
        self._client = boto3.client(
            "s3",
            endpoint_url=settings.storage_endpoint,
            aws_access_key_id=settings.storage_access_key,
            aws_secret_access_key=settings.storage_secret_key,
            region_name=settings.storage_region,
            # Path addressing, because the endpoint is as often a MinIO on a
            # hostname with no wildcard certificate as it is Amazon's, and
            # virtual-host addressing does not work against the first.
            config=Config(s3={"addressing_style": "path"}),
        )

    @property
    def bucket(self) -> str:
        return self._bucket

    def ensure_bucket(self) -> None:
        """Create the bucket if it is not there.

        Idempotent, and run on start-up for the same reason the schema is: a
        deployment that needed a separate step to be usable has a state where
        the image is running and the step has not been taken.
        """
        try:
            self._client.head_bucket(Bucket=self._bucket)
            return
        except ClientError as error:
            code = error.response.get("Error", {}).get("Code", "")
            if code not in ("404", "NoSuchBucket", "NotFound"):
                raise StorageError(f"{self._bucket}: {error}") from error

        try:
            self._client.create_bucket(Bucket=self._bucket)
        except ClientError as error:
            code = error.response.get("Error", {}).get("Code", "")
            # Another process got there first between the two calls above, which
            # is not a failure: what was wanted was a bucket, and there is one.
            if code not in ("BucketAlreadyOwnedByYou", "BucketAlreadyExists"):
                raise StorageError(f"{self._bucket}: {error}") from error

    def put_file(self, key: str, path: Path, content_type: str) -> Stored:
        """Upload a file the worker has just written."""
        try:
            self._client.upload_file(
                str(path),
                self._bucket,
                key,
                ExtraArgs={"ContentType": content_type},
            )
        except (ClientError, OSError) as error:
            raise StorageError(f"{key}: {error}") from error
        return Stored(key=key, size=path.stat().st_size)

    def size(self, key: str) -> int | None:
        """How long the object is, or None if there is no such object."""
        try:
            answer = self._client.head_object(Bucket=self._bucket, Key=key)
        except ClientError:
            return None
        return int(answer["ContentLength"])

    def read(self, key: str, start: int, end: int) -> Iterator[bytes]:
        """Bytes `[start, end]` of an object, in chunks, inclusive of `end`.

        Inclusive because that is what an HTTP range is, and converting between
        the two conventions in the middle of a proxy is how a download comes out
        one byte short.
        """
        try:
            answer = self._client.get_object(
                Bucket=self._bucket, Key=key, Range=f"bytes={start}-{end}"
            )
        except ClientError as error:
            raise StorageError(f"{key}: {error}") from error

        body = answer["Body"]
        try:
            while True:
                chunk = body.read(CHUNK)
                if not chunk:
                    return
                yield chunk
        finally:
            body.close()
