/*
	Copyright (C) 2010, Mike Gashler

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	see http://www.gnu.org/copyleft/lesser.html
*/

#include "GRecommender.h"
#include "GSparseMatrix.h"
#include "GCluster.h"
#include "GMatrix.h"
#include "GActivation.h"
#include "GHeap.h"
#include "GRand.h"
#include "GNeuralNet.h"
#include "GDistance.h"
#include "GVec.h"
#include <math.h>
#include <map>
#include <vector>
#include <cmath>
#include "GDom.h"
#include "GTime.h"

using std::multimap;
using std::vector;

namespace GClasses {

GCollaborativeFilter::GCollaborativeFilter(GDomNode* pNode)
{
}

GDomNode* GCollaborativeFilter::baseDomNode(GDom* pDoc, const char* szClassName)
{
	GDomNode* pNode = pDoc->newObj();
	pNode->addField(pDoc, "class", pDoc->newString(szClassName));
	return pNode;
}

double GCollaborativeFilter::crossValidate(GSparseMatrix* pData, size_t folds, GRand* pRand, size_t maxRecommendationsPerRow, double* pOutMAE)
{
	if(pData->defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");
	
	// Randomly assign each rating to one of the folds
	size_t users = pData->rows();
	size_t ratings = 0;
	for(size_t i = 0; i < users; i++)
		ratings += pData->rowNonDefValues(i);
	size_t* pFolds = new size_t[ratings];
	for(size_t i = 0; i < ratings; i++)
		pFolds[i] = (size_t)pRand->next(folds);

	// Make a copy of the sparse data
	GSparseMatrix clone(pData->rows(), pData->cols(), UNKNOWN_REAL_VALUE);
	clone.copyFrom(pData);

	// Evaluate accuracy
	double sse = 0.0;
	double se = 0.0;
	size_t hits = 0;
	for(size_t i = 0; i < folds; i++)
	{
		// Make a data set with ratings in the current fold removed
		size_t* pF = pFolds;
		for(size_t y = 0; y < users; y++) // for each user...
		{
			vector<size_t> condemnedCols;
			condemnedCols.reserve(clone.rowNonDefValues(y));
			for(GSparseMatrix::Iter rating = clone.rowBegin(y); rating != clone.rowEnd(y); rating++) // for each item that this user has rated...
			{
				if(*pF == i)
					condemnedCols.push_back(rating->first);
				pF++;
			}
			for(vector<size_t>::iterator it = condemnedCols.begin(); it != condemnedCols.end(); it++)
				clone.set(y, *it, UNKNOWN_REAL_VALUE); // remove the rating
		}

		// Train it
		trainBatch(&clone);

		// Predict the ratings in the current fold
		pF = pFolds;
		multimap<double,double> priQ;
		for(size_t y = 0; y < users; y++)
		{
			// Find the best recommendations for this user
			priQ.clear();
			for(GSparseMatrix::Iter rating = pData->rowBegin(y); rating != pData->rowEnd(y); rating++) // for each item that this user has rated...
			{
				if(*pF == i)
				{
					double prediction = predict(y, rating->first);
					priQ.insert(std::pair<double,double>(prediction, rating->second)); // <predicted-value,target-value>
					if(priQ.size() > maxRecommendationsPerRow)
						priQ.erase(priQ.begin()); // drop the pair with the lowest prediction
					clone.set(y, rating->first, rating->second); // Restore the rating to the cloned set
				}
				pF++;
			}

			// Evaluate them
			for(multimap<double,double>::iterator it = priQ.begin(); it != priQ.end(); it++)
			{
				double err = it->second - it->first; // error = target - prediction
				se += std::abs(err);
				sse += (err * err);
				hits++;
			}
		}
	}

	if(pOutMAE)
		*pOutMAE = se / hits;
	return sse / hits;
}

double GCollaborativeFilter::transduce(GSparseMatrix& train, GSparseMatrix& test, double* pOutMAE)
{
	if(train.defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");
	if(test.defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");
	if(train.rows() < test.rows())
		train.newRows(test.rows() - train.rows());

	// Train it
	trainBatch(&train);

	// Predict the ratings in the current fold
	double sse = 0.0;
	double se = 0.0;
	size_t hits = 0;
	for(size_t y = 0; y < test.rows(); y++)
	{
		for(GSparseMatrix::Iter rating = test.rowBegin(y); rating != test.rowEnd(y); rating++)
		{
			double err = rating->second - predict(y, rating->first); // error = target - prediction
			se += std::abs(err);
			sse += (err * err);
			hits++;
		}
	}

	if(pOutMAE)
		*pOutMAE = se / hits;
	return sse / hits;
}

class TarPredComparator
{
public:
	TarPredComparator() {}

	bool operator() (const std::pair<double,double>& a, const std::pair<double,double>& b) const
	{
		return a.second > b.second;
	}
};

GMatrix* GCollaborativeFilter::precisionRecall(GSparseMatrix* pData, GRand* pRand, bool ideal)
{
	if(pData->defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");
	size_t users = pData->rows();
	size_t ratings = 0;
	for(size_t i = 0; i < users; i++)
		ratings += pData->rowNonDefValues(i);
	size_t halfRatings = ratings / 2;
	size_t* pFolds = new size_t[ratings];
	size_t f0 = ratings - halfRatings;
	size_t f1 = halfRatings;
	for(size_t i = 0; i < ratings; i++)
	{
		if(pRand->next(f0 + f1) < f0)
		{
			pFolds[i] = 0;
			f0--;
		}
		else
		{
			pFolds[i] = 1;
			f1--;
		}
	}

	// Make a vector of target values and corresponding predictions
	vector<std::pair<double,double> > tarPred;
	tarPred.reserve(halfRatings);
	if(ideal)
	{
		// Simulate perfect predictions for all of the ratings in fold 1
		size_t* pF = pFolds;
		for(size_t y = 0; y < users; y++)
		{
			for(GSparseMatrix::Iter rating = pData->rowBegin(y); rating != pData->rowEnd(y); rating++) // for each item that this user has rated...
			{
				if(*pF != 0)
					tarPred.push_back(std::make_pair(rating->second, rating->second));
				pF++;
			}
		}
	}
	else
	{
		// Clone the data
		GSparseMatrix clone(pData->rows(), pData->cols(), UNKNOWN_REAL_VALUE);
		clone.copyFrom(pData);

		// Train with the ratings in fold 0
		size_t* pF = pFolds;
		size_t n = 0;
		for(size_t y = 0; y < users; y++) // for each user...
		{
			vector<size_t> condemnedCols;
			condemnedCols.reserve(clone.rowNonDefValues(y));
			for(GSparseMatrix::Iter rating = clone.rowBegin(y); rating != clone.rowEnd(y); rating++) // for each item that this user has rated...
			{
				GAssert(n < ratings);
				n++;
				if(*pF != 0)
					condemnedCols.push_back(rating->first);
				pF++;
			}
			for(vector<size_t>::iterator it = condemnedCols.begin(); it != condemnedCols.end(); it++)
				clone.set(y, *it, UNKNOWN_REAL_VALUE); // remove the rating
		}
		trainBatch(&clone);
	
		// Predict the ratings in fold 1
		pF = pFolds;
		for(size_t y = 0; y < users; y++)
		{
			for(GSparseMatrix::Iter rating = pData->rowBegin(y); rating != pData->rowEnd(y); rating++) // for each item that this user has rated...
			{
				if(*pF != 0)
				{
					double prediction = predict(y, rating->first);
					if(prediction == UNKNOWN_REAL_VALUE)
						prediction = 0.0;
					tarPred.push_back(std::make_pair(rating->second, prediction));
				}
				pF++;
			}
		}
	}

	// Make precision-recall data
	TarPredComparator comp;
	std::sort(tarPred.begin(), tarPred.end(), comp);
	double totalRelevant = 0.0;
	double totalIrrelevant = 0.0;
	for(vector<std::pair<double,double> >::iterator it = tarPred.begin(); it != tarPred.end(); it++)
	{
		totalRelevant += it->first;
		totalIrrelevant += (1.0 - it->first); // Here we assume that all ratings range from 0 to 1.
	}
	double retrievedRelevant = 0.0;
	double retrievedIrrelevant = 0.0;
	GMatrix* pResults = new GMatrix(0, 3);
	for(vector<std::pair<double,double> >::iterator it = tarPred.begin(); it != tarPred.end(); it++)
	{
		retrievedRelevant += it->first;
		retrievedIrrelevant += (1.0 - it->first); // Here we assume that all ratings range from 0 to 1.
		double precision = retrievedRelevant / (retrievedRelevant + retrievedIrrelevant);
		double recall = retrievedRelevant / totalRelevant; // recall is the same as the truePositiveRate
		double falsePositiveRate = retrievedIrrelevant / totalIrrelevant;
		double* pRow = pResults->newRow();
		pRow[0] = recall;
		pRow[1] = precision;
		pRow[2] = falsePositiveRate;
	}
	return pResults;
}

// static
double GCollaborativeFilter::areaUnderCurve(GMatrix* pData)
{
	double a = 0.0;
	double b = 0.0;
	double prevX = 0.0;
	double prevY = 0.0;
	for(size_t i = 0; i < pData->rows(); i++)
	{
		double* pRow = pData->row(i);
		a += (pRow[2] - prevX) * pRow[0];
		b += (pRow[2] - prevX) * prevY;
		prevX = pRow[2];
		prevY = pRow[0];
	}
	a += 1.0 - prevX;
	b += (1.0 - prevX) * prevY;
	return 0.5 * (a + b);
}






GBaselineRecommender::GBaselineRecommender()
: GCollaborativeFilter(), m_pRatings(NULL), m_items(0)
{
}

GBaselineRecommender::GBaselineRecommender(GDomNode* pNode)
: GCollaborativeFilter(pNode)
{
	GDomListIterator it(pNode->field("ratings"));
	m_items = it.remaining();
	m_pRatings = new double[m_items];
	GVec::deserialize(m_pRatings, m_items, it);
}

// virtual
GBaselineRecommender::~GBaselineRecommender()
{
	delete[] m_pRatings;
}

// virtual
void GBaselineRecommender::trainBatch(GSparseMatrix* pData)
{
	delete[] m_pRatings;
	m_items = pData->cols();
	m_pRatings = new double[m_items];
	size_t* pCounts = new size_t[m_items];
	ArrayHolder<size_t> hCounts(pCounts);
	for(size_t i = 0; i < m_items; i++)
	{
		pCounts[i] = 0;
		m_pRatings[i] = 0.0;
	}
	size_t users = pData->rows();
	for(size_t y = 0; y < users; y++)
	{
		for(GSparseMatrix::Iter rating = pData->rowBegin(y); rating != pData->rowEnd(y); rating++) // for each item that this user has rated...
		{
			m_pRatings[rating->first] *= ((double)pCounts[rating->first] / (pCounts[rating->first] + 1));
			m_pRatings[rating->first] += (rating->second / (pCounts[rating->first] + 1));
			pCounts[rating->first]++;
		}
	}
}

// virtual
double GBaselineRecommender::predict(size_t user, size_t item)
{
	if(item >= m_items)
		ThrowError("item out of range");
	return m_pRatings[item];
}

// virtual
void GBaselineRecommender::impute(double* pVec)
{
	for(size_t i = 0; i < m_items; i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
			pVec[i] = m_pRatings[i];
	}
}

// virtual
GDomNode* GBaselineRecommender::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GBaselineRecommender");
	pNode->addField(pDoc, "ratings", GVec::serialize(pDoc, m_pRatings, m_items));
	return pNode;
}







GInstanceRecommender::GInstanceRecommender(size_t neighbors)
: GCollaborativeFilter(), m_neighbors(neighbors), m_ownMetric(true), m_pData(NULL), m_pBaseline(NULL)
{
	m_pMetric = new GCosineSimilarity();
}

// virtual
GInstanceRecommender::~GInstanceRecommender()
{
	delete(m_pData);
	if(m_ownMetric)
		delete(m_pMetric);
	delete[] m_pBaseline;
}

void GInstanceRecommender::setMetric(GSparseSimilarity* pMetric, bool own)
{
	if(m_ownMetric)
		delete(m_pMetric);
	m_pMetric = pMetric;
	m_ownMetric = own;
}

// virtual
void GInstanceRecommender::trainBatch(GSparseMatrix* pData)
{
	// Compute the baseline recommendations
	delete[] m_pBaseline;
	size_t items = pData->cols();
	m_pBaseline = new double[items];
	size_t* pCounts = new size_t[items];
	ArrayHolder<size_t> hCounts(pCounts);
	for(size_t i = 0; i < items; i++)
	{
		pCounts[i] = 0;
		m_pBaseline[i] = 0.0;
	}
	size_t users = pData->rows();
	for(size_t y = 0; y < users; y++)
	{
		for(GSparseMatrix::Iter rating = pData->rowBegin(y); rating != pData->rowEnd(y); rating++) // for each item that this user has rated...
		{
			m_pBaseline[rating->first] *= ((double)pCounts[rating->first] / (pCounts[rating->first] + 1));
			m_pBaseline[rating->first] += (rating->second / (pCounts[rating->first] + 1));
			pCounts[rating->first]++;
		}
	}

	// Store the data
	if(pData->defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");
	delete(m_pData);

	// copy the data
	m_pData = new GSparseMatrix(pData->rows(), pData->cols(), UNKNOWN_REAL_VALUE);
	m_pData->copyFrom(pData);
}

// virtual
double GInstanceRecommender::predict(size_t user, size_t item)
{
	if(!m_pData)
		ThrowError("This model has not been trained");

	// Find the k-nearest neighbors
	multimap<double,size_t> depq; // double-ended priority-queue that maps from similarity to user-id
	for(size_t neigh = 0; neigh < m_pData->rows(); neigh++)
	{
		// Only consider other users that have rated this item
		if(neigh == user)
			continue;
		double rating = m_pData->get(neigh, item);
		if(rating == UNKNOWN_REAL_VALUE)
			continue;

		// Compute the similarity
		double similarity = m_pMetric->similarity(m_pData->row(user), m_pData->row(neigh));

		// If the queue is overfull, drop the worst item
		depq.insert(std::make_pair(similarity, neigh));
		if(depq.size() > m_neighbors)
			depq.erase(depq.begin());
	}

	// Combine the ratings of the nearest neighbors to make a prediction
	double weighted_sum = 0.0;
	double sum_weight = 0.0;
	for(multimap<double,size_t>::iterator it = depq.begin(); it != depq.end(); it++)
	{
		double weight = std::max(0.0, std::min(1.0, it->first));
		double val = m_pData->get(it->second, item);
		weighted_sum += weight * val;
		sum_weight += weight;
	}
	if(sum_weight > 0.0)
		return weighted_sum / sum_weight;
	else
		return m_pBaseline[item];
}

// virtual
void GInstanceRecommender::impute(double* pVec)
{
	if(!m_pData)
		ThrowError("This model has not been trained");

	// Find the k-nearest neighbors
	multimap<double,size_t> depq; // double-ended priority-queue that maps from similarity to user-id
	for(size_t neigh = 0; neigh < m_pData->rows(); neigh++)
	{
		// Compute the similarity
		double similarity = m_pMetric->similarity(m_pData->row(neigh), pVec);

		// If the queue is overfull, drop the worst item
		depq.insert(std::make_pair(similarity, neigh));
		if(depq.size() > m_neighbors)
			depq.erase(depq.begin());
	}

	// Impute missing values by combining the ratings from the neighbors
	for(size_t i = 0; i < m_pData->cols(); i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
		{
			double weighted_sum = 0.0;
			double sum_weight = 0.0;
			for(multimap<double,size_t>::iterator it = depq.begin(); it != depq.end(); it++)
			{
				double val = m_pData->get(it->second, i);
				if(val != UNKNOWN_REAL_VALUE)
				{
					double weight = std::max(0.0, std::min(1.0, it->first));
					weighted_sum += weight * val;
					sum_weight += weight;
				}
			}
			if(sum_weight > 0.0)
				pVec[i] = weighted_sum / sum_weight;
			else
				pVec[i] = m_pBaseline[i];
		}
	}
}

// virtual
GDomNode* GInstanceRecommender::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GInstanceRecommender");
	ThrowError("Sorry, this method has not been implemented yet");
	return pNode;
}






GSparseClusterRecommender::GSparseClusterRecommender(size_t clusters, GRand* pRand)
: GCollaborativeFilter(), m_clusters(clusters), m_pPredictions(NULL), m_pClusterer(NULL), m_ownClusterer(false), m_pRand(pRand)
{
}

// virtual
GSparseClusterRecommender::~GSparseClusterRecommender()
{
	if(m_ownClusterer)
		delete(m_pClusterer);
	delete(m_pPredictions);
}

void GSparseClusterRecommender::setClusterer(GSparseClusterer* pClusterer, bool own)
{
	if(pClusterer->clusterCount() != m_clusters)
		ThrowError("Mismatching number of clusters");
	if(m_ownClusterer)
		delete(m_pClusterer);
	m_pClusterer = pClusterer;
	m_ownClusterer = own;
}

// virtual
void GSparseClusterRecommender::trainBatch(GSparseMatrix* pData)
{
	if(!m_pClusterer)
		setClusterer(new GKMeansSparse(m_clusters, m_pRand), true);

	// Cluster the data
	m_pClusterer->cluster(pData);

	// Gather the mean predictions in each cluster
	delete(m_pPredictions);
	m_pPredictions = new GMatrix(m_clusters, pData->cols());
	m_pPredictions->setAll(0.0);
	size_t* pCounts = new size_t[pData->cols() * m_clusters];
	ArrayHolder<size_t> hCounts(pCounts);
	memset(pCounts, '\0', sizeof(size_t) * pData->cols() * m_clusters);
	for(size_t i = 0; i < pData->rows(); i++)
	{
		size_t clust = m_pClusterer->whichCluster(i);
		double* pRow = m_pPredictions->row(clust);
		size_t* pRowCounts = pCounts + (pData->cols() * clust);
		for(GSparseMatrix::Iter it = pData->rowBegin(i); it != pData->rowEnd(i); it++)
		{
			pRow[it->first] *= ((double)pRowCounts[it->first] / (pRowCounts[it->first] + 1));
			pRow[it->first] += (it->second / (pRowCounts[it->first] + 1));
			pRowCounts[it->first]++;
		}
	}
}

// virtual
double GSparseClusterRecommender::predict(size_t user, size_t item)
{
	size_t clust = m_pClusterer->whichCluster(user);
	double* pRow = m_pPredictions->row(clust);
	return pRow[item];
}

// virtual
void GSparseClusterRecommender::impute(double* pVec)
{
	ThrowError("Sorry, GSparseClusterRecommender::impute is not yet implemented");
	// todo: Find the closest centroid, and use it to impute all values
}

// virtual
GDomNode* GSparseClusterRecommender::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GSparseClusterRecommender");
	ThrowError("Sorry, this method has not been implemented yet");
	return pNode;
}




GDenseClusterRecommender::GDenseClusterRecommender(size_t clusters, GRand* pRand)
: GCollaborativeFilter(), m_clusters(clusters), m_pPredictions(NULL), m_pClusterer(NULL), m_ownClusterer(false), m_pRand(pRand)
{
}

// virtual
GDenseClusterRecommender::~GDenseClusterRecommender()
{
	if(m_ownClusterer)
		delete(m_pClusterer);
	delete(m_pPredictions);
}

void GDenseClusterRecommender::setClusterer(GClusterer* pClusterer, bool own)
{
	if(pClusterer->clusterCount() != m_clusters)
		ThrowError("Mismatching number of clusters");
	if(m_ownClusterer)
		delete(m_pClusterer);
	m_pClusterer = pClusterer;
	m_ownClusterer = own;
}

// virtual
void GDenseClusterRecommender::trainBatch(GSparseMatrix* pData)
{
	if(!m_pClusterer)
		setClusterer(new GFuzzyKMeans(m_clusters, m_pRand), true);

	// Cluster the data
	{
		GMatrix* pDenseMatrix = pData->toFullMatrix();
		Holder<GMatrix> hDenseMatrix(pDenseMatrix);
		m_pClusterer->cluster(pDenseMatrix);
	}

	// Gather the mean predictions in each cluster
	delete(m_pPredictions);
	m_pPredictions = new GMatrix(m_clusters, pData->cols());
	m_pPredictions->setAll(0.0);
	size_t* pCounts = new size_t[pData->cols() * m_clusters];
	ArrayHolder<size_t> hCounts(pCounts);
	memset(pCounts, '\0', sizeof(size_t) * pData->cols() * m_clusters);
	for(size_t i = 0; i < pData->rows(); i++)
	{
		size_t clust = m_pClusterer->whichCluster(i);
		double* pRow = m_pPredictions->row(clust);
		size_t* pRowCounts = pCounts + (pData->cols() * clust);
		for(GSparseMatrix::Iter it = pData->rowBegin(i); it != pData->rowEnd(i); it++)
		{
			pRow[it->first] *= ((double)pRowCounts[it->first] / (pRowCounts[it->first] + 1));
			pRow[it->first] += (it->second / (pRowCounts[it->first] + 1));
			pRowCounts[it->first]++;
		}
	}
}

// virtual
double GDenseClusterRecommender::predict(size_t user, size_t item)
{
	size_t clust = m_pClusterer->whichCluster(user);
	double* pRow = m_pPredictions->row(clust);
	return pRow[item];
}

// virtual
void GDenseClusterRecommender::impute(double* pVec)
{
	ThrowError("Sorry, GDenseClusterRecommender::impute is not yet implemented");
	// todo: Find the closest centroid, and use it to impute all values
}

// virtual
GDomNode* GDenseClusterRecommender::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GDenseClusterRecommender");
	ThrowError("Sorry, this method has not been implemented yet");
	return pNode;
}




class Rating
{
public:
	size_t m_user;
	size_t m_item;
	double m_rating;
};

GMatrixFactorization::GMatrixFactorization(size_t intrinsicDims, GRand& rand)
: GCollaborativeFilter(), m_intrinsicDims(intrinsicDims), m_regularizer(0.01), m_pP(NULL), m_pQ(NULL), m_rand(rand), m_useInputBias(true)
{
}

GMatrixFactorization::GMatrixFactorization(GDomNode* pNode, GRand& rand)
: GCollaborativeFilter(pNode), m_rand(rand)
{
	m_regularizer = pNode->field("reg")->asDouble();
	m_useInputBias = pNode->field("uib")->asBool();
	m_pP = new GMatrix(pNode->field("p"));
	m_pQ = new GMatrix(pNode->field("q"));
	if(m_pP->cols() != m_pQ->cols())
		ThrowError("Mismatching matrix sizes");
	m_intrinsicDims = m_pP->cols() - 1;
}

// virtual
GMatrixFactorization::~GMatrixFactorization()
{
	delete(m_pQ);
	delete(m_pP);
}

// virtual
GDomNode* GMatrixFactorization::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GMatrixFactorization");
	pNode->addField(pDoc, "reg", pDoc->newDouble(m_regularizer));
	pNode->addField(pDoc, "uib", pDoc->newBool(m_useInputBias));
	pNode->addField(pDoc, "p", m_pP->serialize(pDoc));
	pNode->addField(pDoc, "q", m_pQ->serialize(pDoc));
	return pNode;
}

double GMatrixFactorization::validate(vector<Rating*>& data)
{
	double sse = 0;
	for(vector<Rating*>::iterator it = data.begin(); it != data.end(); it++)
	{
		Rating* pRating = *it;
		double* pPref = m_pP->row(pRating->m_user);
		double* pWeights = m_pQ->row(pRating->m_item);
		double pred = *(pWeights++) + *(pPref++);
		for(size_t i = 0; i < m_intrinsicDims; i++)
			pred += *(pPref++) * (*pWeights++);
		double err = pRating->m_rating - pred;
		sse += (err * err);
	}
	return sse;
}

void GMatrixFactorization_sparseMatrixToRatings(GSparseMatrix& data, GHeap& heap, vector<Rating*>& train, GRand& rand)
{
	for(size_t user = 0; user < data.rows(); user++)
	{
		for(GSparseMatrix::Iter it = data.rowBegin(user); it != data.rowEnd(user); it++)
		{
			Rating* pRating = (Rating*)heap.allocAligned(sizeof(Rating));
			train.push_back(pRating);
			pRating->m_user = user;
			pRating->m_item = it->first;
			pRating->m_rating = it->second;
		}
	}
}

void GMatrixFactorization_vectorToRatings(double* pVec, size_t dims, GHeap& heap, vector<Rating*>& train, GRand& rand)
{
	for(size_t i = 0; i < dims; i++)
	{
		if(pVec[i] != UNKNOWN_REAL_VALUE)
		{
			Rating* pRating = (Rating*)heap.allocAligned(sizeof(Rating));
			train.push_back(pRating);
			pRating->m_user = 0;
			pRating->m_item = i;
			pRating->m_rating = pVec[i];
		}
	}
}

// virtual
void GMatrixFactorization::trainBatch(GSparseMatrix* pData)
{
	// Make a single list of all the ratings
	GHeap heap(2048);
	vector<Rating*> train;
	GMatrixFactorization_sparseMatrixToRatings(*pData, heap, train, m_rand);

	// Initialize P with small random values, and Q with zeros
	delete(m_pP);
	size_t colsP = (m_useInputBias ? 1 : 0) + m_intrinsicDims;
	m_pP = new GMatrix(pData->rows(),  colsP);
	for(size_t i = 0; i < m_pP->rows(); i++)
	{
		double* pVec = m_pP->row(i);
		for(size_t j = 0; j < colsP; j++)
			*(pVec++) = 0.02 * m_rand.normal();
	}
	delete(m_pQ);
	m_pQ = new GMatrix(pData->cols(), 1 + m_intrinsicDims);
	for(size_t i = 0; i < m_pQ->rows(); i++)
	{
		double* pVec = m_pQ->row(i);
		for(size_t j = 0; j <= m_intrinsicDims; j++)
			*(pVec++) = 0.02 * m_rand.normal();
	}

	// Train
	double bestErr = 1e308;
	double prevErr = 1e308;
	GMatrix* pBestP = NULL;
	GMatrix* pBestQ = NULL;
	double learningRate = 0.01;
	GTEMPBUF(double, temp_weights, m_intrinsicDims);
	size_t epochs = 0;
	while(learningRate >= 0.002)
	{
		// Shuffle the ratings
		for(size_t n = train.size(); n > 0; n--)
			std::swap(train[(size_t)m_rand.next(n)], train[n - 1]);

		// Do an epoch of training
		for(vector<Rating*>::iterator it = train.begin(); it != train.end(); it++)
		{
			Rating* pRating = *it;

			// Compute the error for this rating
			double* pPref = m_pP->row(pRating->m_user);
			double* pWeights = m_pQ->row(pRating->m_item);
			double pred = *(pWeights++);
			if(m_useInputBias)
				pred += *(pPref++);
			for(size_t i = 0; i < m_intrinsicDims; i++)
				pred += *(pPref++) * (*pWeights++);
			double err = pRating->m_rating - pred;
			GAssert(std::abs(err) < 50);

			// Update Q
			pPref = m_pP->row(pRating->m_user) + (m_useInputBias ? 1 : 0);
			double* pT = temp_weights;
			pWeights = m_pQ->row(pRating->m_item);
			*pWeights += learningRate * (err - m_regularizer * (*pWeights));
			pWeights++;
			for(size_t i = 0; i < m_intrinsicDims; i++)
			{
				*(pT++) = *pWeights;
				*pWeights += learningRate * (err * (*pPref) - m_regularizer * (*pWeights));
				pPref++;
				pWeights++;
			}

			// Update P
			pWeights = temp_weights;
			pPref = m_pP->row(pRating->m_user);
			if(m_useInputBias)
			{
				*pPref += learningRate * (err - m_regularizer * (*pPref));
				pPref++;
			}
			for(size_t i = 0; i < m_intrinsicDims; i++)
			{
				*pPref += learningRate * (err * (*pWeights) - m_regularizer * (*pPref));
				pWeights++;
				pPref++;
			}
		}
		epochs++;

		// Store the best factors
		double rsse = sqrt(validate(train));
		if(rsse < bestErr)
		{
			bestErr = rsse;
			delete(pBestP);
			delete(pBestQ);
			pBestP = m_pP->clone();
			pBestQ = m_pQ->clone();
		}

		// Stopping criteria
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.001) // If the amount of improvement is less than 0.1%
			learningRate *= 0.7; // decay the learning rate
		prevErr = rsse;
	}

	// Accept the best factors
	if(pBestP)
	{
		delete(m_pP);
		delete(m_pQ);
		m_pP = pBestP;
		m_pQ = pBestQ;
	}
}

// virtual
double GMatrixFactorization::predict(size_t user, size_t item)
{
	if(!m_pP)
		ThrowError("Not trained yet");
	double* pWeights = m_pQ->row(item);
	double* pPref = m_pP->row(user);
	double pred = *(pWeights++);
	if(m_useInputBias)
		pred += *(pPref++);
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pred += *(pPref++) * (*pWeights++);
	return pred;
}

// virtual
void GMatrixFactorization::impute(double* pVec)
{
	if(!m_pP)
		ThrowError("Not trained yet");

	// Make a single list of all the ratings
	GHeap heap(2048);
	vector<Rating*> ratings;
	GMatrixFactorization_vectorToRatings(pVec, m_pQ->rows(), heap, ratings, m_rand);

	// Initialize a preference vector
	GTEMPBUF(double, pPrefVec, 1 + m_intrinsicDims);
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pPrefVec[i] = 0.02 * m_rand.normal();

	// Refine the preference vector
	double prevErr = 1e308;
	double learningRate = 0.01;
	while(learningRate >= 0.002)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_rand.next(n)], ratings[n - 1]);

		// Do an epoch of training
		double sse = 0;
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;

			// Compute the error for this rating
			double* pPref = pPrefVec;
			double* pWeights = m_pQ->row(pRating->m_item);
			double pred = *(pWeights++);
			if(m_useInputBias)
				pred += *(pPref++);
			for(size_t i = 0; i < m_intrinsicDims; i++)
				pred += *(pPref++) * (*pWeights++);
			double err = pRating->m_rating - pred;
			sse += (err * err);

			// Update the preference vec
			pWeights = m_pQ->row(pRating->m_item) + 1;
			pPref = pPrefVec;
			if(m_useInputBias)
			{
				*pPref += learningRate * err; // regularization is intentionally not used here
				pPref++;
			}
			for(size_t i = 0; i < m_intrinsicDims; i++)
			{
				*pPref += learningRate * err * (*pWeights); // regularization is intentionally not used here
				pWeights++;
				pPref++;
			}
		}

		// Stopping criteria
		double rsse = sqrt(sse);
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.001) // If the amount of improvement is less than 0.1%
			learningRate *= 0.7; // decay the learning rate
		prevErr = rsse;
	}

	// Impute missing values
	for(size_t i = 0; i < m_pQ->rows(); i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
		{
			double* pWeights = m_pQ->row(i);
			double* pPref = pPrefVec;
			double pred = *(pWeights++);
			if(m_useInputBias)
				pred += *(pPref++);
			for(size_t j = 0; j < m_intrinsicDims; j++)
				pred += *(pPref++) * (*pWeights++);
			pVec[i] = pred;
		}
	}
}






GNeuralRecommender::GNeuralRecommender(size_t intrinsicDims, GRand* pRand)
: GCollaborativeFilter(), m_intrinsicDims(intrinsicDims), m_pMins(NULL), m_pMaxs(NULL), m_pRand(pRand), m_useInputBias(true)
{
	m_pModel = new GNeuralNet(m_pRand);
	m_pUsers = NULL;
}

GNeuralRecommender::GNeuralRecommender(GDomNode* pNode, GRand& rand)
: GCollaborativeFilter(pNode), m_pRand(&rand)
{
	m_useInputBias = pNode->field("uib")->asBool();
	m_pUsers = new GMatrix(pNode->field("users"));
	m_pModel = new GNeuralNet(pNode->field("model"), m_pRand);
	size_t itemCount = m_pModel->layer(m_pModel->layerCount() - 1).m_neurons.size();
	m_pMins = new double[itemCount];
	GDomListIterator it1(pNode->field("mins"));
	GVec::deserialize(m_pMins, itemCount, it1);
	m_pMaxs = new double[itemCount];
	GDomListIterator it2(pNode->field("maxs"));
	GVec::deserialize(m_pMaxs, itemCount, it2);
	m_intrinsicDims = m_pModel->layer(0).m_neurons.size();
}

// virtual
GNeuralRecommender::~GNeuralRecommender()
{
	delete[] m_pMins;
	delete[] m_pMaxs;
	delete(m_pModel);
	delete(m_pUsers);
}

// virtual
GDomNode* GNeuralRecommender::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GNeuralRecommender");
	pNode->addField(pDoc, "uib", pDoc->newBool(m_useInputBias));
	pNode->addField(pDoc, "users", m_pUsers->serialize(pDoc));
	pNode->addField(pDoc, "model", m_pModel->serialize(pDoc));
	size_t itemCount = m_pModel->layer(m_pModel->layerCount() - 1).m_neurons.size();
	pNode->addField(pDoc, "mins", GVec::serialize(pDoc, m_pMins, itemCount));
	pNode->addField(pDoc, "maxs", GVec::serialize(pDoc, m_pMaxs, itemCount));
	return pNode;
}

double GNeuralRecommender::validate(vector<Rating*>& data)
{
	double sse = 0;
	for(vector<Rating*>::iterator it = data.begin(); it != data.end(); it++)
	{
		Rating* pRating = *it;
		double* pUserPreferenceVector = m_pUsers->row(pRating->m_user);
		double predictedRating = m_pModel->forwardPropSingleOutput(pUserPreferenceVector, pRating->m_item);
		double d = pRating->m_rating - predictedRating;
		sse += (d * d);
	}
	return sse;
}

// virtual
void GNeuralRecommender::trainBatch(GSparseMatrix* pData)
{
	if(pData->defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");

	// Use Matrix-factorization to compute the user preference vectors
	GMatrixFactorization mf(m_intrinsicDims - (m_useInputBias ? 1 : 0), *m_pRand);
	if(!m_useInputBias)
		mf.noInputBias();
	mf.trainBatch(pData);
	delete(m_pUsers);
	m_pUsers = mf.getP()->clone();

	// Prep the model for incremental training
	sp_relation pFeatureRel = new GUniformRelation(m_intrinsicDims);
	sp_relation pLabelRel = new GUniformRelation(pData->cols());
	m_pModel->setUseInputBias(m_useInputBias);
	m_pModel->beginIncrementalLearning(pFeatureRel, pLabelRel);

	// Make a single list of all the ratings
	GHeap heap(2048);
	vector<Rating*> ratings;
	GMatrixFactorization_sparseMatrixToRatings(*pData, heap, ratings, *m_pRand);

	// Normalize the ratings
	m_pMins = new double[pData->cols()];
	m_pMaxs = new double[pData->cols()];
	GVec::setAll(m_pMins, 1e200, pData->cols());
	GVec::setAll(m_pMaxs, -1e200, pData->cols());
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		m_pMins[pRating->m_item] = std::min(m_pMins[pRating->m_item], pRating->m_rating);
		m_pMaxs[pRating->m_item] = std::max(m_pMaxs[pRating->m_item], pRating->m_rating);
	}
	for(size_t i = 0; i < pData->cols(); i++)
	{
		if(m_pMins[i] >= 1e200)
			m_pMins[i] = 0.0;
		if(m_pMaxs[i] < m_pMins[i] + 1e-12)
			m_pMaxs[i] = m_pMins[i] + 1.0;
	}
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		pRating->m_rating = (pRating->m_rating - m_pMins[pRating->m_item]) / (m_pMaxs[pRating->m_item] - m_pMins[pRating->m_item]);
	}

	// Train just the weights
double regularizer = 0.0015;
	double* pBestWeights = NULL;
	size_t weightCount = m_pModel->countWeights();
	double prevErr = 1e308;
	double bestErr = 1e308;
	double learningRate = 0.1;
	while(learningRate >= 0.001)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_pRand->next(n)], ratings[n - 1]);

		// Do an epoch of training
		m_pModel->setLearningRate(learningRate);
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;
			double* pUserPreferenceVector = m_pUsers->row(pRating->m_user);
			m_pModel->forwardPropSingleOutput(pUserPreferenceVector, pRating->m_item);
			m_pModel->setErrorSingleOutput(pRating->m_rating, pRating->m_item, m_pModel->backPropTargetFunction());
			m_pModel->backProp()->backpropagateSingleOutput(pRating->m_item);
m_pModel->decayWeightsSingleOutput(pRating->m_item, regularizer);
			m_pModel->backProp()->descendGradientSingleOutput(pRating->m_item, pUserPreferenceVector, learningRate, m_pModel->momentum(), m_pModel->useInputBias());
		}

		// Stopping criteria
		double rsse = sqrt(validate(ratings));
		if(rsse < bestErr)
		{
			bestErr = rsse;
			if(!pBestWeights)
				pBestWeights = new double[weightCount];
			m_pModel->weights(pBestWeights);
		}
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.00001) // If the amount of improvement is less than 0.001%
			learningRate *= 0.7; // decay the learning rate
		prevErr = rsse;
	}
	GMatrix* pBestUsers = m_pUsers->clone();

	// Now refine both item weights and user preferences
	learningRate = 0.01;
	while(learningRate >= 0.0005)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_pRand->next(n)], ratings[n - 1]);

		// Do an epoch of training
		m_pModel->setLearningRate(learningRate);
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;
			double* pUserPreferenceVector = m_pUsers->row(pRating->m_user);
			m_pModel->forwardPropSingleOutput(pUserPreferenceVector, pRating->m_item);
			m_pModel->setErrorSingleOutput(pRating->m_rating, pRating->m_item, m_pModel->backPropTargetFunction());
			m_pModel->backProp()->backpropagateSingleOutput(pRating->m_item);
m_pModel->decayWeightsSingleOutput(pRating->m_item, regularizer);
			m_pModel->backProp()->descendGradientSingleOutput(pRating->m_item, pUserPreferenceVector, learningRate, m_pModel->momentum(), m_pModel->useInputBias());
GVec::multiply(pUserPreferenceVector, 1.0 - learningRate * regularizer, m_intrinsicDims);
			m_pModel->backProp()->adjustFeaturesSingleOutput(pRating->m_item, pUserPreferenceVector, learningRate, m_pModel->useInputBias());
//			GVec::floorValues(pUserPreferenceVector, floor, m_intrinsicDims);
//			GVec::capValues(pUserPreferenceVector, cap, m_intrinsicDims);
		}

		// Stopping criteria
		double rsse = sqrt(validate(ratings));
		if(rsse < bestErr)
		{
			bestErr = rsse;
			delete(pBestUsers);
			pBestUsers = m_pUsers->clone();
			if(!pBestWeights)
				pBestWeights = new double[weightCount];
			m_pModel->weights(pBestWeights);
		}
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.00001) // If the amount of improvement is less than 0.01%
			learningRate *= 0.7; // decay the learning rate
		prevErr = rsse;
	}

	if(pBestWeights)
	{
		m_pModel->setWeights(pBestWeights);
		delete[] pBestWeights;
		delete(m_pUsers);
		m_pUsers = pBestUsers;
	}
}

/*
// virtual
void GNeuralRecommender::trainBatch(GSparseMatrix* pData)
{
	if(pData->defaultValue() != UNKNOWN_REAL_VALUE)
		ThrowError("Expected the default value to be UNKNOWN_REAL_VALUE");

	// Initialize the user preference vectors
	delete(m_pUsers);
	m_pUsers = new GMatrix(pData->rows(), m_intrinsicDims);

	// Prep the model for incremental training
	sp_relation pFeatureRel = new GUniformRelation(m_intrinsicDims);
	sp_relation pLabelRel = new GUniformRelation(pData->cols());
	m_pModel->setUseInputBias(true);
	m_pModel->beginIncrementalLearning(pFeatureRel, pLabelRel);
	GActivationFunction* pAF = m_pModel->layer(0).m_pActivationFunction;
	for(size_t i = 0; i < m_pUsers->rows(); i++)
	{
		double* pVec = m_pUsers->row(i);
		for(size_t j = 0; j < m_intrinsicDims; j++)
			*(pVec++) = pAF->center() + 0.25 * m_pRand->normal();
	}

	// Make a single list of all the ratings
	GHeap heap(2048);
	vector<Rating*> ratings;
	GMatrixFactorization_sparseMatrixToRatings(*pData, heap, ratings, *m_pRand);

	// Normalize the ratings
	m_pMins = new double[pData->cols()];
	m_pMaxs = new double[pData->cols()];
	GVec::setAll(m_pMins, 1e200, pData->cols());
	GVec::setAll(m_pMaxs, -1e200, pData->cols());
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		m_pMins[pRating->m_item] = std::min(m_pMins[pRating->m_item], pRating->m_rating);
		m_pMaxs[pRating->m_item] = std::max(m_pMaxs[pRating->m_item], pRating->m_rating);
	}
	for(size_t i = 0; i < pData->cols(); i++)
	{
		if(m_pMins[i] >= 1e200)
			m_pMins[i] = 0.0;
		if(m_pMaxs[i] < m_pMins[i] + 1e-12)
			m_pMaxs[i] = m_pMins[i] + 1.0;
	}
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		pRating->m_rating = (pRating->m_rating - m_pMins[pRating->m_item]) / (m_pMaxs[pRating->m_item] - m_pMins[pRating->m_item]);
	}

	// Train
double regularizer = 0.0015;
	GMatrix* pBestUsers = NULL;
	double* pBestWeights = NULL;
	size_t weightCount = m_pModel->countWeights();
	double prevErr = 1e308;
	double bestErr = 1e308;
	double floor = std::max(-50.0, pAF->center() - pAF->halfRange());
	double cap = std::min(50.0, pAF->center() + pAF->halfRange());
	double learningRate = 0.05;
	while(learningRate >= 0.0005)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_pRand->next(n)], ratings[n - 1]);

		// Do an epoch of training
		m_pModel->setLearningRate(learningRate);
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;
			double* pUserPreferenceVector = m_pUsers->row(pRating->m_user);
			m_pModel->forwardPropSingleOutput(pUserPreferenceVector, pRating->m_item);
			m_pModel->setErrorSingleOutput(pRating->m_rating, pRating->m_item, m_pModel->backPropTargetFunction());
			m_pModel->backProp()->backpropagateSingleOutput(pRating->m_item);
m_pModel->decayWeightsSingleOutput(pRating->m_item, regularizer);
			m_pModel->backProp()->descendGradientSingleOutput(pRating->m_item, pUserPreferenceVector, learningRate, m_pModel->momentum(), m_pModel->useInputBias());
GVec::multiply(pUserPreferenceVector, 1.0 - learningRate * regularizer, m_intrinsicDims);
			m_pModel->backProp()->adjustFeaturesSingleOutput(pRating->m_item, pUserPreferenceVector, learningRate, m_pModel->useInputBias());
//			GVec::floorValues(pUserPreferenceVector, floor, m_intrinsicDims);
//			GVec::capValues(pUserPreferenceVector, cap, m_intrinsicDims);
		}

		// Stopping criteria
		double rsse = sqrt(validate(ratings));
		if(rsse < bestErr)
		{
			bestErr = rsse;
			delete(pBestUsers);
			pBestUsers = m_pUsers->clone();
			if(!pBestWeights)
				pBestWeights = new double[weightCount];
			m_pModel->weights(pBestWeights);
		}
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.00001) // If the amount of improvement is less than 0.01%
			learningRate *= 0.7; // decay the learning rate
		prevErr = rsse;
	}
	if(pBestUsers)
	{
		delete(m_pUsers);
		m_pUsers = pBestUsers;
		m_pModel->setWeights(pBestWeights);
		delete[] pBestWeights;
	}
}
*/
// virtual
double GNeuralRecommender::predict(size_t user, size_t item)
{
	return (m_pMaxs[item] - m_pMins[item]) * m_pModel->forwardPropSingleOutput(m_pUsers->row(user), item) + m_pMins[item];
}

// virtual
void GNeuralRecommender::impute(double* pVec)
{
	// Initialize a preference vector
	GTEMPBUF(double, pPrefVec, m_intrinsicDims);
	GActivationFunction* pAF = m_pModel->layer(0).m_pActivationFunction;
	for(size_t i = 0; i < m_intrinsicDims; i++)
		pPrefVec[i] = pAF->center() + 0.25 * m_pRand->normal();

	// Make a single list of all the ratings
	size_t itemCount = m_pModel->layer(m_pModel->layerCount() - 1).m_neurons.size();
	GHeap heap(2048);
	vector<Rating*> ratings;
	GMatrixFactorization_vectorToRatings(pVec, itemCount, heap, ratings, *m_pRand);
	for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
	{
		Rating* pRating = *it;
		pRating->m_rating = (pRating->m_rating - m_pMins[pRating->m_item]) / (m_pMaxs[pRating->m_item] - m_pMins[pRating->m_item]);
	}

	// Refine the preference vector
	double prevErr = 1e308;
	double learningRate = 0.2;
	while(learningRate >= 0.01)
	{
		// Shuffle the ratings
		for(size_t n = ratings.size(); n > 0; n--)
			std::swap(ratings[(size_t)m_pRand->next(n)], ratings[n - 1]);

		// Do an epoch of training
		m_pModel->setLearningRate(learningRate);
		double sse = 0;
		for(vector<Rating*>::iterator it = ratings.begin(); it != ratings.end(); it++)
		{
			Rating* pRating = *it;
			double predictedRating = m_pModel->forwardPropSingleOutput(pPrefVec, pRating->m_item);
			double d = pRating->m_rating - predictedRating;
			sse += (d * d);
			m_pModel->setErrorSingleOutput(pRating->m_rating, pRating->m_item, m_pModel->backPropTargetFunction());
			m_pModel->backProp()->backpropagateSingleOutput(pRating->m_item);
			m_pModel->backProp()->adjustFeaturesSingleOutput(pRating->m_item, pPrefVec, learningRate, m_pModel->useInputBias());
		}

		// Stopping criteria
		double rsse = sqrt(sse);
		if(rsse < 1e-12 || 1.0 - (rsse / prevErr) < 0.0001) // If the amount of improvement is less than 0.01%
			learningRate *= 0.8; // decay the learning rate
		prevErr = rsse;
	}

	// Impute missing values
	for(size_t i = 0; i < itemCount; i++)
	{
		if(pVec[i] == UNKNOWN_REAL_VALUE)
			pVec[i] = (m_pMaxs[i] - m_pMins[i]) * m_pModel->forwardPropSingleOutput(pPrefVec, i) + m_pMins[i];
	}
}








GBagOfRecommenders::GBagOfRecommenders(GRand& rand)
: GCollaborativeFilter(), m_itemCount(0), m_rand(rand)
{
}

GBagOfRecommenders::GBagOfRecommenders(GDomNode* pNode, GRand& rand)
: GCollaborativeFilter(pNode), m_rand(rand)
{
	m_itemCount = pNode->field("ic")->asInt();
	GLearnerLoader ll;
	for(GDomListIterator it(pNode->field("filters")); it.current(); it.advance())
		m_filters.push_back(ll.loadCollaborativeFilter(it.current(), rand));
}

GBagOfRecommenders::~GBagOfRecommenders()
{
	clear();
}

void GBagOfRecommenders::clear()
{
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
		delete(*it);
	m_filters.clear();
}

void GBagOfRecommenders::addRecommender(GCollaborativeFilter* pRecommender)
{
	m_filters.push_back(pRecommender);
}

// virtual
void GBagOfRecommenders::trainBatch(GSparseMatrix* pData)
{
	m_itemCount = pData->cols();
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
	{
		// Make a matrix that randomly samples about half of the elements in pData
		GSparseMatrix tmp(pData->rows(), pData->cols(), pData->defaultValue());
		for(size_t i = 0; i < pData->rows(); i++)
		{
			GSparseMatrix::Iter end2 = pData->rowEnd(i);
			for(GSparseMatrix::Iter it2 = pData->rowBegin(i); it2 != end2; it2++)
			{
				if(m_rand.next(2) == 0)
					tmp.set(i, it2->first, it2->second);
			}
		}

		// Train with it
		(*it)->trainBatch(&tmp);
	}
}

// virtual
double GBagOfRecommenders::predict(size_t user, size_t item)
{
	double sum = 0.0;
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
		sum += (*it)->predict(user, item);
	return sum / m_filters.size();
}

// virtual
void GBagOfRecommenders::impute(double* pVec)
{
	GTEMPBUF(double, pBuf1, m_itemCount);
	GTEMPBUF(double, pBuf2, m_itemCount);
	GVec::setAll(pBuf2, 0.0, m_itemCount);
	double i = 0.0;
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
	{
		GVec::copy(pBuf1, pVec, m_itemCount);
		(*it)->impute(pBuf1);
		GVec::multiply(pBuf2, i / (i + 1), m_itemCount);
		GVec::addScaled(pBuf2, 1.0 / (i + 1), pBuf1, m_itemCount);
		i++;
	}
	GVec::copy(pVec, pBuf2, m_itemCount);
}

// virtual
GDomNode* GBagOfRecommenders::serialize(GDom* pDoc)
{
	GDomNode* pNode = baseDomNode(pDoc, "GBagOfRecommenders");
	pNode->addField(pDoc, "ic", pDoc->newInt(m_itemCount));
	GDomNode* pFilters = pNode->addField(pDoc, "filters", pDoc->newList());
	for(vector<GCollaborativeFilter*>::iterator it = m_filters.begin(); it != m_filters.end(); it++)
		pFilters->addItem(pDoc, (*it)->serialize(pDoc));
	return pNode;
}




} // namespace GClasses
