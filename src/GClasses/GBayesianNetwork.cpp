/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    Eric Moyer,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or find a way to pay it forward. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/

#include "GBayesianNetwork.h"
#include "GRand.h"
#include "GError.h"
#include "GMath.h"
#include "GHolders.h"
#include <stddef.h>
#include <cmath>

using namespace GClasses;
using std::vector;


#define MIN_LOG_PROB -1e300


GPGMNode::GPGMNode()
: m_observed(false)
{
}

// virtual
GPGMNode::~GPGMNode()
{
}







GPGMVariable::GPGMVariable()
: GPGMNode()
{

}

// virtual
GPGMVariable::~GPGMVariable()
{
}

// virtual
void GPGMVariable::onNewChild(GPGMVariable* pChild)
{
	m_children.push_back(pChild);
}

size_t GPGMVariable::catCount()
{
	size_t cats = 1;
	for(vector<GPGMCategorical*>::iterator it = m_catParents.begin(); it != m_catParents.end(); it++)
		cats *= (*it)->categories();
	return cats;
}

size_t GPGMVariable::currentCatIndex()
{
	size_t mult = 1;
	size_t ind = 0;
	for(vector<GPGMCategorical*>::iterator it = m_catParents.begin(); it != m_catParents.end(); it++)
	{
		GPGMCategorical* pPar = *it;
		size_t val = (size_t)pPar->currentValue();
		ind += mult * val;
		if(pPar->categories() == 0)
			throw Ex("Categorical parent has no categories. Perhaps addWeights was never called as it ought to have been");
		mult *= pPar->categories();
	}
	GAssert(ind < mult);
	return ind;
}








GPGMCategorical::GPGMCategorical(size_t categories, GPGMNode* pDefaultWeight)
: GPGMVariable(), m_categories(categories), m_val(0)
{
	if(categories < 2)
		throw Ex("Expected at least 2 categories. Got ", to_str(categories));
	m_weights.resize(categories, pDefaultWeight);
}

void GPGMCategorical::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultWeight)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_weights.resize(m_categories * catCount(), pDefaultWeight);
}

void GPGMCategorical::setWeights(size_t cat, GPGMNode* pW1, GPGMNode* pW2, GPGMNode* pW3, GPGMNode* pW4, GPGMNode* pW5, GPGMNode* pW6, GPGMNode* pW7, GPGMNode* pW8)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = cat * m_categories;
	m_weights[base] = pW1;
	pW1->onNewChild(this);
	m_weights[base + 1] = pW2;
	pW2->onNewChild(this);
	size_t given = 2;
	if(pW3){
		m_weights[base + 2] = pW3;
		pW3->onNewChild(this);
		given++;
		if(pW4){
			m_weights[base + 3] = pW4;
			pW4->onNewChild(this);
			given++;
			if(pW5){
				m_weights[base + 4] = pW5;
				pW5->onNewChild(this);
				given++;
				if(pW6){
					m_weights[base + 5] = pW6;
					pW6->onNewChild(this);
					given++;
					if(pW7){
						m_weights[base + 6] = pW7;
						pW7->onNewChild(this);
						given++;
						if(pW8){
							m_weights[base + 7] = pW8;
							pW8->onNewChild(this);
							given++;
						}
					}
				}
			}
		}
	}
	if(given != m_categories)
		throw Ex("Expected ", to_str(m_categories), " sequential non-NULL nodes. Got ", to_str(given));
}

// virtual
double GPGMCategorical::currentValue()
{
	if(m_observed)
		return m_observedValue;
	else
		return m_val;
}

// virtual
void GPGMCategorical::sample(GRand* pRand)
{
	if(m_observed)
		return;

	// Compute the sum Markov-blanket probability over each category
	size_t base = m_categories * currentCatIndex();
	double sumProb = 0.0;
	for(size_t i = 0; i < m_categories; i++)
	{
		double catProb = m_weights[base + i]->currentValue();
		for(vector<GPGMVariable*>::const_iterator it = children().begin(); it != children().end(); it++)
		{
			GPGMVariable* pChildNode = *it;
			double oldVal = m_val;
			m_val = (double)i;
			catProb *= pChildNode->likelihood(pChildNode->currentValue());
			m_val = oldVal;
		}
		sumProb += catProb;
	}

	// Pick a category at random according to Markov-blanket probabilities
	double uni = pRand->uniform();
	double sumProb2 = 0.0;
	for(size_t i = 0; i < m_categories; i++)
	{
		double catProb = m_weights[base + i]->currentValue();
		for(vector<GPGMVariable*>::const_iterator it = children().begin(); it != children().end(); it++)
		{
			GPGMVariable* pChildNode = *it;
			double oldVal = m_val;
			m_val = (double)i;
			catProb *= pChildNode->likelihood(pChildNode->currentValue());
			m_val = oldVal;
		}
		m_val = i;
		sumProb2 += catProb / sumProb;
		if(sumProb2 >= uni)
			break;
	}
}

// virtual
double GPGMCategorical::likelihood(double x)
{
	size_t base = m_categories * currentCatIndex();
	double sumWeight = 0.0;
	for(size_t i = 0; i < m_categories; i++)
		sumWeight += m_weights[base + i]->currentValue();
	size_t xx = (size_t)x;
	GAssert(xx >= 0 && xx < m_categories);
	GPGMNode* pX = m_weights[base + xx];
	double num = pX->currentValue();
	if(num > 0.0 && sumWeight > 0.0)
		return num / sumWeight;
	else
		return 0.0;
}









GPGMMetropolisNode::GPGMMetropolisNode(double priorMean, double priorDeviation)
: GPGMVariable(), m_currentMean(priorMean), m_currentDeviation(priorDeviation), m_nSamples(0), m_nNewValues(0), m_sumOfValues(0), m_sumOfSquaredValues(0)
{
}

double GPGMMetropolisNode::gibbs(double x)
{
	double d;
	double logSum = log(likelihood(x));
	if(logSum >= MIN_LOG_PROB)
	{
		const vector<GPGMVariable*>& kids = children();
		for(vector<GPGMVariable*>::const_iterator it = kids.begin(); it != kids.end(); it++)
		{
			GPGMVariable* pChildNode = *it;
			double oldVal = m_currentMean;
			m_currentMean = x;
			d = log(pChildNode->likelihood(pChildNode->currentValue()));
			m_currentMean = oldVal;
			if(d >= MIN_LOG_PROB)
				logSum += d;
			else
				return MIN_LOG_PROB;
		}
		return logSum;
	}
	else
		return MIN_LOG_PROB;
}

bool GPGMMetropolisNode::metropolis(GRand* pRand)
{
	double dCandidateValue = pRand->normal() * m_currentDeviation + m_currentMean;
	if(isDiscrete())
		dCandidateValue = floor(dCandidateValue + 0.5);
	if(dCandidateValue == m_currentMean)
		return false;
	double cand = gibbs(dCandidateValue);
	if(cand >= MIN_LOG_PROB)
	{
		double curr = gibbs(m_currentMean);
		if(curr >= MIN_LOG_PROB)
		{
			if(log(pRand->uniform()) < cand - curr)
			{
				m_currentMean = dCandidateValue;
				return true;
			}
			else
				return false;
		}
		else
			return false;
	}
	else
		return false;
}

void GPGMMetropolisNode::sample(GRand* pRand)
{
	if(metropolis(pRand))
	{
		if(++m_nNewValues >= 10)
		{
			double dMean = m_sumOfValues / m_nSamples;
			m_currentDeviation = sqrt(m_sumOfSquaredValues / m_nSamples - (dMean * dMean));
			m_nNewValues = 0;
		}
	}
	if(m_nSamples < 0xffffffff)
	{
		m_sumOfValues += m_currentMean;
		m_sumOfSquaredValues += (m_currentMean * m_currentMean);
		m_nSamples++;
	}
}

// virtual
double GPGMMetropolisNode::currentValue()
{
	if(m_observed)
		return m_observedValue;
	else
		return m_currentMean;
}







#define SQRT_2PI 2.50662827463

GPGMNormal::GPGMNormal(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_meanAndDev.resize(2, pDefaultVal);
}

void GPGMNormal::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_meanAndDev.resize(2 * catCount(), pDefaultVal);
}

void GPGMNormal::setMeanAndDev(size_t cat, GPGMNode* pMean, GPGMNode* pDeviation)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_meanAndDev[base] = pMean;
	pMean->onNewChild(this);
	m_meanAndDev[base + 1] = pDeviation;
	pDeviation->onNewChild(this);
}

// virtual
double GPGMNormal::likelihood(double x)
{
	size_t base = 2 * currentCatIndex();
	double mean = m_meanAndDev[base]->currentValue();
	double dev = m_meanAndDev[base + 1]->currentValue();
	double t = x - mean;
	return 1.0 / (dev * SQRT_2PI) * exp(-(t * t) / (2.0 * dev * dev));
}









GPGMLogNormal::GPGMLogNormal(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_meanAndDev.resize(2, pDefaultVal);
}

void GPGMLogNormal::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_meanAndDev.resize(2 * catCount(), pDefaultVal);
}

void GPGMLogNormal::setMeanAndDev(size_t cat, GPGMNode* pMean, GPGMNode* pDeviation)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_meanAndDev[base] = pMean;
	pMean->onNewChild(this);
	m_meanAndDev[base + 1] = pDeviation;
	pDeviation->onNewChild(this);
}

// virtual
double GPGMLogNormal::likelihood(double x)
{
	size_t base = 2 * currentCatIndex();
	double mean = m_meanAndDev[base]->currentValue();
	double dev = m_meanAndDev[base + 1]->currentValue();
	double t = log(x - mean);
	return 1.0 / (x * dev * SQRT_2PI) * exp(-(t * t) / (2.0 * dev * dev));
}









GPGMPareto::GPGMPareto(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_alphaAndM.resize(2, pDefaultVal);
}

void GPGMPareto::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_alphaAndM.resize(2 * catCount(), pDefaultVal);
}

void GPGMPareto::setAlphaAndM(size_t cat, GPGMNode* pAlpha, GPGMNode* pM)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_alphaAndM[base] = pAlpha;
	pAlpha->onNewChild(this);
	m_alphaAndM[base + 1] = pM;
	pM->onNewChild(this);
}

// virtual
double GPGMPareto::likelihood(double x)
{
	size_t base = 2 * currentCatIndex();
	double alpha = m_alphaAndM[base]->currentValue();
	double m = m_alphaAndM[base + 1]->currentValue();
	if(x < m)
		return 0;
	return alpha * pow(m, alpha) / pow(x, alpha + 1.0);
}









GPGMUniformDiscrete::GPGMUniformDiscrete(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_minAndMax.resize(2, pDefaultVal);
}

void GPGMUniformDiscrete::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_minAndMax.resize(2 * catCount(), pDefaultVal);
}

void GPGMUniformDiscrete::setMinAndMax(size_t cat, GPGMNode* pMin, GPGMNode* pMax)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_minAndMax[base] = pMin;
	pMin->onNewChild(this);
	m_minAndMax[base + 1] = pMax;
	pMax->onNewChild(this);
}

// virtual
double GPGMUniformDiscrete::likelihood(double x)
{
	size_t base = 2 * currentCatIndex();
	double a = std::ceil(m_minAndMax[base]->currentValue());
	double b = std::floor(m_minAndMax[base + 1]->currentValue());
	if(x < a)
		return 0.0;
	if(x > b)
		return 0.0;
	return 1.0 / (b - a + 1.0);
}









GPGMUniformContinuous::GPGMUniformContinuous(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_minAndMax.resize(2, pDefaultVal);
}

void GPGMUniformContinuous::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_minAndMax.resize(2 * catCount(), pDefaultVal);
}

void GPGMUniformContinuous::setMinAndMax(size_t cat, GPGMNode* pMin, GPGMNode* pMax)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_minAndMax[base] = pMin;
	pMin->onNewChild(this);
	m_minAndMax[base + 1] = pMax;
	pMax->onNewChild(this);
}

// virtual
double GPGMUniformContinuous::likelihood(double x)
{
	size_t base = 2 * currentCatIndex();
	double a = m_minAndMax[base]->currentValue();
	double b = m_minAndMax[base + 1]->currentValue();
	if(x < a)
		return 0.0;
	if(x > b)
		return 0.0;
	return 1.0 / (b - a);
}









GPGMPoisson::GPGMPoisson(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_lambda.resize(1, pDefaultVal);
}

void GPGMPoisson::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_lambda.resize(1 * catCount(), pDefaultVal);
}

void GPGMPoisson::setLambda(size_t cat, GPGMNode* pLambda)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 1 * cat;
	m_lambda[base] = pLambda;
	pLambda->onNewChild(this);
}

// virtual
double GPGMPoisson::likelihood(double x)
{
	size_t base = 1 * currentCatIndex();
	double l = m_lambda[base]->currentValue();
	if(x < 0)
		return 0.0;
	return pow(l, x) * exp(-l) / GMath::gamma(x + 1);
}









GPGMExponential::GPGMExponential(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_lambda.resize(1, pDefaultVal);
}

void GPGMExponential::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_lambda.resize(1 * catCount(), pDefaultVal);
}

void GPGMExponential::setLambda(size_t cat, GPGMNode* pLambda)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 1 * cat;
	m_lambda[base] = pLambda;
	pLambda->onNewChild(this);
}

// virtual
double GPGMExponential::likelihood(double x)
{
	size_t base = 1 * currentCatIndex();
	double l = m_lambda[base]->currentValue();
	if(x < 0)
		return 0.0;
	return l * exp(-l * x);
}









GPGMBeta::GPGMBeta(double priorMean, double priorDeviation, GPGMNode* pDefaultVal)
: GPGMMetropolisNode(priorMean, priorDeviation)
{
	m_alphaAndBeta.resize(2, pDefaultVal);
}

void GPGMBeta::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_alphaAndBeta.resize(2 * catCount(), pDefaultVal);
}

void GPGMBeta::setAlphaAndBeta(size_t cat, GPGMNode* pAlpha, GPGMNode* pBeta)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_alphaAndBeta[base] = pAlpha;
	pAlpha->onNewChild(this);
	m_alphaAndBeta[base + 1] = pBeta;
	pBeta->onNewChild(this);
}

// virtual
double GPGMBeta::likelihood(double x)
{
	if(x < 0.0 || x > 1.0)
		return 0.0;
	size_t base = 2 * currentCatIndex();
	double alpha = m_alphaAndBeta[base]->currentValue();
	double beta = m_alphaAndBeta[base + 1]->currentValue();
	double denom = GMath::gamma(alpha) * GMath::gamma(beta);
	if(std::abs(denom) < 1e-15)
		denom = (denom < 0 ? -1e-15 : 1e-15);
	return GMath::gamma(alpha + beta) / denom * pow(x, alpha - 1.0) * pow(1.0 - x, beta - 1.0);
}









GPGMGamma::GPGMGamma(double priorMean, double priorDeviation, GPGMNode* pDefaultVal, bool betaIsScaleInsteadOfRate)
: GPGMMetropolisNode(priorMean, priorDeviation), m_betaIsScaleInsteadOfRate(betaIsScaleInsteadOfRate)
{
	m_alphaAndBeta.resize(2, pDefaultVal);
}

void GPGMGamma::addCatParent(GPGMCategorical* pNode, GPGMNode* pDefaultVal)
{
	m_catParents.push_back(pNode);
	pNode->onNewChild(this);
	m_alphaAndBeta.resize(2 * catCount(), pDefaultVal);
}

void GPGMGamma::setAlphaAndBeta(size_t cat, GPGMNode* pAlpha, GPGMNode* pBeta)
{
	if(cat >= catCount())
		throw Ex("out of range");
	size_t base = 2 * cat;
	m_alphaAndBeta[base] = pAlpha;
	pAlpha->onNewChild(this);
	m_alphaAndBeta[base + 1] = pBeta;
	pBeta->onNewChild(this);
}

// virtual
double GPGMGamma::likelihood(double x)
{
	if(x < 0.0)
		return 0.0;
	size_t base = 2 * currentCatIndex();
	double alpha = m_alphaAndBeta[base]->currentValue();
	double beta = m_alphaAndBeta[base + 1]->currentValue();
	if(m_betaIsScaleInsteadOfRate)
		beta = 1.0 / beta;
	return pow(beta, alpha) * pow(x, alpha - 1.0) * exp(-beta * x) / GMath::gamma(alpha);
}









GBayesNet::GBayesNet(size_t seed)
: m_heap(2048), m_rand(seed)
{
	m_pConstOne = newConst(1.0);
}

GBayesNet::~GBayesNet()
{
}

GPGMConstant* GBayesNet::newConst(double val)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMConstant));
	return new (pDest) GPGMConstant(val);
}

GPGMCategorical* GBayesNet::newCat(size_t categories)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMCategorical));
	GPGMCategorical* pNode = new (pDest) GPGMCategorical(categories, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMNormal* GBayesNet::newNormal(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMNormal));
	GPGMNormal* pNode = new (pDest) GPGMNormal(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMLogNormal* GBayesNet::newLogNormal(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMLogNormal));
	GPGMLogNormal* pNode = new (pDest) GPGMLogNormal(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMPareto* GBayesNet::newPareto(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMPareto));
	GPGMPareto* pNode = new (pDest) GPGMPareto(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMUniformDiscrete* GBayesNet::newUniformDiscrete(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMUniformDiscrete));
	GPGMUniformDiscrete* pNode = new (pDest) GPGMUniformDiscrete(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMUniformContinuous* GBayesNet::newUniformContinuous(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMUniformContinuous));
	GPGMUniformContinuous* pNode = new (pDest) GPGMUniformContinuous(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMPoisson* GBayesNet::newPoisson(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMPoisson));
	GPGMPoisson* pNode = new (pDest) GPGMPoisson(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMExponential* GBayesNet::newExponential(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMExponential));
	GPGMExponential* pNode = new (pDest) GPGMExponential(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMBeta* GBayesNet::newBeta(double priorMean, double priorDev)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMBeta));
	GPGMBeta* pNode = new (pDest) GPGMBeta(priorMean, priorDev, m_pConstOne);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

GPGMGamma* GBayesNet::newGamma(double priorMean, double priorDev, bool useScaleInsteadOfRate)
{
	char* pDest = m_heap.allocAligned(sizeof(GPGMGamma));
	GPGMGamma* pNode = new (pDest) GPGMGamma(priorMean, priorDev, m_pConstOne, useScaleInsteadOfRate);
	m_sampleNodes.push_back(pNode);
	return pNode;
}

void GBayesNet::sample()
{
	for(vector<GPGMVariable*>::iterator it = m_sampleNodes.begin(); it != m_sampleNodes.end(); it++)
		(*it)->sample(&m_rand);
}

#ifndef MIN_PREDICT
void GBayesNet_simpleTest()
{
	GBayesNet bn;
	GPGMCategorical* pPar = bn.newCat(2);
	pPar->setWeights(0, bn.newConst(0.4), bn.newConst(0.6));

	GPGMNormal* pChild = bn.newNormal(1.0, 3.0);
	pChild->addCatParent(pPar, bn.def());
	pChild->setMeanAndDev(0, bn.newConst(0.0), bn.newConst(1.0));
	pChild->setMeanAndDev(1, bn.newConst(3.0), bn.newConst(2.0));

	pChild->setObserved(1.0);

	for(size_t burnin = 0; burnin < 10000; burnin++)
		bn.sample();
	size_t parCount = 0;
	size_t sampleCount = 0;
	for(size_t sample = 0; sample < 50000; sample++)
	{
		bn.sample();
		if(pPar->currentValue() == 0.0)
			parCount++;
		sampleCount++;
	}

	if(std::abs((double)parCount / sampleCount - 0.5714286) > 0.001)
		throw Ex("Not close enough");
}

void GBayesNet_threeTest()
{
	GBayesNet bn;
	GPGMCategorical* pA = bn.newCat(2);
	pA->setWeights(0, bn.newConst(2.0 / 5.0), bn.newConst(3.0 / 5.0));

	GPGMCategorical* pB = bn.newCat(2);
	pB->addCatParent(pA, bn.def());
	pB->setWeights(0, bn.newConst(2.0 / 3.0), bn.newConst(1.0 / 3.0));
	pB->setWeights(1, bn.newConst(3.0 / 7.0), bn.newConst(4.0 / 7.0));

	GPGMCategorical* pC = bn.newCat(2);
	pC->addCatParent(pB, bn.def());
	pC->setWeights(0, bn.newConst(1.0 / 2.0), bn.newConst(1.0 / 2.0));
	pC->setWeights(1, bn.newConst(1.0 / 3.0), bn.newConst(2.0 / 3.0));

	pA->setObserved(0.0);
	pC->setObserved(0.0);

	for(size_t burnin = 0; burnin < 10000; burnin++)
		bn.sample();
	size_t bCount = 0;
	size_t sampleCount = 0;
	for(size_t sample = 0; sample < 50000; sample++)
	{
		bn.sample();
		if(pB->currentValue() == 0.0)
			bCount++;
		sampleCount++;
	}

	if(std::abs((double)bCount / sampleCount - 0.75) > 0.005)
		throw Ex("Not close enough");
}

void GBayesNet_alarmTest()
{
	// This example is given in Russell and Norvig page 504. (See also http://www.d.umn.edu/~rmaclin/cs8751/Notes/chapter14a.pdf)
	GBayesNet bn;
	GPGMCategorical* pBurglary = bn.newCat(2);
	pBurglary->setWeights(0, bn.newConst(0.001), bn.newConst(0.999));

	GPGMCategorical* pEarthquake = bn.newCat(2);
	pEarthquake->setWeights(0, bn.newConst(0.002), bn.newConst(0.998));

	GPGMCategorical* pAlarm = bn.newCat(2);
	pAlarm->addCatParent(pBurglary, bn.def());
	pAlarm->addCatParent(pEarthquake, bn.def());
	pAlarm->setWeights(0, bn.newConst(0.95), bn.newConst(0.05));
	pAlarm->setWeights(1, bn.newConst(0.29), bn.newConst(0.71));
	pAlarm->setWeights(2, bn.newConst(0.94), bn.newConst(0.06));
	pAlarm->setWeights(3, bn.newConst(0.001), bn.newConst(0.999));

	GPGMCategorical* pJohnCalls = bn.newCat(2);
	pJohnCalls->addCatParent(pAlarm, bn.def());
	pJohnCalls->setWeights(0, bn.newConst(0.9), bn.newConst(0.1));
	pJohnCalls->setWeights(1, bn.newConst(0.05), bn.newConst(0.95));

	GPGMCategorical* pMaryCalls = bn.newCat(2);
	pMaryCalls->addCatParent(pAlarm, bn.def());
	pMaryCalls->setWeights(0, bn.newConst(0.7), bn.newConst(0.3));
	pMaryCalls->setWeights(1, bn.newConst(0.01), bn.newConst(0.99));

	pJohnCalls->setObserved(0.0);
	pMaryCalls->setObserved(0.0);

	GRand rand(0);
	for(size_t burnin = 0; burnin < 10000; burnin++)
		bn.sample();
	size_t sampleCount = 0;
	vector<double> probs;
	probs.resize(3);
	for(size_t sample = 0; sample < 50000; sample++)
	{
		bn.sample();
		sampleCount++;
		probs[0] *= (1.0 - 1.0 / sampleCount);
		if(pBurglary->currentValue() == 0.0)
			probs[0] += (1.0 / sampleCount);
		probs[1] *= (1.0 - 1.0 / sampleCount);
		if(pEarthquake->currentValue() == 0.0)
			probs[1] += (1.0 / sampleCount);
		probs[2] *= (1.0 - 1.0 / sampleCount);
		if(pAlarm->currentValue() == 0.0)
			probs[2] += (1.0 / sampleCount);
	}

	if(std::abs(probs[0] - 0.284) > 0.005)
		throw Ex("Not close enough");
	if(std::abs(probs[1] - 0.176) > 0.005)
		throw Ex("Not close enough");
	if(std::abs(probs[2] - 0.761) > 0.005)
		throw Ex("Not close enough");
}

void GBayesNet::test()
{
	GBayesNet_simpleTest();
	GBayesNet_threeTest();
	GBayesNet_alarmTest();
}
#endif

