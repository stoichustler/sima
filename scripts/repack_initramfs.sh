#!/bin/sh
# Rebuild the Linux initramfs used by VM2.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE=${1:-"$ROOT/sdk/image/linux/Initramfs.cpio.gz"}
TMPDIR_ROOT=${TMPDIR:-/tmp}
WORKDIR=$(mktemp -d "$TMPDIR_ROOT/beau-initramfs.XXXXXX")

cleanup()
{
	rm -rf "$WORKDIR"
}

trap cleanup EXIT

gzip -dc "$ARCHIVE" | (cd "$WORKDIR" && cpio -id --quiet --no-absolute-filenames)
chmod 0755 "$WORKDIR"

cat > "$WORKDIR/init" <<'EOF'
#!/bin/sh

#  __  __    ______    ______
# /\ \/\ \  /\  __ \  /\  ___\
# \ \ \_\ \ \ \ \/\ \ \ \___  \
#  \ \_____\ \ \_____\ \/\_____\
#   \/_____/  \/_____/  \/_____/
#                                (2026)

PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true

# uos terminal
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/tty ] || mknod /dev/tty c 5 0
[ -c /dev/ttyAMA0 ] || mknod /dev/ttyAMA0 c 204 64

# uos commands
alias ll='ls -la'

TERM=dumb
PS1='uos \w '

export PS1 PATH TERM

while true; do
	if [ -c /dev/ttyAMA0 ]; then
		setsid /bin/sh -c 'exec </dev/ttyAMA0 >/dev/ttyAMA0 2>&1; stty sane rows 24 cols 80 -ixon -ixoff 2>/dev/null || true; exec /bin/sh -i' && continue
	fi

	if [ -c /dev/console ]; then
		setsid /bin/sh -c 'exec </dev/console >/dev/console 2>&1; stty sane rows 24 cols 80 -ixon -ixoff 2>/dev/null || true; exec /bin/sh -i'
	fi

	sleep 1
done
EOF

chmod 0755 "$WORKDIR/init"
(cd "$WORKDIR" && find . -print0 | cpio --null -o --quiet -H newc -R 0:0 | gzip -9 > "$ARCHIVE.tmp")
mv "$ARCHIVE.tmp" "$ARCHIVE"
