#ifndef TRIE_H
#define TRIE_H

#include <limits>
#include "btree_map.h"
#include "btree_set.h"
#include <vector>
#include "fnv/fnv.h"

#include <config.h>
#include "Bloom.h"
#include "Map.h"
#include "FactDB.h"
#include "Utils.h"

#define TRIE_NUM_VALID_INSERTIONS_PER_NODE 3

class Trie;

/**
 * A packed edge, encoding the sense and edge type of
 * the edge, but not the other information (1 byte)
 */
#ifdef __GNUG__
typedef struct {
#else
typedef struct alignas(1) {
#endif
  uint8_t sense:5,
          type:3;
#ifdef __GNUG__
} __attribute__ ((__packed__)) packed_edge;
#else
} packed_edge;
#endif

/**
 * A packed object completely encoding a valid insertion.
 */
#ifdef __GNUG__
typedef struct {
#else
typedef struct alignas(6) {
#endif
  uint8_t type:8,       // The type of the edge being inserted
          sense:5;      // The sense of the word being inserted
  bool    endOfList:1;  // A marker for whether this is the last insertion
  word    source;       // The word being inserted ('deleted' in the proof, thus source)
#ifdef __GNUG__
} __attribute__ ((__packed__)) packed_insertion;
#else
} packed_insertion;
#endif

/**
 * A little helper struct to encode all the information a
 * Trie node needs into 4 bytes.
 */
#ifdef __GNUG__
typedef struct {
#else
typedef struct alignas(4) {
#endif
  packed_edge validInsertions[TRIE_NUM_VALID_INSERTIONS_PER_NODE];
  uint8_t numValidInsertions:2,
          isLeaf:1;
#ifdef __GNUG__
} __attribute__ ((__packed__)) trie_data;
#else
} trie_data;
#endif

/** A placeholder for how much space a Trie node takes */
typedef struct {
#if HIGH_MEMORY
  uint64_t placeholder_size[4];
#else
  uint64_t placeholder_size[3];
#endif
} trie_placeholder;

/** A set of flags for the lossy Trie */
#ifdef __GNUG__
typedef struct {
#else
typedef struct alignas(1) {
#endif
  bool isFact:1,
       hasCompletions:1,
       isFull:1;  // note: these should come before magicBit
  uint8_t magicBits:5;
#ifdef __GNUG__
} __attribute__ ((__packed__)) lossy_trie_data;
#else
} lossy_trie_data;
#endif

#define __LOSSY_TRIE_MAGIC_BITS 0x1F

/**
 * A Trie implementation of a fact database.
 * This is primarily useful as it provides a means of determining possible
 * completions of a candidate element so that it could be a member of the database
 * based on the prefix found so far.
 *
 * Each instance of this class is really a "node" in the Trie.
 * A node stores the children of the node -- that is, what completions exist -- as
 * well as a bit of information on the node itself. This is:
 *   1. Whether this path is a complete path. That is, whether this path exists
 *      in the DB as a fact.
 *   2. A bitmask for the possible insertion edge types for the LAST word in the path.
 *      For example, in the Trie *-the->*-blue->*-cat->[*], the node [*] would have
 *      information on which senses of "cat" could have been inserted.
 */
class Trie : public FactDB {
 public:
  Trie() { 
    data.numValidInsertions = 0;
    data.isLeaf = 0;
  }
  virtual ~Trie();

  /**
   * Like the vanilla add() method, but also keep track of the edge
   * type and words sense that would correspond to each element 
   * begin <b>DELETED</b>, which corresponds to an insertion in the
   * reverse search.
   */
  virtual void add(const edge* elements, const uint8_t& length,
                   const Graph* graph);


  const bool contains(const tagged_word* words,
                          const uint8_t& wordLength,
                          const int16_t& mutationIndex,
                          edge* insertions,
                          uint32_t& mutableIndex) const;
  
  /** {@inheritDoc} */
  virtual const bool contains(const tagged_word* words,
                              const uint8_t& wordLength,
                              const int16_t& mutationIndex,
                              edge* insertions) const {
    printf("[1] Should never call this contains() on a Trie directly!\n");
    print_stacktrace();
    std::exit(3);
  }
  /** {@inheritDoc} */
  virtual bool contains(const tagged_word* words, const uint8_t wordLength) const {
    printf("[2] Should never call this contains() on a Trie directly!\n");
    print_stacktrace();
    std::exit(3);
  }
  /** {@inheritDoc} */
  virtual void add(tagged_word* elements, uint8_t length) {
    printf("[3] Should never call this add() on a Trie directly!\n");
    print_stacktrace();
    std::exit(3);
  }

  virtual uint64_t memoryUsage(uint64_t* onFacts,
                               uint64_t* onStructure,
                               uint64_t* onCompletionCaching) const;
  
  /** A simple helper function for whether this node is a leaf node */
  inline bool isLeaf() const { return data.isLeaf != 0; }

 protected:
  // 8 bytes
  /** The core of the Trie. Check if a sequence of [untagged] words is in the Trie. */
  btree::btree_map<word,trie_placeholder> children;

#if HIGH_MEMORY
  // 8 bytes
  /** A utility structure for words from here which would complete a fact */
  btree::btree_map<word,Trie*> completions;
#endif

  // 4 bytes
  /** A compact representation of the data to be stored at this node of the Trie. */
  trie_data data;

  // 4 bytes left over -- the bloody thing rounds to 24 bytes :/
  
  /**
   * A helper function for adding completions to the output
   * completion list.
   */
  inline void addCompletion(const Trie* child, const word& sink,
                            edge* insertion, uint32_t& index) const;

 private:

  /** Register a new edge type to insert */
  inline void registerEdge(edge e) {
    // Don't register more than 4 senses. This seems like a reasonable limitation...
    assert (data.numValidInsertions <= TRIE_NUM_VALID_INSERTIONS_PER_NODE);
    if (data.numValidInsertions == TRIE_NUM_VALID_INSERTIONS_PER_NODE) { return; }
    // Set the new sense
    data.validInsertions[data.numValidInsertions].type = e.type - EDGE_DELS_BEGIN;
    data.validInsertions[data.numValidInsertions].sense = e.source_sense;
    assert (data.validInsertions[data.numValidInsertions].type  < 8);
    assert (data.validInsertions[data.numValidInsertions].sense < 32);
    data.numValidInsertions += 1;
  }

  /** Get the edge types we can insert */
  inline uint8_t getEdges(edge* buffer) const {
    for (uint8_t i = 0; i < data.numValidInsertions; ++i) {
      buffer[i].type         = data.validInsertions[i].type + EDGE_DELS_BEGIN;
      buffer[i].source_sense = data.validInsertions[i].sense;
      buffer[i].cost         = 1.0f;
      buffer[i].sink         = 0;
      buffer[i].sink_sense   = 0;
      assert (buffer[i].type  >= EDGE_DELS_BEGIN);
      assert (buffer[i].type  <  EDGE_DELS_BEGIN + 8);
      assert (buffer[i].source_sense <  32);
    }
    return data.numValidInsertions;
  }
};

/**
 * A special Trie node denoting the root, storing some additional
 * information bits of information that we don't want to replicate
 * for every node.
 */
class TrieRoot : public Trie {
 public:
  virtual void add(const edge* elements, const uint8_t& length,
                   const Graph* graph);
  
  /** {@inheritDoc} Insert a given fact. */
  virtual void add(tagged_word* elements, uint8_t length) {
    edge edges[length];
    for (int i = 0; i < length; ++i) {
      edge e;
      e.source       = elements[i].word;
      e.source_sense = elements[i].sense;
      e.sink         = 0;
      e.sink_sense   = 0;
      e.type         = ADD_OTHER;
      e.cost         = 1.0f;
      edges[i] = e;
    }
    add(edges, length, NULL);
  }
  
  /** {@inheritDoc} */
  virtual const bool contains(const tagged_word* words,
                              const uint8_t& wordLength,
                              const int16_t& mutationIndex,
                              edge* insertions) const;
  
  /** {@inheritDoc} */
  virtual bool contains(const tagged_word* words, const uint8_t wordLength) const {
    edge edges[MAX_COMPLETIONS];
    return contains(words, wordLength, wordLength - 1, edges);
  }
  
  virtual uint64_t memoryUsage(uint64_t* onFacts,
                               uint64_t* onStructure,
                               uint64_t* onCompletionCaching) const;

 private:
  /** A map from grandchild to valid child values. */
  btree::btree_map<word,std::vector<word>> skipGrams;
};


class LossyTrie : public FactDB {
 public:
  /**
   * Create a new LossyTrie.
   * Note that the 
   */
  LossyTrie(const HashIntMap& countsBecomesPointers);

  /**
   * The destructor. Cleans up the completions and completion data arrays.
   */
  ~LossyTrie();

  
  /** {@inheritDoc} */
  virtual const bool contains(const tagged_word* words, 
                              const uint8_t& wordLength,
                              const int16_t& mutationIndex,
                              edge* insertions) const;
  
  /**
   * Add the given fact completion to the database.
   */
  bool addCompletion(const uint32_t* fact, 
                     const uint32_t& factLength, 
                     const word& source,
                     const uint8_t& sourceSense,
                     const uint8_t& edgeType);
  
  /**
   * Add a given complete fact to the database.
   */
  void addFact(const uint32_t* fact, 
               const uint32_t& factLength);
  
  /**
   * Register w0 as a potential prefix insertion for w1.
   */
  void addBeginInsertion(const word& w0, 
                         const uint8_t w0Sense,
                         const uint8_t w0Type,
                         const word& w1);
  
  /** A simple wrapper around contains() to be more user-friendly */
  inline const bool contains(const tagged_word* words, 
                             const uint8_t& wordLength) {
    edge insertions[MAX_COMPLETIONS];
    return contains(words, wordLength, 0, insertions);
  }
  
  /** A simple wrapper around contains() to be more user-friendly */
  inline const bool contains(const word* words, 
                             const uint8_t& wordLength) {
    tagged_word taggedWord[wordLength];
    for (int i = 0; i < wordLength; ++i) {
      taggedWord[i] = getTaggedWord(words[i], 0, MONOTONE_UP);
    }
    return contains(taggedWord, wordLength);
  }
  
  /** A simple wrapper around contains() to be more user-friendly */
  inline const bool contains(const word* words, 
                             const uint8_t& wordLength,
                             const uint16_t& mutationIndex,
                             edge* insertions) {
    tagged_word taggedWord[wordLength];
    for (int i = 0; i < wordLength; ++i) {
      taggedWord[i] = getTaggedWord(words[i], 0, MONOTONE_UP);
    }
    return contains(taggedWord, wordLength, mutationIndex, insertions);
  }

 private:
  /**
   * The map keyed on the fact's hash, with values corresponding
   * to the completion data for that fact or partial fact.
   */
  HashIntMap completions;

  /**
   * The blob of bytes used to store the completion data.
   * There's no length, because the length is irrelevant anyways.
   * This is just a blob of data.
   * 
   * Format:
   *   [<1 byte>]
   *              v pointer points here
   *   [?????CBA][packed_insertion][packed_insertion]...
   *   A = isCompleteFact bit
   *   B = hasCompletions bit
   *   C = isFull bit
   */
  uint8_t* completionData;

  /**
   * A map from the second word in a fact to the first. This is used
   * to populate completions at the beginning of a fact.
   */
  btree::btree_map<word,std::vector<packed_insertion>> beginInsertions;

  /**
   * Get the metadata for a particular cell.
   */
  inline lossy_trie_data& metadata(const uint64_t& pointer) {
    return *((lossy_trie_data*) &(completionData[pointer - 1]));
  }
   
  /**
   * Get the metadata for a particular cell.
   */
  inline const lossy_trie_data& metadata(const uint64_t& pointer) const {
    return *((lossy_trie_data*) &(completionData[pointer - 1]));
  }
  
  /**
   * Verify that this is indeed a valid cell to insert into.
   */
  inline bool isValidCell(const packed_insertion& cell) const {
    return (*((lossy_trie_data*) &cell)).magicBits != __LOSSY_TRIE_MAGIC_BITS;
  }
  

};


FactDB* ReadOldFactTrie(const uint64_t maxFactsToRead, const Graph* graph);

inline FactDB* ReadOldFactTrie(const Graph* graph) { return ReadOldFactTrie(std::numeric_limits<uint64_t>::max(), graph); }

/**
 * Read in the known database of facts as a Trie.
 * @param maxFactsToRead Cap the number of facts read to this amount, 
 *        ordered by the fact's weight in the database
 */
FactDB* ReadFactTrie(const uint64_t& maxFactsToRead, const Graph* graph);

/** Read all facts in the database; @see ReadFactTrie(uint64_t) */
inline FactDB* ReadFactTrie(const Graph* graph) {
  return ReadFactTrie(std::numeric_limits<uint64_t>::max(), graph);
}

#endif
