#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
config_path="${SHADNET_CONFIG:-$repo_root/build-p2p/shadnet.cfg}"
client_path="${SHADNET_SAMPLE_CLIENT:-$repo_root/clientsample/build-p2p/shadnet-sample}"

read_setting() {
    local key="$1"
    local fallback="$2"

    if [[ ! -f "$config_path" ]]; then
        printf '%s' "$fallback"
        return
    fi

    local value
    value="$({
        awk -F= -v wanted="$key" '
            /^[[:space:]]*[#;]/ { next }
            {
                name = $1
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", name)
                if (name != wanted) next
                sub(/^[^=]*=/, "")
                gsub(/\r$/, "")
                gsub(/^[[:space:]]+|[[:space:]]+$/, "")
                print
                exit
            }
        ' "$config_path"
    } || true)"
    printf '%s' "${value:-$fallback}"
}

usage() {
    cat <<'EOF'
Usage: scripts/create_test_user.sh [options]

Creates a shadNet account through the normal registration protocol and verifies
that it can log in. Values not supplied as options are prompted interactively.

Options:
  --username NAME   Default: TestUser
  --email EMAIL     Default: <lowercase-username>@example.invalid
  --host ADDRESS    Default: Host from build-p2p/shadnet.cfg
  --port PORT       Default: UnsecuredPort from build-p2p/shadnet.cfg
  --secret VALUE    Registration secret (normally read from shadnet.cfg)
  --client PATH     Path to shadnet-sample
  --dry-run         Validate and print non-secret settings without registering
  -h, --help        Show this help

Environment:
  SHADNET_TEST_PASSWORD         Avoid the hidden password prompts
  SHADNET_REGISTRATION_SECRET   Override RegistrationSecretKey
  SHADNET_CONFIG                Override shadnet.cfg path
  SHADNET_SAMPLE_CLIENT         Override shadnet-sample path
EOF
}

username="TestUser"
email=""
host="$(read_setting Host 127.0.0.1)"
port="$(read_setting UnsecuredPort 31313)"
registration_secret="${SHADNET_REGISTRATION_SECRET:-$(read_setting RegistrationSecretKey '')}"
dry_run=false
username_supplied=false
email_supplied=false

while (($# > 0)); do
    case "$1" in
    --username)
        [[ $# -ge 2 ]] || { echo "--username requires a value" >&2; exit 2; }
        username="$2"
        username_supplied=true
        shift 2
        ;;
    --email)
        [[ $# -ge 2 ]] || { echo "--email requires a value" >&2; exit 2; }
        email="$2"
        email_supplied=true
        shift 2
        ;;
    --host)
        [[ $# -ge 2 ]] || { echo "--host requires a value" >&2; exit 2; }
        host="$2"
        shift 2
        ;;
    --port)
        [[ $# -ge 2 ]] || { echo "--port requires a value" >&2; exit 2; }
        port="$2"
        shift 2
        ;;
    --secret)
        [[ $# -ge 2 ]] || { echo "--secret requires a value" >&2; exit 2; }
        registration_secret="$2"
        shift 2
        ;;
    --client)
        [[ $# -ge 2 ]] || { echo "--client requires a value" >&2; exit 2; }
        client_path="$2"
        shift 2
        ;;
    --dry-run)
        dry_run=true
        shift
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if [[ "$username_supplied" == false && -t 0 ]]; then
    read -r -p "Username [TestUser]: " entered_username
    username="${entered_username:-$username}"
fi

if [[ ! "$username" =~ ^[A-Za-z0-9_-]{3,16}$ || "$username" == "DeletedUser" ]]; then
    echo "Username must be 3-16 letters, numbers, hyphens, or underscores." >&2
    exit 2
fi

if [[ "$email_supplied" == false ]]; then
    email="${username,,}@example.invalid"
    if [[ -t 0 ]]; then
        read -r -p "Email [$email]: " entered_email
        email="${entered_email:-$email}"
    fi
fi

if [[ -z "$email" || "$email" != *@* ]]; then
    echo "A nonempty email address is required." >&2
    exit 2
fi

if [[ ! "$port" =~ ^[0-9]+$ ]] || ((port < 1 || port > 65535)); then
    echo "Port must be between 1 and 65535." >&2
    exit 2
fi

if [[ ! -x "$client_path" ]]; then
    echo "Registration client is missing: $client_path" >&2
    echo "Build it with:" >&2
    echo "  cmake --build '$repo_root/clientsample/build-p2p' --target shadnet-sample" >&2
    exit 2
fi

password="${SHADNET_TEST_PASSWORD:-}"
if [[ -z "$password" && "$dry_run" == false ]]; then
    [[ -t 0 ]] || {
        echo "No terminal is available for a hidden password prompt." >&2
        echo "Set SHADNET_TEST_PASSWORD or run this script interactively." >&2
        exit 2
    }
    read -r -s -p "Password: " password
    echo
    read -r -s -p "Confirm password: " password_confirmation
    echo
    if [[ "$password" != "$password_confirmation" ]]; then
        echo "Passwords do not match." >&2
        exit 2
    fi
    unset password_confirmation
fi

if [[ "$dry_run" == false && -z "$password" ]]; then
    echo "Password cannot be empty." >&2
    exit 2
fi

echo
echo "Account settings"
echo "  Server:   $host:$port"
echo "  Username: $username"
echo "  Email:    $email"
echo "  Secret:   $([[ -n "$registration_secret" ]] && echo configured || echo not configured)"

if [[ "$dry_run" == true ]]; then
    echo "Dry run complete; no account was created."
    exit 0
fi

register_command=("$client_path" "$host" "$port" register "$username" "$password" "$email")
if [[ -n "$registration_secret" ]]; then
    register_command+=("$registration_secret")
fi

set +e
register_output="$("${register_command[@]}" 2>&1)"
register_status=$?
set -e
printf '%s\n' "$register_output"

if ((register_status != 0)) ||
   ! grep -Fq '[create] Account created successfully.' <<<"$register_output"; then
    echo "Account creation failed." >&2
    password=""
    unset password register_command
    exit 1
fi

set +e
login_output="$("$client_path" "$host" "$port" login "$username" "$password" 2>&1)"
login_status=$?
set -e
printf '%s\n' "$login_output"

password=""
unset password register_command

if ((login_status != 0)) || ! grep -Fq '[login] OK' <<<"$login_output"; then
    echo "The account was created, but login verification failed." >&2
    exit 1
fi

echo
echo "Account '$username' was created and login was verified."
echo "Give the tester the username, email, and password you entered."
