# %postun -- after files are removed (erase and upgrade). State dirs untouched.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload 2>/dev/null || :
fi
