#include "httplib.h"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <random>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <queue>
#include <set>
#include <sstream>
#include <iomanip>
#include <functional>
#include <fstream>
#include <climits>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

static const int DIMS = 16;   // demo vectors
// Doc embeddings dimension is determined at runtime from Ollama's model output

// =====================================================================
//  DATA TYPES
// =====================================================================

struct VectorItem {
    int id;
    std::string metadata;
    std::string category;
    std::vector<float> emb;
};

using DistFn = std::function<float(const std::vector<float>&, const std::vector<float>&)>;

// =====================================================================
//  DISTANCE METRICS
// =====================================================================

float euclidean(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0;
    for (int i = 0; i < (int)a.size(); i++) { float d = a[i]-b[i]; s += d*d; }
    return std::sqrt(s);
}

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    float dot=0, na=0, nb=0;
    for (int i = 0; i < (int)a.size(); i++) {
        dot += a[i]*b[i]; na += a[i]*a[i]; nb += b[i]*b[i];
    }
    if (na < 1e-9f || nb < 1e-9f) return 1.0f;
    return 1.0f - dot / (std::sqrt(na) * std::sqrt(nb));
}

float manhattan(const std::vector<float>& a, const std::vector<float>& b) {
    float s = 0;
    for (int i = 0; i < (int)a.size(); i++) s += std::abs(a[i]-b[i]);
    return s;
}

DistFn getDistFn(const std::string& m) {
    if (m == "cosine")    return cosine;
    if (m == "manhattan") return manhattan;
    return euclidean;
}

// =====================================================================
//  BRUTE FORCE
// =====================================================================

class BruteForce {
public:
    std::vector<VectorItem> items;

    void insert(const VectorItem& v) { items.push_back(v); }

    std::vector<std::pair<float,int>> knn(
        const std::vector<float>& q, int k, DistFn dist)
    {
        std::vector<std::pair<float,int>> r;
        r.reserve(items.size());
        for (auto& v : items) r.push_back({dist(q, v.emb), v.id});
        std::sort(r.begin(), r.end());
        if ((int)r.size() > k) r.resize(k);
        return r;
    }

    void remove(int id) {
        items.erase(std::remove_if(items.begin(), items.end(),
            [id](const VectorItem& v){ return v.id == id; }), items.end());
    }
};

// =====================================================================
//  KD-TREE
// =====================================================================

struct KDNode {
    VectorItem item;
    KDNode* left  = nullptr;
    KDNode* right = nullptr;
    explicit KDNode(const VectorItem& v) : item(v) {}
};

class KDTree {
    KDNode* root = nullptr;
    int dims;

    void destroy(KDNode* n) {
        if (!n) return; destroy(n->left); destroy(n->right); delete n;
    }

    KDNode* ins(KDNode* n, const VectorItem& v, int d) {
        if (!n) return new KDNode(v);
        int ax = d % dims;
        if (v.emb[ax] < n->item.emb[ax]) n->left  = ins(n->left,  v, d+1);
        else                              n->right = ins(n->right, v, d+1);
        return n;
    }

    void knn(KDNode* n, const std::vector<float>& q, int k, int d, DistFn dist,
             std::priority_queue<std::pair<float,int>>& heap)
    {
        if (!n) return;
        float dn = dist(q, n->item.emb);
        if ((int)heap.size() < k || dn < heap.top().first) {
            heap.push({dn, n->item.id});
            if ((int)heap.size() > k) heap.pop();
        }
        int ax = d % dims;
        float diff = q[ax] - n->item.emb[ax];
        KDNode* closer  = diff < 0 ? n->left  : n->right;
        KDNode* farther = diff < 0 ? n->right : n->left;
        knn(closer, q, k, d+1, dist, heap);
        if ((int)heap.size() < k || std::abs(diff) < heap.top().first)
            knn(farther, q, k, d+1, dist, heap);
    }

public:
    explicit KDTree(int d) : dims(d) {}
    ~KDTree() { destroy(root); }

    void insert(const VectorItem& v) { root = ins(root, v, 0); }

    std::vector<std::pair<float,int>> knn(
        const std::vector<float>& q, int k, DistFn dist)
    {
        std::priority_queue<std::pair<float,int>> heap;
        knn(root, q, k, 0, dist, heap);
        std::vector<std::pair<float,int>> r;
        while (!heap.empty()) { r.push_back(heap.top()); heap.pop(); }
        std::sort(r.begin(), r.end());
        return r;
    }

    void rebuild(const std::vector<VectorItem>& items) {
        destroy(root); root = nullptr;
        for (auto& v : items) insert(v);
    }
};

// =====================================================================
//  HNSW — Hierarchical Navigable Small World
// =====================================================================

class HNSW {
    struct Node {
        VectorItem item;
        int maxLyr;
        std::vector<std::vector<int>> nbrs;
    };

    std::unordered_map<int, Node> G;
    int    M, M0, ef_build;
    float  mL;
    int    topLayer = -1;
    int    entryPt  = -1;
    std::mt19937 rng;

    int randLevel() {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        return (int)std::floor(-std::log(u(rng)) * mL);
    }

    std::vector<std::pair<float,int>> searchLayer(
        const std::vector<float>& q, int ep, int ef, int lyr, DistFn dist)
    {
        std::unordered_map<int,bool> vis;
        std::priority_queue<std::pair<float,int>,
            std::vector<std::pair<float,int>>, std::greater<>> cands;
        std::priority_queue<std::pair<float,int>> found;

        float d0 = dist(q, G[ep].item.emb);
        vis[ep] = true;
        cands.push({d0, ep});
        found.push({d0, ep});

        while (!cands.empty()) {
            auto [cd, cid] = cands.top(); cands.pop();
            if ((int)found.size() >= ef && cd > found.top().first) break;
            if (lyr >= (int)G[cid].nbrs.size()) continue;
            for (int nid : G[cid].nbrs[lyr]) {
                if (vis[nid] || !G.count(nid)) continue;
                vis[nid] = true;
                float nd = dist(q, G[nid].item.emb);
                if ((int)found.size() < ef || nd < found.top().first) {
                    cands.push({nd, nid});
                    found.push({nd, nid});
                    if ((int)found.size() > ef) found.pop();
                }
            }
        }

        std::vector<std::pair<float,int>> res;
        while (!found.empty()) { res.push_back(found.top()); found.pop(); }
        std::sort(res.begin(), res.end());
        return res;
    }

    std::vector<int> selectNbrs(std::vector<std::pair<float,int>>& cands, int maxM) {
        std::vector<int> r;
        for (int i = 0; i < std::min((int)cands.size(), maxM); i++)
            r.push_back(cands[i].second);
        return r;
    }

public:
    HNSW(int m = 16, int efBuild = 200)
        : M(m), M0(2*m), ef_build(efBuild),
          mL(1.0f / std::log((float)m)), rng(42) {}

    void insert(const VectorItem& item, DistFn dist) {
        int id  = item.id;
        int lvl = randLevel();
        G[id]   = {item, lvl, std::vector<std::vector<int>>(lvl + 1)};

        if (entryPt == -1) { entryPt = id; topLayer = lvl; return; }

        int ep = entryPt;
        for (int lc = topLayer; lc > lvl; lc--) {
            if (lc < (int)G[ep].nbrs.size()) {
                auto W = searchLayer(item.emb, ep, 1, lc, dist);
                if (!W.empty()) ep = W[0].second;
            }
        }
        for (int lc = std::min(topLayer, lvl); lc >= 0; lc--) {
            auto W   = searchLayer(item.emb, ep, ef_build, lc, dist);
            int maxM = (lc == 0) ? M0 : M;
            auto sel = selectNbrs(W, maxM);
            G[id].nbrs[lc] = sel;

            for (int nid : sel) {
                if (!G.count(nid)) continue;
                if ((int)G[nid].nbrs.size() <= lc) G[nid].nbrs.resize(lc + 1);
                auto& conn = G[nid].nbrs[lc];
                conn.push_back(id);
                if ((int)conn.size() > maxM) {
                    std::vector<std::pair<float,int>> ds;
                    for (int c : conn) if (G.count(c))
                        ds.push_back({dist(G[nid].item.emb, G[c].item.emb), c});
                    std::sort(ds.begin(), ds.end());
                    conn.clear();
                    for (int i = 0; i < maxM && i < (int)ds.size(); i++)
                        conn.push_back(ds[i].second);
                }
            }
            if (!W.empty()) ep = W[0].second;
        }
        if (lvl > topLayer) { topLayer = lvl; entryPt = id; }
    }

    std::vector<std::pair<float,int>> knn(
        const std::vector<float>& q, int k, int ef, DistFn dist)
    {
        if (entryPt == -1) return {};
        int ep = entryPt;
        for (int lc = topLayer; lc > 0; lc--) {
            if (lc < (int)G[ep].nbrs.size()) {
                auto W = searchLayer(q, ep, 1, lc, dist);
                if (!W.empty()) ep = W[0].second;
            }
        }
        auto W = searchLayer(q, ep, std::max(ef, k), 0, dist);
        if ((int)W.size() > k) W.resize(k);
        return W;
    }

    void remove(int id) {
        if (!G.count(id)) return;
        for (auto& [nid, nd] : G)
            for (auto& layer : nd.nbrs)
                layer.erase(std::remove(layer.begin(), layer.end(), id), layer.end());
        if (entryPt == id) {
            entryPt = -1;
            for (auto& [nid, nd] : G) if (nid != id) { entryPt = nid; break; }
        }
        G.erase(id);
    }

    struct GraphInfo {
        int topLayer, nodeCount;
        std::vector<int> nodesPerLayer, edgesPerLayer;
        struct NV { int id; std::string metadata, category; int maxLyr; };
        struct EV { int src, dst, lyr; };
        std::vector<NV> nodes;
        std::vector<EV> edges;
    };

    GraphInfo getInfo() {
        GraphInfo gi;
        gi.topLayer  = topLayer;
        gi.nodeCount = (int)G.size();
        int maxL = std::max(topLayer + 1, 1);
        gi.nodesPerLayer.assign(maxL, 0);
        gi.edgesPerLayer.assign(maxL, 0);
        for (auto& [id, nd] : G) {
            gi.nodes.push_back({id, nd.item.metadata, nd.item.category, nd.maxLyr});
            for (int lc = 0; lc <= nd.maxLyr && lc < maxL; lc++) {
                gi.nodesPerLayer[lc]++;
                if (lc < (int)nd.nbrs.size())
                    for (int nid : nd.nbrs[lc])
                        if (id < nid) {
                            gi.edgesPerLayer[lc]++;
                            gi.edges.push_back({id, nid, lc});
                        }
            }
        }
        return gi;
    }

    size_t size() const { return G.size(); }
};

// =====================================================================
//  VECTOR DATABASE  (demo 16D index)
// =====================================================================

class VectorDB {
    std::unordered_map<int, VectorItem> store;
    BruteForce bf;
    KDTree     kdt;
    HNSW       hnsw;
    std::mutex mu;
    int nextId = 1;

public:
    const int dims;
    explicit VectorDB(int d) : kdt(d), hnsw(16, 200), dims(d) {}

    int insert(const std::string& meta, const std::string& cat,
               const std::vector<float>& emb, DistFn dist)
    {
        std::lock_guard<std::mutex> lk(mu);
        VectorItem v{nextId++, meta, cat, emb};
        store[v.id] = v;
        bf.insert(v); kdt.insert(v); hnsw.insert(v, dist);
        return v.id;
    }

    bool remove(int id) {
        std::lock_guard<std::mutex> lk(mu);
        if (!store.count(id)) return false;
        store.erase(id); bf.remove(id); hnsw.remove(id);
        std::vector<VectorItem> rem;
        for (auto& [i, v] : store) rem.push_back(v);
        kdt.rebuild(rem);
        return true;
    }

    struct Hit { int id; std::string meta, cat; std::vector<float> emb; float dist; };
    struct SearchOut { std::vector<Hit> hits; long long us; std::string algo, metric; };

    SearchOut search(const std::vector<float>& q, int k,
                     const std::string& metric, const std::string& algo)
    {
        std::lock_guard<std::mutex> lk(mu);
        auto dfn = getDistFn(metric);
        auto t0  = std::chrono::high_resolution_clock::now();

        std::vector<std::pair<float,int>> raw;
        if      (algo == "bruteforce") raw = bf.knn(q, k, dfn);
        else if (algo == "kdtree")     raw = kdt.knn(q, k, dfn);
        else                           raw = hnsw.knn(q, k, 50, dfn);

        long long us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now() - t0).count();

        SearchOut out; out.us = us; out.algo = algo; out.metric = metric;
        for (auto& [d, id] : raw)
            if (store.count(id))
                out.hits.push_back({id, store[id].metadata, store[id].category, store[id].emb, d});
        return out;
    }

    struct BenchOut { long long bfUs, kdUs, hnswUs; int n; };

    BenchOut benchmark(const std::vector<float>& q, int k, const std::string& metric) {
        std::lock_guard<std::mutex> lk(mu);
        auto dfn  = getDistFn(metric);
        auto time = [&](auto fn) -> long long {
            auto t = std::chrono::high_resolution_clock::now();
            fn();
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - t).count();
        };
        return {
            time([&]{ bf.knn(q, k, dfn); }),
            time([&]{ kdt.knn(q, k, dfn); }),
            time([&]{ hnsw.knn(q, k, 50, dfn); }),
            (int)store.size()
        };
    }

    std::vector<VectorItem> all() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<VectorItem> r;
        for (auto& [id, v] : store) r.push_back(v);
        return r;
    }

    HNSW::GraphInfo hnswInfo() {
        std::lock_guard<std::mutex> lk(mu);
        return hnsw.getInfo();
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return store.size();
    }
};

// =====================================================================
//  JSON HELPERS
// =====================================================================

std::string jS(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if      (c == '"')  o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else                o += c;
    }
    return o + '"';
}

std::string jVec(const std::vector<float>& v) {
    std::ostringstream ss; ss << '[';
    for (size_t i = 0; i < v.size(); i++) {
        if (i) ss << ',';
        ss << std::fixed << std::setprecision(4) << v[i];
    }
    return ss.str() + ']';
}

std::vector<float> parseVec(const std::string& s) {
    std::vector<float> v;
    std::istringstream ss(s); std::string t;
    while (std::getline(ss, t, ','))
        try { v.push_back(std::stof(t)); } catch (...) {}
    return v;
}

// Extract a JSON string field value (handles all standard JSON escape sequences
// including \uXXXX Unicode escapes, which Ollama uses for '<', '>', '&', etc.)
std::string extractStr(const std::string& body, const std::string& key) {
    size_t p = body.find('"' + key + '"');
    if (p == std::string::npos) return "";
    p = body.find(':', p) + 1;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size() || body[p] != '"') return "";
    p++;
    std::string result;
    while (p < body.size()) {
        if (body[p] == '"') break;
        if (body[p] == '\\' && p + 1 < body.size()) {
            p++;
            switch (body[p]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                case 'b':  result += '\b'; break;
                case 'f':  result += '\f'; break;
                case 'u': {
                    // Decode \uXXXX → UTF-8
                    // Require exactly 4 hex digits; if malformed, emit literally.
                    if (p + 4 < body.size()) {
                        auto hexVal = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                            return -1;
                        };
                        int h0 = hexVal(body[p+1]);
                        int h1 = hexVal(body[p+2]);
                        int h2 = hexVal(body[p+3]);
                        int h3 = hexVal(body[p+4]);
                        if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                            unsigned int cp = (unsigned int)(
                                (h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                            p += 4; // skip the 4 hex digits (loop does +1 for the 'u')
                            // Encode codepoint as UTF-8
                            if (cp < 0x0080u) {
                                result += (char)cp;
                            } else if (cp < 0x0800u) {
                                result += (char)(0xC0u | (cp >> 6));
                                result += (char)(0x80u | (cp & 0x3Fu));
                            } else {
                                result += (char)(0xE0u | (cp >> 12));
                                result += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                                result += (char)(0x80u | (cp & 0x3Fu));
                            }
                            break;
                        }
                    }
                    // Malformed \uXXXX — emit 'u' literally and let the
                    // surrounding loop handle the remaining characters normally.
                    result += 'u';
                    break;
                }
                default:   result += body[p]; break;
            }
        } else {
            result += body[p];
        }
        p++;
    }
    return result;
}

// Extract a JSON integer field value
int extractInt(const std::string& body, const std::string& key, int def = 0) {
    size_t p = body.find('"' + key + '"');
    if (p == std::string::npos) return def;
    p = body.find(':', p) + 1;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    try { return std::stoi(body.substr(p)); } catch (...) { return def; }
}

bool parseBody(const std::string& b, std::string& meta,
               std::string& cat, std::vector<float>& emb)
{
    meta = extractStr(b, "metadata");
    cat  = extractStr(b, "category");
    auto extractArr = [&](const std::string& key) -> std::vector<float> {
        size_t p = b.find('"' + key + '"');
        if (p == std::string::npos) return {};
        p = b.find('[', p);
        if (p == std::string::npos) return {};
        size_t e = b.find(']', p);
        if (e == std::string::npos) return {};
        return parseVec(b.substr(p + 1, e - p - 1));
    };
    emb = extractArr("embedding");
    return !meta.empty() && !emb.empty();
}

void cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

// =====================================================================
//  PHASE 7 — CODING VERIFICATION & SANDBOXED EXECUTION ENGINE
// =====================================================================

struct TestCase {
    std::string input;
    std::string expected;
};

struct ExecutionResult {
    bool success = false;
    bool compiled = false;
    bool passedAll = false;
    int attempts = 1;
    std::string code;
    std::string compilerError;
    std::string runtimeError;
    std::string stdoutOutput;
    std::vector<std::string> testLogs;
};

class CodeVerifier {
public:
    static std::string extractCode(const std::string& text) {
        size_t p = text.find("```cpp");
        if (p == std::string::npos) p = text.find("```c++");
        if (p == std::string::npos) p = text.find("```C++");
        if (p == std::string::npos) p = text.find("```");
        if (p == std::string::npos) return text;

        size_t start = text.find('\n', p);
        if (start == std::string::npos) return text;
        start++;

        size_t end = text.find("```", start);
        if (end == std::string::npos) return text.substr(start);
        return text.substr(start, end - start);
    }

    static bool compileAndRun(const std::string& code,
                              const std::vector<TestCase>& tests,
                              ExecutionResult& res,
                              int timeoutMs = 2000)
    {
        res.code = code;
        res.compiled = false;
        res.passedAll = false;
        res.compilerError.clear();
        res.runtimeError.clear();
        res.stdoutOutput.clear();
        res.testLogs.clear();

        mkdir("scratch", 0755);
        mkdir("scratch/code_runner", 0755);

        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string uid = std::to_string(now) + "_" + std::to_string(rand() % 10000);
        std::string srcPath = "scratch/code_runner/sol_" + uid + ".cpp";
        std::string binPath = "scratch/code_runner/exec_" + uid;

        {
            std::ofstream f(srcPath);
            if (!f.is_open()) {
                res.compilerError = "Failed to create temp source file";
                return false;
            }
            f << code;
        }

        std::string compileCmd = "g++ -std=c++17 -Wall -O2 " + srcPath + " -o " + binPath + " 2>&1";
        FILE* pipe = popen(compileCmd.c_str(), "r");
        if (!pipe) {
            res.compilerError = "Failed to run compiler command";
            unlink(srcPath.c_str());
            return false;
        }

        char buffer[256];
        std::string compErr;
        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
            compErr += buffer;
        }
        int compRet = pclose(pipe);
        unlink(srcPath.c_str());

        if (compRet != 0) {
            res.compiled = false;
            res.compilerError = compErr.empty() ? "Compilation failed" : compErr;
            return false;
        }

        res.compiled = true;

        if (tests.empty()) {
            std::string runOut, runErr;
            int exitCode = executeBinary(binPath, "", runOut, runErr, timeoutMs);
            unlink(binPath.c_str());
            if (exitCode != 0) {
                res.runtimeError = "Runtime exit code " + std::to_string(exitCode) + ": " + runErr;
                res.testLogs.push_back("Run FAILED: " + runErr);
                return false;
            }
            res.stdoutOutput = runOut;
            res.testLogs.push_back("Executed successfully.");
            res.passedAll = true;
            res.success = true;
            return true;
        }

        bool allPassed = true;
        for (size_t i = 0; i < tests.size(); i++) {
            std::string runOut, runErr;
            int exitCode = executeBinary(binPath, tests[i].input, runOut, runErr, timeoutMs);
            if (exitCode != 0) {
                allPassed = false;
                res.runtimeError = "Runtime error on Test " + std::to_string(i + 1) + " (Exit code " + std::to_string(exitCode) + "): " + runErr;
                res.testLogs.push_back("Test " + std::to_string(i + 1) + " FAILED [Runtime Error]: " + runErr);
                break;
            }

            auto trim = [](std::string s) {
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
                size_t p = 0;
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
                return s.substr(p);
            };

            std::string normOut = trim(runOut);
            std::string normExp = trim(tests[i].expected);

            if (normOut == normExp || normExp.empty()) {
                res.testLogs.push_back("Test " + std::to_string(i + 1) + " PASSED. Output: " + normOut);
            } else {
                allPassed = false;
                res.runtimeError = "Wrong Output on Test " + std::to_string(i + 1) + ": Expected '" + normExp + "', Got '" + normOut + "'";
                res.testLogs.push_back("Test " + std::to_string(i + 1) + " FAILED. Expected: '" + normExp + "', Got: '" + normOut + "'");
                break;
            }
            if (i == 0) res.stdoutOutput = normOut;
        }

        unlink(binPath.c_str());
        res.passedAll = allPassed;
        res.success = allPassed;
        return allPassed;
    }

private:
    static int executeBinary(const std::string& binPath,
                             const std::string& input,
                             std::string& output,
                             std::string& errStr,
                             int timeoutMs)
    {
        int pipeIn[2], pipeOut[2], pipeErr[2];
        if (pipe(pipeIn) < 0 || pipe(pipeOut) < 0 || pipe(pipeErr) < 0) {
            errStr = "Failed to create IPC pipes";
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            errStr = "Failed to fork child process";
            return -1;
        }

        if (pid == 0) {
            close(pipeIn[1]);
            close(pipeOut[0]);
            close(pipeErr[0]);

            dup2(pipeIn[0], STDIN_FILENO);
            dup2(pipeOut[1], STDOUT_FILENO);
            dup2(pipeErr[1], STDERR_FILENO);

            close(pipeIn[0]);
            close(pipeOut[1]);
            close(pipeErr[1]);

            struct rlimit rlCpu;
            rlCpu.rlim_cur = 2;
            rlCpu.rlim_max = 3;
            setrlimit(RLIMIT_CPU, &rlCpu);

            struct rlimit rlMem;
            rlMem.rlim_cur = 512 * 1024 * 1024;
            rlMem.rlim_max = 512 * 1024 * 1024;
            setrlimit(RLIMIT_AS, &rlMem);

            struct rlimit rlProc;
            rlProc.rlim_cur = 0;
            rlProc.rlim_max = 0;
            setrlimit(RLIMIT_NPROC, &rlProc);

            char* args[] = { (char*)binPath.c_str(), nullptr };
            execv(binPath.c_str(), args);
            _exit(127);
        }

        close(pipeIn[0]);
        close(pipeOut[1]);
        close(pipeErr[1]);

        if (!input.empty()) {
            write(pipeIn[1], input.c_str(), input.size());
        }
        close(pipeIn[1]);

        auto startTime = std::chrono::steady_clock::now();
        int status = 0;
        bool timedOut = false;

        while (true) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            if (r < 0) break;

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed > timeoutMs) {
                timedOut = true;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (timedOut) {
            errStr = "Execution timed out (" + std::to_string(timeoutMs) + "ms limit exceeded)";
            close(pipeOut[0]);
            close(pipeErr[0]);
            return -2;
        }

        char buf[256];
        ssize_t n;
        while ((n = read(pipeOut[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            output += buf;
        }
        while ((n = read(pipeErr[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            errStr += buf;
        }

        close(pipeOut[0]);
        close(pipeErr[0]);

        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return WTERMSIG(status);
        return -1;
    }
};

// =====================================================================
//  CHAT HISTORY TYPES + PARSER
// =====================================================================

struct ChatTurn {
    std::string role;    // "user" or "assistant"
    std::string content;
};

// Parse the "history" JSON array from a request body.
// Expected shape: {"history":[{"role":"user","content":"..."}, ...]}
// Uses the existing extractStr() helper to decode each field.
std::vector<ChatTurn> parseHistory(const std::string& body) {
    std::vector<ChatTurn> result;
    size_t p = body.find("\"history\"");
    if (p == std::string::npos) return result;
    p = body.find('[', p);
    if (p == std::string::npos) return result;

    // Walk to the matching ']'
    size_t end = p + 1;
    int depth = 1;
    while (end < body.size() && depth > 0) {
        if      (body[end] == '[') depth++;
        else if (body[end] == ']') depth--;
        end++;
    }
    // Slice out just the array string
    std::string arr = body.substr(p, end - p);

    // Parse each { ... } object in the array
    size_t pos = 0;
    while (pos < arr.size()) {
        size_t objStart = arr.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = objStart + 1;
        int d = 1;
        while (objEnd < arr.size() && d > 0) {
            if      (arr[objEnd] == '{') d++;
            else if (arr[objEnd] == '}') d--;
            objEnd++;
        }
        std::string obj = arr.substr(objStart, objEnd - objStart);
        ChatTurn turn;
        turn.role    = extractStr(obj, "role");
        turn.content = extractStr(obj, "content");
        if (!turn.role.empty() && !turn.content.empty())
            result.push_back(turn);
        pos = objEnd;
    }
    return result;
}

// =====================================================================
//  TEXT CHUNKER
// =====================================================================

std::vector<std::string> chunkText(const std::string& text,
                                   int chunkWords = 250, int overlapWords = 30)
{
    std::istringstream ss(text);
    std::vector<std::string> words;
    std::string w;
    while (ss >> w) words.push_back(w);

    if (words.empty()) return {};
    if ((int)words.size() <= chunkWords) return {text};

    std::vector<std::string> chunks;
    int step = chunkWords - overlapWords;
    for (int i = 0; i < (int)words.size(); i += step) {
        int end = std::min(i + chunkWords, (int)words.size());
        std::string chunk;
        for (int j = i; j < end; j++) { if (j > i) chunk += ' '; chunk += words[j]; }
        chunks.push_back(chunk);
        if (end == (int)words.size()) break;
    }
    return chunks;
}

// =====================================================================
//  OLLAMA CLIENT  — wraps local Ollama REST API
//  Install:  https://ollama.com
//  Models:   ollama pull nomic-embed-text
//            ollama pull llama3          (default, ~4.5 GB)
//  Low-RAM alternatives (set OWN_AI_GEN_MODEL):
//    llama3.2:3b  (~2.2 GB)   phi3:mini  (~2.3 GB)
//    gemma2:2b    (~1.6 GB)   llama3.2:1b (~0.9 GB)
// =====================================================================

class OllamaClient {
private:
    // Persistent clients — avoids TCP connect/teardown on every request and
    // enables HTTP/1.1 keep-alive over loopback. Each is guarded by its own
    // mutex because httplib::Client is NOT thread-safe.
    httplib::Client embedCli_;
    httplib::Client genCli_;
    httplib::Client checkCli_;
    std::mutex      embedMu_;
    std::mutex      genMu_;
    std::mutex      checkMu_;

    // Escape a string for embedding inside a JSON string literal
    std::string esc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if      (c == '"')  o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else if (c == '\t') o += "\\t";
            else                o += c;
        }
        return o;
    }

    // Parse {"embedding":[...]} from Ollama /api/embeddings response
    std::vector<float> parseEmbedding(const std::string& body) {
        size_t p = body.find("\"embedding\"");
        if (p == std::string::npos) return {};
        p = body.find('[', p);
        if (p == std::string::npos) return {};
        // Find matching ]  — embeddings can be large (768+ floats)
        size_t e = p + 1, depth = 1;
        while (e < body.size() && depth > 0) {
            if (body[e] == '[') depth++;
            else if (body[e] == ']') depth--;
            e++;
        }
        return parseVec(body.substr(p + 1, e - p - 2));
    }

    // Parse {"response":"..."} from Ollama /api/generate response
    std::string parseResponse(const std::string& body) {
        return extractStr(body, "response");
    }

public:
    // Configurable via OWN_AI_GEN_MODEL / OWN_AI_EMBED_MODEL env vars
    std::string embedModel;
    std::string genModel;
    // Configurable via OWN_AI_NUM_CTX / OWN_AI_NUM_PREDICT env vars
    // num_ctx=2048 saves ~400-600 MB KV-cache vs Ollama's 4096 default
    int numCtx     = 2048;
    int numPredict = 512;

    explicit OllamaClient(const std::string& h = "127.0.0.1", int p = 11434)
        : embedCli_(h, p), genCli_(h, p), checkCli_(h, p)
    {
        // Configure timeouts and TCP_NODELAY on persistent clients
        embedCli_.set_connection_timeout(3, 0);
        embedCli_.set_read_timeout(30, 0);
        embedCli_.set_tcp_nodelay(true);

        genCli_.set_connection_timeout(3, 0);
        genCli_.set_read_timeout(180, 0);  // LLMs can be slow
        genCli_.set_tcp_nodelay(true);

        checkCli_.set_connection_timeout(2, 0);
        checkCli_.set_read_timeout(5, 0);
        checkCli_.set_tcp_nodelay(true);

        // Read model overrides from environment (no recompile needed)
        embedModel = "nomic-embed-text";
        genModel   = "llama3";
        if (const char* v = std::getenv("OWN_AI_EMBED_MODEL")) embedModel = v;
        if (const char* v = std::getenv("OWN_AI_GEN_MODEL"))   genModel   = v;
        if (const char* v = std::getenv("OWN_AI_NUM_CTX"))     try { numCtx     = std::stoi(v); } catch (...) {}
        if (const char* v = std::getenv("OWN_AI_NUM_PREDICT")) try { numPredict = std::stoi(v); } catch (...) {}
    }

    bool isAvailable() {
        std::lock_guard<std::mutex> lk(checkMu_);
        auto res = checkCli_.Get("/api/tags");
        return res && res->status == 200;
    }

    // Returns empty vector if Ollama is not running or model not found
    std::vector<float> embed(const std::string& text) {
        std::lock_guard<std::mutex> lk(embedMu_);
        std::string body = "{\"model\":\"" + embedModel + "\",\"prompt\":\"" + esc(text) + "\"}";
        auto res = embedCli_.Post("/api/embeddings", body, "application/json");
        if (!res || res->status != 200) return {};
        return parseEmbedding(res->body);
    }

    // Returns answer string, or an error message if Ollama is unavailable.
    // Includes num_ctx and num_predict to control RAM usage during inference.
    std::string generate(const std::string& prompt) {
        std::lock_guard<std::mutex> lk(genMu_);
        std::string body =
            "{\"model\":\""   + genModel + "\","
            "\"prompt\":\""   + esc(prompt) + "\","
            "\"stream\":false,"
            "\"options\":{\"num_ctx\":"     + std::to_string(numCtx) +
                         ",\"num_predict\":" + std::to_string(numPredict) + "}}";
        auto res = genCli_.Post("/api/generate", body, "application/json");
        if (!res || res->status != 200)
            return "ERROR: Ollama unavailable. Run: ollama serve";
        return parseResponse(res->body);
    }

    // Stream tokens from Ollama REST API.
    // On each token chunk, calls tokenCb(token_str). Return false from tokenCb to abort stream early.
    bool generateStream(const std::string& prompt,
                        std::function<bool(const std::string& token)> tokenCb) {
        std::lock_guard<std::mutex> lk(genMu_);
        std::string body =
            "{\"model\":\""   + genModel + "\","
            "\"prompt\":\""   + esc(prompt) + "\","
            "\"stream\":true,"
            "\"options\":{\"num_ctx\":"     + std::to_string(numCtx) +
                         ",\"num_predict\":" + std::to_string(numPredict) + "}}";

        httplib::Headers headers = { {"Content-Type", "application/json"} };
        std::string lineBuf;
        bool aborted = false;

        auto res = genCli_.Post(
            "/api/generate", headers, body, "application/json",
            [&](const char* data, size_t data_len) -> bool {
                if (aborted) return false;
                lineBuf.append(data, data_len);
                size_t pos = 0;
                while ((pos = lineBuf.find('\n')) != std::string::npos) {
                    std::string line = lineBuf.substr(0, pos);
                    lineBuf.erase(0, pos + 1);
                    if (line.empty()) continue;
                    std::string tok = extractStr(line, "response");
                    if (!tok.empty()) {
                        if (!tokenCb(tok)) {
                            aborted = true;
                            genCli_.stop(); // Close socket immediately to cancel Ollama inference
                            return false;
                        }
                    }
                }
                return true;
            });
        return res && res->status == 200 && !aborted;
    }
};

// =====================================================================
//  DOCUMENT DATABASE  — HNSW over real Ollama embeddings
// =====================================================================

struct DocItem {
    int         id;
    std::string title;
    std::string text;
    std::vector<float> emb;
};

class DocumentDB {
    std::unordered_map<int, DocItem> store;
    HNSW       hnsw;
    BruteForce bf;       // brute force fallback for small sets
    std::mutex mu;
    int nextId = 1;
    int dims   = 0;      // determined from first inserted embedding

public:
    DocumentDB() : hnsw(16, 200) {}

    // Insert one chunk with its pre-computed embedding
    int insert(const std::string& title, const std::string& text,
               const std::vector<float>& emb)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (dims == 0) dims = (int)emb.size();
        DocItem item{nextId++, title, text, emb};
        store[item.id] = item;
        VectorItem vi{item.id, title, "doc", emb};
        hnsw.insert(vi, cosine);
        // BruteForce is only queried when store.size() < 10; cap inserts there
        // to avoid accumulating a full redundant embedding copy beyond that point.
        if (store.size() <= 10) bf.insert(vi);
        return item.id;
    }

    // Semantic search — returns top-k most similar chunks
    std::vector<std::pair<float, DocItem>> search(
        const std::vector<float>& q, int k, float max_dist = 0.7f)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (store.empty()) return {};
        auto raw = (store.size() < 10)
                   ? bf.knn(q, k, cosine)
                   : hnsw.knn(q, k, 50, cosine);
        std::vector<std::pair<float, DocItem>> out;
        for (auto& [d, id] : raw)
            if (store.count(id) && d <= max_dist) out.push_back({d, store[id]});
        return out;
    }

    bool remove(int id) {
        std::lock_guard<std::mutex> lk(mu);
        if (!store.count(id)) return false;
        store.erase(id); hnsw.remove(id); bf.remove(id);
        return true;
    }

    std::vector<DocItem> all() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<DocItem> r;
        for (auto& [id, v] : store) r.push_back(v);
        return r;
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return store.size();
    }

    int getDims() { return dims; }
};

// =====================================================================
//  DEMO DATA  (16D categorical vectors)
// =====================================================================

void loadDemo(VectorDB& db) {
    auto dist = getDistFn("cosine");
    // Dims 0-3: CS | Dims 4-7: Math | Dims 8-11: Food | Dims 12-15: Sports
    db.insert("Linked List: nodes connected by pointers", "cs",
        {0.90f,0.85f,0.72f,0.68f,0.12f,0.08f,0.15f,0.10f,0.05f,0.08f,0.06f,0.09f,0.07f,0.11f,0.08f,0.06f}, dist);
    db.insert("Binary Search Tree: O(log n) search and insert", "cs",
        {0.88f,0.82f,0.78f,0.74f,0.15f,0.10f,0.08f,0.12f,0.06f,0.07f,0.08f,0.05f,0.09f,0.06f,0.07f,0.10f}, dist);
    db.insert("Dynamic Programming: memoization overlapping subproblems", "cs",
        {0.82f,0.76f,0.88f,0.80f,0.20f,0.18f,0.12f,0.09f,0.07f,0.06f,0.08f,0.07f,0.08f,0.09f,0.06f,0.07f}, dist);
    db.insert("Graph BFS and DFS: breadth and depth first traversal", "cs",
        {0.85f,0.80f,0.75f,0.82f,0.18f,0.14f,0.10f,0.08f,0.06f,0.09f,0.07f,0.06f,0.10f,0.08f,0.09f,0.07f}, dist);
    db.insert("Hash Table: O(1) lookup with collision chaining", "cs",
        {0.87f,0.78f,0.70f,0.76f,0.13f,0.11f,0.09f,0.14f,0.08f,0.07f,0.06f,0.08f,0.07f,0.10f,0.08f,0.09f}, dist);
    db.insert("Calculus: derivatives integrals and limits", "math",
        {0.12f,0.15f,0.18f,0.10f,0.91f,0.86f,0.78f,0.72f,0.08f,0.06f,0.07f,0.09f,0.07f,0.08f,0.06f,0.10f}, dist);
    db.insert("Linear Algebra: matrices eigenvalues eigenvectors", "math",
        {0.20f,0.18f,0.15f,0.12f,0.88f,0.90f,0.82f,0.76f,0.09f,0.07f,0.08f,0.06f,0.10f,0.07f,0.08f,0.09f}, dist);
    db.insert("Probability: distributions random variables Bayes theorem", "math",
        {0.15f,0.12f,0.20f,0.18f,0.84f,0.80f,0.88f,0.82f,0.07f,0.08f,0.06f,0.10f,0.09f,0.06f,0.09f,0.08f}, dist);
    db.insert("Number Theory: primes modular arithmetic RSA cryptography", "math",
        {0.22f,0.16f,0.14f,0.20f,0.80f,0.85f,0.76f,0.90f,0.08f,0.09f,0.07f,0.06f,0.08f,0.10f,0.07f,0.06f}, dist);
    db.insert("Combinatorics: permutations combinations generating functions", "math",
        {0.18f,0.20f,0.16f,0.14f,0.86f,0.78f,0.84f,0.80f,0.06f,0.07f,0.09f,0.08f,0.06f,0.09f,0.10f,0.07f}, dist);
    db.insert("Neapolitan Pizza: wood-fired dough San Marzano tomatoes", "food",
        {0.08f,0.06f,0.09f,0.07f,0.07f,0.08f,0.06f,0.09f,0.90f,0.86f,0.78f,0.72f,0.08f,0.06f,0.09f,0.07f}, dist);
    db.insert("Sushi: vinegared rice raw fish and nori rolls", "food",
        {0.06f,0.08f,0.07f,0.09f,0.09f,0.06f,0.08f,0.07f,0.86f,0.90f,0.82f,0.76f,0.07f,0.09f,0.06f,0.08f}, dist);
    db.insert("Ramen: noodle soup with chashu pork and soft-boiled eggs", "food",
        {0.09f,0.07f,0.06f,0.08f,0.08f,0.09f,0.07f,0.06f,0.82f,0.78f,0.90f,0.84f,0.09f,0.07f,0.08f,0.06f}, dist);
    db.insert("Tacos: corn tortillas with carnitas salsa and cilantro", "food",
        {0.07f,0.09f,0.08f,0.06f,0.06f,0.07f,0.09f,0.08f,0.78f,0.82f,0.86f,0.90f,0.06f,0.08f,0.07f,0.09f}, dist);
    db.insert("Croissant: laminated pastry with buttery flaky layers", "food",
        {0.06f,0.07f,0.10f,0.09f,0.10f,0.06f,0.07f,0.10f,0.85f,0.80f,0.76f,0.82f,0.09f,0.07f,0.10f,0.06f}, dist);
    db.insert("Basketball: fast-paced shooting dribbling slam dunks", "sports",
        {0.09f,0.07f,0.08f,0.10f,0.08f,0.09f,0.07f,0.06f,0.08f,0.07f,0.09f,0.06f,0.91f,0.85f,0.78f,0.72f}, dist);
    db.insert("Football: tackles touchdowns field goals and strategy", "sports",
        {0.07f,0.09f,0.06f,0.08f,0.09f,0.07f,0.10f,0.08f,0.07f,0.09f,0.08f,0.07f,0.87f,0.89f,0.82f,0.76f}, dist);
    db.insert("Tennis: racket volleys groundstrokes and Wimbledon serves", "sports",
        {0.08f,0.06f,0.09f,0.07f,0.07f,0.08f,0.06f,0.09f,0.09f,0.06f,0.07f,0.08f,0.83f,0.80f,0.88f,0.82f}, dist);
    db.insert("Chess: openings endgames tactics strategic board game", "sports",
        {0.25f,0.20f,0.22f,0.18f,0.22f,0.18f,0.20f,0.15f,0.06f,0.08f,0.07f,0.09f,0.80f,0.84f,0.78f,0.90f}, dist);
    db.insert("Swimming: butterfly freestyle backstroke Olympic competition", "sports",
        {0.06f,0.08f,0.07f,0.09f,0.08f,0.06f,0.09f,0.07f,0.10f,0.08f,0.06f,0.07f,0.85f,0.82f,0.86f,0.80f}, dist);
}

// =====================================================================
//  HTTP SERVER
// =====================================================================

int main() {
    VectorDB   db(DIMS);
    DocumentDB docDB;
    OllamaClient ollama;

    loadDemo(db);

    // Check Ollama at startup (non-fatal)
    bool ollamaUp = ollama.isAvailable();
    std::cout << "=== OWN-AI Server ===" << std::endl;
    std::cout << "http://localhost:8080" << std::endl;
    std::cout << db.size() << " demo vectors | " << DIMS << " dims | HNSW+KD-Tree+BruteForce" << std::endl;
    std::cout << "Ollama: " << (ollamaUp ? "ONLINE" : "OFFLINE (install from ollama.com)") << std::endl;
    if (ollamaUp) {
        std::cout << "  embed model : " << ollama.embedModel << std::endl;
        std::cout << "  gen model   : " << ollama.genModel   << std::endl;
        std::cout << "  num_ctx     : " << ollama.numCtx     << " tokens (KV-cache)" << std::endl;
        std::cout << "  num_predict : " << ollama.numPredict << " max output tokens" << std::endl;
    }
    std::cout << "--- RAM guidance ------------------------------------------" << std::endl;
    std::cout << "  8+ GB : default (llama3, num_ctx=2048) is fine" << std::endl;
    std::cout << "  6-8 GB: set OWN_AI_GEN_MODEL=llama3.2:3b or phi3:mini" << std::endl;
    std::cout << "  4-6 GB: set OWN_AI_GEN_MODEL=gemma2:2b OWN_AI_NUM_CTX=1024" << std::endl;
    std::cout << "  < 4 GB: set OWN_AI_GEN_MODEL=llama3.2:1b OWN_AI_NUM_CTX=1024" << std::endl;
    std::cout << "  Override: OWN_AI_GEN_MODEL  OWN_AI_EMBED_MODEL" << std::endl;
    std::cout << "            OWN_AI_NUM_CTX    OWN_AI_NUM_PREDICT" << std::endl;
    std::cout << "-----------------------------------------------------------" << std::endl;

    httplib::Server svr;
    svr.set_tcp_nodelay(true);
    // Explicit 4-thread pool — sufficient for a single-user local app.
    // Allows status/search/list requests to proceed concurrently while
    // a long generate() call is in flight, without unbounded thread growth.
    svr.new_task_queue = [] { return new httplib::ThreadPool(4); };

    // CORS preflight
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        cors(res); res.status = 204;
    });

    // ── DEMO VECTOR ENDPOINTS ─────────────────────────────────────────

    svr.Get("/search", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto q = parseVec(req.get_param_value("v"));
        if ((int)q.size() != DIMS) {
            res.set_content("{\"error\":\"need " + std::to_string(DIMS) + "D vector\"}",
                            "application/json"); return;
        }
        int k = 5;
        try { k = std::stoi(req.get_param_value("k")); } catch (...) {}
        auto metric = req.get_param_value("metric"); if (metric.empty()) metric = "cosine";
        auto algo   = req.get_param_value("algo");   if (algo.empty())   algo   = "hnsw";

        auto out = db.search(q, k, metric, algo);
        std::ostringstream ss;
        ss << "{\"results\":[";
        for (size_t i = 0; i < out.hits.size(); i++) {
            if (i) ss << ',';
            auto& h = out.hits[i];
            ss << "{\"id\":"        << h.id
               << ",\"metadata\":"  << jS(h.meta)
               << ",\"category\":"  << jS(h.cat)
               << ",\"distance\":"  << std::fixed << std::setprecision(6) << h.dist
               << ",\"embedding\":" << jVec(h.emb) << '}';
        }
        ss << "],\"latencyUs\":" << out.us
           << ",\"algo\":"       << jS(out.algo)
           << ",\"metric\":"     << jS(out.metric) << '}';
        res.set_content(ss.str(), "application/json");
    });

    svr.Post("/insert", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        std::string meta, cat; std::vector<float> emb;
        if (!parseBody(req.body, meta, cat, emb) || (int)emb.size() != DIMS) {
            res.set_content("{\"error\":\"invalid body\"}", "application/json"); return;
        }
        int id = db.insert(meta, cat, emb, getDistFn("cosine"));
        res.set_content("{\"id\":" + std::to_string(id) + "}", "application/json");
    });

    svr.Delete(R"(/delete/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        int id  = std::stoi(req.matches[1]);
        bool ok = db.remove(id);
        res.set_content("{\"ok\":" + std::string(ok ? "true" : "false") + "}",
                        "application/json");
    });

    svr.Get("/items", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        auto items = db.all();
        std::ostringstream ss; ss << '[';
        for (size_t i = 0; i < items.size(); i++) {
            if (i) ss << ',';
            auto& v = items[i];
            ss << "{\"id\":"        << v.id
               << ",\"metadata\":"  << jS(v.metadata)
               << ",\"category\":"  << jS(v.category)
               << ",\"embedding\":" << jVec(v.emb) << '}';
        }
        ss << ']';
        res.set_content(ss.str(), "application/json");
    });

    svr.Get("/benchmark", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto q = parseVec(req.get_param_value("v"));
        if ((int)q.size() != DIMS) {
            res.set_content("{\"error\":\"need " + std::to_string(DIMS) + "D vector\"}",
                            "application/json"); return;
        }
        int k = 5; try { k = std::stoi(req.get_param_value("k")); } catch (...) {}
        auto metric = req.get_param_value("metric"); if (metric.empty()) metric = "cosine";
        auto b = db.benchmark(q, k, metric);
        std::ostringstream ss;
        ss << "{\"bruteforceUs\":" << b.bfUs << ",\"kdtreeUs\":" << b.kdUs
           << ",\"hnswUs\":"       << b.hnswUs << ",\"itemCount\":" << b.n << '}';
        res.set_content(ss.str(), "application/json");
    });

    svr.Get("/hnsw-info", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        auto gi = db.hnswInfo();
        std::ostringstream ss;
        ss << "{\"topLayer\":" << gi.topLayer << ",\"nodeCount\":" << gi.nodeCount
           << ",\"nodesPerLayer\":[";
        for (size_t i = 0; i < gi.nodesPerLayer.size(); i++) {
            if (i) ss << ','; ss << gi.nodesPerLayer[i];
        }
        ss << "],\"edgesPerLayer\":[";
        for (size_t i = 0; i < gi.edgesPerLayer.size(); i++) {
            if (i) ss << ','; ss << gi.edgesPerLayer[i];
        }
        ss << "],\"nodes\":[";
        for (size_t i = 0; i < gi.nodes.size(); i++) {
            if (i) ss << ',';
            auto& n = gi.nodes[i];
            ss << "{\"id\":" << n.id << ",\"metadata\":" << jS(n.metadata)
               << ",\"category\":" << jS(n.category) << ",\"maxLyr\":" << n.maxLyr << '}';
        }
        ss << "],\"edges\":[";
        for (size_t i = 0; i < gi.edges.size(); i++) {
            if (i) ss << ',';
            auto& e = gi.edges[i];
            ss << "{\"src\":" << e.src << ",\"dst\":" << e.dst << ",\"lyr\":" << e.lyr << '}';
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // ── DOCUMENT + RAG ENDPOINTS ──────────────────────────────────────

    // POST /doc/insert  {"title":"...","text":"..."}
    // Chunks the text, embeds each chunk via Ollama, stores in DocumentDB
    svr.Post("/doc/insert", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto title = extractStr(req.body, "title");
        auto text  = extractStr(req.body, "text");
        if (title.empty() || text.empty()) {
            res.set_content("{\"error\":\"need title and text\"}", "application/json"); return;
        }

        auto chunks = chunkText(text, 250, 30);
        std::vector<int> ids;

        for (int i = 0; i < (int)chunks.size(); i++) {
            auto emb = ollama.embed(chunks[i]);
            if (emb.empty()) {
                res.set_content(
                    "{\"error\":\"Ollama unavailable. "
                    "Install from https://ollama.com then run: "
                    "ollama pull nomic-embed-text && ollama pull llama3\"}",
                    "application/json");
                return;
            }
            std::string chunkTitle = (chunks.size() > 1)
                ? title + " [" + std::to_string(i+1) + "/" + std::to_string(chunks.size()) + "]"
                : title;
            ids.push_back(docDB.insert(chunkTitle, chunks[i], emb));
        }

        std::ostringstream ss;
        ss << "{\"ids\":[";
        for (size_t i = 0; i < ids.size(); i++) { if (i) ss << ','; ss << ids[i]; }
        ss << "],\"chunks\":" << chunks.size()
           << ",\"dims\":"    << docDB.getDims() << '}';
        res.set_content(ss.str(), "application/json");
    });

    // DELETE /doc/delete/123
    svr.Delete(R"(/doc/delete/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        int id  = std::stoi(req.matches[1]);
        bool ok = docDB.remove(id);
        res.set_content("{\"ok\":" + std::string(ok ? "true" : "false") + "}",
                        "application/json");
    });

    // GET /doc/list
    svr.Get("/doc/list", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        auto docs = docDB.all();
        std::ostringstream ss; ss << '[';
        for (size_t i = 0; i < docs.size(); i++) {
            if (i) ss << ',';
            // Truncate text preview to 120 chars
            std::string preview = docs[i].text.substr(0, 120);
            if (docs[i].text.size() > 120) preview += "…";
            ss << "{\"id\":" << docs[i].id
               << ",\"title\":" << jS(docs[i].title)
               << ",\"preview\":" << jS(preview)
               << ",\"words\":"  << (int)std::count(docs[i].text.begin(), docs[i].text.end(), ' ') + 1
               << '}';
        }
        ss << ']';
        res.set_content(ss.str(), "application/json");
    });

    // POST /doc/search {"question":"...","k":3}
    // Fast retrieval for the UI visualizer
    svr.Post("/doc/search", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto question = extractStr(req.body, "question");
        int  k        = extractInt(req.body, "k", 3);
        if (question.empty()) {
            res.set_content("{\"error\":\"need question\"}", "application/json"); return;
        }

        auto qEmb = ollama.embed(question);
        if (qEmb.empty()) {
            res.set_content("{\"error\":\"Ollama unavailable\"}", "application/json"); return;
        }

        auto hits = docDB.search(qEmb, k);

        std::ostringstream ss;
        ss << "{\"contexts\":[";
        for (size_t i = 0; i < hits.size(); i++) {
            if (i) ss << ',';
            ss << "{\"id\":"       << hits[i].second.id
               << ",\"title\":"    << jS(hits[i].second.title)
               << ",\"distance\":" << std::fixed << std::setprecision(4) << hits[i].first << '}';
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // POST /doc/ask  {"question":"...","k":3,"history":[{"role":"user","content":"..."},...] }
    // Full RAG pipeline: embed → retrieve → generate (with multi-turn history)
    svr.Post("/doc/ask", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto question = extractStr(req.body, "question");
        int  k        = extractInt(req.body, "k", 3);
        if (question.empty()) {
            res.set_content("{\"error\":\"need question\"}", "application/json"); return;
        }

        // Parse conversation history (up to 10 prior turns)
        auto history = parseHistory(req.body);
        if ((int)history.size() > 10) history.erase(history.begin(), history.begin() + ((int)history.size() - 10));

        // Step 1: embed the question
        auto qEmb = ollama.embed(question);
        if (qEmb.empty()) {
            res.set_content("{\"error\":\"Ollama unavailable\"}", "application/json"); return;
        }

        // Step 2: retrieve top-k chunks
        auto hits = docDB.search(qEmb, k);

        // Step 3: filter by relevance threshold
        const float relevanceThreshold = 0.5f;
        std::vector<std::pair<float, DocItem>> relevantHits;
        for (auto& hit : hits) {
            if (hit.first < relevanceThreshold)
                relevantHits.push_back(hit);
        }

        // Step 4a: build conversation history block
        std::string historyBlock;
        if (!history.empty()) {
            historyBlock = "Previous conversation:\n";
            for (auto& turn : history) {
                if (turn.role == "user")
                    historyBlock += "User: " + turn.content + "\n";
                else
                    historyBlock += "Assistant: " + turn.content + "\n";
            }
            historyBlock += "\n";
        }

        // Step 4b: build full prompt
        std::string prompt;
        if (!relevantHits.empty()) {
            // RAG mode: include context
            std::ostringstream ctx;
            for (int i = 0; i < (int)relevantHits.size(); i++) {
                ctx << "[" << (i+1) << "] " << relevantHits[i].second.title << ":\n"
                    << relevantHits[i].second.text << "\n\n";
            }
            prompt = "You are a helpful assistant. Answer the user's question directly. "
                     "Use the provided context if it contains relevant information. "
                     "If it doesn't, just use your own general knowledge. "
                     "IMPORTANT: Do NOT mention the 'context', 'provided text', or say things like "
                     "'the context doesn't mention'. Just answer the question naturally.\n\n"
                     + historyBlock
                     + "Context:\n" + ctx.str()
                     + "Current question: " + question + "\n\n"
                     + "Answer:";
        } else {
            // Fallback: no relevant context, answer directly
            prompt = "You are a helpful assistant. Answer the question clearly and concisely.\n\n"
                     + historyBlock
                     + "Current question: " + question + "\n\n"
                     + "Answer:";
        }

        // Step 5: generate answer
        auto answer = ollama.generate(prompt);

        // Step 6: return everything
        std::ostringstream ss;
        ss << "{\"answer\":" << jS(answer)
           << ",\"model\":"  << jS(ollama.genModel)
           << ",\"usingContext\":" << (!relevantHits.empty() ? "true" : "false")
           << ",\"relevanceThreshold\":" << std::fixed << std::setprecision(2) << relevanceThreshold
           << ",\"contexts\":[";
        for (size_t i = 0; i < relevantHits.size(); i++) {
            if (i) ss << ',';
            ss << "{\"id\":"       << relevantHits[i].second.id
               << ",\"title\":"    << jS(relevantHits[i].second.title)
               << ",\"text\":"     << jS(relevantHits[i].second.text)
               << ",\"distance\":" << std::fixed << std::setprecision(4) << relevantHits[i].first << '}';
        }
        ss << "],\"docCount\":" << docDB.size() << '}';
        res.set_content(ss.str(), "application/json");
    });

    // POST /doc/ask/stream  {"question":"...","k":3,"history":[{"role":"user","content":"..."},...] }
    // Real-time streaming RAG pipeline using Server-Sent Events (SSE)
    svr.Post("/doc/ask/stream", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto question = extractStr(req.body, "question");
        int  k        = extractInt(req.body, "k", 3);
        if (question.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"need question\"}", "application/json"); return;
        }

        auto history = parseHistory(req.body);
        if ((int)history.size() > 10) history.erase(history.begin(), history.begin() + ((int)history.size() - 10));

        // Step 1: embed the question
        auto qEmb = ollama.embed(question);
        if (qEmb.empty()) {
            res.status = 503;
            res.set_content("{\"error\":\"Ollama unavailable\"}", "application/json"); return;
        }

        // Step 2: retrieve top-k chunks
        auto hits = docDB.search(qEmb, k);

        // Step 3: filter by relevance threshold
        const float relevanceThreshold = 0.5f;
        std::vector<std::pair<float, DocItem>> relevantHits;
        for (auto& hit : hits) {
            if (hit.first < relevanceThreshold)
                relevantHits.push_back(hit);
        }

        // Step 4a: build conversation history block
        std::string historyBlock;
        if (!history.empty()) {
            historyBlock = "Previous conversation:\n";
            for (auto& turn : history) {
                if (turn.role == "user")
                    historyBlock += "User: " + turn.content + "\n";
                else
                    historyBlock += "Assistant: " + turn.content + "\n";
            }
            historyBlock += "\n";
        }

        // Step 4b: build full prompt
        std::string prompt;
        if (!relevantHits.empty()) {
            std::ostringstream ctx;
            for (int i = 0; i < (int)relevantHits.size(); i++) {
                ctx << "[" << (i+1) << "] " << relevantHits[i].second.title << ":\n"
                    << relevantHits[i].second.text << "\n\n";
            }
            prompt = "You are a helpful assistant. Answer the user's question directly. "
                     "Use the provided context if it contains relevant information. "
                     "If it doesn't, just use your own general knowledge. "
                     "IMPORTANT: Do NOT mention the 'context', 'provided text', or say things like "
                     "'the context doesn't mention'. Just answer the question naturally.\n\n"
                     + historyBlock
                     + "Context:\n" + ctx.str()
                     + "Current question: " + question + "\n\n"
                     + "Answer:";
        } else {
            prompt = "You are a helpful assistant. Answer the question clearly and concisely.\n\n"
                     + historyBlock
                     + "Current question: " + question + "\n\n"
                     + "Answer:";
        }

        // Prepare context payload string for final event
        std::ostringstream ctxSs;
        ctxSs << "[";
        for (size_t i = 0; i < relevantHits.size(); i++) {
            if (i) ctxSs << ',';
            ctxSs << "{\"id\":"       << relevantHits[i].second.id
                  << ",\"title\":"    << jS(relevantHits[i].second.title)
                  << ",\"text\":"     << jS(relevantHits[i].second.text)
                  << ",\"distance\":" << std::fixed << std::setprecision(4) << relevantHits[i].first << '}';
        }
        ctxSs << "]";
        std::string contextsJson = ctxSs.str();

        // Step 5: Stream answer over SSE
        res.set_chunked_content_provider("text/event-stream",
            [prompt, contextsJson, &ollama](size_t offset, httplib::DataSink& sink) -> bool {
                if (offset > 0) return false;

                bool ok = ollama.generateStream(prompt, [&](const std::string& token) -> bool {
                    std::string sse = "data: {\"token\":" + jS(token) + "}\n\n";
                    return sink.write(sse.data(), sse.size());
                });

                if (sink.is_writable()) {
                    std::string finalEv = "data: {\"done\":true,\"contexts\":" + contextsJson + "}\n\n";
                    sink.write(finalEv.data(), finalEv.size());
                }
                return false;
            });
    });

    // POST /code/verify {"code":"...", "tests":[{"input":"...", "expected":"..."}, ...]}
    svr.Post("/code/verify", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto code = extractStr(req.body, "code");
        if (code.empty()) {
            code = CodeVerifier::extractCode(req.body);
        }
        if (code.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"need code\"}", "application/json");
            return;
        }

        std::vector<TestCase> tests;
        size_t tp = req.body.find("\"tests\"");
        if (tp != std::string::npos) {
            size_t start = req.body.find('[', tp);
            size_t end = req.body.find(']', start);
            if (start != std::string::npos && end != std::string::npos) {
                std::string arr = req.body.substr(start, end - start + 1);
                size_t p = 0;
                while ((p = arr.find('{', p)) != std::string::npos) {
                    size_t e = arr.find('}', p);
                    if (e == std::string::npos) break;
                    std::string item = arr.substr(p, e - p + 1);
                    std::string in  = extractStr(item, "input");
                    std::string exp = extractStr(item, "expected");
                    tests.push_back({in, exp});
                    p = e + 1;
                }
            }
        }

        ExecutionResult execRes;
        CodeVerifier::compileAndRun(code, tests, execRes);

        std::ostringstream ss;
        ss << "{\"verified\":"     << (execRes.passedAll ? "true" : "false")
           << ",\"compiled\":"     << (execRes.compiled ? "true" : "false")
           << ",\"passedAll\":"    << (execRes.passedAll ? "true" : "false")
           << ",\"compilerError\":" << jS(execRes.compilerError)
           << ",\"runtimeError\":"  << jS(execRes.runtimeError)
           << ",\"stdoutOutput\":"  << jS(execRes.stdoutOutput)
           << ",\"testLogs\":[";
        for (size_t i = 0; i < execRes.testLogs.size(); i++) {
            if (i) ss << ',';
            ss << jS(execRes.testLogs[i]);
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // POST /doc/ask/code {"question":"...", "k":3, "history":[...]}
    // Automated multi-turn code generation, compilation, verification & self-correction loop
    svr.Post("/doc/ask/code", [&](const httplib::Request& req, httplib::Response& res) {
        cors(res);
        auto question = extractStr(req.body, "question");
        if (question.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"need question\"}", "application/json");
            return;
        }

        auto history = parseHistory(req.body);
        if ((int)history.size() > 10) history.erase(history.begin(), history.begin() + ((int)history.size() - 10));

        std::string historyBlock;
        if (!history.empty()) {
            historyBlock = "Previous conversation:\n";
            for (auto& turn : history) {
                if (turn.role == "user")
                    historyBlock += "User: " + turn.content + "\n";
                else
                    historyBlock += "Assistant: " + turn.content + "\n";
            }
            historyBlock += "\n";
        }

        std::string basePrompt =
            "You are an expert C++ competitive programmer and software engineer.\n"
            "Provide a complete, production-ready, self-contained C++ program that solves the problem below.\n"
            "Requirements:\n"
            "1. Include all necessary header files (#include <iostream>, <vector>, etc.).\n"
            "2. Include a main() function that reads sample test case input or runs test assertions and prints the output to stdout.\n"
            "3. Enclose the C++ code inside a ```cpp ``` code block.\n"
            "4. Provide a clear Time Complexity and Space Complexity analysis at the end.\n\n"
            + historyBlock
            + "Coding Question:\n" + question + "\n\n"
            + "C++ Solution:";

        std::string currentPrompt = basePrompt;
        std::string rawResponse;
        ExecutionResult execRes;
        int maxRetries = 3;
        int attempt = 0;

        while (attempt < maxRetries) {
            attempt++;
            execRes.attempts = attempt;
            rawResponse = ollama.generate(currentPrompt);
            std::string code = CodeVerifier::extractCode(rawResponse);

            std::vector<TestCase> emptyTests;
            bool ok = CodeVerifier::compileAndRun(code, emptyTests, execRes);

            if (ok && execRes.compiled && execRes.passedAll) {
                break; // Verified success!
            }

            // Verification failed: construct feedback prompt for self-correction retry
            if (attempt < maxRetries) {
                std::string errDetails = execRes.compiled ? execRes.runtimeError : execRes.compilerError;
                currentPrompt = basePrompt + "\n\n"
                    + "CRITICAL: Your previous generated C++ solution FAILED verification on Attempt " + std::to_string(attempt) + ".\n"
                    + "Generated Code:\n```cpp\n" + code + "\n```\n"
                    + "Error details:\n" + errDetails + "\n\n"
                    + "Please fix the code above, eliminate any compilation/runtime errors, and return the corrected, self-contained C++ program inside a ```cpp ``` block.";
            }
        }

        std::ostringstream ss;
        ss << "{\"answer\":"         << jS(rawResponse)
           << ",\"verified\":"       << (execRes.passedAll ? "true" : "false")
           << ",\"attempts\":"       << execRes.attempts
           << ",\"compiled\":"       << (execRes.compiled ? "true" : "false")
           << ",\"passedAll\":"      << (execRes.passedAll ? "true" : "false")
           << ",\"compilerError\":"   << jS(execRes.compilerError)
           << ",\"runtimeError\":"    << jS(execRes.runtimeError)
           << ",\"stdoutOutput\":"    << jS(execRes.stdoutOutput)
           << ",\"testLogs\":[";
        for (size_t i = 0; i < execRes.testLogs.size(); i++) {
            if (i) ss << ',';
            ss << jS(execRes.testLogs[i]);
        }
        ss << "]}";
        res.set_content(ss.str(), "application/json");
    });

    // GET /status
    svr.Get("/status", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        bool up = ollama.isAvailable();
        std::ostringstream ss;
        ss << "{\"ollamaAvailable\":"  << (up ? "true" : "false")
           << ",\"embedModel\":"       << jS(ollama.embedModel)
           << ",\"genModel\":"         << jS(ollama.genModel)
           << ",\"docCount\":"         << docDB.size()
           << ",\"docDims\":"          << docDB.getDims()
           << ",\"demoDims\":"         << DIMS
           << ",\"demoCount\":"        << db.size() << '}';
        res.set_content(ss.str(), "application/json");
    });

    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        cors(res);
        std::ostringstream ss;
        ss << "{\"count\":"      << db.size()
           << ",\"dims\":"       << DIMS
           << ",\"algorithms\":[\"bruteforce\",\"kdtree\",\"hnsw\"]"
           << ",\"metrics\":[\"euclidean\",\"cosine\",\"manhattan\"]}";
        res.set_content(ss.str(), "application/json");
    });

    // Serve index.html
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("index.html");
        if (!f.is_open()) { res.status = 404; return; }
        res.set_content(
            std::string(std::istreambuf_iterator<char>(f),
                        std::istreambuf_iterator<char>()),
            "text/html");
    });

    svr.listen("0.0.0.0", 8080);
    return 0;
}
