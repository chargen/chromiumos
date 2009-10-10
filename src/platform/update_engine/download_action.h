// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UPDATE_ENGINE_DOWNLOAD_ACTION_H__
#define UPDATE_ENGINE_DOWNLOAD_ACTION_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <map>
#include <string>

#include <curl/curl.h>

#include "base/scoped_ptr.h"
#include "update_engine/action.h"
#include "update_engine/decompressing_file_writer.h"
#include "update_engine/file_writer.h"
#include "update_engine/http_fetcher.h"
#include "update_engine/omaha_hash_calculator.h"

// The Download Action downloads a requested url to a specified path on disk.
// It takes no input object, but if successful, passes the output path
// to the output pipe.

using std::map;
using std::string;

namespace chromeos_update_engine {

class DownloadAction;
class NoneType;

template<>
class ActionTraits<DownloadAction> {
 public:
  // Does not take an object for input
  typedef NoneType InputObjectType;
  // On success, puts the output path on output
  typedef std::string OutputObjectType;
};

class DownloadAction : public Action<DownloadAction>,
                       public HttpFetcherDelegate {
 public:
  // Takes ownership of the passed in HttpFetcher. Useful for testing.
  // A good calling pattern is:
  // DownloadAction(..., new WhateverHttpFetcher);
  DownloadAction(const std::string& url, const std::string& output_path,
                 off_t size, const std::string& hash,
                 const bool should_decompress,
                 HttpFetcher* http_fetcher);
  virtual ~DownloadAction();
  typedef ActionTraits<DownloadAction>::InputObjectType InputObjectType;
  typedef ActionTraits<DownloadAction>::OutputObjectType OutputObjectType;
  void PerformAction();
  void TerminateProcessing();

  // Debugging/logging
  std::string Type() const { return "DownloadAction"; }

  // Delegate methods (see http_fetcher.h)
  virtual void ReceivedBytes(HttpFetcher *fetcher,
                             const char* bytes, int length);
  virtual void TransferComplete(HttpFetcher *fetcher, bool successful);

 private:
  // Expected size of the file (will be used for progress info)
  const size_t size_;

  // URL to download
  const std::string url_;

  // Path to save URL to
  const std::string output_path_;

  // Expected hash of the file. The hash must match for this action to
  // succeed.
  const std::string hash_;

  // Whether the caller requested that we decompress the downloaded data.
  const bool should_decompress_;

  // The FileWriter that downloaded data should be written to. It will
  // either point to *decompressing_file_writer_ or *direct_file_writer_.
  FileWriter* writer_;

  // If non-null, a FileWriter used for gzip decompressing downloaded data
  scoped_ptr<GzipDecompressingFileWriter> decompressing_file_writer_;

  // Used to write out the downloaded file
  scoped_ptr<DirectFileWriter> direct_file_writer_;

  // pointer to the HttpFetcher that does the http work
  scoped_ptr<HttpFetcher> http_fetcher_;

  // Used to find the hash of the bytes downloaded
  OmahaHashCalculator omaha_hash_calculator_;
  DISALLOW_COPY_AND_ASSIGN(DownloadAction);
};

// We want to be sure that we're compiled with large file support on linux,
// just in case we find ourselves downloading large images.
COMPILE_ASSERT(8 == sizeof(off_t), off_t_not_64_bit);

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_DOWNLOAD_ACTION_H__
