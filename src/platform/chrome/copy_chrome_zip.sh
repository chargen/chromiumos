#!/bin/bash

# Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Script to copy Chrome for ChromeOS zipfile in preparation for repackaging as
# a debian package.

set -e

# ----------------------------------------------------------------------------
# Configuration
# For release builds, set USE_RELEASE_CHROME=1 and specify a CHROME_BUILD.

# If set to 1, use a released version of Chrome.  If 0, use latest build from
# Chrome continuous build.
USE_RELEASE_CHROME=0

# Specify a build number to use a specific build of Chrome.  If this is blank,
# the latest version will be used.
CHROME_BUILD=

# Option if set will pull a stable version of the Chromium browser
WGET_STABLE_BUILD=$GET_STABLE_CHROME

ARCH=i386

# ----------------------------------------------------------------------------

# Check to see if the user wants to grab stable browser build
while getopts 'sa:' OPTION
do
  case $OPTION in
  s)    WGET_STABLE_BUILD=1
        ;;
  a)    ARCH="$OPTARG"
        ;;
  esac
done

# Function to download a file using wget or scp
function download {
  echo "Downloading $1"
  if [ $USE_WGET -eq 1 ]
  then
    wget "$1"
  else 
    scp "$1" .
  fi
}

# Use wget if possible
USE_WGET=1

# Name of Chrome zip file
CHROME_ZIP=chrome-chromeos.zip
CHROME_ZIP_SECOND_TRY=chrome-linux.zip

# The following code pulls prebuilt versions of Chromium / Chrome from the
# Chrome buildbots.  It's pretty much only useful inside our buildbot 
# cluster / subnetwork.
#
# Most users should build Chromium locally; see below.
if [ "$WGET_STABLE_BUILD" = 1 -o "$USE_RELEASE_CHROME" = 1 ]
then
  # Use released version of Chrome
  BASE_FROM="http://codf196.jail.google.com/archive/chrome-official"
else
  # Use most recent snapshot from Chrome buildbot
  case $ARCH in
  i386)   BASE_NAME='linux-chromiumos' ;;
  *)      BASE_NAME='arm' ;;
  esac
  BASE_FROM="http://build.chromium.org/buildbot/snapshots/chromium-rel-${BASE_NAME}"

  # Chrome OS buildbot does not have http access to chrome-web, so scp
  # the build result instead
  if [ "$USER" = "chrome-bot" ]
  then
    USE_WGET=0
    BASE_FROM="chrome-web:~/www/snapshots/chromium-rel-${BASE_NAME}"
  fi
fi

# Path to local Chrome
case $ARCH in
i386)   ARCHDIRS='x86 i386' ;;
*)      ARCHDIRS="$ARCH" ;;
esac
for archdir in $ARCHDIRS
do
  LOCAL_CHROME="${HOME}/trunk/src/build/${archdir}/local_assets/${CHROME_ZIP}"
  if [ -f "$LOCAL_CHROME" ]
  then
    break
  fi
done

# Clobber any existing destination files
rm -f "./$CHROME_ZIP" ./LATEST

# We support four ways of getting chrome into our image.
#
# 1. Use wget to pull the latest Chrome build from the Chrome build server
# 2. Use scp to pull the latest Chrome build from the Chrome build server;
#    necessary when running as buildbot, which does not have http access to
#    that server.
# 3. Build chrome locally and put the zip image in src/build/local_packages.
# 4. Use wget to pull a tested version of the browser as opposed to the latest

if [ -f "$LOCAL_CHROME" ]
then 
  # Use local Chrome
  echo "Using locally-built Chrome from $LOCAL_CHROME"
  cp "$LOCAL_CHROME" .
else
  # Download Chrome build
  echo "Copying Chrome"
 
  if [ -z "$CHROME_BUILD" ]
  then
    # Find latest build of Chrome
    echo "Checking for latest build of Chrome"
    download "${BASE_FROM}/LATEST"
    CHROME_BUILD=`cat LATEST`
    echo "Latest build of Chrome is $CHROME_BUILD"
  fi

  download "${BASE_FROM}/${CHROME_BUILD}/${CHROME_ZIP}" || \
   download "${BASE_FROM}/${CHROME_BUILD}/${CHROME_ZIP_SECOND_TRY}"
  if [ -f "$CHROME_ZIP_SECOND_TRY" ]
  then
    mv "$CHROME_ZIP_SECOND_TRY" "$CHROME_ZIP"
  fi
fi
