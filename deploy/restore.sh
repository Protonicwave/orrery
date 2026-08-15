#!/bin/sh
# Restore a dump, by default into a scratch database beside the live one.
#
#     deploy/restore.sh backups/orrery-20260815T101500Z.dump
#     deploy/restore.sh backups/orrery-20260815T101500Z.dump orrery
#
# The default target is `orrery_restore_check`, which is the whole point of this
# script existing rather than being three lines in a document: a restore
# procedure nobody has run is a hope. Restoring into a scratch database can be
# done on any afternoon, against the live stack, and it either works or it is a
# problem worth knowing about before the day it matters.
#
# Naming the live database restores over it. That is deliberate and it is
# deliberately the second argument: the destructive form is the one somebody has
# to type, and it is refused unless ORRERY_RESTORE_OVER_LIVE is set as well.

set -eu

FILE="${1:?usage: restore.sh <dump> [database]}"
TARGET="${2:-orrery_restore_check}"
USER_NAME="${ORRERY_POSTGRES_USER:-orrery}"
LIVE="${ORRERY_POSTGRES_DB:-orrery}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMPOSE="docker compose -f $HERE/compose.yaml -f $HERE/production.yaml"

if [ ! -s "$FILE" ]; then
    echo "no such dump, or it is empty: $FILE" >&2
    exit 1
fi

if [ "$TARGET" = "$LIVE" ] && [ -z "${ORRERY_RESTORE_OVER_LIVE:-}" ]; then
    echo "refusing to restore over $LIVE. Set ORRERY_RESTORE_OVER_LIVE=1 if" >&2
    echo "that is what you mean, and stop the API and the worker first." >&2
    exit 1
fi

# Dropped and made again rather than restored into, so that what comes out is
# the dump and not the dump merged with whatever was there.
$COMPOSE exec -T postgres \
    psql --username "$USER_NAME" --dbname postgres \
    --command "DROP DATABASE IF EXISTS \"$TARGET\"" \
    --command "CREATE DATABASE \"$TARGET\""

$COMPOSE exec -T postgres \
    pg_restore --username "$USER_NAME" --dbname "$TARGET" --no-owner \
    < "$FILE"

# What was restored, in the terms somebody checking a restore actually wants:
# how many jobs there are and when the last one was submitted.
$COMPOSE exec -T postgres \
    psql --username "$USER_NAME" --dbname "$TARGET" --command \
    "SELECT count(*) AS jobs, max(created_at) AS newest FROM job"

echo "restored $FILE into $TARGET"
