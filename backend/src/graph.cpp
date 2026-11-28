#include "gsa/graph.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <limits>
#include <queue>

namespace gsa::graph {

int max_kv(const std::string& s) {
  int best = 0, cur = 0;
  bool in = false;
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      cur = cur * 10 + (c - '0');
      in = true;
      if (cur > 2000) cur = 2000;  // guards against junk; the highest SA voltage is 765 kV
    } else {
      if (in) best = std::max(best, cur);
      cur = 0, in = false;
    }
  }
  if (in) best = std::max(best, cur);
  return best;
}

std::shared_ptr<const Graph> Graph::load(sql::Db& db, const std::string& network) {
  auto g = std::make_shared<Graph>();
  g->network_ = network;
  db.query("SELECT node_id, ext_id, kind, name, operator, kv, lon, lat FROM node WHERE network = ? ORDER BY node_id",
           {network}, [&](const sql::Row& r) {
             Node n;
             n.id = r.i64(0);
             n.ext_id = r.opt_text(1).value_or("");
             n.kind = r.opt_text(2).value_or("");
             n.name = r.opt_text(3).value_or("");
             n.op = r.opt_text(4).value_or("");
             n.kv = max_kv(r.opt_text(5).value_or(""));
             if (!r.is_null(6) && !r.is_null(7)) n.lon = r.f64(6), n.lat = r.f64(7), n.has_pos = true;
             g->node_idx_[n.id] = int(g->nodes_.size());
             g->nodes_.push_back(std::move(n));
           });
  g->adj_.resize(g->nodes_.size());
  db.query("SELECT edge_id, source, target, cost, reverse_cost, cls, kv, inferred FROM edge WHERE network = ? ORDER BY edge_id",
           {network}, [&](const sql::Row& r) {
             const auto a = g->node_idx_.find(r.i64(1)), b = g->node_idx_.find(r.i64(2));
             if (a == g->node_idx_.end() || b == g->node_idx_.end()) return;  // dangling edge: skip, never invent nodes
             Edge e;
             e.id = r.i64(0);
             e.a = a->second, e.b = b->second;
             e.cost = r.is_null(3) ? 0 : std::max(0.0, r.f64(3));
             e.reverse = r.is_null(4) ? e.cost : r.f64(4);
             e.cls = r.opt_text(5).value_or("");
             e.kv = max_kv(r.opt_text(6).value_or(""));
             e.inferred = !r.is_null(7) && r.i64(7) != 0;
             const int ei = int(g->edges_.size());
             g->edge_idx_[e.id] = ei;
             g->adj_[size_t(e.a)].push_back({ei, e.b});
             g->adj_[size_t(e.b)].push_back({ei, e.a});
             g->edges_.push_back(std::move(e));
           });
  // a node's voltage falls back to the highest voltage of its lines (towers and junctions carry none)
  for (size_t i = 0; i < g->nodes_.size(); ++i) {
    if (g->nodes_[i].kv) continue;
    for (const auto& h : g->adj_[i]) g->nodes_[i].kv = std::max(g->nodes_[i].kv, g->edges_[size_t(h.edge)].kv);
  }
  return g;
}

std::optional<int> Graph::node_index(int64_t id) const {
  const auto it = node_idx_.find(id);
  if (it == node_idx_.end()) return std::nullopt;
  return it->second;
}
std::optional<int> Graph::edge_index(int64_t id) const {
  const auto it = edge_idx_.find(id);
  if (it == edge_idx_.end()) return std::nullopt;
  return it->second;
}

bool Graph::usable(const Hop& h, int from_node, int min_kv) const {
  const Edge& e = edges_[size_t(h.edge)];
  if (min_kv > 0 && e.kv > 0 && e.kv < min_kv) return false;
  if (from_node == e.b && e.reverse < 0) return false;  // one way
  return true;
}

Trace Graph::connected(int from, int min_kv, size_t limit) const {
  Trace t;
  std::vector<char> seen(nodes_.size(), 0), eseen(edges_.size(), 0);
  std::deque<int> q{from};
  seen[size_t(from)] = 1;
  t.nodes.push_back(nodes_[size_t(from)].id);
  while (!q.empty()) {
    const int u = q.front();
    q.pop_front();
    for (const auto& h : adj_[size_t(u)]) {
      if (!usable(h, u, min_kv)) continue;
      if (!eseen[size_t(h.edge)]) {
        eseen[size_t(h.edge)] = 1;
        t.edges.push_back(edges_[size_t(h.edge)].id);
        t.km += edges_[size_t(h.edge)].cost;
      }
      if (seen[size_t(h.other)]) continue;
      if (t.nodes.size() >= limit) {
        t.truncated = true;
        continue;
      }
      seen[size_t(h.other)] = 1;
      t.nodes.push_back(nodes_[size_t(h.other)].id);
      q.push_back(h.other);
    }
  }
  return t;
}

namespace {
struct Dij {
  std::vector<double> dist;
  std::vector<int> via;  // edge index used to reach the node, -1 at the start
};
}  // namespace

static Trace walk_back(const Graph& g, const Dij& d, int from, int to) {
  Trace t;
  if (d.dist[size_t(to)] == std::numeric_limits<double>::infinity()) {
    t.found = false;
    return t;
  }
  int cur = to;
  t.nodes.push_back(g.node(cur).id);
  while (cur != from) {
    const Edge& e = g.edge(d.via[size_t(cur)]);
    t.edges.push_back(e.id);
    cur = (e.a == cur) ? e.b : e.a;
    t.nodes.push_back(g.node(cur).id);
  }
  std::reverse(t.nodes.begin(), t.nodes.end());
  std::reverse(t.edges.begin(), t.edges.end());
  t.km = d.dist[size_t(to)];
  return t;
}

Trace Graph::path(int from, int to, int min_kv) const {
  Dij d{std::vector<double>(nodes_.size(), std::numeric_limits<double>::infinity()), std::vector<int>(nodes_.size(), -1)};
  using QE = std::pair<double, int>;
  std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
  d.dist[size_t(from)] = 0;
  pq.push({0, from});
  while (!pq.empty()) {
    const auto [du, u] = pq.top();
    pq.pop();
    if (du > d.dist[size_t(u)]) continue;
    if (u == to) break;
    for (const auto& h : adj_[size_t(u)]) {
      if (!usable(h, u, min_kv)) continue;
      const double nd = du + edges_[size_t(h.edge)].cost;
      if (nd < d.dist[size_t(h.other)]) {
        d.dist[size_t(h.other)] = nd, d.via[size_t(h.other)] = h.edge;
        pq.push({nd, h.other});
      }
    }
  }
  return walk_back(*this, d, from, to);
}

std::vector<Source> Graph::feed(int from, int source_kv, size_t k) const {
  Dij d{std::vector<double>(nodes_.size(), std::numeric_limits<double>::infinity()), std::vector<int>(nodes_.size(), -1)};
  using QE = std::pair<double, int>;
  std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
  d.dist[size_t(from)] = 0;
  pq.push({0, from});
  std::vector<int> hits;
  while (!pq.empty() && hits.size() < k) {
    const auto [du, u] = pq.top();
    pq.pop();
    if (du > d.dist[size_t(u)]) continue;
    const Node& n = nodes_[size_t(u)];
    // a source is a substation or station at the threshold voltage, not a tower on a line of that voltage
    if (u != from && n.kv >= source_kv && n.kind != "endpoint" && n.kind != "tower") {
      hits.push_back(u);
      continue;  // do not search past a source
    }
    for (const auto& h : adj_[size_t(u)]) {
      if (!usable(h, u, 0)) continue;
      const double nd = du + edges_[size_t(h.edge)].cost;
      if (nd < d.dist[size_t(h.other)]) {
        d.dist[size_t(h.other)] = nd, d.via[size_t(h.other)] = h.edge;
        pq.push({nd, h.other});
      }
    }
  }
  std::vector<Source> out;
  for (int h : hits) out.push_back({nodes_[size_t(h)].id, d.dist[size_t(h)], walk_back(*this, d, from, h)});
  return out;
}

Trace Graph::fail(std::optional<int> fail_node, std::optional<int> fail_edge, int source_kv, size_t limit) const {
  Trace t;
  // the piece of network the failed element sits in
  const int start = fail_node ? *fail_node : edges_[size_t(*fail_edge)].a;
  const Trace piece = connected(start, 0, std::numeric_limits<size_t>::max());
  std::vector<char> in_piece(nodes_.size(), 0);
  for (auto id : piece.nodes) in_piece[size_t(node_idx_.at(id))] = 1;

  // flood from every source in that piece, around the failed element
  std::vector<char> fed(nodes_.size(), 0), efed(edges_.size(), 0);
  std::deque<int> q;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!in_piece[i] || (fail_node && int(i) == *fail_node)) continue;
    const Node& n = nodes_[i];
    if (n.kv >= source_kv && n.kind != "endpoint" && n.kind != "tower") fed[i] = 1, q.push_back(int(i));
  }
  while (!q.empty()) {
    const int u = q.front();
    q.pop_front();
    for (const auto& h : adj_[size_t(u)]) {
      if (fail_edge && h.edge == *fail_edge) continue;
      if (fail_node && h.other == *fail_node) continue;
      efed[size_t(h.edge)] = 1;
      if (!fed[size_t(h.other)]) fed[size_t(h.other)] = 1, q.push_back(h.other);
    }
  }
  for (size_t i = 0; i < nodes_.size(); ++i) {
    if (!in_piece[i] || fed[i] || (fail_node && int(i) == *fail_node)) continue;
    if (t.nodes.size() >= limit) {
      t.truncated = true;
      break;
    }
    t.nodes.push_back(nodes_[i].id);
  }
  for (auto id : piece.edges) {
    const int ei = edge_idx_.at(id);
    if (efed[size_t(ei)] || (fail_edge && ei == *fail_edge)) continue;
    const Edge& e = edges_[size_t(ei)];
    if (fail_node && (e.a == *fail_node || e.b == *fail_node) && (fed[size_t(e.a)] || fed[size_t(e.b)])) continue;
    t.edges.push_back(id);
    t.km += e.cost;
  }
  return t;
}

}  // namespace gsa::graph
