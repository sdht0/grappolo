// ***********************************************************************
//
//            Grappolo: A C++ library for graph clustering
//               Mahantesh Halappanavar (hala@pnnl.gov)
//               Pacific Northwest National Laboratory
//
// ***********************************************************************
//
//       Copyright (2014) Battelle Memorial Institute
//                      All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// ************************************************************************

#include "defs.h"
#include "utilityClusteringFunctions.h"
#include "basic_comm.h"

#ifdef USE_PMEM_ALLOC
#include <memkind.h>

#define PATH_MAX (128)
#define PMEM_MAX_SIZE (1ULL << 40)

static char path[PATH_MAX]="/pmem1";

static void print_err_message(int err)
{
    char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
    memkind_error_message(err, error_message, MEMKIND_ERROR_MESSAGE_SIZE);
    fprintf(stderr, "%s\n", error_message);
}
#endif

using namespace std;


double parallelLouvianMethod(graph *G, long *C, int nThreads, double Lower,
                             double thresh, double *totTime, int *numItr) {
#ifdef PRINT_DETAILED_STATS_
    printf("Within parallelLouvianMethod()\n");
#endif
    if (nThreads < 1)
        omp_set_num_threads(1);
    else
        omp_set_num_threads(nThreads);
    int nT;
#pragma omp parallel
    {
        nT = omp_get_num_threads();
    }
#ifdef PRINT_DETAILED_STATS_
    printf("Actual number of threads: %d (requested: %d)\n", nT, nThreads);
#endif
    
    
    double time1, time2, time3, time4; //For timing purposes
    double total = 0, totItr = 0;
    
    long    NV        = G->numVertices;
    long    NS        = G->sVertices;
    long    NE        = G->numEdges;
#ifdef USE_PMEM_ALLOC
    // create PMEM partition with specific size
    struct memkind *pmem_kind = NULL;
    int err = memkind_create_pmem(path, PMEM_MAX_SIZE, &pmem_kind);
    if (err) {
        print_err_message(err);
        return 1;
    }
#ifdef DO_PMEM_DEBUG
    size_t resident, active, allocated;
    memkind_update_cached_stats();
    memkind_get_stat(pmem_kind, MEMKIND_STAT_TYPE_RESIDENT, &resident);
    memkind_get_stat(pmem_kind, MEMKIND_STAT_TYPE_ACTIVE, &active);
    memkind_get_stat(pmem_kind, MEMKIND_STAT_TYPE_ALLOCATED, &allocated);
    printf ("PMEM stats: resident=%zu active=%zu allocated=%zu\n", resident, active, allocated);
#endif
    long  *edgeListPtrs_pmem = (long *)  memkind_malloc(pmem_kind, (NV+1) * sizeof(long));
    edge *edgeList_pmem = (edge *) memkind_malloc(pmem_kind, 2*NE*sizeof(edge));
    if (edgeListPtrs_pmem == NULL) {
            fprintf(stderr, "Exiting after unable to allocate pmem for edgeListPtrs_pmem of size: %zu.\n", (NV+1)*sizeof(long));
            exit(1);
    }
    if (edgeList_pmem == NULL) {
            fprintf(stderr, "Exiting after unable to allocate pmem for edgeList_pmem of size: %zu.\n", 2*NE*sizeof(edge));
            exit(1);
    }
    memcpy(edgeListPtrs_pmem, G->edgeListPtrs, (NV+1)*sizeof(long));
    memcpy(edgeList_pmem, G->edgeList, 2*NE*sizeof(edge));
    long    *vtxPtr   = edgeListPtrs_pmem;
    edge    *vtxInd   = edgeList_pmem;
    printf("PMEM allocation successful: copied edge list and pointers to PMEM (in bytes): %zu.\n", ((NV+1)*sizeof(long) + 2*NE*sizeof(edge)));
#else
    long    *vtxPtr   = G->edgeListPtrs;
    edge    *vtxInd   = G->edgeList;
#endif    
       
    /* Variables for computing modularity */
    long totalEdgeWeightTwice;
    double constantForSecondTerm;
    double prevMod=-1;
    double currMod=-1;
    //double thresMod = 0.000001;
    double thresMod = thresh; //Input parameter

    /********************** Initialization **************************/
    time1 = omp_get_wtime();
    //Community info. (ai and size)
    //Store the degree of all vertices
    double* vDegree = (double *) malloc (NV * sizeof(double)); assert(vDegree != 0);
    Comm* cInfo = (Comm *) malloc (NV * sizeof(Comm)); assert(cInfo != 0);
#if defined(SPLIT_LOOP_SUMVDEG)
    sumVertexDegreeEdgeScan(vtxInd, vtxPtr, vDegree, NE , NV, cInfo);	// Sum up the vertex degree
#else
    sumVertexDegree(vtxInd, vtxPtr, vDegree, NV , cInfo);	// Sum up the vertex degree
#endif
    //use for updating Community
    Comm *cUpdate = (Comm*)malloc(NV*sizeof(Comm)); assert(cUpdate != 0);

    //use for Modularity calculation (eii)
    double* clusterWeightInternal = (double*) malloc (NV*sizeof(double)); assert(clusterWeightInternal != 0);

    /*** Compute the total edge weight (2m) and 1/2m ***/
    constantForSecondTerm = calConstantForSecondTerm(vDegree, NV); // 1 over sum of the degree

    //cout<<"CHECK THIS:              "<<constantForSecondTerm<<endl;
    //Community assignments:
    //Store previous iteration's community assignment
    long* pastCommAss = (long *) malloc (NV * sizeof(long)); assert(pastCommAss != 0);
    //Store current community assignment
    long* currCommAss = (long *) malloc (NV * sizeof(long)); assert(currCommAss != 0);
    //Store the target of community assignment
    long* targetCommAss = (long *) malloc (NV * sizeof(long)); assert(targetCommAss != 0);

    //Vectors used in place of maps: Total size = |V|+2*|E| -- The |V| part takes care of self loop
    //  mapElement* clusterLocalMapX = (mapElement *) malloc ((NV + 2*NE) * sizeof(mapElement)); assert(clusterLocalMapX != 0);
    //double* Counter             = (double *)     malloc ((NV + 2*NE) * sizeof(double));     assert(Counter != 0);

    //Initialize each vertex to its own cluster
    //initCommAssOpt(pastCommAss, currCommAss, NV, clusterLocalMapX, vtxPtr, vtxInd, cInfo, constantForSecondTerm, vDegree);

    //Initialize each vertex to its own cluster
    initCommAss(pastCommAss, currCommAss, NV);

    time2 = omp_get_wtime();
    // printf(" Initialized iter %d: %.3lf\n", time2-time1);

#ifdef PRINT_DETAILED_STATS_
    printf("========================================================================================================\n");
    printf("Itr      E_xx            A_x2           Curr-Mod         Time-1(s)       Time-2(s)        T/Itr(s)\n");
    printf("========================================================================================================\n");
#endif
#ifdef PRINT_TERSE_STATS_
    printf("=====================================================\n");
    printf("Itr      Curr-Mod         T/Itr(s)      T-Cumulative\n");
    printf("=====================================================\n");
#endif
    //Start maximizing modularity
    for (int numItrss = 0u; numItrss < 20; ++numItrss) {
        time1 = omp_get_wtime();
        /* Re-initialize datastructures */
#pragma omp parallel for
        for (long i=0; i<NV; i++) {
            clusterWeightInternal[i] = 0;
            cUpdate[i].degree =0;
            cUpdate[i].size =0;
        }

#pragma omp parallel for
        for (long i=0; i<NV; i++) {
            long adj1 = vtxPtr[i];
            long adj2 = vtxPtr[i+1];
            double selfLoop = 0;
            //Build a datastructure to hold the cluster structure of its neighbors
            map<long, long> clusterLocalMap; //Map each neighbor's cluster to a local number
            map<long, long>::iterator storedAlready;
            vector<double> Counter; //Number of edges in each unique cluster
            //Add v's current cluster:
            if(adj1 != adj2){
                clusterLocalMap[currCommAss[i]] = 0;
                Counter.push_back(0); //Initialize the counter to ZERO (no edges incident yet)
                //Find unique cluster ids and #of edges incident (eicj) to them
                selfLoop = buildLocalMapCounter(adj1, adj2, clusterLocalMap, Counter, vtxInd, currCommAss, i);
                // Update delta Q calculation
                clusterWeightInternal[i] += Counter[0]; //(e_ix)
                //Calculate the max
                targetCommAss[i] = max(clusterLocalMap, Counter, selfLoop, cInfo, vDegree[i], currCommAss[i], constantForSecondTerm);
                //assert((targetCommAss[i] >= 0)&&(targetCommAss[i] < NV));
            } else {
                targetCommAss[i] = -1;
            }

            //Update
            if(targetCommAss[i] != currCommAss[i]  && targetCommAss[i] != -1) {
#pragma omp atomic update
                cUpdate[targetCommAss[i]].degree += vDegree[i];
#pragma omp atomic update
                cUpdate[targetCommAss[i]].size += 1;
#pragma omp atomic update
                cUpdate[currCommAss[i]].degree -= vDegree[i];
#pragma omp atomic update
                cUpdate[currCommAss[i]].size -=1;
                /*
                 __sync_fetch_and_add(&cUpdate[targetCommAss[i]].size, 1);
                 __sync_fetch_and_sub(&cUpdate[currCommAss[i]].degree, vDegree[i]);
                 __sync_fetch_and_sub(&cUpdate[currCommAss[i]].size, 1);*/
            }//End of If()
            clusterLocalMap.clear();
            Counter.clear();
        }//End of for(i)
        time2 = omp_get_wtime();
        printf("  Ran iteration %d: %.3lf s\n", numItrss, (time2 - time1));

        time3 = omp_get_wtime();
        double e_xx = 0;
        double a2_x = 0;

#pragma omp parallel for \
reduction(+:e_xx) reduction(+:a2_x)
        for (long i=0; i<NV; i++) {
            e_xx += clusterWeightInternal[i];
            a2_x += (cInfo[i].degree)*(cInfo[i].degree);
        }
        currMod = (e_xx*(double)constantForSecondTerm) - (a2_x*(double)constantForSecondTerm*(double)constantForSecondTerm);

        time4 = omp_get_wtime();
        printf("    Computed modularity: %lf %.3lf s\n", currMod, (time4 - time3));

        totItr = (time2-time1) + (time4-time3);
        total += totItr;
#ifdef PRINT_DETAILED_STATS_
        printf("%d \t %g \t %g \t %lf \t %3.3lf \t %3.3lf  \t %3.3lf\n",numItrs, e_xx, a2_x, currMod, (time2-time1), (time4-time3), totItr );
#endif
#ifdef PRINT_TERSE_STATS_
        printf("%d \t %lf \t %3.3lf  \t %3.3lf\n",numItrs, currMod, totItr, total);
#endif

        *numItr  = numItrss+1;
        //Break if modularity gain is not sufficient
        if((currMod - prevMod) < thresMod) {
            break;
        }

        double time5 = omp_get_wtime();
        //Else update information for the next iteration
        prevMod = currMod;
        if(prevMod < Lower)
            prevMod = Lower;
#pragma omp parallel for
        for (long i=0; i<NV; i++) {
            cInfo[i].size += cUpdate[i].size;
            cInfo[i].degree += cUpdate[i].degree;
        }
        printf("    Updated comminfo %.3lf s\n", (omp_get_wtime() - time5));

        //Do pointer swaps to reuse memory:
        long* tmp;
        tmp = pastCommAss;
        pastCommAss = currCommAss; //Previous holds the current
        currCommAss = targetCommAss; //Current holds the chosen assignment
        targetCommAss = tmp;      //Reuse the vector

    }//End of while(true)
    *totTime = total; //Return back the total time for clustering

#ifdef PRINT_DETAILED_STATS_
    printf("========================================================================================================\n");
    printf("Total time for %d iterations is: %lf\n",numItrs, total);
    printf("========================================================================================================\n");
#endif
#ifdef PRINT_TERSE_STATS_
    printf("========================================================================================================\n");
    printf("Total time for %d iterations is: %lf\n",numItrs, total);  
    printf("========================================================================================================\n");
#endif
    
    //Store back the community assignments in the input variable:
    //Note: No matter when the while loop exits, we are interested in the previous assignment
#pragma omp parallel for 
    for (long i=0; i<NV; i++) {
        C[i] = pastCommAss[i];
    }
    //Cleanup
    free(pastCommAss);
    free(currCommAss);
    free(targetCommAss);
    free(cUpdate);
    free(clusterWeightInternal);
    
#ifdef USE_PMEM_ALLOC
    memkind_free(pmem_kind, edgeListPtrs_pmem);
    memkind_free(pmem_kind, edgeList_pmem);

    err = memkind_destroy_kind(pmem_kind);
    if (err) {
        print_err_message(err);
        return 1;
    }
#endif
    return prevMod;
}
