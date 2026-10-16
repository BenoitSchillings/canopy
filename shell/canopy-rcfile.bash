# Wrapper rcfile passed to bash --rcfile: loads the user's normal config, then Canopy integration.
if [[ -f /etc/bash.bashrc ]]; then source /etc/bash.bashrc; fi
if [[ -f "$HOME/.bashrc" ]]; then source "$HOME/.bashrc"; fi
if [[ -n "${CANOPY_SHELL_DIR:-}" && -f "$CANOPY_SHELL_DIR/canopy.bash" ]]; then source "$CANOPY_SHELL_DIR/canopy.bash"; fi
