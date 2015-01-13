#include <cstring>
#include <sstream>
#include <thread>

#include "SynSearch.h"
#include "Utils.h"

using namespace std;

// ----------------------------------------------
// SEARCH ALGORITHM
// ----------------------------------------------


inline uint64_t memoryItem(const uint64_t& fact, const uint8_t& currentIndex,
                           const bool& truth) {
  uint64_t currentIndexShifted = currentIndex << 1;
  return (fact << 9) | currentIndexShifted | (truth ? 1l : 0l);
} 

#pragma GCC push_options  // matches pop_options below
#pragma GCC optimize ("unroll-loops")
inline uint64_t searchLoop(
    std::function<void(const ScoredSearchNode)> enqueue,
    std::function<const bool (ScoredSearchNode*)> dequeue,
    std::function<void(const ScoredSearchNode&)> registerVisited,
    SearchNode* history, uint64_t& historySize,
    const SynSearchCosts* costs, const syn_search_options& opts, 
    const Graph* graph, const Tree& tree) {
  // Variables
  uint64_t ticks = 0;
  uint8_t  dependentIndices[8];
  natlog_relation  dependentRelations[8];
  ScoredSearchNode scoredNode;
#if SEARCH_FULL_MEMORY!=0
  btree::btree_set<uint64_t> fullMemory;
#else
#if SEARCH_CYCLE_MEMORY!=0
  uint8_t memorySize = 0;
  SearchNode memory[SEARCH_CYCLE_MEMORY];
#endif
#endif

  // Main Loop
  while (ticks < opts.maxTicks && dequeue(&scoredNode)) {
    
    // Register the dequeue'd element
    const SearchNode& node = scoredNode.node;
#if SEARCH_FULL_MEMORY!=0
  const uint64_t fullMemoryItem = memoryItem(node.factHash(), node.tokenIndex(), node.truthState());
  if (fullMemory.find(fullMemoryItem) != fullMemory.end()) {
    continue;  // Prohibit duplicate visits
  }
  fullMemory.insert(fullMemoryItem);
#else
#if SEARCH_CYCLE_MEMORY!=0
    memory[0] = history[node.getBackpointer()];
    memorySize = 1;
    for (uint8_t i = 1; i < SEARCH_CYCLE_MEMORY; ++i) {
      if (memory[i - 1].getBackpointer() != 0) {
        memory[i] = history[memory[i - 1].getBackpointer()];
        memorySize = i + 1;
      }
    }
    const SearchNode& parent = history[node.getBackpointer()];
#endif
#endif
    // Register visited
    registerVisited(scoredNode);

    // Update history
    const uint32_t myIndex = historySize;
//    fprintf(stderr, "%u>> %s (points to %u)\n", 
//      myIndex, toString(*graph, tree, node).c_str(),
//      node.getBackpointer());
    history[myIndex] = node;
    historySize += 1;
    ticks += 1;
    if (!opts.silent && ticks % 100000 == 0) { 
      printTime("[%c] "); 
      fprintf(stderr, "  |Search Progress| ticks=%luK\n", ticks / 1000);
    }

    // PUSH 1: Mutations
    uint32_t numEdges;
    const tagged_word nodeToken = node.token();
    assert(nodeToken.word < graph->vocabSize());
    const edge* edges = graph->incomingEdgesFast(nodeToken.word, &numEdges);
    const uint8_t tokenIndex = node.tokenIndex();
    for (uint32_t edgeI = 0; edgeI < numEdges; ++edgeI) {
      const edge& edge = edges[edgeI];
      assert(edge.source < graph->vocabSize());
      assert(nodeToken.word < graph->vocabSize());
      assert(edge.sink == nodeToken.word);
      // (ignore when sense doesn't match)
      if (edge.sink_sense != nodeToken.sense) { continue; }
      // (ignore multiple quantifier mutations)
      bool mutatingQuantifer;
      if (edge.type == QUANTIFIER_REWORD || edge.type == QUANTIFIER_NEGATE ||
          edge.type == QUANTIFIER_UP || edge.type == QUANTIFIER_DOWN) {
        if (tree.word(tokenIndex) != node.token().word) { continue; }
        mutatingQuantifer = true;
      }
      // (get cost)
      bool newTruthValue;
      assert (!isinf(edge.cost));
      assert (edge.cost == edge.cost);
      assert (edge.cost >= 0.0);
      const float mutationCost = costs->mutationCost(
          tree, node, edge.type,
          node.truthState(), &newTruthValue);
      if (isinf(mutationCost)) { continue; }
      const float cost = mutationCost * edge.cost;

      // (create child)
      const SearchNode mutatedChild
        = mutation(node, edge, myIndex, newTruthValue, tree, graph);
      assert(mutatedChild.word() < graph->vocabSize());
      if (mutatingQuantifer) {
        // TODO(gabor) update quantifier with mutated version!
      }
      // (push child)
#if SEARCH_FULL_MEMORY!=0
#else
#if SEARCH_CYCLE_MEMORY!=0
      bool isNewChild = true;
      for (uint8_t i = 0; i < memorySize; ++i) {
        isNewChild &= (mutatedChild != memory[i]);
      }
      if (isNewChild) {
#endif
#endif
//      fprintf(stderr, "  push mutation %s\n", toString(*graph, tree, mutatedChild).c_str());
      assert(!isinf(cost));
      assert(cost == cost);  // NaN check
      assert(cost >= 0.0);
      enqueue(ScoredSearchNode(mutatedChild, cost));
#if SEARCH_FULL_MEMORY!=0
#else
#if SEARCH_CYCLE_MEMORY!=0
      }
#endif
#endif
    }
  
    // INTERM: Get Children
    uint8_t numDependents;
    tree.dependents(node.tokenIndex(), 8, dependentIndices,
                    dependentRelations, &numDependents);
    
    for (uint8_t dependentI = 0; dependentI < numDependents; ++dependentI) {
      const uint8_t& dependentIndex = dependentIndices[dependentI];

      // PUSH 2: Deletions
      if (node.isDeleted(dependentIndex)) { continue; }
      bool newTruthValue;
      const float cost = costs->insertionCost(
            tree, node, tree.relation(dependentIndex),
            tree.word(dependentIndex), node.truthState(), &newTruthValue);
      if (!isinf(cost)) {
        // (create child)
        const SearchNode deletedChild 
          = deletion(node, myIndex, newTruthValue, tree, dependentIndex);
        assert(deletedChild.word() < graph->vocabSize());
        // (push child)
        assert(!isinf(cost));
        assert(cost == cost);  // NaN check
        assert(cost >= 0.0);
        enqueue(ScoredSearchNode(deletedChild, cost));
//        fprintf(stderr, "  push deletion %s\n", toString(*graph, tree, deletedChild).c_str());
      }

      // PUSH 3: Index Move
      // (create child)
      const SearchNode indexMovedChild(node, tree, dependentIndex,
                                       myIndex);
      assert(indexMovedChild.word() < graph->vocabSize());
      // (push child)
      assert(!isinf(scoredNode.cost));
      assert(cost == cost);  // NaN check
      assert(cost >= 0.0);
      enqueue(ScoredSearchNode(indexMovedChild, scoredNode.cost));
//      fprintf(stderr, "  push index move %s\n", toString(*graph, tree, indexMovedChild).c_str());
    }
  }

  if (!opts.silent) {
    printTime("[%c] ");
    fprintf(stderr, "  |Search End| ticks=%lu\n", ticks);
  }
  return ticks;
}
#pragma GCC pop_options  // matches push_options above



//
// The entry method for searching
//
syn_search_response SynSearch(
    const Graph* mutationGraph, 
    const btree::btree_set<uint64_t>& kb,
    const btree::btree_set<uint64_t>& auxKB,
    const Tree* input, const SynSearchCosts* costs,
    const bool& assumedInitialTruth, const syn_search_options& opts) {
  syn_search_response response;

  // Debug print parameters
  if (opts.maxTicks >= 0x1 << 25) {
    printTime("[%c] ");
    fprintf(stderr, "ERROR: Max number of ticks is too large: %u\n", opts.maxTicks);
    response.totalTicks = 0;
    return response;
  }
  if (!opts.silent) {
    printTime("[%c] ");
    fprintf(stderr, "|BEGIN SEARCH| fact='%s'\n", toString(*mutationGraph, *input).c_str());
  }
  
  // -- Helpers --
  // Allocate history
  SearchNode* history = (SearchNode*) malloc(opts.maxTicks * sizeof(SearchNode));
  uint64_t historySize = 0;
  // The fringe
  KNHeap<float,SearchNode> fringe(
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity());
  // The database lookup function
  // (the matches found)
  vector<syn_search_path>& matches = response.paths;
  // (the lookup function)
  std::function<bool(uint64_t)> lookupFn = [&kb,&auxKB](const uint64_t& value) -> bool {
    return kb.find(value) != kb.end() || auxKB.find(value) != auxKB.end();
  };
  // (register a node as visited)
  auto registerVisited = [&matches,&lookupFn,&history,&mutationGraph,&input] (const ScoredSearchNode& scoredNode) -> void {
    const SearchNode& node = scoredNode.node;
    if (node.truthState() && lookupFn(node.factHash())) {
      bool unique = true;
      for (auto iter = matches.begin(); iter != matches.end(); ++iter) {
        vector<SearchNode> path = iter->nodeSequence;
        SearchNode endOfPath = path.back();
        if (endOfPath.factHash() == node.factHash()) {
          unique = false;
        }
      }
      if (unique) {
        // Add this path to the result
        // (get the complete path)
        vector<SearchNode> path;
        path.push_back(node);
        if (node.getBackpointer() != 0) {
          SearchNode head = node;
          while (head.getBackpointer() != 0) {
            path.push_back(head);
            head = history[head.getBackpointer()];
          }
        }
        // (reverse the path)
        std::reverse(path.begin(), path.end());
        // (add to the results list)
        printTime("[%c] "); 
        fprintf(stderr, "found premise: %s\n", kbGloss(*mutationGraph, *input, path).c_str());
        matches.push_back(syn_search_path(path, scoredNode.cost));
      }
    }
  };

  // -- Run Search --
  // Enqueue the first element
  // (to the fringe)
  fringe.insert(0.0f, SearchNode(*input, assumedInitialTruth));
  // (to the history)
  history[0] = SearchNode(*input);
  historySize += 1;

  // Run Search
  response.totalTicks = searchLoop(
    // Insert to fringe
    [&fringe](const ScoredSearchNode elem) -> void { 
      fringe.insert(elem.cost, elem.node);
    },
    // Pop from fringe
    [&fringe](ScoredSearchNode* output) -> bool { 
      if (fringe.isEmpty()) { return false; }
      fringe.deleteMin(&(output->cost), &(output->node));
      return true;
    },
    // Register visited
    registerVisited,
    // Other crap
    history, historySize, costs, opts, mutationGraph, *input);

  // Clean up
  free(history);
  
  // Return
  return response;
}
