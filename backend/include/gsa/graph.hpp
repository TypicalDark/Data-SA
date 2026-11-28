// In-memory network graph (power grid now; roads and rivers use the same tables) built from the
// SQLite pack's node and edge tables. Powers /v1/trace: TR (connected), PATH, FEED and FAIL.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gsa/sqlite.hpp"

namespace gsa::graph {

struct Node {
  int64_t id = 0;
  std::string ext_id, kind, name, op;
  int kv = 0;  // highest voltage at the node, 0 = unknown
  double lon = 0, lat = 0;
  bool has_pos = false;
};

struct Edge {
  int64_t id = 0;
  int a = 0, b = 0;      // node indexes
  double cost = 0;       // km for power
  double reverse = 0;    // < 0 = one way (a -> b only)
  int kv = 0;
  std::string cls;
  bool inferred = false;
};

struct Hop {
  int edge;   // edge index
  int other;  // node index at the far end
};

// Result of a trace. Ids are the pack's node_id / edge_id (the same ids the grid tiles carry).
struct Trace {
  std::vector<int64_t> nodes, edges;
  double km = 0;
  bool truncated = false;
  bool found = true;
};

struct Source {
  int64_t node;
  double km;
  Trace path;
};

class Graph {
 public:
  // Loads one network from the pack (tables node, edge). Throws sql::Error if they are missing.
  static std::shared_ptr<const Graph> load(sql::Db& db, const std::string& network);

  const std::string& network() const { return network_; }
  size_t node_count() const { return nodes_.size(); }
  size_t edge_count() const { return edges_.size(); }
  std::optional<int> node_index(int64_t id) const;
  std::optional<int> edge_index(int64_t id) const;
  const Node& node(int i) const { return nodes_[size_t(i)]; }
  const Edge& edge(int i) const { return edges_[size_t(i)]; }
  const std::vector<Hop>& hops(int i) const { return adj_[size_t(i)]; }

  // Everything reachable from `from` over edges of at least min_kv (0 = all), up to limit nodes.
  Trace connected(int from, int min_kv, size_t limit) const;
  // Shortest path by cost (km). found=false when `to` is not reachable.
  Trace path(int from, int to, int min_kv) const;
  // The k nearest nodes of at least source_kv reachable from `from`, each with its path.
  std::vector<Source> feed(int from, int source_kv, size_t k) const;
  // What loses every route to a node of at least source_kv when node `fail_node` (or edge
  // `fail_edge`) is taken out. Only the failed element's own connected piece is examined.
  Trace fail(std::optional<int> fail_node, std::optional<int> fail_edge, int source_kv, size_t limit) const;

 private:
  bool usable(const Hop& h, int from_node, int min_kv) const;
  std::string network_;
  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::vector<std::vector<Hop>> adj_;
  std::unordered_map<int64_t, int> node_idx_, edge_idx_;
};

// Highest number in a "132/66" style voltage list; 0 when none.
int max_kv(const std::string& s);

}  // namespace gsa::graph
