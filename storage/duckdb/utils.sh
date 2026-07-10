#!/bin/bash

# ─── Color setup ────────────────────────────────────────────────────────────────

if [[ -n "$TERM" && "$TERM" != "dumb" ]] && command -v tput &>/dev/null; then
  _CLR_RESET=$(tput sgr0)
  _CLR_BOLD=$(tput bold)
  _CLR_RED="${_CLR_BOLD}$(tput setaf 1)"
  _CLR_GREEN="${_CLR_BOLD}$(tput setaf 2)"
  _CLR_YELLOW="${_CLR_BOLD}$(tput setaf 3)"
  _CLR_BLUE="${_CLR_BOLD}$(tput setaf 4)"
  _CLR_CYAN="${_CLR_BOLD}$(tput setaf 6)"
  _CLR_GRAY=$(tput setaf 7)
  _CLR_DARKGRAY="${_CLR_BOLD}$(tput setaf 0)"

  if [[ $(tput colors) -ge 256 ]]; then
    _CLR_RED=$(tput setaf 196)
    _CLR_GREEN=$(tput setaf 156)
    _CLR_YELLOW=$(tput setaf 228)
    _CLR_CYAN=$(tput setaf 87)
    _CLR_DARKGRAY=$(tput setaf 59)
  fi
else
  _CLR_RESET="\e[0m"
  _CLR_BOLD="\e[1m"
  _CLR_RED="\e[1;31m"
  _CLR_GREEN="\e[1;32m"
  _CLR_YELLOW="\e[1;33m"
  _CLR_BLUE="\e[1;34m"
  _CLR_CYAN="\e[1;36m"
  _CLR_GRAY="\e[37m"
  _CLR_DARKGRAY="\e[1;30m"
fi

# ─── Logging functions ──────────────────────────────────────────────────────────

info() {
  echo -e "${_CLR_CYAN} ● $*${_CLR_RESET}"
}

warn() {
  echo -e "${_CLR_YELLOW} ⚠ $*${_CLR_RESET}"
}

error() {
  echo -e "${_CLR_RED} ✖ $*${_CLR_RESET}"
}

success() {
  echo -e "${_CLR_GREEN} ✔ $*${_CLR_RESET}"
}

fail() {
  echo -e "${_CLR_RED} ✖ $*${_CLR_RESET}"
  exit 1
}

separator() {
  echo -e "${_CLR_DARKGRAY} ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${_CLR_RESET}"
}

header() {
  separator
  echo -e "${_CLR_GREEN} $*${_CLR_RESET}"
  separator
}

# ─── One-liner pipe ─────────────────────────────────────────────────────────────
# Usage: cmake --build . | one_liner
# Pipes stdout through a single updating line. Stderr goes straight to terminal.

one_liner() {
  local cols
  cols=$(tput cols 2>/dev/null || echo 120)
  while IFS= read -r line; do
    printf "\r\e[K  %s" "${line:0:$((cols - 4))}"
  done
  printf "\r\e[K"
}

# ─── Arrow-key menu ─────────────────────────────────────────────────────────────
# Usage:
#   options=("Debug" "RelWithDebInfo" "Release")
#   menu_choice "Select build type:" options
#   echo "Selected: $MENU_RESULT"
#
# Sets global MENU_RESULT to the chosen item string.

menu_choice() {
  local prompt="$1"
  local -n _opts=$2
  local total=${#_opts[@]}
  local selected=0
  local filter=""
  local prev_lines=0

  if [[ $total -eq 0 ]]; then
    error "menu_choice: no options provided"
    return 1
  fi

  # Build filtered list (indices into _opts that match the filter)
  local -a _filtered=()
  _menu_filter() {
    _filtered=()
    local lc_filter="${filter,,}"
    for ((i = 0; i < total; i++)); do
      if [[ -z "$filter" || "${_opts[$i],,}" == *"$lc_filter"* ]]; then
        _filtered+=("$i")
      fi
    done
  }
  _menu_filter

  # Max visible rows (cap long lists)
  local max_visible=20

  # Hide cursor, restore on exit/ctrl-c
  printf "\e[?25l"
  trap 'printf "\e[?25h"' RETURN
  trap 'printf "\e[?25h"; exit 130' INT

  _menu_render() {
    local fcount=${#_filtered[@]}
    local visible=$fcount
    (( visible > max_visible )) && visible=$max_visible
    local lines=$((visible + 2)) # prompt + filter line + items

    # Move up to overwrite previous render (skip on first draw)
    if [[ $prev_lines -gt 0 ]]; then
      printf "\e[%dA" "$prev_lines"
    fi
    # Clear old lines
    for ((i = 0; i < prev_lines; i++)); do
      printf "\e[K\n"
    done
    if [[ $prev_lines -gt 0 ]]; then
      printf "\e[%dA" "$prev_lines"
    fi

    echo -e " ${_CLR_CYAN}${prompt}${_CLR_RESET}"
    if [[ -n "$filter" ]]; then
      echo -e "   ${_CLR_YELLOW}🔍 ${filter}${_CLR_DARKGRAY} (${fcount}/${total})${_CLR_RESET}"
    else
      echo -e "   ${_CLR_DARKGRAY}type to filter... (${fcount}/${total})${_CLR_RESET}"
    fi

    if [[ $fcount -eq 0 ]]; then
      echo -e "   ${_CLR_RED}no matches${_CLR_RESET}"
      lines=3
    else
      # Scroll window: keep selected in view
      local scroll_start=0
      if (( selected >= scroll_start + visible )); then
        scroll_start=$(( selected - visible + 1 ))
      fi
      if (( scroll_start > fcount - visible )); then
        scroll_start=$(( fcount - visible ))
      fi
      (( scroll_start < 0 )) && scroll_start=0

      for ((j = scroll_start; j < scroll_start + visible; j++)); do
        local idx=${_filtered[$j]}
        if [[ $j -eq $selected ]]; then
          echo -e "   ${_CLR_GREEN}▸ ${_opts[$idx]}${_CLR_RESET}"
        else
          echo -e "   ${_CLR_GRAY}  ${_opts[$idx]}${_CLR_RESET}"
        fi
      done
      if (( fcount > visible )); then
        echo -e "   ${_CLR_DARKGRAY}… $((fcount - visible)) more${_CLR_RESET}"
        lines=$((lines + 1))
      fi
    fi
    prev_lines=$lines
  }

  _menu_render

  while true; do
    read -rsn1 key || true
    case "$key" in
      $'\x1b')
        read -rsn2 rest || true
        case "$rest" in
          '[A') # Up
            if [[ ${#_filtered[@]} -gt 0 ]]; then
              selected=$(( (selected - 1 + ${#_filtered[@]}) % ${#_filtered[@]} ))
              _menu_render
            fi
            ;;
          '[B') # Down
            if [[ ${#_filtered[@]} -gt 0 ]]; then
              selected=$(( (selected + 1) % ${#_filtered[@]} ))
              _menu_render
            fi
            ;;
        esac
        ;;
      '') # Enter
        if [[ ${#_filtered[@]} -gt 0 ]]; then
          break
        fi
        ;;
      $'\x7f'|$'\b') # Backspace
        if [[ -n "$filter" ]]; then
          filter="${filter%?}"
          _menu_filter
          selected=0
          _menu_render
        fi
        ;;
      [[:print:]]) # Printable character — add to filter
        filter+="$key"
        _menu_filter
        selected=0
        _menu_render
        ;;
    esac
  done

  local result_idx=${_filtered[$selected]}

  # Clear the menu from terminal
  if [[ $prev_lines -gt 0 ]]; then
    printf "\e[%dA" "$prev_lines"
    for ((i = 0; i < prev_lines; i++)); do
      printf "\e[K\n"
    done
    printf "\e[%dA" "$prev_lines"
  fi

  printf "\e[?25h"
  echo -e " ${_CLR_CYAN}${prompt}${_CLR_RESET} ${_CLR_GREEN}${_opts[$result_idx]}${_CLR_RESET}"

  MENU_RESULT="${_opts[$result_idx]}"
}
