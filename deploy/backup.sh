#!/bin/sh
# Back up the metadata database.
#
#     deploy/backup.sh [directory]
#
# The jobs, the queue and what every finished run measured. Not the
# trajectories: those are in object storage, they are the large half by three
# orders of magnitude, and they expire after a week anyway, so a backup that
# carried them would be mostly a copy of things about to be deleted. What is
# worth keeping is the record of what ran, and it is small enough to keep often.
#
# The dump is PostgreSQL's own custom format, which `pg_restore` can read
# selectively and which compresses itself. It is written through a pipe from
# `compose exec` rather than into a volume and copied out, so nothing is left
# inside the container if this fails half way.
#
# Every dump is listed after it is written. A backup that has never been read
# is a file, not a backup, and `pg_restore --list` is the cheapest way to find
# out that what was written is not a dump at all. `deploy/restore.sh` is the
# other half, and `deploy/README.md` says how often to take one.

set -eu

DIRECTORY="${1:-backups}"
KEEP_DAYS="${ORRERY_BACKUP_DAYS:-14}"
USER_NAME="${ORRERY_POSTGRES_USER:-orrery}"
DATABASE="${ORRERY_POSTGRES_DB:-orrery}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The deployment's two files, unless something says otherwise. Continuous
# integration says otherwise: it runs this against the plain stack, so that the
# script somebody's cron calls is one that has been taken rather than one that
# has been read. Deliberately unquoted below, since it is several arguments.
FILES="${ORRERY_COMPOSE_FILES:--f $HERE/compose.yaml -f $HERE/production.yaml}"
COMPOSE="docker compose $FILES"

mkdir -p "$DIRECTORY"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
FILE="$DIRECTORY/orrery-$STAMP.dump"

# --no-owner, because the restore may go into a database owned by somebody
# else: a dump that insists on its original owner is one that will not restore
# on a new machine, which is the machine a restore is usually for.
$COMPOSE exec -T postgres \
    pg_dump --username "$USER_NAME" --dbname "$DATABASE" --format custom --no-owner \
    > "$FILE"

if [ ! -s "$FILE" ]; then
    echo "the dump is empty: $FILE" >&2
    exit 1
fi

# Read back what was just written, so that a broken dump is found now rather
# than on the day somebody needs it.
#
# Through the container rather than on the host, even where the host has the
# tools. A dump carries the version of the pg_dump that wrote it and an older
# pg_restore refuses it, so a host whose client is a release behind the server
# would report every backup as broken. The one client that is always the right
# one is the one in the image that made the dump, which is also the one the
# restore uses. Copied in rather than piped, since a custom-format dump has to
# be seekable to be listed.
$COMPOSE exec -T postgres sh -c \
    'cat > /tmp/verify.dump && pg_restore --list /tmp/verify.dump > /dev/null;
     status=$?; rm -f /tmp/verify.dump; exit $status' < "$FILE"

echo "wrote $FILE ($(wc -c < "$FILE") bytes)"

# Old dumps go. Kept by age rather than by count, so that a week when nothing
# ran does not quietly expire the last good backup of a busy one.
find "$DIRECTORY" -name 'orrery-*.dump' -type f -mtime "+$KEEP_DAYS" -print -delete
