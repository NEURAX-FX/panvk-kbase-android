#!/bin/sh
set -eu
current=$(readlink -f /opt/panvk/current)
case "$current" in
  /opt/panvk/xr30-39e82f3) echo 'Previous xr30-39e82f3 stack already selected.'; exit 0 ;;
  /opt/panvk/wldmabuf-c63caad0) ;;
  *) echo "Refusing to overwrite unexpected selection: $current" >&2; exit 1 ;;
esac
tmp=/opt/panvk/.rollback-wldmabuf-c63caad0-$$
trap 'rm -f "$tmp"' EXIT
ln -s /opt/panvk/xr30-39e82f3 "$tmp"
mv -Tf "$tmp" /opt/panvk/current
/sbin/ldconfig
echo 'Previous xr30-39e82f3 default restored. Restart affected applications.'
