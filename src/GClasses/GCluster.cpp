/*
	Copyright (C) 2006, Mike Gashler

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	see http://www.gnu.org/copyleft/lesser.html
*/

#include "GCluster.h"
#include "GNeighborFinder.h"
#include "GDistance.h"
#include "GBitTable.h"
#include "GHeap.h"
#include "GMath.h"
#include "GVec.h"
#include <math.h>
#include <stdlib.h>
#include "GNeuralNet.h"
#include "GHillClimber.h"
#include "GBitTable.h"
#include "GSparseMatrix.h"
#include "GKNN.h"
#include "GTime.h"
#include "GGraph.h"
#include "GTwt.h"
#include <iostream>
#include <map>

using namespace GClasses;
using std::cout;
using std::vector;
using std::map;


GAgglomerativeClusterer::GAgglomerativeClusterer(size_t clusters)
: GClusterer(clusters), m_pMetric(NULL), m_ownMetric(false), m_pClusters(NULL)
{
}

GAgglomerativeClusterer::~GAgglomerativeClusterer()
{
	if(m_ownMetric)
		delete(m_pMetric);
	delete[] m_pClusters;
}

void GAgglomerativeClusterer::setMetric(GDissimilarityMetric* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

// virtual
void GAgglomerativeClusterer::cluster(GMatrix* pData)
{
	// Init the metric
	if(!m_pMetric)
		setMetric(new GRowDistance(), true);
	m_pMetric->init(pData->relation());

	// Find enough neighbors to form a connected graph
	GNeighborFinderCacheWrapper* pNF = NULL;
	size_t neighbors = 6;
	while(true)
	{
		GKdTree* pKdTree = new GKdTree(pData, neighbors, m_pMetric, false);
		pNF = new GNeighborFinderCacheWrapper(pKdTree, true);
		pNF->fillCache();
		if(pNF->isConnected())
			break;
		if(neighbors + 1 >= pData->rows())
		{
			delete(pNF);
			ThrowError("internal problem--a graph with so many neighbors must be connected");
		}
		neighbors = std::min((neighbors * 3) / 2, pData->rows() - 1);
	}

	// Sort all the neighbors by their distances
	size_t count = pData->rows() * neighbors;
	vector< std::pair<double,size_t> > distNeighs;
	distNeighs.resize(count);
	double* pDistances = pNF->squaredDistanceTable();
	size_t* pRows = pNF->cache();
	size_t index = 0;
	vector< std::pair<double,size_t> >::iterator it = distNeighs.begin();
	for(size_t i = 0; i < count; i++)
	{
		if(*pRows < pData->rows())
		{
			it->first = *pDistances;
			it->second = i;
			it++;
		}
		else
			index--;
		pRows++;
		pDistances++;
	}
	std::sort(distNeighs.begin(), it);

	// Assign each row to its own cluster
	delete[] m_pClusters;
	m_pClusters = new size_t[pData->rows()]; // specifies which cluster each row belongs to
	GIndexVec::makeIndexVec(m_pClusters, pData->rows());
	size_t* pSiblings = new size_t[pData->rows()]; // a cyclical linked list of each row in the cluster
	ArrayHolder<size_t> hSiblings(pSiblings);
	GIndexVec::makeIndexVec(pSiblings, pData->rows()); // init such that each row is in a cluster of 1
	size_t currentClusterCount = pData->rows();
	if(currentClusterCount <= m_clusterCount)
		return; // nothing to do

	// Merge until we have the desired number of clusters
	pRows = pNF->cache();
	for(vector< std::pair<double,size_t> >::iterator dn = distNeighs.begin(); dn != it; dn++)
	{
		// Get the next two closest points
		size_t a = dn->second / neighbors;
		size_t b = pRows[dn->second];
		GAssert(a != b && a < pData->rows() && b < pData->rows());
		size_t clustA = m_pClusters[a];
		size_t clustB = m_pClusters[b];
		
		// Merge the clusters
		if(clustA == clustB)
			continue; // The two points are already in the same cluster
		if(clustB < clustA) // Make sure clustA has the smaller value
		{
			std::swap(a, b);
			std::swap(clustA, clustB);
		}
		for(size_t i = pSiblings[b]; true; i = pSiblings[i]) // Convert every row in clustB to clustA
		{
			m_pClusters[i] = clustA;
			if(i == b)
				break;
		}
		std::swap(pSiblings[a], pSiblings[b]); // This line joins the cyclical linked lists into one big cycle
		if(clustB < m_clusterCount) // Ensure that the first m_clusterCount cluster numbers are always in use
		{
			for(size_t i = 0; i < pData->rows(); i++) // rename another cluster to take the spot of clustB
			{
				if(m_pClusters[i] >= m_clusterCount)
				{
					for(size_t j = pSiblings[i]; true; j = pSiblings[j])
					{
						m_pClusters[j] = clustB;
						if(j == i)
							break;
					}
					break;
				}
			}
		}
		if(--currentClusterCount <= m_clusterCount)
			return;
	}
	ThrowError("internal error--should have found the desired number of clusters before now");
}

// virtual
size_t GAgglomerativeClusterer::whichCluster(size_t nVector)
{
	return m_pClusters[nVector];
}

#ifndef NO_TEST_CODE

#define SPRIAL_POINTS 250
#define SPIRAL_HEIGHT 3

#include "GImage.h"
//#include "G3D.h"

// static
void GAgglomerativeClusterer::test()
{
	// Make a 3D data set with 3 entwined spirals
	GHeap heap(1000);
	GMatrix data(0, 3, &heap);
	double dThirdCircle = M_PI * 2 / 3;
	double* pVector;
	double rads;
	for(size_t i = 0; i < SPRIAL_POINTS; i += 3)
	{
		rads = (double)i * 2 * M_PI / SPRIAL_POINTS;

		pVector = data.newRow();
		pVector[0] = cos(rads);
		pVector[2] = sin(rads);
		pVector[1] = (double)i * SPIRAL_HEIGHT / SPRIAL_POINTS;

		pVector = data.newRow();
		pVector[0] = cos(rads + dThirdCircle);
		pVector[2] = sin(rads + dThirdCircle);
		pVector[1] = (double)i * SPIRAL_HEIGHT / SPRIAL_POINTS;

		pVector = data.newRow();
		pVector[0] = cos(rads + dThirdCircle + dThirdCircle);
		pVector[2] = sin(rads + dThirdCircle + dThirdCircle);
		pVector[1] = (double)i * SPIRAL_HEIGHT / SPRIAL_POINTS;
	}

	// Cluster the points
	GAgglomerativeClusterer clust(3);
	clust.cluster(&data);

	// Test for correctness
	if(clust.whichCluster(0) == clust.whichCluster(1))
		ThrowError("failed");
	if(clust.whichCluster(1) == clust.whichCluster(2))
		ThrowError("failed");
	if(clust.whichCluster(2) == clust.whichCluster(0))
		ThrowError("failed");
	for(size_t i = 3; i < SPRIAL_POINTS; i += 3)
	{
		if(clust.whichCluster(i) != clust.whichCluster(0))
			ThrowError("Wrong cluster");
		if(clust.whichCluster(i + 1) != clust.whichCluster(1))
			ThrowError("Wrong cluster");
		if(clust.whichCluster(i + 2) != clust.whichCluster(2))
			ThrowError("Wrong cluster");
	}

/*  // Uncomment this to make a spiffy visualization of the entwined spirals

	// Draw the classifications
	GImage image;
	image.SetSize(600, 600);
	image.Clear(0xff000000);
	GCamera camera;
	camera.SetViewAngle(PI / 2);
	camera.GetLookFromPoint()->Set(2, 1.5, 3);
	camera.GetLookDirection()->Set(-2, 0, -3);
	camera.ComputeSideVector();
	camera.SetImageSize(600, 600);
	double* pVec;
	G3DVector point;
	GColor col = 0;
	for(i = 0; i < SPRIAL_POINTS; i++)
	{
		pVec = data.row(i);
		point.Set(pVec[0], pVec[1], pVec[2]);
		switch(clust.whichCluster(i))
		{
			case 0: col = 0xffff0000; break;
			case 1: col = 0xff00ff00; break;
			case 2: col = 0xff0000ff; break;
		}
		image.Draw3DLine(&point, &point, &camera, col);
	}

	// Draw the bounding box
	G3DVector point2;
	size_t x, y, z;
	for(z = 0; z < 2; z++)
	{
		for(y = 0; y < 2; y++)
		{
			for(x = 0; x < 2; x++)
			{
				if(x == 0)
				{
					point.Set(-1, 3 * y, 2 * z - 1);
					point2.Set(1, 3 * y, 2 * z - 1);
					image.Draw3DLine(&point, &point2, &camera, 0xff808080);
				}
				if(y == 0)
				{
					point.Set(2 * x - 1, 0, 2 * z - 1);
					point2.Set(2 * x - 1, 3, 2 * z - 1);
					image.Draw3DLine(&point, &point2, &camera, 0xff808080);
				}
				if(z == 0)
				{
					point.Set(2 * x - 1, 3 * y, -1);
					point2.Set(2 * x - 1, 3 * y, 1);
					image.Draw3DLine(&point, &point2, &camera, 0xff808080);
				}
			}
		}
	}
	image.SavePNGFile("spirals.png");
*/
}
#endif // !NO_TEST_CODE

// -----------------------------------------------------------------------------------------

GAgglomerativeTransducer::GAgglomerativeTransducer()
: GTransducer(), m_pMetric(NULL), m_ownMetric(false)
{
}

GAgglomerativeTransducer::~GAgglomerativeTransducer()
{
}

void GAgglomerativeTransducer::setMetric(GDissimilarityMetric* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

// virtual
GMatrix* GAgglomerativeTransducer::transduce(GMatrix& features1, GMatrix& labels1, GMatrix& features2)
{
	// Check assumptions
	if(labels1.cols() != 1 || !labels1.relation()->areNominal(0, 1))
		ThrowError("Only one nominal label dimension is supported");
	if(features1.cols() != features2.cols())
		ThrowError("Expected both feature matrices to have the same number of cols");
	if(features1.rows() != labels1.rows())
		ThrowError("Expected features1 to have the same number of rows as labels1");

	// Init the metric
	if(!m_pMetric)
		setMetric(new GRowDistance(), true);
	m_pMetric->init(features1.relation());

	// Make a dataset with all featuers
	GMatrix featuresAll(features1.relation());
	featuresAll.reserve(features1.rows() + features2.rows());
	GReleaseDataHolder hFeaturesAll(&featuresAll);
	for(size_t i = 0; i < features1.rows(); i++)
		featuresAll.takeRow(features1[i]);
	for(size_t i = 0; i < features2.rows(); i++)
		featuresAll.takeRow(features2[i]);

	// Find enough neighbors to form a connected graph
	GNeighborFinderCacheWrapper* pNF = NULL;
	size_t neighbors = 6;
	while(true)
	{
		GKdTree* pKdTree = new GKdTree(&featuresAll, neighbors, m_pMetric, false);
		pNF = new GNeighborFinderCacheWrapper(pKdTree, true);
		pNF->fillCache();
		if(pNF->isConnected())
			break;
		if(neighbors + 1 >= featuresAll.rows())
		{
			delete(pNF);
			ThrowError("internal problem--a graph with so many neighbors must be connected");
		}
		neighbors = std::min((neighbors * 3) / 2, featuresAll.rows() - 1);
	}

	// Sort all the neighbors by their distances
	size_t count = featuresAll.rows() * neighbors;
	vector< std::pair<double,size_t> > distNeighs;
	distNeighs.resize(count);
	double* pDistances = pNF->squaredDistanceTable();
	size_t* pRows = pNF->cache();
	size_t index = 0;
	vector< std::pair<double,size_t> >::iterator it = distNeighs.begin();
	for(size_t i = 0; i < count; i++)
	{
		if(*pRows < featuresAll.rows())
		{
			it->first = *pDistances;
			it->second = i;
			it++;
		}
		else
			index--;
		pRows++;
		pDistances++;
	}
	std::sort(distNeighs.begin(), it);

	// Assign each row to its own cluster
	GMatrix* pOut = new GMatrix(labels1.relation());
	Holder<GMatrix> hOut(pOut);
	pOut->newRows(features2.rows());
	pOut->setAll(-1);
	size_t* pSiblings = new size_t[featuresAll.rows()]; // a cyclical linked list of each row in the cluster
	ArrayHolder<size_t> hSiblings(pSiblings);
	GIndexVec::makeIndexVec(pSiblings, featuresAll.rows()); // init such that each row is in a cluster of 1
	size_t missingLabels = features2.rows();

	// Merge until we have the desired number of clusters
	pRows = pNF->cache();
	for(vector< std::pair<double,size_t> >::iterator dn = distNeighs.begin(); dn != it; dn++)
	{
		// Get the next two closest points
		size_t a = dn->second / neighbors;
		size_t b = pRows[dn->second];
		GAssert(a != b && a < featuresAll.rows() && b < featuresAll.rows());
		int labelA = (a < features1.rows() ? (int)labels1[a][0] : (int)pOut->row(a - features1.rows())[0]);
		int labelB = (b < features1.rows() ? (int)labels1[b][0] : (int)pOut->row(b - features1.rows())[0]);

		// Merge the clusters
		if(labelA >= 0 && labelB >= 0)
			continue; // Both points are already labeled, so there is no point in merging their clusters
		if(labelA < 0 && labelB >= 0) // Make sure that if one of them has a valid label, it is point a
		{
			std::swap(a, b);
			std::swap(labelA, labelB);
		}
		if(labelA >= 0)
		{
			for(size_t i = pSiblings[b]; true; i = pSiblings[i]) // Label every row in cluster b
			{
				GAssert(i >= features1.rows());
				GAssert(pOut->row(i - features1.rows())[0] == (double)-1);
				pOut->row(i - features1.rows())[0] = labelA;
				if(--missingLabels == 0)
					return hOut.release();
				if(i == b)
					break;
			}
		}
		std::swap(pSiblings[a], pSiblings[b]); // This line joins the cyclical linked lists into one big cycle
	}
	ThrowError("internal error--should have finished before now");
	return NULL;
}


// -----------------------------------------------------------------------------------------

GKMeans::GKMeans(size_t nClusters, GRand* pRand)
: GClusterer(nClusters)
{
	m_pRand = pRand;
	m_nClusters = nClusters;
	m_pClusters = NULL;
}

GKMeans::~GKMeans()
{
	delete[] m_pClusters;
}

bool GKMeans::selectSeeds(GMatrix* pSeeds)
{
	// Randomly select the seed points
	size_t i, j, k, index;
	double* pVector;
	for(i = 0; i < m_nClusters; i++)
	{
		for(j = 0; j < m_nClusters; j++)
		{
			// Pick a point
			index = (size_t)m_pRand->next(m_pData->rows());
			pVector = m_pData->row(index);

			// Make sure we didn't pick the same point already
			bool bOK = true;
			for(k = 0; k < i; k++)
			{
				if(GVec::squaredDistance(pVector, pSeeds->row(k), m_nDims) <= 0)
				{
					bOK = false;
					break;
				}
			}

			// Accept this seed point
			if(bOK)
			{
				pSeeds->copyRow(pVector);
				break;
			}
		}
		if(j >= m_nClusters)
			return false; // failed to find "m_nClusters" unique seed points
	}
	return true;
}

bool GKMeans::clusterAttempt(size_t nMaxIterations)
{
	// Pick the seeds
	GHeap heap(1000);
	GMatrix means(0, m_nDims, &heap);
	means.reserve(m_nClusters);
	if(!selectSeeds(&means))
		return false;
	GAssert(means.rows() == m_nClusters);

	// Do the clustering
	GTEMPBUF(size_t, pCounts, means.rows());
	double d, dBestDist;
	double* pVector;
	bool bChanged;
	size_t i;
	for(i = 0; i < (size_t)nMaxIterations; i++)
	{
		// Assign new cluster to each point
		bChanged = false;
		for(size_t j = 0; j < m_pData->rows(); j++)
		{
			pVector = m_pData->row(j);
			dBestDist = 1e200;
			size_t nCluster = 0;
			for(size_t k = 0; k < m_nClusters; k++)
			{
				d = GVec::squaredDistance(pVector, means.row(k), m_nDims);
				if(d < dBestDist)
				{
					dBestDist = d;
					nCluster = k;
				}
			}
			if(m_pClusters[j] != nCluster)
				bChanged = true;
			m_pClusters[j] = nCluster;
		}
		if(!bChanged)
			break;

		// Update the seeds
		for(size_t j = 0; j < means.rows(); j++)
			GVec::setAll(means.row(j), 0.0, m_nDims);
		memset(pCounts, '\0', sizeof(size_t) * means.rows());
		for(size_t j = 0; j < m_pData->rows(); j++)
		{
			GVec::add(means.row(m_pClusters[j]), m_pData->row(j), m_nDims);
			pCounts[m_pClusters[j]]++;
		}
		for(size_t j = 0; j < means.rows(); j++)
			GVec::multiply(means.row(j), 1.0 / pCounts[j], m_nDims);
	}
	return (i < (size_t)nMaxIterations);
}

// virtual
void GKMeans::cluster(GMatrix* pData)
{
	if(!pData->relation()->areContinuous(0, pData->cols()))
		ThrowError("GKMeans doesn't support nominal attributes. You should filter with the NominalToCat transform to convert nominal values to reals.");
	m_nDims = pData->relation()->size();
	if(pData->rows() < m_nClusters)
		throw "The number of clusters must be less than the number of data points";
	delete[] m_pClusters;
	m_pData = pData;
	m_pClusters = new size_t[pData->rows()];
	memset(m_pClusters, 0xff, sizeof(size_t) * pData->rows());
	size_t i;
	for(i = 0; i < 5; i++)
	{
		if(clusterAttempt(m_nDims * pData->rows()))
			break;
	}
	GAssert(i < 5);
}

// virtual
size_t GKMeans::whichCluster(size_t nVector)
{
	GAssert(nVector < m_pData->rows());
	return m_pClusters[nVector];
}


// -----------------------------------------------------------------------------------------

GKMedoids::GKMedoids(size_t clusters)
: GClusterer(clusters)
{
	m_pMedoids = new size_t[clusters];
	m_pMetric = NULL;
	m_pData = NULL;
}

// virtual
GKMedoids::~GKMedoids()
{
	delete[] m_pMedoids;
	delete(m_pMetric);
}

void GKMedoids::setMetric(GDissimilarityMetric* pMetric)
{
	delete(m_pMetric);
	m_pMetric = pMetric;
}

double GKMedoids::curErr()
{
	double err = 0;
	for(size_t i = 0; i < m_pData->rows(); i++)
	{
		whichCluster(i);
		err += m_d;
	}
	return err;
}

// virtual
void GKMedoids::cluster(GMatrix* pData)
{
	m_pData = pData;
	if(!m_pMetric)
		setMetric(new GRowDistance());
	m_pMetric->init(pData->relation());
	if(pData->rows() < (size_t)m_clusterCount)
		ThrowError("Fewer data point than clusters");
	for(size_t i = 0; i < m_clusterCount; i++)
		m_pMedoids[i] = i;
	double err = curErr();
	while(true)
	{
		bool improved = false;
		for(size_t i = 0; i < pData->rows(); i++)
		{
			// See if it's already a medoid
			size_t j;
			for(j = 0; j < m_clusterCount; j++)
			{
				if(m_pMedoids[j] == i)
					break;
			}
			if(j < m_clusterCount)
				continue;

			// Try this point in place of each medoid
			for(j = 0; j < m_clusterCount; j++)
			{
				size_t old = m_pMedoids[j];
				m_pMedoids[j] = i;
				double cand = curErr();
				if(cand < err)
				{
					err = cand;
					improved = true;
					break;
				}
				else
					m_pMedoids[j] = old;
			}
		}
		if(!improved)
			break;
	}
}

// virtual
size_t GKMedoids::whichCluster(size_t nVector)
{
	double* pVec = m_pData->row(nVector);
	size_t cluster = 0;
	m_d = m_pMetric->dissimilarity(pVec, m_pData->row(m_pMedoids[0]));
	for(size_t i = 1; i < m_clusterCount; i++)
	{
		double d = m_pMetric->dissimilarity(pVec, m_pData->row(m_pMedoids[i]));
		if(d < m_d)
		{
			m_d = d;
			cluster = i;
		}
	}
	return cluster;
}


// -----------------------------------------------------------------------------------------

GKMedoidsSparse::GKMedoidsSparse(size_t clusters)
: GSparseClusterer(clusters)
{
	m_pMedoids = new size_t[clusters];
	m_pMetric = NULL;
	m_ownMetric = false;
	m_pData = NULL;
}

// virtual
GKMedoidsSparse::~GKMedoidsSparse()
{
	delete[] m_pMedoids;
	delete(m_pMetric);
}

void GKMedoidsSparse::setMetric(GSparseSimilarity* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

double GKMedoidsSparse::curGoodness()
{
	double goodness = 0;
	for(size_t i = 0; i < m_pData->rows(); i++)
	{
		whichCluster(i);
		goodness += m_d;
	}
	return goodness;
}

// virtual
void GKMedoidsSparse::cluster(GSparseMatrix* pData)
{
	m_pData = pData;
	if(!m_pMetric)
		setMetric(new GCosineSimilarity(), true);
	if(pData->rows() < (size_t)m_clusterCount)
		ThrowError("Fewer data point than clusters");
	for(size_t i = 0; i < m_clusterCount; i++)
		m_pMedoids[i] = i;
	double goodness = curGoodness();
	while(true)
	{
		bool improved = false;
		for(size_t i = 0; i < pData->rows(); i++)
		{
			// See if it's already a medoid
			size_t j;
			for(j = 0; j < m_clusterCount; j++)
			{
				if(m_pMedoids[j] == i)
					break;
			}
			if(j < m_clusterCount)
				continue;

			// Try this point in place of each medoid
			for(j = 0; j < m_clusterCount; j++)
			{
				size_t old = m_pMedoids[j];
				m_pMedoids[j] = i;
				double cand = curGoodness();
				if(cand > goodness)
				{
					goodness = cand;
					improved = true;
					break;
				}
				else
					m_pMedoids[j] = old;
			}
		}
		if(!improved)
			break;
	}
}

// virtual
size_t GKMedoidsSparse::whichCluster(size_t nVector)
{
	std::map<size_t,double>& vec = m_pData->row(nVector);
	size_t cluster = 0;
	m_d = m_pMetric->similarity(vec, m_pData->row(m_pMedoids[0]));
	for(size_t i = 1; i < m_clusterCount; i++)
	{
		double d = m_pMetric->similarity(vec, m_pData->row(m_pMedoids[i]));
		if(d > m_d)
		{
			m_d = d;
			cluster = i;
		}
	}
	return cluster;
}


// -----------------------------------------------------------------------------------------

GKMeansSparse::GKMeansSparse(size_t nClusters, GRand* pRand)
: GSparseClusterer(nClusters)
{
	m_pRand = pRand;
	m_nClusters = nClusters;
	m_pClusters = NULL;
	m_pMetric = NULL;
	m_ownMetric = false;
}

GKMeansSparse::~GKMeansSparse()
{
	delete[] m_pClusters;
	if(m_ownMetric)
		delete(m_pMetric);
}

void GKMeansSparse::setMetric(GSparseSimilarity* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

// virtual
void GKMeansSparse::cluster(GSparseMatrix* pData)
{
	if(!m_pMetric)
		setMetric(new GCosineSimilarity(), true);

	// Pick the seeds (by randomly picking a known value for each element independently)
	size_t* pCounts = new size_t[pData->cols()];
	ArrayHolder<size_t> hCounts(pCounts);
	GMatrix means(0, pData->cols());
	means.newRows(m_nClusters);
	{
		for(size_t i = 0; i < m_nClusters; i++)
		{
			size_t* pC = pCounts;
			for(size_t j = 0; j < pData->cols(); j++)
				*(pC++) = 0;
			double* pMean = means.row(i);
			for(size_t k = 0; k < pData->rows(); k++)
			{
				GSparseMatrix::Iter it;
				for(it = pData->rowBegin(k); it != pData->rowEnd(k); it++)
				{
					if(m_pRand->next(pCounts[it->first] + 1) == 0)
					{
						pCounts[it->first]++;
						pMean[it->first] = it->second;
					}
				}
			}
		}
	}

	// Do the clustering
	delete[] m_pClusters;
	m_pClusters = new size_t[pData->rows()];
	memset(m_pClusters, 0xff, sizeof(size_t) * pData->rows());
	double bestSim = -1e300;
	size_t patience = 16;
	while(true)
	{
		// Determine the cluster of each point
		bool somethingChanged = false;
		double sumSim = 0.0;
		size_t* pClust = m_pClusters;
		for(size_t i = 0; i < pData->rows(); i++)
		{
			size_t oldClust = *pClust;
			*pClust = 0;
			double maxSimilarity = -1e300;
			map<size_t,double>& sparseRow = pData->row(i);
			for(size_t j = 0; j < m_nClusters; j++)
			{
				double sim = m_pMetric->similarity(sparseRow, means.row(j));
				if(sim > maxSimilarity)
				{
					maxSimilarity = sim;
					*pClust = j;
				}
			}
			if(*pClust != oldClust)
				somethingChanged = true;
			pClust++;
			sumSim += maxSimilarity;
		}
		if(!somethingChanged)
			break;
		if(sumSim > bestSim)
		{
			bestSim = sumSim;
			patience = 16;
		}
		else
		{
			if(--patience == 0)
				break;
		}

		// Update the means
		for(size_t j = 0; j < m_nClusters; j++)
		{
			memset(pCounts, '\0', sizeof(size_t) * pData->cols());
			size_t* pClust = m_pClusters;
			double* pMean = means.row(j);
			for(size_t i = 0; i < pData->rows(); i++)
			{
				if(*pClust != j)
					continue;

				// Update only the mean of the elements that this row specifies
				GSparseMatrix::Iter it;
				for(it = pData->rowBegin(i); it != pData->rowEnd(i); it++)
				{
					pMean[it->first] *= (pCounts[it->first] / (pCounts[it->first] + 1));
					pMean[it->first] += (1.0 / (pCounts[it->first] + 1) * it->second);
					pCounts[it->first]++;
				}
				pClust++;
			}
		}
	}
}

// virtual
size_t GKMeansSparse::whichCluster(size_t nVector)
{
	return m_pClusters[nVector];
}


// -----------------------------------------------------------------------------------------
/*
void BlurVector(size_t nDims, double* pInput, double* pOutput, double dAmount)
{
	double dWeight, dSumWeight;
	size_t i, j;
	for(i = 0; i < nDims; i++)
	{
		pOutput[i] = 0;
		dSumWeight = 0;
		for(j = 0; j < nDims; j++)
		{
			dWeight = GMath::gaussian((double)(j - i) / dAmount);
			dSumWeight += dWeight;
			pOutput[i] += dWeight * pInput[j];
		}
		pOutput[i] /= dSumWeight;
	}
}
*/
/*
void MakeHistogramWithGaussianParzenWindow(size_t nElements, double* pInput, double* pOutput, double dBlurAmount)
{
	size_t i, j;
	for(i = 0; i < nElements; i++)
	{
		pOutput[i] = 0;
		for(j = 0; j < nElements; j++)
			pOutput[i] += GMath::gaussian((pOutput[j] - pOutput[i]) / dBlurAmount);
	}
}

size_t CountLocalMaximums(size_t nElements, double* pData)
{
	if(nElements < 2)
		return nElements;
	size_t nCount = 0;
	if(pData[0] > pData[1])
		nCount++;
	size_t i;
	nElements--;
	for(i = 1; i < nElements; i++)
	{
		if(pData[i] > pData[i - 1] && pData[i] > pData[i + 1])
			nCount++;
	}
	if(pData[nElements] > pData[nElements - 1])
		nCount++;
	return nCount;
}
*/

// -----------------------------------------------------------------------------------------

GGraphCutTransducer::GGraphCutTransducer(size_t neighborCount, GRand* pRand)
: GTransducer(), m_neighborCount(neighborCount), m_pRand(pRand)
{
	m_pNeighbors = new size_t[neighborCount];
	m_pDistances = new double[neighborCount];
}

// virtual
GGraphCutTransducer::~GGraphCutTransducer()
{
	delete[] m_pNeighbors;
	delete[] m_pDistances;
}

// virtual
GMatrix* GGraphCutTransducer::transduce(GMatrix& features1, GMatrix& labels1, GMatrix& features2)
{
	if(labels1.cols() != 1)
		ThrowError("Only 1 nominal label dim is supported");

	// Use k-NN to compute a distance metric with good scale factors for prediction
	GKNN knn(m_neighborCount, m_pRand);
	knn.setOptimizeScaleFactors(true);
	knn.train(features1, labels1);
	GRowDistanceScaled* pMetric = knn.metric();

	// Merge the features into one dataset and build a kd-tree
	GMatrix both(features1.relation());
	GReleaseDataHolder hBoth(&both);
	both.reserve(features1.rows() + features2.rows());
	for(size_t i = 0; i < features1.rows(); i++)
		both.takeRow(features1[i]);
	for(size_t i = 0; i < features2.rows(); i++)
		both.takeRow(features2[i]);
	GRowDistanceScaled metric2;
	GKdTree neighborFinder(&both, m_neighborCount, &metric2, false);
	GVec::copy(metric2.scaleFactors(), pMetric->scaleFactors(), features1.cols());

	// Use max-flow/min-cut graph-cut to separate out each label value
	GMatrix* pOut = new GMatrix(labels1.relation());
	Holder<GMatrix> hOut(pOut);
	pOut->setAll(0);
	int valueCount = (int)labels1.relation()->valueCount(0);
	for(int val = 1; val < valueCount; val++)
	{
		// Add neighborhood edges
		GGraphCut gc(features1.rows() + features2.rows() + 2);
		for(size_t i = 0; i < both.rows(); i++)
		{
			neighborFinder.neighbors(m_pNeighbors, m_pDistances, i);
			for(size_t j = 0; j < m_neighborCount; j++)
			{
				if(m_pNeighbors[j] >= both.rows())
					continue;
				gc.addEdge(2 + i, 2 + m_pNeighbors[j], (float)(1.0 / std::max(sqrt(m_pDistances[j]), 1e-9))); // connect neighbors
			}
		}

		// Add source and sink edges
		for(size_t i = 0; i < features1.rows(); i++)
		{
			if((int)labels1[i][0] == val)
				gc.addEdge(0, 2 + (int)i, 1e12f); // connect to source
			else
				gc.addEdge(1, 2 + (int)i, 1e12f); // connect to sink
		}

		// Cut
		gc.cut(0, 1);

		// Label the unlabeled rows
		for(size_t i = 0; i < features2.rows(); i++)
		{
			if(gc.isSource(2 + features1.rows() + i))
				pOut->row(i)[0] = (double)val;
		}
	}
	return hOut.release();
}






/*
GNeuralTransducer::GNeuralTransducer(GRand* pRand)
: m_pRand(pRand)
{
	m_pNN = new GNeuralNet(m_pRand);
	m_pNN->setLearningRate(0.005);
}

// virtual
GNeuralTransducer::~GNeuralTransducer()
{
	delete(m_pNN);
}

void GNeuralTransducer::setParams(std::vector<size_t>& ranges)
{
	m_paramRanges.resize(ranges.size());
	vector<size_t>::iterator itSrc = ranges.begin();
	vector<size_t>::iterator itDst = m_paramRanges.begin();
	while(itSrc != ranges.end())
		*(itDst++) = *(itSrc++);
}

// virtual
void GNeuralTransducer::transduce(GMatrix* pDataLabeled, GMatrix* pDataUnlabeled, size_t labelDims)
{
	if(labelDims != 1)
		ThrowError("Sorry, only one label dim is currently supported");
	if(pDataLabeled->cols() != pDataUnlabeled->cols())
		ThrowError("mismatching number of columns");
	size_t featureDims = pDataLabeled->cols() - 1;
	int labelValues = pDataLabeled->relation()->valueCount(featureDims);
	if(labelValues < 1)
		ThrowError("expected the labels to be nominal");

	// Compute the number of pixels and channels
	size_t pixels = 1;
	for(size_t i = 0; i < m_paramRanges.size(); i++)
		pixels *= m_paramRanges[i];
	size_t channels = featureDims / pixels;
	if((channels * pixels) != featureDims)
		ThrowError("params don't align with the number of feature dims");
	size_t paramDims = m_paramRanges.size();

	// Make the initial cluster data
	GMatrix outData(paramDims + labelValues);
	size_t totalRows = pDataLabeled->rows() + pDataUnlabeled->rows();
	outData.newRows(pDataLabeled->rows() + pDataUnlabeled->rows());
	for(size_t i = 0; i < pDataLabeled->rows(); i++)
	{
		double* pVec = outData.row(i);
		int label = (int)pDataLabeled->row(i)[featureDims];
		if(label >= 0 && label < labelValues)
		{
			GVec::setAll(pVec, 0.0, labelValues);
			pVec[label] = 1.0;
		}
		else
			GVec::setAll(pVec, 1.0 / labelValues, labelValues);
	}
	{
		double dist;
		size_t neigh;
		GKdTree neighborFinder(pDataLabeled, 1, 1, NULL, false);
		for(size_t i = 0; i < pDataUnlabeled->rows(); i++)
		{
			double* pVec = outData.row(pDataLabeled->rows() + i);
			neighborFinder.neighbors(&neigh, &dist, pDataUnlabeled->row(i));
			int label = (int)pDataLabeled->row(neigh)[featureDims];
			if(label >= 0 && label < labelValues)
			{
				GVec::setAll(pVec, 0.0, labelValues);
				pVec[label] = 1.0;
			}
			else
				GVec::setAll(pVec, 1.0 / labelValues, labelValues);
		}
	}

	// Prepare for incremental learning
	sp_relation pRel = new GUniformRelation(paramDims + labelValues + channels);
	m_pNN->enableIncrementalLearning(pRel, channels, NULL, NULL);

	// Iterate
	GBackProp* pBP = m_pNN->backProp();
	GBackPropLayer& bpLayer = pBP->layer(0);
	GNeuralNetLayer& nnLayer = m_pNN->getLayer(0);
	GCoordVectorIterator cvi(m_paramRanges);
	double startTime = GTime::seconds();
	while(true)
	{
		size_t index = (size_t)m_pRand->next(totalRows);
		if(index == 0 && GTime::seconds() - startTime > 18 * 60 * 60)
			break;
		double* pRow = index < pDataLabeled->rows() ? pDataLabeled->row(index) : pDataUnlabeled->row(index - pDataLabeled->rows());
		double* pVec = outData.row(index);
		cvi.setRandom(m_pRand);
		cvi.currentNormalized(pVec);

		// Train the weights
		m_pNN->trainIncremental(pVec, pRow + channels * cvi.currentIndex());

		// Train the contexts
		for(size_t i = 0; i < labelValues; i++)
		{
			for(size_t j = 0; j < nnLayer.m_neurons.size(); j++)
				pVec[paramDims + i] += m_pNN->learningRate() * bpLayer.m_neurons[j].m_error * nnLayer.m_neurons[j].m_weights[1 + i];
		}

		// Use semi-supervision with the labeled contexts
		if(index < pDataLabeled->rows())
		{
			size_t mi = GVec::indexOfMax(pVec + paramDims, labelValues, m_pRand);
			int ti = (int)pRow[featureDims];
			if(ti != mi)
			{
				for(size_t i = 0; i < labelValues; i++)
				{
					if(i == ti)
						pVec[paramDims + i] += m_pNN->learningRate() * (1.0 - pVec[paramDims + i]);
					else
						pVec[paramDims + i] += m_pNN->learningRate() * (0.0 - pVec[paramDims + i]);
				}
			}
		}

		// Regularize the weights
		m_pNN->decayWeights(0.001);
	}

GTwtDoc doc;
doc.setRoot(m_pNN->toTwt(&doc));
doc.save("cluster_model.twt");

	// Deterimine the most likely label for each row
	for(size_t i = 0; i < pDataUnlabeled->rows(); i++)
	{
		double* pVec = outData.row(pDataLabeled->rows() + i);
		size_t mi = GVec::indexOfMax(pVec + paramDims, labelValues, m_pRand);
		pDataUnlabeled->row(i)[featureDims] = (double)mi;
	}
}
*/
