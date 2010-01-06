#!/bin/bash

# Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Script to install and launch autotest.

. "$(dirname "$0")/common.sh"

# Script must be run inside the chroot
assert_inside_chroot

DEFINE_string client_control "" "client test case to execute" "c"
DEFINE_boolean force false "force reinstallation of autotest" "f"
DEFINE_string machine "" "if present, execute autotest on this host." "m"
DEFINE_string test_key "${GCLIENT_ROOT}/src/platform/testing/testing_rsa" \
"rsa key to use for autoserv" "k"

# More useful help
FLAGS_HELP="usage: $0 [flags]"

# parse the command-line
FLAGS "$@" || exit 1
eval set -- "${FLAGS_ARGV}"
set -e

AUTOTEST_CHROOT_DEST="/usr/local/autotest"
AUTOTEST_SRC="${GCLIENT_ROOT}/src/third_party/autotest/files"
echo $AUTOTEST_SRC

CHROOT_AUTHSOCK_PREFIX="/tmp/chromiumos_test_agent"

function cleanup {
  if [ "${TEST_AUTH_SOCKET:0:26}" == ${CHROOT_AUTHSOCK_PREFIX} ]
  then
    echo "cleaning up chrooted ssh-agent."
    kill ${SSH_AGENT_PID}
  fi
}

trap cleanup EXIT

# Copy a local "installation" of autotest into the chroot, to avoid
# polluting the src dir with tmp files, results, etc.
# TODO: use rsync to ensure we don't get stuck with an old install.
if [ 1 != ${FLAGS_force} ] || [ ! -f "${AUTOTEST_CHROOT_DEST}/server/autosrv" ]
  then 
    echo -n "Installing Autotest... "
    sudo mkdir -p "${AUTOTEST_CHROOT_DEST}"
    sudo cp -rp ${AUTOTEST_SRC}/* ${AUTOTEST_CHROOT_DEST}
    echo "done."
  else
    echo "Autotest found in chroot, skipping copy."
fi

# Add all third_party and system tests to site_tests.
for type in client server
do
  echo -n "Adding ${type}_tests into autotest's ${type}/site_tests... "
  sudo cp -rp ${GCLIENT_ROOT}/src/platform/testing/${type}_tests/* \
    ${AUTOTEST_CHROOT_DEST}/${type}/site_tests/
done

# If ssh-agent isn't already running, start one (possibly inside the chroot)
if [ ! -n "${SSH_AGENT_PID}" ]
then
  echo "Setting up ssh-agent in chroot for testing."
  TEST_AUTH_SOCKET=$(mktemp -u ${CHROOT_AUTHSOCK_PREFIX}.XXXX)
  eval $(/usr/bin/ssh-agent -a ${TEST_AUTH_SOCKET})
fi

# Install authkey for testing

chmod 400 $FLAGS_test_key
/usr/bin/ssh-add $FLAGS_test_key 2> /dev/null

if [ -n "${FLAGS_machine}" ]
then
  CLIENT_CONTROL_FILE=${FLAGS_client_control}
  # run only a specific test/suite if requested
  if [ ! -n "${CLIENT_CONTROL_FILE}" ]
  then
    # Generate meta-control file to run all existing site tests.
    CLIENT_CONTROL_FILE=\
      "${AUTOTEST_CHROOT_DEST}/client/site_tests/AllTests/control"
    echo "No control file specified. Running all tests."
  fi
  # Kick off autosrv for specified test
  autoserv_cmd="${AUTOTEST_CHROOT_DEST}/server/autoserv \
    -m ${FLAGS_machine} \
    -c ${CLIENT_CONTROL_FILE}"
  echo "running autoserv: " ${autoserv_cmd}
  ${autoserv_cmd}
else
  echo "To execute autotest manually:
  eval \$(ssh-agent)        # start ssh-agent
  ssh-add $FLAGS_test_key  # add test key to agent
  # Then execute autoserv:
  $autoserv_cmd"
fi
