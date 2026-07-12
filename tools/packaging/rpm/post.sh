# %post -- $1 >= 1 on install and upgrade; == 1 on fresh install only.
# RPM has no interactive prompt path: fresh install registers, enables AND
# starts the service. Upgrades never restart a running node.
if [ "$1" -ge 1 ] && [ -d /run/systemd/system ]; then
    systemd-tmpfiles --create /usr/lib/tmpfiles.d/wire-sysio.conf || :
    systemctl daemon-reload || :
    if [ "$1" -eq 1 ]; then
        systemctl enable --now wire-sysio-nodeop.service || :
    fi
fi
