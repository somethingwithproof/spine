#!/bin/bash
# vultr-matrix.sh - Spin up Vultr instances, test spine build, tear down.
#
# Usage: ./test/vultr-matrix.sh [branch]
#   branch: git branch to test (default: current branch)
#
# Requires: curl, jq, ssh-keygen, VULTR_API_KEY env var
# Cost: ~$0.06 total (4 instances x $0.015/hr x ~15 min each)
set -euo pipefail

BRANCH="${1:-$(cd /tmp/spine-verify && git branch --show-current)}"
REGION="ewr"
PLAN="vc2-1c-2gb"
LABEL_PREFIX="spine-ci"
REPO_URL="https://github.com/thomasvincent/spine.git"

OS_NAMES=(rocky9 rocky10 opensuse15 freebsd14)
OS_IDS=(1869 2594 2157 2212)

log() { printf '[vultr-ci] %s | %s\n' "$(date +%H:%M:%S)" "$*"; }

command -v jq >/dev/null || { log "ERROR: jq not found"; exit 1; }
[ -n "${VULTR_API_KEY:-}" ] || { log "ERROR: VULTR_API_KEY not set"; exit 1; }

# Force IPv4 for Vultr API (IPv6 may not be in API ACL)
vapi() {
	local method="$1" endpoint="$2"
	shift 2
	curl -4 -s -X "$method" \
		"https://api.vultr.com/v2${endpoint}" \
		-H "Authorization: Bearer $VULTR_API_KEY" \
		-H "Content-Type: application/json" \
		"$@"
}

# Get or create SSH key
SSH_KEY_FILE="$HOME/.ssh/spine_ci_ed25519"
if [ ! -f "$SSH_KEY_FILE" ]; then
	log "Generating ephemeral SSH key for CI"
	ssh-keygen -t ed25519 -f "$SSH_KEY_FILE" -N "" -q
fi
SSH_PUB="$(cat "${SSH_KEY_FILE}.pub")"

EXISTING_KEY_ID=$(vapi GET /ssh-keys | jq -r '.ssh_keys[]? | select(.name == "spine-ci-key") | .id' || true)
if [ -z "$EXISTING_KEY_ID" ]; then
	SSHKEY_ID=$(vapi POST /ssh-keys -d "{\"name\":\"spine-ci-key\",\"ssh_key\":\"$SSH_PUB\"}" | jq -r '.ssh_key.id')
	log "Created SSH key: $SSHKEY_ID"
else
	SSHKEY_ID="$EXISTING_KEY_ID"
	log "Using existing SSH key: $SSHKEY_ID"
fi

# Track instances for cleanup
INSTANCE_IDS=()
INSTANCE_NAMES=()
cleanup() {
	log "Cleaning up instances..."
	for i in "${!INSTANCE_IDS[@]}"; do
		local iid="${INSTANCE_IDS[$i]}"
		local name="${INSTANCE_NAMES[$i]}"
		if [ -n "$iid" ]; then
			log "Destroying $name ($iid)"
			vapi DELETE "/instances/$iid" 2>/dev/null || true
		fi
	done
}
trap cleanup EXIT

# Create instances
for i in "${!OS_NAMES[@]}"; do
	os_name="${OS_NAMES[$i]}"
	os_id="${OS_IDS[$i]}"
	label="${LABEL_PREFIX}-${os_name}"

	log "Creating $label (OS=$os_id, plan=$PLAN, region=$REGION)"
	resp=$(vapi POST /instances -d "{
		\"region\":\"$REGION\",
		\"plan\":\"$PLAN\",
		\"os_id\":$os_id,
		\"label\":\"$label\",
		\"hostname\":\"$label\",
		\"sshkey_id\":[\"$SSHKEY_ID\"]
	}")

	INSTANCE_ID=$(echo "$resp" | jq -r '.instance.id // empty')
	if [ -z "$INSTANCE_ID" ]; then
		log "ERROR creating $os_name: $resp"
		INSTANCE_IDS+=("")
	else
		INSTANCE_IDS+=("$INSTANCE_ID")
		log "Created $os_name: $INSTANCE_ID"
	fi
	INSTANCE_NAMES+=("$os_name")
done

# Wait for all instances to become active and get IPs
INSTANCE_IPS=()
for i in "${!INSTANCE_IDS[@]}"; do
	INSTANCE_IPS+=("")
done

log "Waiting for instances to boot..."
for attempt in $(seq 1 60); do
	all_ready=true
	for i in "${!INSTANCE_IDS[@]}"; do
		if [ -n "${INSTANCE_IPS[$i]}" ] || [ -z "${INSTANCE_IDS[$i]}" ]; then
			continue
		fi
		iid="${INSTANCE_IDS[$i]}"
		info=$(vapi GET "/instances/$iid" 2>/dev/null || echo '{}')
		status=$(echo "$info" | jq -r '.instance.status // empty')
		ip=$(echo "$info" | jq -r '.instance.main_ip // empty')

		if [ "$status" = "active" ] && [ -n "$ip" ] && [ "$ip" != "0.0.0.0" ]; then
			INSTANCE_IPS[$i]="$ip"
			log "${INSTANCE_NAMES[$i]} is ready at $ip"
		else
			all_ready=false
		fi
	done
	if $all_ready; then break; fi
	sleep 10
done

# Wait for SSH to become reachable on each instance
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -i $SSH_KEY_FILE"

log "Waiting for SSH to become reachable..."
for i in "${!INSTANCE_IDS[@]}"; do
	ip="${INSTANCE_IPS[$i]}"
	if [ -z "$ip" ]; then continue; fi
	for try in $(seq 1 30); do
		if ssh $SSH_OPTS -o BatchMode=yes "root@$ip" true 2>/dev/null; then
			log "${INSTANCE_NAMES[$i]} SSH ready"
			break
		fi
		sleep 5
	done
done

# Run build test on each instance
RESULTS_DIR="/tmp/spine-ci-results-$(date +%s)"
mkdir -p "$RESULTS_DIR"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=60 -i $SSH_KEY_FILE"

run_test() {
	local idx="$1"
	local os_name="${INSTANCE_NAMES[$idx]}"
	local ip="${INSTANCE_IPS[$idx]}"
	local user="root"
	local logfile="$RESULTS_DIR/${os_name}.log"
	local branch="$BRANCH"
	local repo_url="$REPO_URL"

	log "[$os_name] Starting build test on $ip"

	# Build the remote script locally (avoids heredoc indentation issues)
	local script="set -eu
cd /tmp
if command -v dnf >/dev/null 2>&1; then
  dnf install -y --quiet git
elif command -v zypper >/dev/null 2>&1; then
  zypper --quiet --non-interactive install git
elif command -v pkg >/dev/null 2>&1; then
  pkg install -y git
fi
git clone --depth=1 --branch ${branch} ${repo_url} spine-test
cd spine-test
sh test/install-deps.sh
sh test/build-test.sh"

	# Use sh (not bash) for FreeBSD compatibility
	echo "$script" | ssh $SSH_OPTS "$user@$ip" sh -s > "$logfile" 2>&1

	local rc=$?
	if [ $rc -eq 0 ]; then
		log "[$os_name] PASS"
	else
		log "[$os_name] FAIL (exit $rc) - see $logfile"
	fi
	return $rc
}

# Run tests in parallel
TEST_PIDS=()
for i in "${!INSTANCE_IDS[@]}"; do
	if [ -n "${INSTANCE_IPS[$i]}" ]; then
		run_test "$i" &
		TEST_PIDS+=("$!")
	else
		log "[${INSTANCE_NAMES[$i]}] SKIP - no IP assigned"
		TEST_PIDS+=("")
	fi
done

# Collect results
PASS=0
FAIL=0
for i in "${!TEST_PIDS[@]}"; do
	pid="${TEST_PIDS[$i]}"
	if [ -z "$pid" ]; then
		FAIL=$((FAIL + 1))
		continue
	fi
	if wait "$pid"; then
		PASS=$((PASS + 1))
	else
		FAIL=$((FAIL + 1))
	fi
done

log "========================================="
log "Results: $PASS passed, $FAIL failed"
log "Logs: $RESULTS_DIR/"
log "========================================="

exit $FAIL
