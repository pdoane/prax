#!/usr/bin/env bash
# Backs up a directory into a dated archive and keeps the newest few.
set -euo pipefail

readonly SOURCE="${1:?usage: backup.sh <directory> [keep]}"
readonly KEEP="${2:-5}"
readonly DEST="$HOME/backups"
stamp=$(date +%Y-%m-%d_%H%M%S)

log() {
    printf '[%s] %s\n' "$(date +%T)" "$*" >&2
}

if [[ ! -d "$SOURCE" ]]; then
    log "not a directory: $SOURCE"
    exit 1
fi

mkdir -p "$DEST"
archive="$DEST/$(basename "$SOURCE")-$stamp.tar.gz"
tar -czf "$archive" -C "$(dirname "$SOURCE")" "$(basename "$SOURCE")"
log "wrote $archive ($(du -h "$archive" | cut -f1))"

# The oldest archives past the newest $KEEP.
count=0
for old in $(ls -1t "$DEST"/*.tar.gz); do
    count=$((count + 1))
    if (( count > KEEP )); then
        rm -- "$old" && log "removed ${old##*/}"
    fi
done

case "${BACKUP_NOTIFY:-none}" in
    mail) mail -s "backup done" "$USER" <<EOF
Archive: $archive
Kept: $KEEP
EOF
        ;;
    none | "") ;;
    *) log "unknown BACKUP_NOTIFY=$BACKUP_NOTIFY" ;;
esac
