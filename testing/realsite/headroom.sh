#!/bin/bash
# MiB between the memory limit of the cgroup this command runs in (the nearest ancestor that has one) and that
# cgroup's anonymous memory (total_rss). Every command of a session shares the limit, so a heavy run asks this
# before it starts; -1 and exit 1 when no ancestor carries a limit. cgroup v1's memory controller only.
c=$(awk -F: '$2=="memory"{print "/sys/fs/cgroup/memory"$3}' /proc/self/cgroup)
[ -n "$c" ] || { echo -1; exit 1; }
while [ ! -f "$c/memory.limit_in_bytes" ] || [ "$(cat "$c/memory.limit_in_bytes")" -gt 1000000000000 ]; do
  c=$(dirname "$c"); [ "$c" = /sys/fs/cgroup ] && { echo -1; exit 1; }
done
lim=$(cat "$c/memory.limit_in_bytes"); rss=$(awk '$1=="total_rss"{print $2}' "$c/memory.stat")
echo $(( (lim - rss) / 1048576 ))
