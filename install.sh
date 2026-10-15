#!/usr/bin/env bash
#
# OneBoard — Linux installer
#
# Installs everything needed to build/flash the board and run the host tools:
#   - System packages (git, curl, python3, venv, pip) via the distro manager
#   - PlatformIO build toolchain (official installer, or pip fallback)
#   - The user's serial (dialout) group membership so flashing works
#   - A project-local Python venv + requirements.txt (only the Telegram bot
#     needs a third-party package; everything else is stdlib)
#   - Optional: install the behavior store and/or Telegram bot as systemd
#     user services
#
# Idempotent: safe to re-run any time. Nothing here touches the board.
#
# Usage:
#   ./install.sh                 # full base install (system deps + pio + venv)
#   ./install.sh --no-system     # skip distro package install (need sudo)
#   ./install.sh --with-store    # + install the behavior store as a service
#   ./install.sh --with-telegram # + install the Telegram bot as a service
#   ./install.sh --help

set -euo pipefail

# ---- flags ---------------------------------------------------------------
WITH_STORE=0
WITH_TELEGRAM=0
SYSTEM_PKGS=1
for arg in "$@"; do
  case "$arg" in
    --no-system)   SYSTEM_PKGS=0 ;;
    --with-store)  WITH_STORE=1 ;;
    --with-telegram) WITH_TELEGRAM=1 ;;
    --help|-h)
      grep '^#!' -v "$0" | grep '^#' | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "Unknown option: $arg (see ./install.sh --help)"; exit 1 ;;
  esac
done

C_RED=$'\e[31m'; C_GRN=$'\e[32m'; C_YLW=$'\e[33m'; C_BLU=$'\e[34m'; C_RST=$'\e[0m'
info()  { echo -e "${C_BLU}==>${C_RST} $*"; }
ok()    { echo -e "${C_GRN}  ok${C_RST}  $*"; }
warn()  { echo -e "${C_YLW}  warn${C_RST} $*"; }
die()   { echo -e "${C_RED}error${C_RST} $*" >&2; exit 1; }

# ---- must run from the repo root ----------------------------------------
if [ ! -f platformio.ini ]; then
  die "run this from the OneBoard repo root (where platformio.ini lives)."
fi
REPO_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---- base requirements ---------------------------------------------------
command -v python3 >/dev/null || die "python3 is required (and will be installed by --no-system's parent)."
command -v git      >/dev/null || die "git is required."
command -v curl     >/dev/null || die "curl is required."

# ---- 1. system packages --------------------------------------------------
if [ "$SYSTEM_PKGS" = "1" ]; then
  if [ "$(id -u)" = "0" ]; then
    warn "running as root — will install system packages directly (no sudo)."
    SUDO=""
  else
    command -v sudo >/dev/null || die "need sudo to install system packages (or re-run with --no-system)."
    SUDO="sudo"
  fi

  if command -v apt-get >/dev/null; then
    info "Installing system packages (Debian/Ubuntu)…"
    $SUDO apt-get update -qq
    $SUDO apt-get install -y -qq git curl python3 python3-venv python3-pip
  elif command -v dnf >/dev/null; then
    info "Installing system packages (Fedora/RHEL)…"
    $SUDO dnf install -y git curl python3 python3-pip
  elif command -v pacman >/dev/null; then
    info "Installing system packages (Arch)…"
    $SUDO pacman -Sy --noconfirm --needed git curl python python-pip
  else
    warn "no apt/dnf/pacman found — install git, curl, python3, python3-venv yourself."
  fi
  ok "system packages"
else
  info "Skipping system package install (--no-system)."
fi

# ---- 2. PlatformIO -------------------------------------------------------
PIO_BIN=""
for c in pio ~/.local/bin/pio ~/.platformio/penv/bin/pio; do
  if command -v "$c" >/dev/null 2>&1; then PIO_BIN="$(command -v "$c")"; break; fi
done

if [ -n "$PIO_BIN" ]; then
  ok "PlatformIO already installed: $PIO_BIN ($("$PIO_BIN" --version 2>/dev/null | head -1))"
else
  info "Installing PlatformIO…"
  # Preferred: the official standalone installer (isolated, no root needed).
  if curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py \
        -o /tmp/get-platformio.py 2>/dev/null && python3 /tmp/get-platformio.py >/tmp/pio-install.log 2>&1; then
    ok "PlatformIO installed via official installer"
    rm -f /tmp/get-platformio.py
    PIO_BIN="$(command -v ~/.local/bin/pio || true)"
  else
    # Fallback: pip user install (some distros need --break-system-packages).
    warn "official installer failed — trying pip --user …"
    python3 -m pip install --user platformio \
      || python3 -m pip install --user --break-system-packages platformio
    PIO_BIN="$(command -v ~/.local/bin/pio || command -v pio || true)"
  fi
  [ -n "$PIO_BIN" ] || die "PlatformIO install failed — see /tmp/pio-install.log"
  # Ensure ~/.local/bin is on PATH for the current + future shells.
  case ":$PATH:" in
    *":$HOME/.local/bin:"*) : ;;
    *) export PATH="$HOME/.local/bin:$PATH"
       warn "added ~/.local/bin to PATH for this session only — add it to ~/.bashrc:"
       warn "  echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc" ;;
  esac
  ok "PlatformIO: $PIO_BIN"
fi

# ---- 3. serial (dialout) group ------------------------------------------
if [ "$(id -u)" != "0" ]; then
  if ! id -nG | tr ' ' '\n' | grep -qx dialout; then
    if command -v sudo >/dev/null; then
      info "Adding $USER to the dialout group (needed to flash via USB serial)…"
      sudo usermod -aG dialout "$USER"
      warn "dialout added — LOG OUT and back in (or reboot) for it to take effect."
    else
      warn "not in dialout group and no sudo — flashing may fail. Ask an admin:"
      warn "  sudo usermod -aG dialout $USER"
    fi
  else
    ok "already in dialout group"
  fi
fi

# ---- 4. project venv + Python deps --------------------------------------
if [ -x "$REPO_DIR/.venv/bin/python" ]; then
  ok "venv already exists: $REPO_DIR/.venv"
else
  info "Creating project venv…"
  # Unset PYTHONPATH: a leaked PYTHONPATH makes pip install into the wrong
  # site-packages while the import still 'works' — then fails under systemd.
  env -u PYTHONPATH python3 -m venv "$REPO_DIR/.venv"
  ok "venv created"
fi
info "Installing Python requirements…"
env -u PYTHONPATH "$REPO_DIR/.venv/bin/python" -m pip install --quiet --upgrade pip
env -u PYTHONPATH "$REPO_DIR/.venv/bin/python" -m pip install --quiet -r "$REPO_DIR/requirements.txt"
# Verify from a clean env (no leaked PYTHONPATH) that the bot dep imports.
env -u PYTHONPATH "$REPO_DIR/.venv/bin/python" -c "import telegram; print('  telegram ok', telegram.__version__)" \
  || die "python-telegram-bot failed to import in the venv."
ok "Python requirements installed"

# ---- 5. optional: systemd user services ---------------------------------
SYS_UDIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
if [ "$WITH_STORE" = "1" ] || [ "$WITH_TELEGRAM" = "1" ]; then
  mkdir -p "$SYS_UDIR"
  info "Installing systemd user services…"
fi

if [ "$WITH_STORE" = "1" ]; then
  UNIT="$SYS_UDIR/oneboard-store.service"
  cat > "$UNIT" <<EOF
[Unit]
Description=OneBoard behavior store
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$REPO_DIR
ExecStart=$REPO_DIR/.venv/bin/python $REPO_DIR/store/serve.py --port 8082
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
  systemctl --user daemon-reload
  systemctl --user enable --now oneboard-store.service
  ok "behavior store service installed + started (http://<this-host>:8082)"
fi

if [ "$WITH_TELEGRAM" = "1" ]; then
  # The unit must reference the real repo path (the checked-in service file
  # has this machine's hardcoded paths) and a token env file.
  ENV_FILE="$HOME/.hermes/keys/oneboard-telegram.env"
  if [ ! -f "$ENV_FILE" ]; then
    mkdir -p "$(dirname "$ENV_FILE")"
    cp "$REPO_DIR/systemd/oneboard-telegram.env.template" "$ENV_FILE"
    chmod 600 "$ENV_FILE"
    warn "created $ENV_FILE — fill in ONBOARD_BOT_TOKEN / ONBOARD_BOARD_IP /"
    warn "  ONBOARD_STORE_URL / ONBOARD_ALLOWED, then re-run with --with-telegram."
    WITH_TELEGRAM=2   # mark "pending config" so we don't start a broken unit
  fi
  if [ "$WITH_TELEGRAM" = "1" ]; then
    UNIT="$SYS_UDIR/oneboard-tg-bot.service"
    cat > "$UNIT" <<EOF
[Unit]
Description=OneBoard Telegram control bot
After=network-online.target

[Service]
Type=simple
WorkingDirectory=$REPO_DIR
EnvironmentFile=$ENV_FILE
ExecStart=$REPO_DIR/.venv/bin/python $REPO_DIR/tools/oneboard_bot.py
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now oneboard-tg-bot.service
    ok "Telegram bot service installed + started"
  fi
fi

# ---- done ----------------------------------------------------------------
echo
echo -e "${C_GRN}OneBoard install complete.${C_RST}"
echo "  Firmware build:  cd $REPO_DIR && pio run -t upload"
echo "  Behavior store:  (manual)  python3 store/serve.py --port 8082"
if [ "$WITH_STORE" = "1" ]; then
  echo "  Behavior store:  (service) systemctl --user status oneboard-store"
fi
if [ "$WITH_TELEGRAM" = "1" ]; then
  echo "  Telegram bot:    systemctl --user status oneboard-tg-bot"
elif [ "$WITH_TELEGRAM" = "2" ]; then
  echo "  Telegram bot:    edit $ENV_FILE, then run: ./install.sh --with-telegram"
fi
echo "  Author tools:    OPENROUTER_KEY_FILE=~/.openrouter_key \\"
echo "                     .venv/bin/python tools/author.py \"<request>\" --board <ip>"
echo
echo "Next: plug in the board, flash, and join it to WiFi — see INSTALLING.md."
