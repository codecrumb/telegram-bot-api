//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2025
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "telegram-bot-api/HttpConnection.h"

#include "telegram-bot-api/Query.h"

#include "td/net/HttpHeaderCreator.h"

#include "td/utils/common.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/Parser.h"
#include "td/utils/PathView.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Promise.h"
#include "td/utils/SliceBuilder.h"

namespace telegram_bot_api {

void HttpConnection::handle(td::unique_ptr<td::HttpQuery> http_query,
                            td::ActorOwn<td::HttpInboundConnection> connection) {
  CHECK(connection_.empty());
  connection_ = std::move(connection);

  LOG(DEBUG) << "Handle " << *http_query;
  td::Parser url_path_parser(http_query->url_path_);
  if (url_path_parser.peek_char() != '/') {
    return send_http_error(404, "Not Found: absolute URI is specified in the Request-Line");
  }

  // the same layout as on api.telegram.org: /file/bot<token>[/test]/<file_path>
  if (url_path_parser.try_skip("/file/bot")) {
    auto token = url_path_parser.read_till('/');
    bool is_test_dc = false;
    if (url_path_parser.try_skip("/test")) {
      is_test_dc = true;
    }
    url_path_parser.skip('/');
    if (url_path_parser.status().is_error()) {
      return send_http_error(404, "Not Found");
    }
    return serve_file(token, is_test_dc, url_path_parser.data(), http_query->get_header("range"));
  }

  if (!url_path_parser.try_skip("/bot")) {
    return send_http_error(404, "Not Found");
  }

  auto token = url_path_parser.read_till('/');
  bool is_test_dc = false;
  if (url_path_parser.try_skip("/test")) {
    is_test_dc = true;
  }
  url_path_parser.skip('/');
  if (url_path_parser.status().is_error()) {
    return send_http_error(404, "Not Found");
  }

  auto method = url_path_parser.data();
  auto query = td::make_unique<Query>(std::move(http_query->container_), token, is_test_dc, method,
                                      std::move(http_query->args_), std::move(http_query->headers_),
                                      std::move(http_query->files_), shared_data_, http_query->peer_address_, false);

  auto promise = td::PromiseCreator::lambda([actor_id = actor_id(this)](td::Result<td::unique_ptr<Query>> r_query) {
    send_closure(actor_id, &HttpConnection::on_query_finished, std::move(r_query));
  });
  auto promised_query = PromisedQueryPtr(query.release(), PromiseDeleter(std::move(promise)));
  send_closure(client_manager_, &ClientManager::send, std::move(promised_query));
}

void HttpConnection::on_query_finished(td::Result<td::unique_ptr<Query>> r_query) {
  LOG_CHECK(r_query.is_ok()) << r_query.error();

  auto query = r_query.move_as_ok();
  send_response(query->http_status_code(), std::move(query->answer()), query->retry_after());
}

void HttpConnection::send_response(int http_status_code, td::BufferSlice &&content, int retry_after) {
  td::HttpHeaderCreator hc;
  hc.init_status_line(http_status_code);
  hc.set_keep_alive();
  hc.set_content_type("application/json");
  if (retry_after > 0) {
    hc.add_header("Retry-After", PSLICE() << retry_after);
  }
  hc.set_content_size(content.size());

  auto r_header = hc.finish();
  LOG(DEBUG) << "Response headers: " << r_header.ok();
  if (r_header.is_error()) {
    LOG(ERROR) << "Bad response headers";
    send_closure(std::move(connection_), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  LOG(DEBUG) << "Send result: " << content;

  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, std::move(content));
  send_closure(std::move(connection_), &td::HttpInboundConnection::write_ok);
}

void HttpConnection::send_http_error(int http_status_code, td::Slice description) {
  send_response(http_status_code, td::json_encode<td::BufferSlice>(JsonQueryError(http_status_code, description)), 0);
}

static td::Slice get_file_content_type(td::Slice path) {
  auto ext = td::PathView(path).extension();
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "mp4") return "video/mp4";
  if (ext == "webm") return "video/webm";
  if (ext == "mov") return "video/quicktime";
  if (ext == "avi") return "video/x-msvideo";
  if (ext == "mkv") return "video/x-matroska";
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "ogg" || ext == "oga") return "audio/ogg";
  if (ext == "m4a") return "audio/mp4";
  if (ext == "flac") return "audio/flac";
  if (ext == "wav") return "audio/wav";
  if (ext == "pdf") return "application/pdf";
  if (ext == "zip") return "application/zip";
  if (ext == "txt") return "text/plain";
  return "application/octet-stream";
}

// parses a single "bytes=a-b", "bytes=a-" or "bytes=-n" range; returns false if the header must be ignored
static bool parse_range_header(td::Slice header, td::int64 file_size, td::int64 &first, td::int64 &last, bool &is_valid) {
  is_valid = true;
  if (header.empty()) {
    return false;
  }
  if (!td::begins_with(header, "bytes=") || header.find(',') != td::Slice::npos) {
    return false;
  }
  header.remove_prefix(6);
  auto dash = header.find('-');
  if (dash == td::Slice::npos) {
    return false;
  }
  auto first_str = header.substr(0, dash);
  auto last_str = header.substr(dash + 1);
  if (first_str.empty()) {
    auto r_suffix = td::to_integer_safe<td::int64>(last_str);
    if (r_suffix.is_error() || r_suffix.ok() <= 0) {
      return false;
    }
    first = td::max(static_cast<td::int64>(0), file_size - r_suffix.ok());
    last = file_size - 1;
  } else {
    auto r_first = td::to_integer_safe<td::int64>(first_str);
    if (r_first.is_error() || r_first.ok() < 0) {
      return false;
    }
    first = r_first.ok();
    last = file_size - 1;
    if (!last_str.empty()) {
      auto r_last = td::to_integer_safe<td::int64>(last_str);
      if (r_last.is_error() || r_last.ok() < first) {
        return false;
      }
      last = td::min(last, r_last.ok());
    }
  }
  if (first >= file_size) {
    is_valid = false;
  }
  return true;
}

void HttpConnection::serve_file(td::Slice token, bool is_test_dc, td::Slice path, td::Slice range_header) {
  auto decoded_path = td::url_decode(path, false);

  // only files inside subdirectories of a bot's directory can be downloaded, which excludes the databases
  if (token.empty() || decoded_path.empty() || decoded_path[0] == '/' || decoded_path.find('\0') != td::string::npos ||
      decoded_path.find("..") != td::string::npos || decoded_path.find("//") != td::string::npos ||
      decoded_path.find('\\') != td::string::npos || decoded_path.find('/') == td::string::npos ||
      token.find("..") != td::Slice::npos || token.find('\\') != td::Slice::npos ||
      token.find('\0') != td::Slice::npos) {
    return send_http_error(404, "Not Found");
  }
  if (shared_data_->working_directory_.empty()) {
    return send_http_error(404, "Not Found");
  }

  auto dir_name = token.str() + (is_test_dc ? ":T" : "");
  if (!shared_data_->allow_colon_in_filenames_) {
    for (auto &c : dir_name) {
      if (c == ':') {
        c = '~';
      }
    }
  }
  auto r_dir = td::realpath(shared_data_->working_directory_ + dir_name);
  if (r_dir.is_error()) {
    return send_http_error(404, "Not Found");
  }
  auto dir = r_dir.move_as_ok() + TD_DIR_SLASH;
  auto r_full_path = td::realpath(dir + decoded_path);
  if (r_full_path.is_error() || !td::begins_with(r_full_path.ok(), dir)) {  // also rejects symlinks leaving the directory
    return send_http_error(404, "Not Found");
  }
  auto full_path = r_full_path.move_as_ok();

  auto r_file = td::FileFd::open(full_path, td::FileFd::Read);
  if (r_file.is_error()) {
    return send_http_error(404, "Not Found");
  }
  auto file = r_file.move_as_ok();
  auto r_stat = td::fstat(file.get_native_fd().fd());
  if (r_stat.is_error() || !r_stat.ok().is_reg_) {
    return send_http_error(404, "Not Found");
  }
  auto file_size = static_cast<td::int64>(r_stat.ok().size_);

  td::int64 first = 0;
  td::int64 last = file_size - 1;
  bool is_valid = true;
  bool is_partial = file_size > 0 && parse_range_header(range_header, file_size, first, last, is_valid);
  if (is_partial && !is_valid) {
    td::HttpHeaderCreator hc;
    hc.init_status_line(416);
    hc.add_header("Content-Range", PSLICE() << "bytes */" << file_size);
    hc.set_content_size(0);
    return send_raw_header(hc);
  }
  if (!is_partial) {
    first = 0;
    last = file_size - 1;
  }
  auto content_size = file_size == 0 ? td::int64(0) : last - first + 1;

  td::HttpHeaderCreator hc;
  hc.init_status_line(is_partial ? 206 : 200);
  hc.set_keep_alive();
  hc.set_content_type(get_file_content_type(full_path));
  hc.add_header("Accept-Ranges", "bytes");
  if (is_partial) {
    hc.add_header("Content-Range", PSLICE() << "bytes " << first << '-' << last << '/' << file_size);
  }
  hc.set_content_size(static_cast<size_t>(content_size));

  auto r_header = hc.finish();
  if (r_header.is_error()) {
    LOG(ERROR) << "Bad file response headers";
    send_closure(std::move(connection_), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }

  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  // the connection streams the file with backpressure and finishes the response itself
  send_closure(connection_, &td::HttpInboundConnection::write_file, std::move(file), first, content_size);
}

void HttpConnection::send_raw_header(td::HttpHeaderCreator &hc) {
  auto r_header = hc.finish();
  if (r_header.is_error()) {
    LOG(ERROR) << "Bad response headers";
    send_closure(std::move(connection_), &td::HttpInboundConnection::write_error, r_header.move_as_error());
    return;
  }
  send_closure(connection_, &td::HttpInboundConnection::write_next_noflush, td::BufferSlice(r_header.ok()));
  send_closure(std::move(connection_), &td::HttpInboundConnection::write_ok);
}

}  // namespace telegram_bot_api
