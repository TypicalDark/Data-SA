#include "api_client.hpp"

#include <httplib.h>

#include "gsa/gzip.hpp"

namespace gsa::api {

struct Client::Impl {
  httplib::Client http;
  explicit Impl(const Endpoint& ep) : http(ep.host, ep.port) {
    http.set_keep_alive(true);
    http.set_connection_timeout(2, 0);
    http.set_read_timeout(15, 0);
    http.set_decompress(false);  // we inflate ourselves, so the client needs no zlib build option
    if (!ep.token.empty()) http.set_bearer_token_auth(ep.token);
  }
};

Client::Client(Endpoint ep) : ep_(std::move(ep)), impl_(std::make_unique<Impl>(ep_)) {}
Client::~Client() = default;

std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

Response Client::get(const std::string& path) { return get(path, std::string()); }

Response Client::send(const std::string& method, const std::string& path, const std::string& body) {
  Response out;
  const char* type = "application/json";
  httplib::Result r = method == "PUT"    ? impl_->http.Put(path, body, type)
                      : method == "POST" ? impl_->http.Post(path, body, type)
                                         : impl_->http.Delete(path, body, type);
  if (!r) {
    out.error = httplib::to_string(r.error());
    return out;
  }
  out.status = r->status;
  out.body = r->body;
  return out;
}

Response Client::get(const std::string& path, const std::string& etag) {
  Response out;
  httplib::Headers h;
  if (!etag.empty()) h.emplace("If-None-Match", etag);
  auto r = impl_->http.Get(path, h);
  if (!r) {
    out.error = httplib::to_string(r.error());
    return out;
  }
  out.status = r->status;
  out.body = r->body;
  out.etag = r->get_header_value("ETag");
  if (r->get_header_value("Content-Encoding") == "gzip" && !out.body.empty()) {
    try {
      auto raw = gsa::gunzip(reinterpret_cast<const uint8_t*>(out.body.data()), out.body.size());
      out.body.assign(raw.begin(), raw.end());
    } catch (const std::exception& e) {
      out.status = 0;
      out.error = e.what();
    }
  }
  return out;
}

std::optional<nlohmann::json> Client::get_json(const std::string& path, std::string* err) {
  auto r = get(path);
  if (!r.ok()) {
    if (err) *err = r.status ? std::to_string(r.status) + " " + r.body : r.error;
    return std::nullopt;
  }
  auto j = nlohmann::json::parse(r.body, nullptr, false);
  if (j.is_discarded()) {
    if (err) *err = "bad JSON from " + path;
    return std::nullopt;
  }
  return j;
}

Response Client::tile(const std::string& pack, int z, uint32_t x, uint32_t y) {
  return get("/v1/tiles/" + pack + "/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y));
}

std::vector<SearchHit> Client::search(const std::string& q, int limit, std::string* err) {
  std::vector<SearchHit> out;
  auto j = get_json("/v1/search?q=" + url_encode(q) + "&limit=" + std::to_string(limit), err);
  if (!j || !j->is_array()) return out;
  for (const auto& h : *j) {
    SearchHit s{h.value("ref", ""), h.value("kind", ""), h.value("label", ""), h.value("detail", ""), {}, {}};
    if (h.contains("lon")) s.lon = h["lon"].get<double>();
    if (h.contains("lat")) s.lat = h["lat"].get<double>();
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace gsa::api
