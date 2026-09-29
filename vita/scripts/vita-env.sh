# SPDX-License-Identifier: GPL-3.0-or-later
# Source this to put VitaSDK on PATH.
# Expects VitaSDK under $HOME/vitasdk (or /usr/local/vitasdk).
# shellcheck shell=sh
if [ -z "${VITASDK:-}" ]; then
  if [ -x "$HOME/vitasdk/bin/arm-vita-eabi-gcc" ]; then
    VITASDK="$HOME/vitasdk"
  elif [ -x /usr/local/vitasdk/bin/arm-vita-eabi-gcc ]; then
    VITASDK=/usr/local/vitasdk
  fi
fi
if [ -n "${VITASDK:-}" ]; then
  export VITASDK
  case ":$PATH:" in
    *":$VITASDK/bin:"*) ;;
    *) PATH="$VITASDK/bin:$PATH"; export PATH ;;
  esac
fi
