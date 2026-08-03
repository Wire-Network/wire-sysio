# %preun -- $1 == 0 on full erase (not on upgrade).
if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
    systemctl stop wire-sysio-nodeop.service 2>/dev/null || :
    systemctl disable wire-sysio-nodeop.service 2>/dev/null || :
fi
