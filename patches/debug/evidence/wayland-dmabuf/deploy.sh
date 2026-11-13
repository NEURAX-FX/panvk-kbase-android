#!/bin/sh
# Deploy the Wayland-dmabuf PanVK ICD as the MT6989 default.
# New stack = byte copy of the current XR30 stack with only the ICD replaced.
set -eu
PREV=/opt/panvk/xr30-39e82f3
NAME=wldmabuf-c63caad0
NEW=/opt/panvk/$NAME
SRC=/root/panvk-wsi-debug/icd-wldmabuf2/libvulkan_panfrost.so
SHA=c63caad047a7b59e9903f13b36698b446d6ea24e191defb34eee4fccbb867ffa
BK=/root/panvk-wsi-debug/backup-$NAME

[ "$(readlink -f /opt/panvk/current)" = "$PREV" ] || { echo "unexpected current: $(readlink -f /opt/panvk/current)" >&2; exit 1; }
[ "$(sha256sum "$SRC" | cut -d' ' -f1)" = "$SHA" ] || { echo "ICD hash mismatch" >&2; exit 1; }
[ ! -e "$NEW" ] || { echo "$NEW exists" >&2; exit 1; }

mkdir -p "$BK"
sha256sum /etc/vulkan/icd.d/panvk-kbase.json /usr/share/glvnd/egl_vendor.d/50_mesa.json \
  /etc/opt/mt6989/graphics.env /etc/ld.so.conf.d/00-panvk.conf > "$BK/unchanged.sha256"
readlink /opt/panvk/current > "$BK/previous-current"

cp -a "$PREV" "$NEW.tmp"
install -m 755 "$SRC" "$NEW.tmp/lib64/libvulkan_panfrost.so"
(cd "$PREV" && find . -type f ! -path ./lib64/libvulkan_panfrost.so -exec sha256sum {} +) | sort -k2 > "$BK/prev-rest.sha256"
(cd "$NEW.tmp" && find . -type f ! -path ./lib64/libvulkan_panfrost.so -exec sha256sum {} +) | sort -k2 > "$BK/new-rest.sha256"
cmp -s "$BK/prev-rest.sha256" "$BK/new-rest.sha256" || { echo "stack copy differs" >&2; exit 1; }
mv -T "$NEW.tmp" "$NEW"

cat > "$BK/rollback.sh" <<EOF
#!/bin/sh
set -eu
current=\$(readlink -f /opt/panvk/current)
case "\$current" in
  $PREV) echo 'Previous xr30-39e82f3 stack already selected.'; exit 0 ;;
  $NEW) ;;
  *) echo "Refusing to overwrite unexpected selection: \$current" >&2; exit 1 ;;
esac
tmp=/opt/panvk/.rollback-$NAME-\$\$
trap 'rm -f "\$tmp"' EXIT
ln -s $PREV "\$tmp"
mv -Tf "\$tmp" /opt/panvk/current
/sbin/ldconfig
echo 'Previous xr30-39e82f3 default restored. Restart affected applications.'
EOF
chmod 755 "$BK/rollback.sh"
sh -n "$BK/rollback.sh"

tmp=/opt/panvk/.deploy-$NAME-$$
ln -s "$NEW" "$tmp"
mv -Tf "$tmp" /opt/panvk/current
/sbin/ldconfig
echo "current -> $(readlink -f /opt/panvk/current)"
sha256sum /opt/panvk/current/lib64/libvulkan_panfrost.so
sha256sum -c --quiet "$BK/unchanged.sha256" && echo "system manifests/config unchanged"
