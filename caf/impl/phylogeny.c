/*
 * phlogeny.c
 *
 *  Created on: Jun 2, 2014
 *      Author: benedictpaten
 */

#include "sonLib.h"
#include "cactus.h"
#include "stPinchGraphs.h"
#include "stCactusGraphs.h"
#include "stPinchPhylogeny.h"
#include "stCaf.h"
#include "stCafPhylogeny.h"

// Doesn't have to be exported since nothing outside this file should
// really care about split branches.
typedef struct {
    stTree *child; // Child of the branch.
    stPinchBlock *block; // Block the tree refers to (can and should
                         // refer to more than the child subtree).
    double support; // Bootstrap support for this branch.
} stCaf_SplitBranch;

stHash *stCaf_getThreadStrings(Flower *flower, stPinchThreadSet *threadSet) {
    stHash *threadStrings = stHash_construct2(NULL, free);
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Sequence *sequence = cap_getSequence(cap);
        assert(sequence != NULL);
        assert(stPinchThread_getLength(thread)-2 >= 0);
        char *string = sequence_getString(sequence, stPinchThread_getStart(thread)+1, stPinchThread_getLength(thread)-2, 1); //Gets the sequence excluding the empty positions representing the caps.
        char *paddedString = stString_print("N%sN", string); //Add in positions to represent the flanking bases
        stHash_insert(threadStrings, thread, paddedString);
        free(string);
    }
    return threadStrings;
}

stSet *stCaf_getOutgroupThreads(Flower *flower, stPinchThreadSet *threadSet) {
    stSet *outgroupThreads = stSet_construct();
    stPinchThreadSetIt threadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    while ((thread = stPinchThreadSetIt_getNext(&threadIt)) != NULL) {
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(event_isOutgroup(event)) {
            stSet_insert(outgroupThreads, thread);
        }
    }
    return outgroupThreads;
}


/*
 * Gets a list of the segments in the block that are part of outgroup threads.
 * The list contains stIntTuples, each of length 1, representing the index of a particular segment in
 * the block.
 */
static stList *getOutgroupThreads(stPinchBlock *block, stSet *outgroupThreads) {
    stList *outgroups = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        if(stSet_search(outgroupThreads, stPinchSegment_getThread(segment)) != NULL) {
            stList_append(outgroups, stIntTuple_construct1(i));
        }
        i++;
    }
    assert(i == stPinchBlock_getDegree(block));
    return outgroups;
}

/*
 * Splits the block using the given partition into a set of new blocks.
 */
static void splitBlock(stPinchBlock *block, stList *partitions, bool allowSingleDegreeBlocks) {
    assert(stList_length(partitions) > 0);
    if(stList_length(partitions) == 1) {
        return; //Nothing to do.
    }
    //Build a mapping of indices of the segments in the block to the segments
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stPinchSegment **segments = st_calloc(blockDegree, sizeof(stPinchSegment *));
    bool *orientations = st_calloc(blockDegree, sizeof(bool));
    stPinchBlockIt segmentIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i=0;
    while((segment = stPinchBlockIt_getNext(&segmentIt)) != NULL) {
        segments[i] = segment;
        assert(segments[i] != NULL);
        orientations[i++] = stPinchSegment_getBlockOrientation(segment);
    }
    assert(i == stPinchBlock_getDegree(block));
    //Destruct old block, as we build new blocks now.
    stPinchBlock_destruct(block);
    //Now build the new blocks.
    for(int64_t i=0; i<stList_length(partitions); i++) {
        stList *partition = stList_get(partitions, i);
        assert(stList_length(partition) > 0);
        int64_t k = stIntTuple_get(stList_get(partition, 0), 0);
        assert(segments[k] != NULL);
        assert(stPinchSegment_getBlock(segments[k]) == NULL);

        if (!allowSingleDegreeBlocks && stList_length(partition) == 1) {
            // We need to avoid assigning this single-degree block
            segments[k] = NULL;
            continue;
        }

        block = stPinchBlock_construct3(segments[k], orientations[k]);
        assert(stPinchSegment_getBlock(segments[k]) == block);
        assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
        segments[k] = NULL; //Defensive, and used for debugging.
        for(int64_t j=1; j<stList_length(partition); j++) {
            k = stIntTuple_get(stList_get(partition, j), 0);
            assert(segments[k] != NULL);
            assert(stPinchSegment_getBlock(segments[k]) == NULL);
            stPinchBlock_pinch2(block, segments[k], orientations[k]);
            assert(stPinchSegment_getBlock(segments[k]) == block);
            assert(stPinchSegment_getBlockOrientation(segments[k]) == orientations[k]);
            segments[k] = NULL; //Defensive, and used for debugging.
        }
    }
    //Now check the segments have all been used - this is just debugging.
    for(int64_t i=0; i<blockDegree; i++) {
        assert(segments[i] == NULL);
    }
    //Cleanup
    free(segments);
    free(orientations);
}

/*
 * For logging purposes gets the total number of similarities and differences in the matrix.
 */
static void getTotalSimilarityAndDifferenceCounts(stMatrix *matrix, double *similarities, double *differences) {
    *similarities = 0.0;
    *differences = 0.0;
    for(int64_t i=0; i<stMatrix_n(matrix); i++) {
        for(int64_t j=i+1; j<stMatrix_n(matrix); j++) {
            *similarities += *stMatrix_getCell(matrix, i, j);
            *differences += *stMatrix_getCell(matrix, j, i);
        }
    }
}

// If the tree contains any zero branch lengths (i.e. there were
// negative branch lengths when neighbor-joining), fudge the branch
// lengths so that both children have non-zero branch lengths, but are
// still the same distance apart. When both children have zero branch
// lengths, give them both a small branch length. This makes
// likelihood methods usable.

// Only works on binary trees.
static void fudgeZeroBranchLengths(stTree *tree, double fudgeFactor, double smallNonZeroBranchLength) {
    assert(stTree_getChildNumber(tree) == 2 || stTree_getChildNumber(tree) == 0);
    assert(fudgeFactor < 1.0 && fudgeFactor > 0.0);
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        stTree *child = stTree_getChild(tree, i);
        fudgeZeroBranchLengths(child, fudgeFactor, smallNonZeroBranchLength);
        if (stTree_getBranchLength(child) == 0.0) {
            stTree *otherChild = stTree_getChild(tree, !i);
            if (stTree_getBranchLength(otherChild) == 0.0) {
                // Both children have zero branch lengths, set them
                // both to some very small but non-zero branch length
                // so that probabilistic methods can actually work
                stTree_setBranchLength(child, smallNonZeroBranchLength);
                stTree_setBranchLength(otherChild, smallNonZeroBranchLength);
            } else {
                // Keep the distance between the children equal, but
                // move it by fudgeFactor so that no branch length is
                // zero.
                stTree_setBranchLength(child, fudgeFactor * stTree_getBranchLength(otherChild));
                stTree_setBranchLength(otherChild, (1 - fudgeFactor) * stTree_getBranchLength(otherChild));
            }
        }
    }
}

/*
 * Get a gene node->species node mapping from a gene tree, a species
 * tree, and the pinch block.
 */

static stHash *getLeafToSpecies(stTree *geneTree, stTree *speciesTree,
                                stPinchBlock *block, Flower *flower) {
    stHash *leafToSpecies = stHash_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        char *eventNameString = stString_print("%" PRIi64, event_getName(event));
        stTree *species = stTree_findChild(speciesTree, eventNameString);
        free(eventNameString);
        assert(species != NULL);
        stTree *gene = stPhylogeny_getLeafByIndex(geneTree, i);
        assert(gene != NULL);
        stHash_insert(leafToSpecies, gene, species);
        i++;
    }
    return leafToSpecies;
}

/*
 * Get a mapping from matrix index -> join cost index for use in
 * neighbor-joining guided by a species tree.
 */

static stHash *getMatrixIndexToJoinCostIndex(stPinchBlock *block, Flower *flower, stTree *speciesTree, stHash *speciesToJoinCostIndex) {
    stHash *matrixIndexToJoinCostIndex = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey, (int (*)(const void *, const void *)) stIntTuple_equalsFn, (void (*)(void *)) stIntTuple_destruct, (void (*)(void *)) stIntTuple_destruct);
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    int64_t i = 0; // Current segment index in block.
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        Event *event = cap_getEvent(cap);
        char *eventNameString = stString_print("%" PRIi64, event_getName(event));
        stTree *species = stTree_findChild(speciesTree, eventNameString);
        free(eventNameString);
        assert(species != NULL);

        stIntTuple *joinCostIndex = stHash_search(speciesToJoinCostIndex, species);
        assert(joinCostIndex != NULL);

        stIntTuple *matrixIndex = stIntTuple_construct1(i);
        stHash_insert(matrixIndexToJoinCostIndex, matrixIndex,
                      // Copy the join cost index so it has the same
                      // lifetime as the hash
                      stIntTuple_construct1(stIntTuple_get(joinCostIndex, 0)));
        i++;
    }
    return matrixIndexToJoinCostIndex;
}

static stTree *eventTreeToStTree_R(Event *event) {
    stTree *ret = stTree_construct();
    stTree_setLabel(ret, stString_print("%" PRIi64, event_getName(event)));
    stTree_setBranchLength(ret, event_getBranchLength(event));
    for(int64_t i = 0; i < event_getChildNumber(event); i++) {
        Event *child = event_getChild(event, i);
        stTree *childStTree = eventTreeToStTree_R(child);
        stTree_setParent(childStTree, ret);
    }
    return ret;
}

// Get species tree from event tree (labeled by the event Names),
// which requires ignoring the root event.
static stTree *eventTreeToStTree(EventTree *eventTree) {
    Event *rootEvent = eventTree_getRootEvent(eventTree);
    // Need to skip the root event, since it is added onto the real
    // species tree.
    assert(event_getChildNumber(rootEvent) == 1);
    Event *speciesRoot = event_getChild(rootEvent, 0);
    return eventTreeToStTree_R(speciesRoot);
}

static double scoreTree(stTree *tree, enum stCaf_ScoringMethod scoringMethod, stTree *speciesStTree, stPinchBlock *block, Flower *flower, stList *featureColumns) {
    double ret = 0.0;
    if (scoringMethod == RECON_COST) {
        stHash *leafToSpecies = getLeafToSpecies(tree,
                                                 speciesStTree,
                                                 block, flower);
        int64_t dups, losses;
        stPhylogeny_reconciliationCostBinary(tree, speciesStTree,
                                             leafToSpecies, &dups,
                                             &losses);
        ret = -dups - losses;

        stHash_destruct(leafToSpecies);
    } else if (scoringMethod == NUCLEOTIDE_LIKELIHOOD) {
        ret = stPinchPhylogeny_likelihood(tree, featureColumns);
    } else if (scoringMethod == RECON_LIKELIHOOD) {
        // copy tree before use -- we are modifying the client-data
        // field. Not necessary if we end up adding
        // stReconciliationInfo before this method
        stTree *tmp = stTree_clone(tree);
        stHash *leafToSpecies = getLeafToSpecies(tmp,
                                                 speciesStTree,
                                                 block, flower);
        stPhylogeny_reconcileBinary(tmp, speciesStTree, leafToSpecies, false);
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tmp, speciesStTree, 1.0);
        stReconciliationInfo_destructOnTree(tmp);
        stTree_destruct(tmp);
    } else if (scoringMethod == COMBINED_LIKELIHOOD) {
        // copy tree before use -- we are modifying the client-data
        // field. Not necessary if we end up adding
        // stReconciliationInfo before this method
        stTree *tmp = stTree_clone(tree);
        stHash *leafToSpecies = getLeafToSpecies(tmp,
                                                 speciesStTree,
                                                 block, flower);
        stPhylogeny_reconcileBinary(tmp, speciesStTree, leafToSpecies, false);
        // FIXME: hardcoding dup-rate parameter for now
        ret = stPinchPhylogeny_reconciliationLikelihood(tmp, speciesStTree, 1.0);
        ret += stPinchPhylogeny_likelihood(tree, featureColumns);
        stReconciliationInfo_destructOnTree(tmp);
        stTree_destruct(tmp);        
    }
    return ret;
}

// Build a tree from a set of feature columns and root it according to
// the rooting method.
static stTree *buildTree(stList *featureColumns,
                         enum stCaf_TreeBuildingMethod treeBuildingMethod,
                         enum stCaf_RootingMethod rootingMethod,
                         double breakPointScalingFactor,
                         bool bootstrap,
                         stList *outgroups, stPinchBlock *block,
                         Flower *flower, stTree *speciesStTree,
                         stMatrix *joinCosts,
                         stHash *speciesToJoinCostIndex) {
    // Make substitution matrix
    stMatrix *substitutionMatrix = stPinchPhylogeny_getMatrixFromSubstitutions(featureColumns, block, NULL, bootstrap);
    assert(stMatrix_n(substitutionMatrix) == stPinchBlock_getDegree(block));
    assert(stMatrix_m(substitutionMatrix) == stPinchBlock_getDegree(block));
    //Make breakpoint matrix
    stMatrix *breakpointMatrix = stPinchPhylogeny_getMatrixFromBreakpoints(featureColumns, block, NULL, bootstrap);
    
    //Combine the matrices into distance matrices
    stMatrix_scale(breakpointMatrix, breakPointScalingFactor, 0.0);
    stMatrix *combinedMatrix = stMatrix_add(substitutionMatrix, breakpointMatrix);
    stMatrix *distanceMatrix = stPinchPhylogeny_getSymmetricDistanceMatrix(combinedMatrix);

    stTree *tree = NULL;
    if(rootingMethod == OUTGROUP_BRANCH) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, outgroups);
        } else {
            assert(treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-outgroup-branch rooting not supported with guided neighbor joining");
        }
    } else if(rootingMethod == LONGEST_BRANCH) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            assert(treeBuildingMethod == GUIDED_NEIGHBOR_JOINING);
            st_errAbort("Longest-branch rooting not supported with guided neighbor joining");
        }
    } else if(rootingMethod == BEST_RECON) {
        if (treeBuildingMethod == NEIGHBOR_JOINING) {
            tree = stPhylogeny_neighborJoin(distanceMatrix, NULL);
        } else {
            // FIXME: Could move this out of the function as
            // well. It's the same for each tree generated for the
            // block.
            stHash *matrixIndexToJoinCostIndex = getMatrixIndexToJoinCostIndex(block, flower, speciesStTree,
                                                                               speciesToJoinCostIndex);
            tree = stPhylogeny_guidedNeighborJoining(combinedMatrix, joinCosts, matrixIndexToJoinCostIndex, speciesToJoinCostIndex, speciesStTree);
            stHash_destruct(matrixIndexToJoinCostIndex);
        }
        stHash *leafToSpecies = getLeafToSpecies(tree,
                                                 speciesStTree,
                                                 block, flower);
        stTree *newTree = stPhylogeny_rootAndReconcileBinary(tree, speciesStTree, leafToSpecies);
        stPhylogeny_addStPhylogenyInfo(newTree);

        stPhylogenyInfo_destructOnTree(tree);
        stTree_destruct(tree);
        stHash_destruct(leafToSpecies);
        tree = newTree;
    }

    // Needed for likelihood methods not to have 0/100% probabilities
    // overly often (normally, almost every other leaf has a branch
    // length of 0)
    fudgeZeroBranchLengths(tree, 0.02, 0.0001);

    return tree;
}

// Check if the block's phylogeny is simple:
// - the block has only one event, or
// - the block has < 3 segments, or
// - the block does not contain any segments that are part of an
//   outgroup thread.
static bool hasSimplePhylogeny(stPinchBlock *block,
                               stSet *outgroupThreads,
                               Flower *flower) {
    if(stPinchBlock_getDegree(block) <= 2) {
        return true;
    }
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    bool foundOutgroup = 0, found2Events = 0;
    Event *currentEvent = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        if(stSet_search(outgroupThreads, thread) != NULL) {
            foundOutgroup = 1;
        }
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(currentEvent == NULL) {
            currentEvent = event;
        } else if(currentEvent != event) {
            found2Events = 1;
        }
    }
    return !(foundOutgroup && found2Events);
}

// Check if the block contains as many species as segments
static bool isSingleCopyBlock(stPinchBlock *block, Flower *flower) {
    stSet *seenEvents = stSet_construct();
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        assert(cap != NULL);
        Event *event = cap_getEvent(cap);
        if(stSet_search(seenEvents, event) != NULL) {
            stSet_destruct(seenEvents);
            return false;
        }
        stSet_insert(seenEvents, event);
    }
    stSet_destruct(seenEvents);
    return true;
}

// relabel a tree so it's useful for debug output
static void relabelMatrixIndexedTree(stTree *tree, stHash *matrixIndexToName) {
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        relabelMatrixIndexedTree(stTree_getChild(tree, i), matrixIndexToName);
    }
    if (stTree_getChildNumber(tree) == 0) {
        stPhylogenyInfo *info = stTree_getClientData(tree);
        assert(info != NULL);
        assert(info->matrixIndex != -1);
        stIntTuple *query = stIntTuple_construct1(info->matrixIndex);
        char *header = stHash_search(matrixIndexToName, query);
        assert(header != NULL);
        stTree_setLabel(tree, stString_copy(header));
        stIntTuple_destruct(query);
    }
}

// Print the debug info for blocks that are normally not printed
// (those that cannot be partitioned). The debug info contains just
// the "partition", i.e. the sequences and positions within the block
// in a list of lists.
static void printSimpleBlockDebugInfo(Flower *flower, stPinchBlock *block, FILE *outFile) {
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    fprintf(outFile, "[[");
    int64_t i = 0;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
        Event *event = cap_getEvent(cap);
        const char *eventHeader = event_getHeader(event);
        char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
        
        if (i != 0) {
            fprintf(outFile, ",");
        }
        fprintf(outFile, "\"%s\"", segmentHeader);
        free(segmentHeader);
        i++;
    }
    assert(i == stPinchBlock_getDegree(block));
    fprintf(outFile, "]]\n");
}

// print debug info: "tree\tpartition\n" to the file
static void printTreeBuildingDebugInfo(Flower *flower, stPinchBlock *block, stTree *bestTree, stList *partition, stMatrix *matrix, double score, FILE *outFile) {
    // First get a map from matrix indices to names
    // The format we will use for leaf names is "genome.seq|posStart-posEnd"
    int64_t blockDegree = stPinchBlock_getDegree(block);
    stHash *matrixIndexToName = stHash_construct3((uint64_t (*)(const void *)) stIntTuple_hashKey,
                                                  (int (*)(const void *, const void *)) stIntTuple_equalsFn,
                                                  (void (*)(void *)) stIntTuple_destruct, free);
    int64_t i = 0;
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        stPinchThread *thread = stPinchSegment_getThread(segment);
        Cap *cap = flower_getCap(flower, stPinchThread_getName(thread));
        const char *seqHeader = sequence_getHeader(cap_getSequence(cap));
        Event *event = cap_getEvent(cap);
        const char *eventHeader = event_getHeader(event);
        char *segmentHeader = stString_print("%s.%s|%" PRIi64 "-%" PRIi64, eventHeader, seqHeader, stPinchSegment_getStart(segment), stPinchSegment_getStart(segment) + stPinchSegment_getLength(segment));
        stHash_insert(matrixIndexToName, stIntTuple_construct1(i), segmentHeader);
        i++;
    }
    assert(i == blockDegree);

    // Relabel (our copy of) the best tree.
    stTree *treeCopy = stTree_clone(bestTree);
    relabelMatrixIndexedTree(treeCopy, matrixIndexToName);
    char *newick = stTree_getNewickTreeString(treeCopy);

    fprintf(outFile, "%s\t", newick);

    // Print the partition
    fprintf(outFile, "[");
    for (i = 0; i < stList_length(partition); i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        stList *subList = stList_get(partition, i);
        fprintf(outFile, "[");
        for (int64_t j = 0; j < stList_length(subList); j++) {
            if (j != 0) {
                fprintf(outFile, ",");
            }
            stIntTuple *index = stList_get(subList, j);
            assert(stIntTuple_get(index, 0) < blockDegree);
            char *header = stHash_search(matrixIndexToName, index);
            assert(header != NULL);
            fprintf(outFile, "\"%s\"", header);
        }
        fprintf(outFile, "]");
    }
    fprintf(outFile, "]\t");

    // print the matrix
    fprintf(outFile, "[");
    for (i = 0; i < stMatrix_m(matrix); i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        fprintf(outFile, "[");
        for (int64_t j = 0; j < stMatrix_n(matrix); j++) {
            if (j != 0) {
                fprintf(outFile, ",");
            }
            fprintf(outFile, "%lf", *stMatrix_getCell(matrix, i, j));
        }
        fprintf(outFile, "]");
    }
    fprintf(outFile, "]\t");

    // print the sequences corresponding to the matrix indices
    fprintf(outFile, "[");
    for (int64_t i = 0; i < blockDegree; i++) {
        if (i != 0) {
            fprintf(outFile, ",");
        }
        stIntTuple *query = stIntTuple_construct1(i);
        char *header = stHash_search(matrixIndexToName, query);
        assert(header != NULL);
        fprintf(outFile, "\"%s\"", header);
        stIntTuple_destruct(query);
    }
    fprintf(outFile, "]\t");

    // print the score
    fprintf(outFile, "%lf\n", score);
    stTree_destruct(treeCopy);
    free(newick);
    stHash_destruct(matrixIndexToName);
}

static int64_t countBasesBetweenSingleDegreeBlocks(stPinchThreadSet *threadSet) {
    stPinchThreadSetIt pinchThreadIt = stPinchThreadSet_getIt(threadSet);
    stPinchThread *thread;
    int64_t numBases = 0;
    int64_t numBasesInSingleCopyBlocks = 0;
    while ((thread = stPinchThreadSetIt_getNext(&pinchThreadIt)) != NULL) {
        stPinchSegment *segment = stPinchThread_getFirst(thread);
        if (segment == NULL) {
            // No segments on this thread.
            continue;
        }
        bool wasInSingleDegreeBlock = stPinchBlock_getDegree(stPinchSegment_getBlock(segment)) == 1;
        stPinchSegment *oldSegment = NULL;
        while ((segment = stPinchSegment_get3Prime(segment)) != NULL) {
            stPinchBlock *block = stPinchSegment_getBlock(segment);
            if (block == NULL) {
                // Segment without a block.
                continue;
            }
            bool isInSingleDegreeBlock = stPinchBlock_getDegree(block) == 1;
            if (isInSingleDegreeBlock) {
                numBasesInSingleCopyBlocks += stPinchBlock_getLength(block);
            }
            int64_t numBasesBetweenSegments = 0;
            if (oldSegment != NULL) {
                numBasesBetweenSegments = stPinchSegment_getStart(segment) - (stPinchSegment_getStart(oldSegment) + stPinchSegment_getLength(oldSegment));
            }
            assert(numBasesBetweenSegments >= 0); // could be 0 if the
                                                  // blocks aren't
                                                  // identical
            if (wasInSingleDegreeBlock && isInSingleDegreeBlock) {
                numBases += numBasesBetweenSegments;
            }
            oldSegment = segment;
            wasInSingleDegreeBlock = isInSingleDegreeBlock;
        }
    }
    // FIXME: tmp
    fprintf(stdout, "There were %" PRIi64 " bases in single degree blocks.\n", numBasesInSingleCopyBlocks);
    return numBases;
}

// Compare two bootstrap scores of split branches. Use the pointer
// value of the branches as a tiebreaker since we are using a sorted
// set and don't want to merge together all branches with the same
// support value.
int compareSplitBranches(stCaf_SplitBranch *branch1,
                         stCaf_SplitBranch *branch2) {
    if (branch1->support > branch2->support) {
        return 2;
    } else if (branch1->support == branch2->support) {
        if (branch1->child == branch2->child) {
            return 0;
        } else if  (branch1->child > branch2->child) {
            return 1;
        } else {
            return -1;
        }
    } else {
        return -2;
    }
}

stCaf_SplitBranch *stCaf_SplitBranch_construct(stTree *child,
                                               stPinchBlock *block,
                                               double support) {
    stCaf_SplitBranch *ret = calloc(1, sizeof(stCaf_SplitBranch));
    ret->support = support;
    ret->child = child;
    ret->block = block;
    return ret;
}

// Find new split branches from the block and add them to the sorted set.
// speciesToSplitOn is just the species that are on the path from the
// reference node to the root.
void findSplitBranches(stPinchBlock *block, stTree *tree,
                       stSortedSet *splitBranches,
                       stSet *speciesToSplitOn) {
    stTree *parent = stTree_getParent(tree);
    if (parent != NULL) {
        stPhylogenyInfo *parentInfo = stTree_getClientData(parent);
        assert(parentInfo != NULL);
        stReconciliationInfo *parentReconInfo = parentInfo->recon;
        assert(parentReconInfo != NULL);
        if (stSet_search(speciesToSplitOn, parentReconInfo->species)) {
            // Found a split branch.
            stPhylogenyInfo *info = stTree_getClientData(tree);
            stCaf_SplitBranch *splitBranch = stCaf_SplitBranch_construct(tree, block, info->bootstrapSupport);
            stSortedSet_insert(splitBranches, splitBranch);
        } else {
            // Since the reconciliation must follow the order of the
            // species tree, any child of this node cannot be
            // reconciled to the speciesToSplitOnSet.
            return;
        }
    }

    // Recurse down the tree as far as makes sense.
    for (int64_t i = 0; i < stTree_getChildNumber(tree); i++) {
        findSplitBranches(block, stTree_getChild(tree, i), splitBranches,
                          speciesToSplitOn);
    }
}

// FIXME: Not quite right at the moment. Sometimes nodes are added to
// binarize the tree that aren't labeled as outgroups.
// Return value indicates whether there are outgroups below the current node.
// 0 = ingroups below
// 1 = outgroups below
// 2 = both below
static int getSpeciesToSplitOn(stTree *speciesTree, EventTree *eventTree,
                               stSet *speciesToSplitOn) {
    // We want to split on any node that contains both outgroups and
    // ingroups below it.
    bool outgroupsBelow = false;
    bool ingroupsBelow = false;
    for (int64_t i = 0; i < stTree_getChildNumber(speciesTree); i++) {
        int childStatus = getSpeciesToSplitOn(stTree_getChild(speciesTree, i),
                                              eventTree,
                                              speciesToSplitOn);
        if (childStatus == 0) {
            ingroupsBelow = true;
        } else if (childStatus == 1) {
            outgroupsBelow = true;
        } else {
            ingroupsBelow = true;
            outgroupsBelow = true;
        }
    }

    if (outgroupsBelow && ingroupsBelow) {
        stSet_insert(speciesToSplitOn, speciesTree);
    }

    Name eventName = -1;
    int k = sscanf(stTree_getLabel(speciesTree), "%" PRIi64, &eventName);
    (void) k;
    assert(k == 1);
    Event *event = eventTree_getEvent(eventTree, eventName);

    if (event_isOutgroup(event)) {
        // If we didn't add extra nodes to binarize this tree this assert would have to be true.
        // assert(ingroupsBelow == false);
        return 1;
    } else {
        if (ingroupsBelow && outgroupsBelow) {
            return 2;
        } else if (outgroupsBelow) {
            return 1;
        } else {
            return 0;
        }
    }
}

// O(n).
static stPinchSegment *getSegmentByBlockIndex(stPinchBlock *block,
                                              int64_t index) {
    assert(index >= 0);
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment = NULL;
    int64_t i = 0;
    while((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        if (i == index) {
            return segment;
        }
        i++;
    }
    return NULL;
}

// Add any stPinchBlocks close enough to the given block to be
// affected by its breakpoint information to the given set.
static void addContextualBlocksToSet(stPinchBlock *block,
                                     int64_t maxBaseDistance,
                                     int64_t maxBlockDistance,
                                     bool ignoreUnalignedBases,
                                     stSet *contextualBlocks) {
    stPinchBlockIt blockIt = stPinchBlock_getSegmentIterator(block);
    stPinchSegment *segment;
    while ((segment = stPinchBlockIt_getNext(&blockIt)) != NULL) {
        // Go toward the 5' end of this thread adding blocks, until we
        // reach the end or maxBaseDistance or maxBlockDistance.
        stPinchSegment *curSegment = stPinchSegment_get5Prime(segment);
        int64_t curBlockDistance = 0;
        int64_t curBaseDistance = stPinchSegment_getLength(segment) / 2;
        while ((curSegment != NULL) && (curBlockDistance < maxBlockDistance)
               && (curBaseDistance < maxBaseDistance)) {
            stPinchBlock *curBlock = stPinchSegment_getBlock(curSegment);
            if (curBlock != NULL) {
                stSet_insert(contextualBlocks, curBlock);
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            } else if (!ignoreUnalignedBases) {
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            }
            curSegment = stPinchSegment_get5Prime(curSegment);
        }

        // Do the same for the 3' side.
        curSegment = stPinchSegment_get3Prime(segment);
        curBlockDistance = 0;
        curBaseDistance = stPinchSegment_getLength(segment) / 2;
        while ((curSegment != NULL) && (curBlockDistance < maxBlockDistance)
               && (curBaseDistance < maxBaseDistance)) {
            stPinchBlock *curBlock = stPinchSegment_getBlock(curSegment);
            if (curBlock != NULL) {
                stSet_insert(contextualBlocks, curBlock);
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            } else if (!ignoreUnalignedBases) {
                curBaseDistance += stPinchSegment_getLength(segment);
                curBlockDistance++;
            }
            curSegment = stPinchSegment_get3Prime(curSegment);
        }
    }
}

static void removeOldSplitBranches(stPinchBlock *block, stTree *tree,
                                   stSet *speciesToSplitOn,
                                   stSortedSet *splitBranches) {
    if (block == NULL || tree == NULL) {
        return;
    }
    stSortedSet *splitBranchesToDelete = stSortedSet_construct3((int (*)(const void *, const void *)) compareSplitBranches, free);
    findSplitBranches(block, tree, splitBranchesToDelete,
                      speciesToSplitOn);
    // Could set splitBranches = splitBranches \ splitBranchesToDelete.
    // But this is probably faster.
    stSortedSetIterator *splitBranchToDeleteIt = stSortedSet_getIterator(splitBranchesToDelete);
    stCaf_SplitBranch *splitBranchToDelete;
    while ((splitBranchToDelete = stSortedSet_getNext(splitBranchToDeleteIt)) != NULL) {
        stSortedSet_remove(splitBranches, splitBranchToDelete);
    }
    stSortedSet_destructIterator(splitBranchToDeleteIt);
    stSortedSet_destruct(splitBranchesToDelete);
}

static void buildTreeForBlock(stPinchBlock *block, stHash *threadStrings, stSet *outgroupThreads, Flower *flower, int64_t maxBaseDistance, int64_t maxBlockDistance, int64_t numTrees, enum stCaf_TreeBuildingMethod treeBuildingMethod, enum stCaf_RootingMethod rootingMethod, enum stCaf_ScoringMethod scoringMethod, double breakPointScalingFactor, bool skipSingleCopyBlocks, bool allowSingleDegreeBlocks, bool ignoreUnalignedBases, bool onlyIncludeCompleteFeatureBlocks, double costPerDupPerBase, double costPerLossPerBase, stMatrix *joinCosts, stHash *speciesToJoinCostIndex, stTree *speciesStTree, stHash *blocksToTrees) {
    if (!hasSimplePhylogeny(block, outgroupThreads, flower)) { //No point trying to build a phylogeny for certain blocks.
        if (isSingleCopyBlock(block, flower) && skipSingleCopyBlocks) {
            return;
        }

        // Get the feature blocks
        stList *featureBlocks = stFeatureBlock_getContextualFeatureBlocks(block, maxBaseDistance, maxBlockDistance,
                                                                          ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, threadStrings);

        // Make feature columns
        stList *featureColumns = stFeatureColumn_getFeatureColumns(featureBlocks, block);

        //Get the outgroup threads
        stList *outgroups = getOutgroupThreads(block, outgroupThreads);

        //Build the canonical tree.
        stTree *blockTree = buildTree(featureColumns, GUIDED_NEIGHBOR_JOINING, rootingMethod,
                                      breakPointScalingFactor,
                                      0, outgroups, block, flower,
                                      speciesStTree, joinCosts, speciesToJoinCostIndex);

        // Sample the rest of the trees.
        stList *trees = stList_construct();
        stList_append(trees, blockTree);
        for (int64_t i = 0; i < numTrees - 1; i++) {
            stTree *tree = buildTree(featureColumns, GUIDED_NEIGHBOR_JOINING, rootingMethod,
                                     breakPointScalingFactor,
                                     1, outgroups, block, flower,
                                     speciesStTree, joinCosts, speciesToJoinCostIndex);
            stList_append(trees, tree);
        }

        // Get the best-scoring tree.
        double maxScore = -INFINITY;
        stTree *bestTree = NULL;
        for (int64_t i = 0; i < stList_length(trees); i++) {
            stTree *tree = stList_get(trees, i);
            double score = scoreTree(tree, scoringMethod,
                                     speciesStTree, block, flower,
                                     featureColumns);
            if (score > maxScore) {
                maxScore = score;
                bestTree = tree;
            }
        }

        if(bestTree == NULL) {
            // Can happen if/when the nucleotide likelihood score
            // is used and a block is all N's. Just use the
            // canonical NJ tree in that case.
            bestTree = blockTree;
        }

        assert(bestTree != NULL);

        // Update the bootstrap support for each branch.
        bestTree = stPhylogeny_scoreFromBootstraps(bestTree, trees);
        // Create reconciliation info on each node.
        stHash *leafToSpecies = getLeafToSpecies(bestTree,
                                                 speciesStTree,
                                                 block, flower);
        stPhylogeny_reconcileBinary(bestTree, speciesStTree, leafToSpecies,
                                    false);

        stHash_insert(blocksToTrees, block, bestTree);

        // Cleanup
        for (int64_t i = 0; i < stList_length(trees); i++) {
            stTree *tree = stList_get(trees, i);
            if (tree != bestTree) {
                stPhylogenyInfo_destructOnTree(tree);
                stTree_destruct(tree);
            }
        }
        stList_destruct(featureColumns);
        stList_destruct(featureBlocks);
        stList_destruct(outgroups);
        stHash_destruct(leafToSpecies);
    }
}

void stCaf_buildTreesToRemoveAncientHomologies(stPinchThreadSet *threadSet, stHash *threadStrings, stSet *outgroupThreads, Flower *flower, int64_t maxBaseDistance, int64_t maxBlockDistance, int64_t numTrees, enum stCaf_TreeBuildingMethod treeBuildingMethod, enum stCaf_RootingMethod rootingMethod, enum stCaf_ScoringMethod scoringMethod, double breakPointScalingFactor, bool skipSingleCopyBlocks, bool allowSingleDegreeBlocks, double costPerDupPerBase, double costPerLossPerBase, FILE *debugFile) {
    // Functions we aren't using right now but should stick around anyway.
    (void) printSimpleBlockDebugInfo;
    (void) getTotalSimilarityAndDifferenceCounts;
    (void) printTreeBuildingDebugInfo;

    // Parameters.
    bool ignoreUnalignedBases = 1;
    bool onlyIncludeCompleteFeatureBlocks = 0;

    stPinchThreadSetBlockIt blockIt = stPinchThreadSet_getBlockIt(threadSet);
    stPinchBlock *block;

    //Hash in which we store a map of blocks to their trees
    stHash *blocksToTrees = stHash_construct2(NULL, NULL);

    //Get species tree as an stTree
    EventTree *eventTree = flower_getEventTree(flower);
    stTree *speciesStTree = eventTreeToStTree(eventTree);

    // Get info for guided neighbor-joining
    stHash *speciesToJoinCostIndex = stHash_construct2(NULL, (void (*)(void *)) stIntTuple_destruct);
    stMatrix *joinCosts = stPhylogeny_computeJoinCosts(speciesStTree, speciesToJoinCostIndex, costPerDupPerBase * 2 * maxBaseDistance, costPerLossPerBase * 2 * maxBaseDistance);

    stSortedSet *splitBranches = stSortedSet_construct3((int (*)(const void *, const void *)) compareSplitBranches, free);
    stSet *speciesToSplitOn = stSet_construct();
    getSpeciesToSplitOn(speciesStTree, eventTree, speciesToSplitOn);

    // Temp debug print
    printf("Chose events in the species tree to split on:");
    stSetIterator *speciesToSplitOnIt = stSet_getIterator(speciesToSplitOn);
    stTree *speciesNodeToSplitOn;
    while ((speciesNodeToSplitOn = stSet_getNext(speciesToSplitOnIt)) != NULL) {
        Name name;
        sscanf(stTree_getLabel(speciesNodeToSplitOn), "%" PRIi64, &name);
        Event *event = eventTree_getEvent(eventTree, name);
        printf(" %s", event_getHeader(event));
    }
    printf("\n");
    stSet_destructIterator(speciesToSplitOnIt);

    // The loop to build a tree for each block
    while ((block = stPinchThreadSetBlockIt_getNext(&blockIt)) != NULL) {
        buildTreeForBlock(block, threadStrings, outgroupThreads, flower, maxBaseDistance, maxBlockDistance, numTrees, treeBuildingMethod, rootingMethod, scoringMethod, breakPointScalingFactor, skipSingleCopyBlocks, allowSingleDegreeBlocks, ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, costPerDupPerBase, costPerLossPerBase, joinCosts, speciesToJoinCostIndex, speciesStTree, blocksToTrees);
        stTree *tree = stHash_search(blocksToTrees, block);
        if (tree != NULL) {
            findSplitBranches(block, stHash_search(blocksToTrees, block),
                              splitBranches, speciesToSplitOn);
        }
    }

    st_logDebug("Got homology partition for each block\n");

    fprintf(stdout, "Before partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));
    fprintf(stdout, "Found %" PRIi64 " split branches initially\n", stSortedSet_size(splitBranches));
    // Now walk through the split branches, doing the most confident
    // splits first, and updating the blocks whose breakpoint
    // information is modified.
    int64_t numberOfSplitsMade = 0;
    stCaf_SplitBranch *splitBranch = stSortedSet_getLast(splitBranches);
    while (splitBranch != NULL) {
        block = splitBranch->block;
        // Do the partition on the block.
        stList *partition = stList_construct();
        // Create a leaf set with all leaves below this split branch.
        stList *leafSet = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
        int64_t segmentBelowBranchIndex = -1; // Arbitrary index of
                                              // segment below the
                                              // branch so we can
                                              // recover the blocks we
                                              // split into later.
        stList *bfQueue = stList_construct();
        stList_append(bfQueue, splitBranch->child);
        while (stList_length(bfQueue) != 0) {
            stTree *node = stList_pop(bfQueue);
            for (int64_t i = 0; i < stTree_getChildNumber(node); i++) {
                stList_append(bfQueue, stTree_getChild(node, i));
            }

            if (stTree_getChildNumber(node) == 0) {
                stPhylogenyInfo *info = stTree_getClientData(node);
                assert(info->matrixIndex != -1);
                stList_append(leafSet, stIntTuple_construct1(info->matrixIndex));
                segmentBelowBranchIndex = info->matrixIndex;
            }
        }
        stList_append(partition, leafSet);
        // Create a leaf set with all leaves that aren't below the
        // split branch.
        leafSet = stList_construct3(0, (void (*)(void *))stIntTuple_destruct);
        stTree *root = splitBranch->child;
        while (stTree_getParent(root) != NULL) {
            root = stTree_getParent(root);
        }
        int64_t segmentNotBelowBranchIndex = -1; // Arbitrary index of
                                                 // segment not below the
                                                 // branch so we can
                                                 // recover the blocks
                                                 // we split into
                                                 // later.
        stList_append(bfQueue, root);
        while (stList_length(bfQueue) != 0) {
            stTree *node = stList_pop(bfQueue);
            if (node != splitBranch->child) {
                for (int64_t i = 0; i < stTree_getChildNumber(node); i++) {
                    stList_append(bfQueue, stTree_getChild(node, i));
                }

                if (stTree_getChildNumber(node) == 0) {
                    stPhylogenyInfo *info = stTree_getClientData(node);
                    assert(info->matrixIndex != -1);
                    stList_append(leafSet, stIntTuple_construct1(info->matrixIndex));
                    segmentNotBelowBranchIndex = info->matrixIndex;
                }
            }
        }
        stList_append(partition, leafSet);
        // Get arbitrary segments from the blocks we will split
        // into. This is so that we can recover the blocks after the
        // split.
        stPinchSegment *segmentBelowBranch = getSegmentByBlockIndex(block, segmentBelowBranchIndex);
        stPinchSegment *segmentNotBelowBranch = getSegmentByBlockIndex(block, segmentNotBelowBranchIndex);

        // Remove all split branch entries for this block tree.
        removeOldSplitBranches(block, root, speciesToSplitOn, splitBranches);

        // Destruct the block tree.
        stPhylogenyInfo_destructOnTree(root);
        stTree_destruct(root);
        stHash_remove(blocksToTrees, block);

        // Actually perform the split according to the partition.
        splitBlock(block, partition, allowSingleDegreeBlocks);
        // Recover the blocks.
        stPinchBlock *blockBelowBranch = stPinchSegment_getBlock(segmentBelowBranch);
        stPinchBlock *blockNotBelowBranch = stPinchSegment_getBlock(segmentNotBelowBranch);

        // Make a new tree for both of the blocks we split into, if
        // they are not too simple to make a tree.
        if (blockBelowBranch != NULL) {
            buildTreeForBlock(blockBelowBranch, threadStrings, outgroupThreads, flower, maxBaseDistance, maxBlockDistance, numTrees, treeBuildingMethod, rootingMethod, scoringMethod, breakPointScalingFactor, skipSingleCopyBlocks, allowSingleDegreeBlocks, ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, costPerDupPerBase, costPerLossPerBase, joinCosts, speciesToJoinCostIndex, speciesStTree, blocksToTrees);
        }
        stTree *treeBelowBranch = stHash_search(blocksToTrees, blockBelowBranch);
        if (treeBelowBranch != NULL) {
            findSplitBranches(blockBelowBranch, treeBelowBranch,
                              splitBranches, speciesToSplitOn);
        }
        if (blockNotBelowBranch != NULL) {
            buildTreeForBlock(blockNotBelowBranch, threadStrings, outgroupThreads, flower, maxBaseDistance, maxBlockDistance, numTrees, treeBuildingMethod, rootingMethod, scoringMethod, breakPointScalingFactor, skipSingleCopyBlocks, allowSingleDegreeBlocks, ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, costPerDupPerBase, costPerLossPerBase, joinCosts, speciesToJoinCostIndex, speciesStTree, blocksToTrees);
        }
        stTree *treeNotBelowBranch = stHash_search(blocksToTrees, blockNotBelowBranch);
        if (treeNotBelowBranch != NULL) {
            findSplitBranches(blockNotBelowBranch, treeNotBelowBranch,
                              splitBranches, speciesToSplitOn);
        }

        // Finally, update the trees for all blocks close enough to
        // either of the new blocks to be affected by this breakpoint
        // change.
        stSet *blocksToUpdate = stSet_construct();
        if (blockBelowBranch != NULL) {
            addContextualBlocksToSet(blockBelowBranch, maxBaseDistance,
                                     maxBlockDistance, ignoreUnalignedBases,
                                     blocksToUpdate);
        }
        if (blockNotBelowBranch != NULL) {
            addContextualBlocksToSet(blockNotBelowBranch, maxBaseDistance,
                                     maxBlockDistance, ignoreUnalignedBases,
                                     blocksToUpdate);
        }
        stSetIterator *blocksToUpdateIt = stSet_getIterator(blocksToUpdate);
        stPinchBlock *blockToUpdate;
        while ((blockToUpdate = stSet_getNext(blocksToUpdateIt)) != NULL) {
            stTree *oldTree = stHash_search(blocksToTrees, blockToUpdate);
            removeOldSplitBranches(blockToUpdate, oldTree, speciesToSplitOn,
                                   splitBranches);
            buildTreeForBlock(blockToUpdate, threadStrings, outgroupThreads, flower, maxBaseDistance, maxBlockDistance, numTrees, treeBuildingMethod, rootingMethod, scoringMethod, breakPointScalingFactor, skipSingleCopyBlocks, allowSingleDegreeBlocks, ignoreUnalignedBases, onlyIncludeCompleteFeatureBlocks, costPerDupPerBase, costPerLossPerBase, joinCosts, speciesToJoinCostIndex, speciesStTree, blocksToTrees);
            stTree *tree = stHash_search(blocksToTrees, blockToUpdate);
            if (tree != NULL) {
                findSplitBranches(blockToUpdate, tree,
                                  splitBranches, speciesToSplitOn);
            }
        }
        stSet_destruct(blocksToUpdate);
        numberOfSplitsMade++;
        splitBranch = stSortedSet_getLast(splitBranches);
    }

    st_logDebug("Finished partitioning the blocks\n");
    fprintf(stdout, "There were %" PRIi64 "splits made overall in the end.\n",
            numberOfSplitsMade);
    fprintf(stdout, "After partitioning, there were %" PRIi64 " bases lost in between single-degree blocks\n", countBasesBetweenSingleDegreeBlocks(threadSet));

    //Cleanup
    stTree_destruct(speciesStTree);
}